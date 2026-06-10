#include "test_framework.h"
#include "ir.h"
#include "irlower.h"
#include "iremit.h"
#include "regalloc.h"
#include "codegen.h"
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "types.h"
#include "resolve.h"
#include "prelude.h"
#include <stdio.h>
#include <string.h>

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

/* The first for/while statement in f's top-level body, or NULL. */
static const Stmt *first_loop(const Func *f)
{
	for (int i = 0; i < f->body->count; i++)
	{
		Stmt *s = f->body->stmts[i];
		if (s->kind == ST_FOR || s->kind == ST_WHILE)
		{
			return s;
		}
	}

	return 0;
}

static void test_region_eligible_despite_call(void)
{
	/* The helper() call makes the whole function ineligible, but the loop subtree
	   alone is a valid region. */
	const Func *f = parse_one_func(
		"void k(byte[] buf, int[] tbl)\n"
		"{\n"
		"	helper();\n"
		"	for (int i = 0; i < tbl.length; i = i + 1)\n"
		"	{\n"
		"		tbl[i] = (int)buf[i] & 255;\n"
		"	}\n"
		"}\n"
		"void helper() { }\n");
	ASSERT_INT(ir_eligible(f), 0);
	const Stmt *loop = first_loop(f);
	ASSERT(loop != 0);
	ASSERT_INT(ir_region_eligible(loop), 1);
}

static void test_region_rejects_return(void)
{
	/* A region cannot run the function epilogue, so returns inside it bail. */
	const Func *f = parse_one_func(
		"int k(int[] a)\n"
		"{\n"
		"	for (int i = 0; i < a.length; i = i + 1)\n"
		"	{\n"
		"		if (a[i] < 0)\n"
		"		{\n"
		"			return i;\n"
		"		}\n"
		"	}\n"
		"	return 0 - 1;\n"
		"}\n");
	ASSERT_INT(ir_region_eligible(first_loop(f)), 0);
}

static void test_region_rejects_managed_assign(void)
{
	/* Reassigning an array local inside a region would skip refcounting. */
	const Func *f = parse_one_func(
		"void k(int[] a, int[] b)\n"
		"{\n"
		"	for (int i = 0; i < 3; i = i + 1)\n"
		"	{\n"
		"		a = b;\n"
		"	}\n"
		"}\n");
	ASSERT_INT(ir_region_eligible(first_loop(f)), 0);
}

static void test_region_lower_has_no_ret(void)
{
	const Func *f = parse_one_func(
		"void k(int[] tbl)\n"
		"{\n"
		"	helper();\n"
		"	for (int i = 0; i < tbl.length; i = i + 1)\n"
		"	{\n"
		"		tbl[i] = tbl[i] * 3;\n"
		"	}\n"
		"}\n"
		"void helper() { }\n");
	IRFunc *irf = ir_lower_region(f, first_loop(f));
	ASSERT(irf != NULL);
	for (int b = 0; b < irf->block_count; b++)
	{
		for (int i = 0; i < irf->blocks[b].count; i++)
		{
			ASSERT(irf->blocks[b].instrs[i].op != IR_RET);
		}
	}

	/* The fall-off block is last and empty: emission falls through it into the
	   region epilogue. */
	ASSERT_INT(irf->blocks[irf->block_count - 1].count, 0);
	ir_func_free(irf);
}

static void test_whole_function_eligibility_unaffected(void)
{
	/* The region-mode flag must not leak into whole-function checks. */
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
	ASSERT_INT(ir_region_eligible(first_loop(f)), 1);
	ASSERT_INT(ir_eligible(f), 1);
}

/* Emit a region directly through ir_emit_region and load the asm text. */
static char g_region_asm[1 << 15];
static void emit_region_to_buf(const Func *f, const Stmt *loop, Target target, int spill_base)
{
	IRFunc *irf = ir_lower_region(f, loop);
	ASSERT(irf != NULL);
	IRAlloc *a = ra_run(irf);
	FILE *out = fopen("out_ir_region_test.asm", "w+");
	ASSERT(out != NULL);
	Codegen cg;
	cg_init(&cg, out);
	cg.target = target;
	ir_emit_region(&cg, irf, a, spill_base);
	fflush(out);
	rewind(out);
	size_t n = fread(g_region_asm, 1, sizeof(g_region_asm) - 1, out);
	g_region_asm[n] = 0;
	fclose(out);
	remove("out_ir_region_test.asm");
	ra_free(a);
	ir_func_free(irf);
}

static void test_region_emit_no_prologue_no_ret(void)
{
	const Func *f = parse_one_func(
		"void k(byte[] buf, int[] tbl)\n"
		"{\n"
		"	helper();\n"
		"	for (int i = 0; i < tbl.length; i = i + 1)\n"
		"	{\n"
		"		tbl[i] = (int)buf[i] & 255;\n"
		"	}\n"
		"}\n"
		"void helper() { }\n");
	emit_region_to_buf(f, first_loop(f), TARGET_WINDOWS, 256);

	/* A region is body-only: no frame setup, no epilogue, no return. */
	ASSERT(strstr(g_region_asm, "push rbp") == NULL);
	ASSERT(strstr(g_region_asm, "    ret") == NULL);
	ASSERT(strstr(g_region_asm, "pop rbp") == NULL);
	ASSERT(strstr(g_region_asm, "section .") == NULL);

	/* Begin/end markers bracket the region (the Task 3 dispatch test greps them). */
	ASSERT(strstr(g_region_asm, "; ir-region begin") != NULL);
	ASSERT(strstr(g_region_asm, "; ir-region end") != NULL);

	/* The allocation hands out rbx first (RA_REGS[0], callee-saved): the region
	   must save it into the region area and restore it at the end. */
	ASSERT(strstr(g_region_asm, "], rbx") != NULL);
	ASSERT(strstr(g_region_asm, "mov rbx, [rbp - ") != NULL);

	/* The loop body is register-resident: between the begin marker's local loads
	   and the end marker's store-backs there is a .L label (the loop head). */
	ASSERT(strstr(g_region_asm, ".L") != NULL);
}

