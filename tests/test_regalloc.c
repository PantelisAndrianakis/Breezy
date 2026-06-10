#include "test_framework.h"
#include "ir.h"
#include "irlower.h"
#include "regalloc.h"
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "types.h"
#include "resolve.h"

/* Lex -> parse -> register -> resolve a snippet; return its first free function. */
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

static void test_reg_tables(void)
{
	ASSERT_STR(ra_reg_name(0), "rbx");
	ASSERT_STR(ra_reg_name(RA_NREGS - 1), "r15");
	ASSERT_INT(ra_is_callee_saved(0, 0), 1);       /* rbx, Win64. */
	ASSERT_INT(ra_is_callee_saved(1, 0), 1);       /* rsi, Win64 -> callee-saved. */
	ASSERT_INT(ra_is_callee_saved(1, 1), 0);       /* rsi, System V -> caller-saved. */
}

static void test_alloc_trivial_in_register(void)
{
	const Func *f = parse_one_func("int five() { return 5; }\n");
	IRFunc *ir = ir_lower_func(f, 0);
	ASSERT(ir != NULL);
	IRAlloc *a = ra_run(ir);
	ASSERT(a != NULL);

	int any_reg = 0;
	for (int v = 0; v < ir->vreg_count; v++)
	{
		if (ra_vreg_reg(a, v) >= 0)
		{
			any_reg = 1;
		}
	}

	ASSERT_INT(any_reg, 1);
	ra_free(a);
	ir_func_free(ir);
}

static void test_loop_value_interval_spans_loop(void)
{
	const Func *loopf = parse_one_func(
		"long sum(int n)\n"
		"{\n"
		"	long s;\n"
		"	s = 0;\n"
		"	for (int i = 0; i < n; i = i + 1) { s = s + (long)i; }\n"
		"	return s;\n"
		"}\n");
	IRFunc *li = ir_lower_func(loopf, 0);
	IRAlloc *la = ra_run(li);
	int loop_span = ra_debug_max_interval(la);

	const Func *flat = parse_one_func("int five() { return 5; }\n");
	IRFunc *fi = ir_lower_func(flat, 0);
	IRAlloc *fa = ra_run(fi);
	int flat_span = ra_debug_max_interval(fa);

	/* The accumulator s is live across the whole loop, so its interval is far
	   longer than anything in a straight-line function. */
	ASSERT(loop_span > flat_span);
	ASSERT(loop_span > 5);

	ra_free(la);
	ir_func_free(li);
	ra_free(fa);
	ir_func_free(fi);
}

static void test_small_function_no_spill(void)
{
	const Func *f = parse_one_func(
		"long sum(int n)\n"
		"{\n"
		"	long s;\n"
		"	s = 0;\n"
		"	for (int i = 0; i < n; i = i + 1) { s = s + (long)i; }\n"
		"	return s;\n"
		"}\n");
	IRFunc *ir = ir_lower_func(f, 0);
	IRAlloc *a = ra_run(ir);
	/* A handful of live values fit in 11 registers - nothing spills. */
	ASSERT_INT(a->spill_bytes, 0);
	ra_free(a);
	ir_func_free(ir);
}

static void test_many_locals_force_spill(void)
{
	/* Eighteen longs all live at the final sum exceed even the expanded pool
	   (11 base + rcx + rdx = 13, claimable here because there is no shift/div). */
	const Func *f = parse_one_func(
		"long many(long a)\n"
		"{\n"
		"	long b0; b0 = a + 0;  long b1; b1 = a + 1;  long b2; b2 = a + 2;\n"
		"	long b3; b3 = a + 3;  long b4; b4 = a + 4;  long b5; b5 = a + 5;\n"
		"	long b6; b6 = a + 6;  long b7; b7 = a + 7;  long b8; b8 = a + 8;\n"
		"	long b9; b9 = a + 9;  long b10; b10 = a + 10;  long b11; b11 = a + 11;\n"
		"	long b12; b12 = a + 12;  long b13; b13 = a + 13;  long b14; b14 = a + 14;\n"
		"	long b15; b15 = a + 15;  long b16; b16 = a + 16;  long b17; b17 = a + 17;\n"
		"	return b0+b1+b2+b3+b4+b5+b6+b7+b8+b9+b10+b11+b12+b13+b14+b15+b16+b17;\n"
		"}\n");
	IRFunc *ir = ir_lower_func(f, 0);
	IRAlloc *a = ra_run(ir);
	ASSERT(a->spill_bytes > 0);
	ra_free(a);
	ir_func_free(ir);
}

int main(void)
{
	RUN(test_reg_tables);
	RUN(test_alloc_trivial_in_register);
	RUN(test_loop_value_interval_spans_loop);
	RUN(test_small_function_no_spill);
	RUN(test_many_locals_force_spill);
	SUMMARY();
	return 0;
}
