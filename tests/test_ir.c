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

static void test_eligible_rejects_array(void)
{
	const Func *f = parse_one_func(
		"int first(int[] xs)\n"
		"{\n"
		"	return xs[0];\n"
		"}\n");
	ASSERT_INT(ir_eligible(f), 0);
}

int main(void)
{
	RUN(test_ir_build_basic);
	RUN(test_eligible_accepts_scalar_loop);
	RUN(test_eligible_rejects_string);
	RUN(test_eligible_rejects_call);
	RUN(test_eligible_rejects_array);
	SUMMARY();
	return 0;
}