static void test_region_lowers_bitwise_not(void)
{
	/* ~x lowers as x ^ -1 (one xor, no new IR op); compiler-bench solve-loop shape. */
	const Func *f = parse_one_func(
		"void k(long[] a, long[] b)\n"
		"{\n"
		"	for (int i = 0; i < a.length; i = i + 1)\n"
		"	{\n"
		"		a[i] = a[i] & ~b[i];\n"
		"	}\n"
		"}\n");
	const Stmt *loop = first_loop(f);
	ASSERT_INT(ir_region_eligible(loop), 1);
	IRFunc *irf = ir_lower_region(f, loop);
	ASSERT(irf != NULL);
	int has_xor = 0;
	for (int blk = 0; blk < irf->block_count; blk++)
	{
		for (int i = 0; i < irf->blocks[blk].count; i++)
		{
			if (irf->blocks[blk].instrs[i].op == IR_XOR)
			{
				has_xor = 1;
			}
		}
	}

	ASSERT_INT(has_xor, 1);
	ir_func_free(irf);
}

/* Compile a full unit (prelude + src) through cg_program, exactly as the driver
   does, and load the emitted asm. The test_ir process never sets BZY_IR, so the
   IR backend and regions run with their defaults (on). */
static char g_full_asm[1 << 16];
static void emit_unit_asm(const char *src, Target target)
{
	static Parser parsers[12];
	static Unit *units[12];
	static TypeTable tt;
	int np = BZY_PRELUDE_COUNT;
	for (int i = 0; i < np; i++)
	{
		parser_init(&parsers[i], BZY_PRELUDE[i]);
		units[i] = parse_unit(&parsers[i]);
	}

	parser_init(&parsers[np], src);
	units[np] = parse_unit(&parsers[np]);
	int total = np + 1;
	types_init(&tt);
	types_register_builtins(&tt);
	for (int i = 0; i < total; i++)
	{
		types_register_unit_names(&tt, units[i]);
	}

	for (int i = 0; i < total; i++)
	{
		types_register_interfaces(&tt, units[i]);
	}

	for (int i = 0; i < total; i++)
	{
		types_register_unit_members(&tt, units[i]);
	}

	resolve_program(&tt, units, total);
	FILE *f = fopen("out_ir_full_test.asm", "w+");
	ASSERT(f != NULL);
	Codegen cg;
	cg_init(&cg, f);
	cg.target = target;
	cg_program(&cg, &tt, units, total);
	fflush(f);
	rewind(f);
	size_t n = fread(g_full_asm, 1, sizeof(g_full_asm) - 1, f);
	g_full_asm[n] = 0;
	fclose(f);
	remove("out_ir_full_test.asm");
}

static void test_region_dispatch_in_main_shaped_function(void)
{
	/* A main with allocation and a print after the loop: whole-function IR is
	   impossible, so the loop must be emitted as a region. */
	emit_unit_asm(
		"void main()\n"
		"{\n"
		"	int[] tbl;\n"
		"	tbl = new int[64];\n"
		"	long s;\n"
		"	s = 0;\n"
		"	for (int i = 0; i < tbl.length; i = i + 1)\n"
		"	{\n"
		"		tbl[i] = i * 3;\n"
		"		s = s + (long)tbl[i];\n"
		"	}\n"
		"	print(\"\" + s);\n"
		"}\n", TARGET_WINDOWS);
	ASSERT(strstr(g_full_asm, "; ir-region begin") != NULL);
	ASSERT(strstr(g_full_asm, "; ir-region end") != NULL);
}

static void test_region_skipped_in_function_with_try(void)
{
	/* A catch resuming in this frame would read home slots the region holds in
	   registers: any try in the function disables regions. */
	emit_unit_asm(
		"void main()\n"
		"{\n"
		"	int[] tbl;\n"
		"	tbl = new int[64];\n"
		"	try\n"
		"	{\n"
		"		for (int i = 0; i < tbl.length; i = i + 1)\n"
		"		{\n"
		"			tbl[i] = i;\n"
		"		}\n"
		"	}\n"
		"	catch (Exception e)\n"
		"	{\n"
		"	}\n"
		"}\n", TARGET_WINDOWS);
	ASSERT(strstr(g_full_asm, "; ir-region begin") == NULL);
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
	RUN(test_region_eligible_despite_call);
	RUN(test_region_rejects_return);
	RUN(test_region_rejects_managed_assign);
	RUN(test_region_lower_has_no_ret);
	RUN(test_whole_function_eligibility_unaffected);
	RUN(test_region_lowers_bitwise_not);
	RUN(test_region_emit_no_prologue_no_ret);
	RUN(test_region_dispatch_in_main_shaped_function);
	RUN(test_region_skipped_in_function_with_try);
	SUMMARY();
	return 0;
}
