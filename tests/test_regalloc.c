#include "test_framework.h"
#include "ir.h"
#include "irlower.h"
#include "regalloc.h"
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "types.h"
#include "resolve.h"
#include <string.h>   /* memset for hand-built IR functions. */

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
	types_register_all_members(&g_tt, &u, 1);
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

static void test_no_hot_spill_small_loop(void)
{
	/* A handful of inner-loop values fit in registers - no hot spill. */
	const Func *f = parse_one_func(
		"long sum(int n)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	for (int i = 0; i < n; i = i + 1) { s = s + (long)i; }\n"
		"	return s;\n"
		"}\n");
	IRFunc *ir = ir_lower_func(f, 0);
	ASSERT_INT(ra_hot_spill(ir), 0);
	ir_func_free(ir);
}

static void test_hot_spill_detected(void)
{
	/* Many values all live at the inner-loop sum exceed the register file, forcing
	   a spill of a value used at the deepest depth - the safety gate must see it. */
	const Func *f = parse_one_func(
		"long hot(int n)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	for (int i = 1; i < n; i = i + 1)\n"
		"	{\n"
		"		long c0; c0 = (long)i * 1;  long c1; c1 = (long)i * 2;\n"
		"		long c2; c2 = (long)i * 3;  long c3; c3 = (long)i * 4;\n"
		"		long c4; c4 = (long)i * 5;  long c5; c5 = (long)i * 6;\n"
		"		long c6; c6 = (long)i * 7;  long c7; c7 = (long)i * 8;\n"
		"		long c8; c8 = (long)i * 9;  long c9; c9 = (long)i * 10;\n"
		"		long d0; d0 = (long)i * 11; long d1; d1 = (long)i * 12;\n"
		"		long d2; d2 = (long)i * 13; long d3; d3 = (long)i * 14;\n"
		"		long d4; d4 = (long)i * 15; long d5; d5 = (long)i * 16;\n"
		"		s = s + c0+c1+c2+c3+c4+c5+c6+c7+c8+c9+d0+d1+d2+d3+d4+d5;\n"
		"	}\n"
		"	return s;\n"
		"}\n");
	IRFunc *ir = ir_lower_func(f, 0);
	ASSERT_INT(ra_hot_spill(ir), 1);
	ir_func_free(ir);
}

/* Build the remat scenario: 14 loads of distinct never-stored locals, all
   simultaneously live (loads first, consuming stores after), overflowing the
   register pool so several classes spill - and every spilled class is a load
   of a read-only local. `store_back` rewrites every source local between the
   loads and their uses, disqualifying them all for the negative test. */
static IRFunc *build_remat_func(IRReg *loads, int store_back)
{
	static Func dummy;
	memset(&dummy, 0, sizeof(dummy));
	IRFunc *f = ir_func_new(&dummy);
	int b0 = ir_block_new(f);
	f->blocks[b0].depth = 1;   /* Deepest block: the hot-spill gate watches it. */

	for (int i = 0; i < 14; i++)
	{
		loads[i] = ir_reg(f);
		IRInstr *ld = ir_emit(f, b0, IR_LOAD, TY_LONG);
		ld->dst = loads[i];
		ld->a = IR_NO_REG;
		ld->b = IR_NO_REG;
		ld->is_frame = 1;
		ld->disp = 8 + i * 8;
	}

	/* Disqualifying stores sit BETWEEN the defs and their last uses: a reload
	   after one of these would observe the new value, so remat must not fire.
	   (Stores after the last use would be harmless and do not disqualify.) */
	if (store_back)
	{
		for (int i = 0; i < 14; i++)
		{
			IRInstr *st = ir_emit(f, b0, IR_STORE, TY_LONG);
			st->a = IR_NO_REG;
			st->b = IR_NO_REG;
			st->c = loads[(i + 1) % 14];
			st->is_frame = 1;
			st->disp = 8 + i * 8;
		}
	}

	/* Consume each load as the value of an unindexed array-element store
	   through one read-only base: no chain temporaries and no extra frame
	   locals, so nothing but the loads themselves can ever spill. */
	IRReg base = ir_reg(f);
	IRInstr *bl = ir_emit(f, b0, IR_LOAD, TY_LONG);
	bl->dst = base;
	bl->a = IR_NO_REG;
	bl->b = IR_NO_REG;
	bl->is_frame = 1;
	bl->disp = 800;

	for (int i = 0; i < 14; i++)
	{
		IRInstr *st = ir_emit(f, b0, IR_STORE, TY_LONG);
		st->a = base;
		st->b = IR_NO_REG;
		st->c = loads[i];
		st->is_frame = 0;
		st->scale = 8;
		st->disp = 32;
		st->checked = 0;
	}

	IRInstr *rt = ir_emit(f, b0, IR_RET, TY_VOID);
	rt->a = IR_NO_REG;
	return f;
}

static void test_remat_readonly_frame_load(void)
{
	/* 14 mutually-live classes overflow the pool: some spill, and every spill
	   is the load of a never-stored local - each must carry its local's disp
	   for rematerialization, and none may count as a hot spill. */
	IRReg loads[14];
	IRFunc *f = build_remat_func(loads, 0);
	IRAlloc *a = ra_run(f);

	int spilled = 0;
	int remat_ok = 1;
	for (int i = 0; i < 14; i++)
	{
		if (ra_vreg_reg(a, loads[i]) == RA_SPILLED)
		{
			spilled++;
			if (ra_vreg_remat(a, loads[i]) != 8 + i * 8)
			{
				remat_ok = 0;
			}
		}
	}

	ASSERT(spilled > 0);            /* Precondition: the pool overflowed. */

	ASSERT_INT(remat_ok, 1);        /* Every spilled load rematerializes. */
	ASSERT_INT(a->hot_spill, 0);    /* Remat spills are exempt from the gate. */
	ra_free(a);
	ir_func_free(f);
}

static void test_remat_skipped_when_local_stored(void)
{
	/* Same shape, but every source local is stored at the end: a stale
	   rematerialized read would observe the new value, so remat must not fire
	   anywhere and the spills count as hot again. */
	IRReg loads[14];
	IRFunc *f = build_remat_func(loads, 1);
	IRAlloc *a = ra_run(f);

	int spilled = 0;
	for (int i = 0; i < 14; i++)
	{
		if (ra_vreg_reg(a, loads[i]) == RA_SPILLED)
		{
			spilled++;
			ASSERT_INT((int)ra_vreg_remat(a, loads[i]), -1);
		}
	}

	ASSERT(spilled > 0);
	ASSERT(a->hot_spill_count > 0);   /* Counted (non-remat), even if under the gate's tolerance. */
	ra_free(a);
	ir_func_free(f);
}
int main(void)
{
	RUN(test_reg_tables);
	RUN(test_remat_readonly_frame_load);
	RUN(test_remat_skipped_when_local_stored);
	RUN(test_no_hot_spill_small_loop);
	RUN(test_hot_spill_detected);
	RUN(test_alloc_trivial_in_register);
	RUN(test_loop_value_interval_spans_loop);
	RUN(test_small_function_no_spill);
	RUN(test_many_locals_force_spill);
	SUMMARY();
	return 0;
}
