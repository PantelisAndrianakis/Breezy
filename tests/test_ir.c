#include "test_framework.h"
#include "ir.h"
#include "irlower.h"
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "types.h"
#include "resolve.h"

/* Lex -> parse -> register -> resolve a snippet and return its first free
   function (u->funcs[0]). Mirrors test_resolve.c's build1. */
static TypeTable g_tt;
static const Func *parse_one_func(const char *src)
{
	static Parser ps;
	Unit *u;
	types_init(&g_tt);
	types_register_builtins(&g_tt);
	parser_init(&ps, src);
	u = parse_unit(&ps);
	types_register_unit_names(&g_tt, u);
	types_register_interfaces(&g_tt, u);
	types_register_unit_members(&g_tt, u);
	resolve_program(&g_tt, &u, 1);
	return u->funcs[0];
}

static void test_ir_build_basic(void)
{
	Func dummy;
	memset(&dummy, 0, sizeof(dummy));
	IRFunc *f = ir_func_new(&dummy);
	ASSERT(f != NULL);

	int b0 = ir_block_new(f);
	ASSERT_INT(b0, 0);
	ASSERT_INT(f->block_count, 1);

	IRReg r0 = ir_reg(f);
	IRReg r1 = ir_reg(f);
	ASSERT_INT(r0, 0);
	ASSERT_INT(r1, 1);
	ASSERT_INT(f->vreg_count, 2);

	IRInstr *c = ir_emit(f, b0, IR_CONST, TY_INT);
	c->dst = r0;
	c->imm = 7;
	IRInstr *m = ir_emit(f, b0, IR_MOVE, TY_INT);
	m->dst = r1;
	m->a = r0;

	ASSERT_INT(f->blocks[0].count, 2);
	ASSERT_INT(f->blocks[0].instrs[0].op, IR_CONST);
	ASSERT_INT((int)f->blocks[0].instrs[0].imm, 7);
	ASSERT_INT(f->blocks[0].instrs[1].a, r0);

	ir_func_free(f);
}

static void test_eligible_accepts_scalar_loop(void)
{
	const Func *f = parse_one_func(
		"long sum(int limit)\n"
		"{\n"
		"	long s;\n"
		"	s = 0;\n"
		"	for (int i = 0; i < limit; i = i + 1)\n"
		"	{\n"
		"		s = s + (long)i;\n"
		"	}\n"
		"	return s;\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 1);
}

static void test_eligible_rejects_string(void)
{
	const Func *f = parse_one_func(
		"string greet()\n"
		"{\n"
		"	return \"hi\";\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 0);
}

static void test_eligible_rejects_call(void)
{
	const Func *f = parse_one_func(
		"int two() { return one() + one(); }\n"
		"int one() { return 1; }\n");
	/* funcs[0] is `two`, which contains calls -> ineligible. */
	ASSERT_INT(ir_eligible(f), 0);
}

static void test_eligible_accepts_int_array(void)
{
	/* Integer arrays are supported (bounds-checked): an int[] parameter indexed by
	   a constant is eligible. */
	const Func *f = parse_one_func(
		"int first(int[] xs)\n"
		"{\n"
		"	return xs[0];\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 1);
}

static void test_eligible_rejects_float_array(void)
{
	/* Float-element arrays are out of scope (Plan 3b / xmm). */
	const Func *f = parse_one_func(
		"int firstd(double[] xs)\n"
		"{\n"
		"	return (int)xs[0];\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 0);
}

static void test_eligible_rejects_new_array(void)
{
	/* Allocating an array is a managed call, excluded from the call-free IR. */
	const Func *f = parse_one_func(
		"int mk()\n"
		"{\n"
		"	int[] a;\n"
		"	a = new int[4];\n"
		"	return a[0];\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 0);
}

static void test_lower_produces_blocks_and_ret(void)
{
	const Func *f = parse_one_func(
		"int idret(int x)\n"
		"{\n"
		"	return x + 1;\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 1);
	IRFunc *ir = ir_lower_func(f, 0);
	ASSERT(ir != NULL);
	ASSERT(ir->block_count >= 1);
	IRBlock *last = &ir->blocks[ir->block_count - 1];
	ASSERT(last->count >= 1);
	ASSERT_INT(last->instrs[last->count - 1].op, IR_RET);
	ir_func_free(ir);
}

static void test_lower_collatz_succeeds(void)
{
	/* The whole collatz kernel lowers (no NULL fallback): for/while/if/else,
	   casts, %/ and comparisons all translate. */
	const Func *f = parse_one_func(
		"long collatzSum(int n)\n"
		"{\n"
		"	long checksum;\n"
		"	checksum = 0;\n"
		"	for (int start = 1; start <= n; start = start + 1)\n"
		"	{\n"
		"		long m;\n"
		"		m = (long)start;\n"
		"		int steps;\n"
		"		steps = 0;\n"
		"		while (m > 1)\n"
		"		{\n"
		"			if (m % 2 == 0) { m = m / 2; }\n"
		"			else { m = 3 * m + 1; }\n"
		"			steps = steps + 1;\n"
		"		}\n"
		"		checksum = checksum + (long)steps;\n"
		"	}\n"
		"	return checksum;\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 1);
	IRFunc *ir = ir_lower_func(f, 0);
	ASSERT(ir != NULL);
	ir_func_free(ir);
}

/* The `checked` bit of the first non-frame element load (scale 4, disp 32) in a
   lowered function, or -1 if there is none. checked == !anno_index_safe. */
static int first_elem_load_checked(const char *src)
{
	const Func *f = parse_one_func(src);
	if (!ir_eligible(f))
	{
		return -2;
	}

	IRFunc *ir = ir_lower_func(f, 0);
	int checked = -1;
	for (int b = 0; b < ir->block_count && checked < 0; b++)
	{
		for (int i = 0; i < ir->blocks[b].count; i++)
		{
			IRInstr *in = &ir->blocks[b].instrs[i];
			if (in->op == IR_LOAD && !in->is_frame && in->scale == 4 && in->disp == 32)
			{
				checked = in->checked;
				break;
			}
		}
	}

	ir_func_free(ir);
	return checked;
}

static void test_bce_guarded_sweep_is_safe(void)
{
	/* for (i = 0; i < a.length; i++) a[i]  -> provably in range, no bounds check. */
	ASSERT_INT(first_elem_load_checked(
		"long asum(int[] a)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	for (int i = 0; i < a.length; i = i + 1) { s = s + (long)a[i]; }\n"
		"	return s;\n"
		"}\n"), 0);
}

/* Adversarial cases - each MUST stay bounds-checked (checked == 1); a wrong
   anno_index_safe here is a memory-safety hole. */
static void test_bce_offbyone_stays_checked(void)
{
	/* `<=` lets i reach a.length. */
	ASSERT_INT(first_elem_load_checked(
		"long f(int[] a)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	for (int i = 0; i <= a.length; i = i + 1) { s = s + (long)a[i]; }\n"
		"	return s;\n"
		"}\n"), 1);
}

static void test_bce_other_array_stays_checked(void)
{
	/* Guard is on a.length but the access is into b. */
	ASSERT_INT(first_elem_load_checked(
		"long f(int[] a, int[] b)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	for (int i = 0; i < a.length; i = i + 1) { s = s + (long)b[i]; }\n"
		"	return s;\n"
		"}\n"), 1);
}

static void test_bce_counter_reassigned_stays_checked(void)
{
	/* The body bumps i past the guarded value before the access. */
	ASSERT_INT(first_elem_load_checked(
		"long f(int[] a)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	for (int i = 0; i < a.length; i = i + 1) { i = i + 5; s = s + (long)a[i]; }\n"
		"	return s;\n"
		"}\n"), 1);
}

int main(void)
{
	RUN(test_ir_build_basic);
	RUN(test_eligible_accepts_scalar_loop);
	RUN(test_eligible_rejects_string);
	RUN(test_eligible_rejects_call);
	RUN(test_eligible_accepts_int_array);
	RUN(test_eligible_rejects_float_array);
	RUN(test_eligible_rejects_new_array);
	RUN(test_lower_produces_blocks_and_ret);
	RUN(test_lower_collatz_succeeds);
	RUN(test_bce_guarded_sweep_is_safe);
	RUN(test_bce_offbyone_stays_checked);
	RUN(test_bce_other_array_stays_checked);
	RUN(test_bce_counter_reassigned_stays_checked);
	SUMMARY();
	return 0;
}
