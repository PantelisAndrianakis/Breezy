#include "codegen.h"
#include "grow.h"
#include "resolve.h"
#include "symtable.h"
#include "lexer.h"
#include "enums.h"
#include "config.h"
#include "irlower.h"
#include "iremit.h"
#include "regalloc.h"
#include "overload.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Set during resolution when any program expression touches an AVX (f64x4)
   vector; drives the startup AVX-support guard emitted into bzy_user_main. */
int g_program_uses_avx = 0;

void cg_init(Codegen *cg, FILE *out)
{
	cg->out=out;
	cg->last_line[0]='\0';
	cg->label_count=0;
	cg->fpk_count=0;
	cg->fpk=NULL;
	cg->fpk_cap=0;
	cg->strk_count=0;
	cg->strk=NULL;
	cg->strk_cap=0;
	cg->uc_off=NULL;
	cg->uc_val=NULL;
	cg->uc_cap=0;
	cg->cur_try_k=NULL;
	cg->cur_try_c=NULL;
	cg->cur_try_vt=NULL;
	cg->cur_try_cap=0;
	cg->breeze_thunks=NULL;
	cg->breeze_thunk_cap=0;
	cg->blocking_thunks=NULL;
	cg->blocking_thunk_cap=0;
	cg->cur_break_label=-1;
	cg->cur_continue_label=-1;
	cg->hoist_n=0;
	cg->hoist_depth=0;
	cg->sr_n=0;
	cg->sr_ivreg=NULL;
	cg->cur_counter_off=0;
	cg->defer_n=0;
	cg->low32_ok=0;
	cg->unrolling=0;
	cg->unroll_iv_off=0;
	cg->uc_n=0;
	cg->exception_fn_count=0;
	cg->exception_try_count=0;
	cg->breeze_thunk_count=0;
	cg->blocking_thunk_count=0;
	cg->region_count=0;
	cg->region_cap=0;
	cg->region_stmt=NULL;
	cg->region_irf=NULL;
	cg->region_alloc=NULL;
	cg->region_base=0;
	cg->target=TARGET_WINDOWS;   /* Driver overrides via --target. */
}

void cg_emit(Codegen *cg, const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap,fmt);
	int n = vsnprintf(line,sizeof line,fmt,ap);
	va_end(ap);

	/* Spill-reload peephole: a `mov REG, [rbp - N]` immediately after a
	   `mov [rbp - N], REG` (same REG, same slot) is a redundant reload -- the store
	   left REG holding the value, so drop it. Provably safe: the two lines are the
	   tail of one instruction and the head of the next with nothing between, and any
	   intervening instruction becomes last_line and breaks the match (so a later
	   clobber + reload is never dropped). Cuts the spilled-temp store/reload churn
	   the IR backend emits in register-pressured loops. */
	if (n > 0 && (size_t)n < sizeof line)
	{
		int s_off, l_off;
		char s_reg[24], l_reg[24];
		if (sscanf(line, "    mov %23[^,], [rbp - %d]", l_reg, &l_off) == 2
			&& sscanf(cg->last_line, "    mov [rbp - %d], %23s", &s_off, s_reg) == 2
			&& s_off == l_off && strcmp(s_reg, l_reg) == 0)
		{
			return;   /* Skip the redundant reload; last_line stays the store. */
		}
	}

	fputs(line, cg->out);
	fputc('\n', cg->out);
	if ((size_t)n < sizeof line)
	{
		memcpy(cg->last_line, line, (size_t)n + 1);
	}
	else
	{
		cg->last_line[0] = '\0';   /* Truncated/oversized line: no peephole across it. */
	}
}

int  cg_label(Codegen *cg)
{
	return cg->label_count++;
}

/* Register promotion: the four callee-saved registers a promoting frame may use
   to hold hot 64-bit integer locals (chosen because codegen never uses them as
   scratch). Index i corresponds to f->promo_reg == i. */
static const char *const CG_PROMO_REGS[4] = { "r12", "r13", "r14", "r15" };

/* If local slot offset `off` is promoted in the function currently being emitted,
   return its register name ("r12".."r15"); otherwise NULL (it lives in its stack
   slot). Returns NULL for synthesized/hand-rolled frames (cur_func == NULL). */
static const char *cg_local_reg(Codegen *cg, int off)
{
	Func *f = cg->cur_func;
	if (!f)
	{
		return NULL;
	}

	for (int i = 0; i < f->promo_count; i++)
	{
		if (f->promo_off[i] == off)
		{
			return CG_PROMO_REGS[f->promo_reg[i]];
		}
	}

	return NULL;
}

/* Float promotion: the four caller-saved XMM registers a promoting frame may use to
   hold hot double locals. Caller-saved on both Win64 and SysV and never used as
   codegen scratch (xmm0/xmm1), so a promoted double - which promote.c guarantees is
   never live across a call - needs no prologue save/restore. Index i == fpromo_reg. */
static const char *const CG_FPROMO_REGS[4] = { "xmm2", "xmm3", "xmm4", "xmm5" };

/* Loop-scoped float promotion: callee-saved XMM homes for hot doubles inside the
   current innermost call-free loop (see cg_lpromo_begin). Callee-saved on Win64
   (preserved around the loop with movups), volatile on SysV. */
#define LPROMO_NREGS 6
static const char *const CG_LPROMO_REGS[LPROMO_NREGS] = { "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11" };

/* If double local slot `off` is float-promoted in the current function, return its
   XMM register name ("xmm2".."xmm5"); otherwise NULL. NULL for synthesized frames.
   A loop-scoped promotion (xmm6..xmm11) takes effect only while its loop is being
   emitted - lpromo_n is zero outside the armed region. */
static const char *cg_local_xmm(Codegen *cg, int off)
{
	Func *f = cg->cur_func;
	if (!f)
	{
		return NULL;
	}

	for (int i = 0; i < f->fpromo_count; i++)
	{
		if (f->fpromo_off[i] == off)
		{
			return CG_FPROMO_REGS[f->fpromo_reg[i]];
		}
	}

	for (int i = 0; i < cg->lpromo_n; i++)
	{
		if (cg->lpromo_off[i] == off)
		{
			return CG_LPROMO_REGS[i];
		}
	}

	return NULL;
}

/* Caller-saved registers used to cache loop-invariant locals across an innermost,
   call-free loop (see cg_loop_hoist_begin). Codegen uses rax/rbx/rcx/rdx as
   scratch and r12..r15 for promotion, so r8..r11 are free in a loop that emits no
   call (a call would clobber them, and the bzy_oob slow path uses r8/r9 - hence
   the hoist predicate requires every index to be bounds-check-eliminated). */
static const char *const CG_HOIST_REGS[4]   = { "r8", "r9", "r10", "r11" };
static const char *const CG_HOIST_REGS32[4] = { "r8d", "r9d", "r10d", "r11d" };

/* If slot `off` is cached in a hoist register for the current loop, return its
   64-bit register name; else NULL. The slot itself still holds the value, so any
   path that ignores this and reads the slot is still correct - the cache is a
   pure speed-up over re-reading loop-invariant memory each iteration. */
static const char *cg_hoist_reg(Codegen *cg, int off)
{
	for (int i = 0; i < cg->hoist_n; i++)
	{
		if (cg->hoist_off[i] == off)
		{
			return CG_HOIST_REGS[cg->hoist_reg[i]];
		}
	}

	return NULL;
}

/* Store rax into local `off`: its register home if promoted, else its stack slot.
   Slot-safe for any offset not in the map, so internal temporaries are unaffected.
   A promoted 32-bit int is sign-extended (movsxd reg, eax) so the bare register
   read path always sees a correct 64-bit value; 64-bit locals take the full rax. */
static void cg_store_local_off(Codegen *cg, int off, TypeKind k)
{
	const char *r = cg_local_reg(cg, off);
	if (r)
	{
		if (k == TY_INT)
		{
			cg_emit(cg, "    movsxd %s, eax", r);
		}
		else
		{
			cg_emit(cg, "    mov %s, rax", r);
		}
	}
	else
	{
		cg_emit(cg, "    mov [rbp - %d], rax", off);
	}
}

/* Store xmm0 into double local `off`: its XMM home if float-promoted, else its
   stack slot. Slot-safe for any non-promoted offset, so temporaries are unaffected. */
static void cg_store_local_fp(Codegen *cg, int off, TypeKind k)
{
	const char *xr = k==TY_DOUBLE ? cg_local_xmm(cg, off) : NULL;
	if (xr)
	{
		/* movaps, not movsd: the register form of movsd merges into the
		   destination's upper half, adding a false dependency on the home's
		   previous value - a loop-carried stall for a promoted accumulator. */
		cg_emit(cg, "    movaps %s, xmm0", xr);
	}
	else
	{
		cg_emit(cg, k==TY_FLOAT ? "    movss dword [rbp - %d], xmm0" : "    movsd qword [rbp - %d], xmm0", off);
	}
}

/* Translate a 128-bit home register name ("xmm2") to its 256-bit alias ("ymm2")
   into `buf`; the promotion pool reg index is the same physical register. */
static const char *cg_ymm_alias(const char *xmm, char *buf, int bufsz)
{
	snprintf(buf, bufsz, "ymm%s", xmm + 3);   /* After "xmm". */
	return buf;
}

/* Store the packed SIMD value in xmm0/ymm0 into local `off` (kind `k`): its
   register home if promoted (a full-width reg-reg copy), else its 16/32-byte
   stack slot (unaligned). */
static void cg_store_local_simd(Codegen *cg, int off, TypeKind k)
{
	const char *xr = cg_local_xmm(cg, off);
	int w256 = (ty_simd_bytes(k) == 32);
	if (xr)
	{
		if (w256)
		{
			char yb[8];
			cg_emit(cg, "    vmovaps %s, ymm0", cg_ymm_alias(xr, yb, sizeof yb));
		}
		else
		{
			cg_emit(cg, "    movaps %s, xmm0", xr);
		}
	}
	else
	{
		cg_emit(cg, w256 ? "    vmovups [rbp - %d], ymm0" : "    movupd [rbp - %d], xmm0", off);
	}
}

/* Load packed SIMD local `off` (kind `k`) into xmm`reg`/ymm`reg`: from its
   register home if promoted, else from its stack slot. */
static void cg_load_local_simd(Codegen *cg, int off, int reg, TypeKind k)
{
	const char *xr = cg_local_xmm(cg, off);
	int w256 = (ty_simd_bytes(k) == 32);
	if (xr)
	{
		if (w256)
		{
			char yb[8];
			cg_emit(cg, "    vmovaps ymm%d, %s", reg, cg_ymm_alias(xr, yb, sizeof yb));
		}
		else
		{
			cg_emit(cg, "    movaps xmm%d, %s", reg, xr);
		}
	}
	else
	{
		cg_emit(cg, w256 ? "    vmovups ymm%d, [rbp - %d]" : "    movupd xmm%d, [rbp - %d]", reg, off);
	}
}

static void cg_load_scalar_into(Codegen *cg, TypeKind k, const char *mem, const char *r64, const char *r32);
static void cg_expr(Codegen *cg, TypeTable *tt, Expr *e);
static void cg_extend_reg(Codegen *cg, TypeKind k);
static int cg_hoist_expr_ok(Expr *e);

/* In-place arithmetic op support: the two-operand register ops we lower directly
   onto a promoted target register. Shifts (<<,>>) and div/mod are excluded. */
static int cg_op_inplace_ok(int op)
{
	return op==TOKEN_PLUS || op==TOKEN_MINUS || op==TOKEN_STAR
		   || op==TOKEN_AMP || op==TOKEN_PIPE || op==TOKEN_CARET;
}

static int cg_op_commutative(int op)
{
	return op==TOKEN_PLUS || op==TOKEN_STAR
		   || op==TOKEN_AMP || op==TOKEN_PIPE || op==TOKEN_CARET;
}

/* A "target-free leaf": an integer/bool literal, or a local that is NOT the target.
   These are the only right-operands an in-place chain may carry, so incremental
   mutation of the target register never feeds a later read a wrong value. */
static int cg_is_inplace_leaf(Expr *e, Expr *target)
{
	if (e->kind==EX_INT || e->kind==EX_BOOL)
	{
		return 1;
	}

	return e->kind==EX_IDENT && e->anno_int > 0 && e->anno_int != target->anno_int;
}

/* Materialize an in-place leaf operand. A fits-imm32 literal returns its text in
   `buf` and sets *is_imm=1; anything else is loaded (width-correct) into rbx and
   "rbx" is returned with *is_imm=0. */
static const char *cg_inplace_operand(Codegen *cg, Expr *leaf, char *buf, int *is_imm)
{
	if ((leaf->kind==EX_INT || leaf->kind==EX_BOOL)
		&& leaf->int_val >= -2147483648LL && leaf->int_val <= 2147483647LL)
	{
		snprintf(buf, 24, "%lld", leaf->int_val);
		*is_imm = 1;
		return buf;
	}

	*is_imm = 0;
	if (leaf->kind==EX_INT || leaf->kind==EX_BOOL)
	{
		cg_emit(cg, "    mov rbx, %lld", leaf->int_val);
	}
	else   /* EX_IDENT */
	{
		const char *r = cg_local_reg(cg, leaf->anno_int);
		if (r)
		{
			cg_emit(cg, "    mov rbx, %s", r);
		}
		else
		{
			char mem[32];
			sprintf(mem, "[rbp - %d]", leaf->anno_int);
			cg_load_scalar_into(cg, leaf->type.kind, mem, "rbx", "ebx");
		}
	}

	return "rbx";
}

/* Emit one in-place op `R <op>= operand`. */
static void cg_inplace_step(Codegen *cg, const char *R, int op, Expr *leaf)
{
	char buf[24];
	int is_imm;
	const char *operand = cg_inplace_operand(cg, leaf, buf, &is_imm);
	switch (op)
	{
	case TOKEN_PLUS:
		cg_emit(cg, "    add %s, %s", R, operand);
		break;
	case TOKEN_MINUS:
		cg_emit(cg, "    sub %s, %s", R, operand);
		break;
	case TOKEN_STAR:
		if (is_imm)
		{
			cg_emit(cg, "    imul %s, %s, %s", R, R, operand);
		}
		else
		{
			cg_emit(cg, "    imul %s, rbx", R);
		}

		break;
	case TOKEN_AMP:
		cg_emit(cg, "    and %s, %s", R, operand);
		break;
	case TOKEN_PIPE:
		cg_emit(cg, "    or %s, %s", R, operand);
		break;
	case TOKEN_CARET:
		cg_emit(cg, "    xor %s, %s", R, operand);
		break;
	}
}

/* True if slot `off` is a deferred-extension accumulator for the current loop. */
static int cg_off_deferred(Codegen *cg, int off)
{
	for (int i = 0; i < cg->defer_n; i++)
	{
		if (cg->defer_off[i] == off)
		{
			return 1;
		}
	}

	return 0;
}

/* Emit `R <op>= rax` in place, width-correct: a 64-bit op for a long target, or a
   32-bit op plus a movsxd re-extension for an int target (so the register stays a
   valid sign-extended 64-bit value for the bare-register read path). When
   defer_extend, the re-extension is skipped (a loop accumulator re-extended once
   at the loop exit). */
static void cg_inplace_reg_rax(Codegen *cg, const char *R, int op, TypeKind k, int defer_extend)
{
	if (k == TY_INT)
	{
		char r32[8];
		snprintf(r32, sizeof r32, "%sd", R);   /* r12 -> r12d. */
		switch (op)
		{
		case TOKEN_PLUS:  cg_emit(cg, "    add %s, eax", r32);  break;
		case TOKEN_MINUS: cg_emit(cg, "    sub %s, eax", r32);  break;
		case TOKEN_STAR:  cg_emit(cg, "    imul %s, eax", r32); break;
		case TOKEN_AMP:   cg_emit(cg, "    and %s, eax", r32);  break;
		case TOKEN_PIPE:  cg_emit(cg, "    or %s, eax", r32);   break;
		case TOKEN_CARET: cg_emit(cg, "    xor %s, eax", r32);  break;
		}

		if (!defer_extend)
		{
			cg_emit(cg, "    movsxd %s, %s", R, r32);   /* Keep R sign-valid; deferred accumulators re-extend once at loop exit. */
		}
	}
	else
	{
		switch (op)
		{
		case TOKEN_PLUS:  cg_emit(cg, "    add %s, rax", R);  break;
		case TOKEN_MINUS: cg_emit(cg, "    sub %s, rax", R);  break;
		case TOKEN_STAR:  cg_emit(cg, "    imul %s, rax", R); break;
		case TOKEN_AMP:   cg_emit(cg, "    and %s, rax", R);  break;
		case TOKEN_PIPE:  cg_emit(cg, "    or %s, rax", R);   break;
		case TOKEN_CARET: cg_emit(cg, "    xor %s, rax", R);  break;
		}
	}
}

/* Try to emit `target = value` as in-place arithmetic on the target's promoted
   register. Returns 1 if handled, 0 to fall through to the generic path.
   Matches a left-spine chain rooted at the target: i=i+1, state=state*C1+C2,
   n=3*n+1 (commutative reorder normalizes 3*n to n*3). */
static int cg_try_inplace(Codegen *cg, TypeTable *tt, Expr *target, Expr *value)
{
	if (target->kind != EX_IDENT)
	{
		return 0;
	}

	const char *R = cg_local_reg(cg, target->anno_int);
	if (!R)
	{
		return 0;
	}

	/* target = target +/- CONST: one immediate add/sub on the register (an int
	   re-extends to stay a valid 64-bit value). This is the loop-counter step,
	   which the generic path otherwise round-trips through rax with a reload and
	   two sign-extensions. */
	if (value->kind==EX_BINARY && (value->op==TOKEN_PLUS || value->op==TOKEN_MINUS)
		&& (target->type.kind==TY_INT || target->type.kind==TY_LONG || target->type.kind==TY_ULONG))
	{
		Expr *k = NULL;
		if (value->lhs->kind==EX_IDENT && value->lhs->anno_int==target->anno_int && value->rhs->kind==EX_INT)
		{
			k = value->rhs;
		}
		else if (value->op==TOKEN_PLUS && value->rhs->kind==EX_IDENT
				 && value->rhs->anno_int==target->anno_int && value->lhs->kind==EX_INT)
		{
			k = value->lhs;
		}

		if (k && k->int_val >= -2147483648LL && k->int_val <= 2147483647LL)
		{
			const char *opc = (value->op==TOKEN_PLUS) ? "add" : "sub";
			if (target->type.kind==TY_INT)
			{
				char r32[8];
				snprintf(r32, sizeof r32, "%sd", R);
				cg_emit(cg,"    %s %s, %lld", opc, r32, k->int_val);
				/* A non-negative unit-positive step on the loop counter keeps the
				   value in [0, 2^31): the 32-bit add already zero-extended the
				   register, so the sign-extension is redundant. Any other int target
				   (or a negative / decrementing step) still re-extends. */
				int is_counter = (cg->cur_counter_off != 0
								  && target->anno_int == cg->cur_counter_off
								  && value->op == TOKEN_PLUS && k->int_val >= 0);
				if (!is_counter && !cg_off_deferred(cg, target->anno_int))
				{
					cg_emit(cg,"    movsxd %s, %s", R, r32);
				}
			}
			else
			{
				cg_emit(cg,"    %s %s, %lld", opc, R, k->int_val);
			}

			return 1;
		}
	}

	/* target = target <op> CONST for the multiplicative/bitwise self-updates
	   (`hash = hash * 16777619`, `m = m & 0xFF`, `acc = acc ^ K`). All four ops are
	   commutative, so the target may sit on either side. One immediate-form op runs
	   on the register - no `mov rbx, CONST` materialization and no round-trip through
	   rax that the generic path carries. An int target re-extends to stay a valid
	   64-bit value (unless it is a deferred accumulator); a long target is exact. */
	if (value->kind==EX_BINARY
		&& (value->op==TOKEN_STAR || value->op==TOKEN_AMP || value->op==TOKEN_PIPE || value->op==TOKEN_CARET)
		&& (target->type.kind==TY_INT || target->type.kind==TY_LONG || target->type.kind==TY_ULONG))
	{
		Expr *k = NULL;
		if (value->lhs->kind==EX_IDENT && value->lhs->anno_int==target->anno_int && value->rhs->kind==EX_INT)
		{
			k = value->rhs;
		}
		else if (value->rhs->kind==EX_IDENT && value->rhs->anno_int==target->anno_int && value->lhs->kind==EX_INT)
		{
			k = value->lhs;
		}

		if (k && k->int_val >= -2147483648LL && k->int_val <= 2147483647LL)
		{
			if (target->type.kind==TY_INT)
			{
				char r32[8];
				snprintf(r32, sizeof r32, "%sd", R);
				switch (value->op)
				{
				case TOKEN_STAR:  cg_emit(cg,"    imul %s, %s, %lld", r32, r32, k->int_val); break;
				case TOKEN_AMP:   cg_emit(cg,"    and %s, %lld", r32, k->int_val);  break;
				case TOKEN_PIPE:  cg_emit(cg,"    or %s, %lld", r32, k->int_val);   break;
				case TOKEN_CARET: cg_emit(cg,"    xor %s, %lld", r32, k->int_val);  break;
				}

				if (!cg_off_deferred(cg, target->anno_int))
				{
					cg_emit(cg,"    movsxd %s, %s", R, r32);
				}
			}
			else
			{
				switch (value->op)
				{
				case TOKEN_STAR:  cg_emit(cg,"    imul %s, %s, %lld", R, R, k->int_val); break;
				case TOKEN_AMP:   cg_emit(cg,"    and %s, %lld", R, k->int_val);  break;
				case TOKEN_PIPE:  cg_emit(cg,"    or %s, %lld", R, k->int_val);   break;
				case TOKEN_CARET: cg_emit(cg,"    xor %s, %lld", R, k->int_val);  break;
				}
			}

			return 1;
		}
	}

	/* General single-op in-place: `target = target <op> EXPR` (or, for a
	   commutative op, `EXPR <op> target`) where EXPR is a non-leaf expression -
	   most importantly an A*B product, i.e. a multiply-accumulate. EXPR evaluates
	   into rax without disturbing the target's callee-saved register, so the
	   accumulator never spills to the stack (the generic path pushes it across the
	   rhs evaluation). The op then runs in-place on the register, width-correct for
	   int or long. This handles the cases the leaf-only chain below cannot. */
	if (value->kind==EX_BINARY && cg_op_inplace_ok(value->op)
		&& (target->type.kind==TY_LONG || target->type.kind==TY_ULONG || target->type.kind==TY_INT))
	{
		Expr *other = NULL;
		if (value->lhs->kind==EX_IDENT && value->lhs->anno_int==target->anno_int)
		{
			other = value->rhs;
		}
		else if (cg_op_commutative(value->op)
				 && value->rhs->kind==EX_IDENT && value->rhs->anno_int==target->anno_int)
		{
			other = value->lhs;
		}

		if (other && !cg_is_inplace_leaf(other, target))
		{
			/* An int target's in-place step is `<op> R32, eax` (cg_inplace_reg_rax),
			   which reads only eax - so EXPR's trailing sign-extension is dead. Signal
			   low-32-only before evaluating it. A long target's step is `<op> R, rax`
			   (full width), so it still needs the normalised 64-bit value. */
			if (target->type.kind==TY_INT && other->type.kind==TY_INT)
			{
				cg->low32_ok = 1;
			}

			cg_expr(cg, tt, other);                  /* EXPR -> rax; R is untouched. */
			if (other->type.kind != target->type.kind)
			{
				cg_extend_reg(cg, target->type.kind);  /* Re-width only when the operand differs; cg_expr already normalised it to its own width. */
			}

			cg_inplace_reg_rax(cg, R, value->op, target->type.kind, cg_off_deferred(cg, target->anno_int));
			return 1;
		}
	}

	/* The leaf-chain form below mutates R with 64-bit ops, which is exact only for
	   a 64-bit target. A promoted 32-bit int needs its result wrapped to 32 bits
	   and sign-extended after each step, so defer it to the generic store path
	   (cg_store_local_off re-applies movsxd). */
	if (target->type.kind != TY_LONG && target->type.kind != TY_ULONG)
	{
		return 0;
	}

	/* Collect the chain top-down: ops[0]/leaves[0] is the outermost op. */
	int ops[8];
	Expr *leaves[8];
	int n = 0;
	Expr *e = value;
	while (e->kind==EX_BINARY && cg_op_inplace_ok(e->op))
	{
		Expr *spine;
		Expr *leaf;
		if (cg_is_inplace_leaf(e->rhs, target))
		{
			spine = e->lhs;
			leaf = e->rhs;
		}
		else if (cg_op_commutative(e->op) && cg_is_inplace_leaf(e->lhs, target))
		{
			spine = e->rhs;
			leaf = e->lhs;
		}
		else
		{
			return 0;
		}

		if (n >= 8)
		{
			return 0;
		}

		ops[n] = e->op;
		leaves[n] = leaf;
		n++;
		e = spine;
	}

	/* The spine must bottom out at the target itself, already in R. */
	if (!(e->kind==EX_IDENT && e->anno_int==target->anno_int))
	{
		return 0;
	}

	if (n == 0)
	{
		return 0;   /* value is just the bare target; nothing to do (and not our job). */
	}

	/* Emit deepest-first (the leftmost op applies first): reverse of collection. */
	for (int i = n - 1; i >= 0; i--)
	{
		cg_inplace_step(cg, R, ops[i], leaves[i]);
	}

	return 1;
}

static void cg_expr(Codegen *cg, TypeTable *tt, Expr *e);
static void cg_request_blocking_thunk(Codegen *cg, FuncInfo *fi);
/* The i-th integer/pointer argument register for the current target's ABI.
   Win64 uses 4 (rcx,rdx,r8,r9); System V AMD64 uses 6 (rdi,rsi,rdx,rcx,r8,r9). */
static const char *cg_iarg(Codegen *cg, int i)
{
	static const char *win[4] = { "rcx", "rdx", "r8", "r9" };
	static const char *sysv[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
	return (cg->target == TARGET_LINUX) ? sysv[i] : win[i];
}

/* Frame temp-slot helpers (defined below; forward-declared for early users like
   the bounds-check helper). See the fixed-rsp frame plan. */
static void cg_temp_push(Codegen *cg);
static void cg_temp_pop(Codegen *cg);
static void cg_temp_pop_reg(Codegen *cg, const char *reg);

/* Load a scalar from 'mem' into rax, sign- or zero-extending to 64 bits per its
   declared width. Objects and 64-bit integers load with a plain mov. */
/* Load a scalar of kind k from mem into the named register, widening to 64 bits
   with the kind's signedness. r64 is the full register ("rax"); r32 is its 32-bit
   name ("eax"), used for the unsigned-32 zero-extending load. */
static void cg_load_scalar_into(Codegen *cg, TypeKind k, const char *mem, const char *r64, const char *r32)
{
	if (k==TY_BOOL)
	{
		cg_emit(cg,"    movzx %s, byte %s", r64, mem);
		return;
	}

	switch (ty_bits(k))
	{
	case 8:
		if (ty_is_signed(k))
		{
			cg_emit(cg,"    movsx %s, byte %s", r64, mem);
		}
		else
		{
			cg_emit(cg,"    movzx %s, byte %s", r64, mem);
		}

		break;
	case 16:
		if (ty_is_signed(k))
		{
			cg_emit(cg,"    movsx %s, word %s", r64, mem);
		}
		else
		{
			cg_emit(cg,"    movzx %s, word %s", r64, mem);
		}

		break;
	case 32:
		if (ty_is_signed(k))
		{
			cg_emit(cg,"    movsxd %s, dword %s", r64, mem);
		}
		else
		{
			cg_emit(cg,"    mov %s, dword %s", r32, mem);   /* Writing the 32-bit reg zero-extends its 64-bit parent. */
		}

		break;
	default:
		cg_emit(cg,"    mov %s, %s", r64, mem);             /* 64-bit integer or object. */
		break;
	}
}

static void cg_load_scalar(Codegen *cg, TypeKind k, const char *mem)
{
	cg_load_scalar_into(cg,k,mem,"rax","eax");
}

/* Bytes per element of a value array; object/string arrays hold an 8-byte
   pointer. Always a valid x86 index-scale (1/2/4/8). 8-bit widths pack to one
   byte per element, so byte[]/ubyte[]/bool[] store n bytes contiguously. */
static int cg_elem_stride(TypeKind k)
{
	if (ty_is_managed(k))
	{
		return 8;
	}

	int bits = (k == TY_BOOL) ? 8 : ty_bits(k);
	if (bits <= 8)
	{
		return 1;
	}

	if (bits <= 16)
	{
		return 2;
	}

	if (bits <= 32)
	{
		return 4;
	}

	return 8;
}

/* Store the low bits of rax into 'mem' at the element's natural width. The width
   is taken from cg_elem_stride so a store can never disagree with the stride the
   address was computed at. */
static void cg_store_scalar(Codegen *cg, TypeKind k, const char *mem)
{
	switch (cg_elem_stride(k))
	{
	case 1:
		cg_emit(cg,"    mov byte %s, al", mem);
		break;
	case 2:
		cg_emit(cg,"    mov word %s, ax", mem);
		break;
	case 4:
		cg_emit(cg,"    mov dword %s, eax", mem);
		break;
	default:
		cg_emit(cg,"    mov %s, rax", mem);
		break;
	}
}

/* Runtime map key_kind from a key type: 0=int family (raw value), 1=string
   (content), 2=object/enum (identity), 3=record (value: synthesized
   hashCode/equals at vtable slots 0/1). */
static int cg_map_key_kind(TypeTable *tt, TypeRef *t)
{
	if (t->kind==TY_STRING)
	{
		return 1;
	}

	if (t->kind==TY_OBJECT)
	{
		ClassInfo *ci = types_find_class(tt, t->class_name);
		return (ci && ci->is_record) ? 3 : 2;
	}

	return 0;
}

/* Re-extend the value already in rax to 64 bits at the given integer width, the
   way a fresh load would. Used after width-truncating arithmetic and for casts. */
/* The 32-bit name of a 64-bit GPR (rax->eax, r12->r12d, ...). Anything else (an
   immediate, a memory operand) is returned unchanged, so it is safe to wrap any
   cg_binop_rhs operand. Used to emit 32-bit int comparisons. */
static const char *cg_reg32(const char *r)
{
	if (!strcmp(r, "rax")) return "eax";
	if (!strcmp(r, "rbx")) return "ebx";
	if (!strcmp(r, "rcx")) return "ecx";
	if (!strcmp(r, "rdx")) return "edx";
	if (!strcmp(r, "rsi")) return "esi";
	if (!strcmp(r, "rdi")) return "edi";
	if (!strcmp(r, "rbp")) return "ebp";
	if (!strcmp(r, "r8"))  return "r8d";
	if (!strcmp(r, "r9"))  return "r9d";
	if (!strcmp(r, "r10")) return "r10d";
	if (!strcmp(r, "r11")) return "r11d";
	if (!strcmp(r, "r12")) return "r12d";
	if (!strcmp(r, "r13")) return "r13d";
	if (!strcmp(r, "r14")) return "r14d";
	if (!strcmp(r, "r15")) return "r15d";
	return r;
}

static void cg_extend_reg(Codegen *cg, TypeKind k)
{
	switch (ty_bits(k))
	{
	case 8:
		cg_emit(cg, ty_is_signed(k) ? "    movsx rax, al" : "    movzx rax, al");
		break;
	case 16:
		cg_emit(cg, ty_is_signed(k) ? "    movsx rax, ax" : "    movzx rax, ax");
		break;
	case 32:
		cg_emit(cg, ty_is_signed(k) ? "    movsxd rax, eax" : "    mov eax, eax");
		break;
	default:
		break;   /* 64-bit: already full width. */
	}
}

/* Record a float/double literal in the constant pool; returns its __fpk id. */
static int cg_fp_const(Codegen *cg, Expr *e)
{
	cg->fpk = grow_ensure(cg->fpk, cg->fpk_count, &cg->fpk_cap, sizeof(*cg->fpk));
	int id = cg->fpk_count++;
	if (e->type.kind == TY_FLOAT)
	{
		float fv = (float)e->float_val;
		unsigned int u;
		memcpy(&u, &fv, 4);
		cg->fpk[id].is_float = 1;
		cg->fpk[id].bits = u;
	}
	else
	{
		double dv = e->float_val;
		unsigned long long u;
		memcpy(&u, &dv, 8);
		cg->fpk[id].is_float = 0;
		cg->fpk[id].bits = u;
	}

	return id;
}

/* Record a string literal in the constant pool; returns its __str id. */
static int cg_str_const(Codegen *cg, Expr *e)
{
	cg->strk = grow_ensure(cg->strk, cg->strk_count, &cg->strk_cap, sizeof(*cg->strk));
	int id = cg->strk_count++;
	int n = 0;
	while (e->str_val[n])   /* No embedded NUL by construction; no length cap. */
	{
		n++;
	}

	cg->strk[id].bytes = malloc((size_t)(n > 0 ? n : 1));
	for (int i = 0; i < n; i++)
	{
		cg->strk[id].bytes[i] = e->str_val[i];
	}

	cg->strk[id].len = n;
	return id;
}

/* Load a float/double from 'mem' into xmm0. */
static void cg_load_fp(Codegen *cg, TypeKind k, const char *mem)
{
	cg_emit(cg, k==TY_FLOAT ? "    movss xmm0, dword %s" : "    movsd xmm0, qword %s", mem);
}

/* Store xmm0 to 'mem' at the declared float/double width. */
static void cg_store_fp(Codegen *cg, TypeKind k, const char *mem)
{
	cg_emit(cg, k==TY_FLOAT ? "    movss dword %s, xmm0" : "    movsd qword %s, xmm0", mem);
}

/* Coerce the just-evaluated value (rax if integer, xmm0 if float) to the target
   type. The only implicit cross-channel conversion is int->double. */
static void cg_coerce(Codegen *cg, TypeKind to, TypeKind from)
{
	if (to==TY_DOUBLE && ty_is_int(from))
	{
		cg_emit(cg,"    cvtsi2sd xmm0, rax");
	}
}

/* The copy-constant value of local slot `off`, if recorded for the current
   unrolled copy. */
static int cg_unroll_const(Codegen *cg, int off, long long *out)
{
	for (int i = cg->uc_n - 1; i >= 0; i--)
	{
		if (cg->uc_off[i] == off)
		{
			*out = cg->uc_val[i];
			return 1;
		}
	}

	return 0;
}

/* True if `off` is currently recorded as a copy-constant. */
static int cg_is_unroll_const(Codegen *cg, int off)
{
	long long t;
	return cg_unroll_const(cg, off, &t);
}

/* Record/update a copy-constant. Silently full beyond 16 entries (sound: an
   unrecorded local just reads its slot as before). */
static void cg_unroll_const_set(Codegen *cg, int off, long long v)
{
	for (int i = 0; i < cg->uc_n; i++)
	{
		if (cg->uc_off[i] == off)
		{
			cg->uc_val[i] = v;
			return;
		}
	}

	if (cg->uc_n == cg->uc_cap)
	{
		cg->uc_cap = cg->uc_cap ? cg->uc_cap * 2 : 8;
		cg->uc_off = realloc(cg->uc_off, (size_t)cg->uc_cap * sizeof(*cg->uc_off));
		cg->uc_val = realloc(cg->uc_val, (size_t)cg->uc_cap * sizeof(*cg->uc_val));
	}

	cg->uc_off[cg->uc_n] = off;
	cg->uc_val[cg->uc_n] = v;
	cg->uc_n++;
}

/* Drop a recorded copy-constant (the local was assigned a non-foldable value).
   Tombstones in place rather than swap-removing: an enclosing unrolled loop
   restores uc_n to a snapshot at its inner loop's exit, and a swap-remove
   would let that restore resurrect the killed entry as a stale ghost. */
static void cg_unroll_const_kill(Codegen *cg, int off)
{
	for (int i = 0; i < cg->uc_n; i++)
	{
		if (cg->uc_off[i] == off)
		{
			cg->uc_off[i] = -1;
			return;
		}
	}
}

/* Fold `e` to a compile-time constant under the copy-constant environment:
   integer literals, recorded locals, casts between integer kinds, and the
   low-32-pure operators of foldable operands. TY_INT results wrap to 32 bits,
   mirroring what the emitted 32-bit ops would compute. */
static int cg_fold_const(Codegen *cg, Expr *e, long long *out)
{
	if (!e || !cg->unrolling)
	{
		return 0;
	}

	long long va, vb;
	switch (e->kind)
	{
	case EX_INT:
		*out = e->int_val;
		return 1;
	case EX_IDENT:
		return ty_is_int(e->type.kind) && e->anno_int > 0
			   && cg_unroll_const(cg, e->anno_int, out);
	case EX_CAST:
		if (!ty_is_int(e->type.kind) || !ty_is_int(e->lhs->type.kind)
			|| !cg_fold_const(cg, e->lhs, &va))
		{
			return 0;
		}

		*out = (e->type.kind == TY_INT) ? (long long)(int)va : va;
		return 1;
	case EX_BINARY:
		if (!ty_is_int(e->type.kind)
			|| !cg_fold_const(cg, e->lhs, &va) || !cg_fold_const(cg, e->rhs, &vb))
		{
			return 0;
		}

		switch (e->op)
		{
		case TOKEN_PLUS:
			*out = va + vb;
			break;
		case TOKEN_MINUS:
			*out = va - vb;
			break;
		case TOKEN_STAR:
			*out = va * vb;
			break;
		case TOKEN_SHL:
			if (vb < 0 || vb > 63)
			{
				return 0;
			}

			*out = va << vb;
			break;
		case TOKEN_SHR:
			if (vb < 0 || vb > 63)
			{
				return 0;
			}

			*out = va >> vb;
			break;
		case TOKEN_AMP:
			*out = va & vb;
			break;
		case TOKEN_PIPE:
			*out = va | vb;
			break;
		case TOKEN_CARET:
			*out = va ^ vb;
			break;
		default:
			return 0;
		}

		if (e->type.kind == TY_INT)
		{
			*out = (long long)(int)*out;
		}

		return 1;
	default:
		return 0;
	}
}

/* Direct memory-operand text for a BCE-safe array access whose index folds
   under the copy-constant environment ("[r8 + 284]"), or whose index is
   IDENT + foldable-constant ("[r8 + rcx*4 + 60]", after loading the ident
   into `scratch`). Returns 1 with the operand in buf; 0 when not matched.
   The array base must already live in a register (promoted local or hoist
   cache) - a stack-resident base falls back to the generic path. */
static int cg_index_mem(Codegen *cg, Expr *e, char *buf, const char *scratch)
{
	if (!cg->unrolling || !e->anno_index_safe
		|| e->lhs->kind != EX_IDENT || e->lhs->anno_int <= 0)
	{
		return 0;
	}

	const char *br = cg_local_reg(cg, e->lhs->anno_int);
	if (!br)
	{
		br = cg_hoist_reg(cg, e->lhs->anno_int);
	}

	if (!br)
	{
		return 0;
	}

	int stride = cg_elem_stride(e->type.kind);
	long long c;
	if (cg_fold_const(cg, e->rhs, &c) && c >= 0)
	{
		sprintf(buf, "[%s + %lld]", br, 32 + c * stride);
		return 1;
	}

	/* IDENT + foldable-constant (either order): one scratch load + scaled mode. */
	if (e->rhs->kind == EX_BINARY && e->rhs->op == TOKEN_PLUS)
	{
		Expr *id = NULL;
		long long k = 0;
		if (e->rhs->lhs->kind == EX_IDENT && e->rhs->lhs->anno_int > 0
			&& cg_fold_const(cg, e->rhs->rhs, &k))
		{
			id = e->rhs->lhs;
		}
		else if (e->rhs->rhs->kind == EX_IDENT && e->rhs->rhs->anno_int > 0
				 && cg_fold_const(cg, e->rhs->lhs, &k))
		{
			id = e->rhs->rhs;
		}

		if (id && k >= 0 && !cg_is_unroll_const(cg, id->anno_int))
		{
			const char *ir = cg_local_reg(cg, id->anno_int);
			if (!ir)
			{
				if (id->type.kind == TY_LONG)
				{
					cg_emit(cg, "    mov %s, [rbp - %d]", scratch, id->anno_int);
				}
				else
				{
					cg_emit(cg, "    movsxd %s, dword [rbp - %d]", scratch, id->anno_int);
				}

				ir = scratch;
			}

			sprintf(buf, "[%s + %s*%d + %lld]", br, ir, stride, 32 + k * stride);
			return 1;
		}
	}

	return 0;
}

/* Pure probe for cg_index_mem: true if it would produce a direct operand.
   Emits nothing, so a caller can pick its evaluation order first. */
static int cg_index_mem_match(Codegen *cg, Expr *e)
{
	if (!cg->unrolling || e->kind != EX_INDEX || !e->anno_index_safe
		|| e->lhs->kind != EX_IDENT || e->lhs->anno_int <= 0)
	{
		return 0;
	}

	if (!cg_local_reg(cg, e->lhs->anno_int) && !cg_hoist_reg(cg, e->lhs->anno_int))
	{
		return 0;
	}

	long long c;
	if (cg_fold_const(cg, e->rhs, &c) && c >= 0)
	{
		return 1;
	}

	if (e->rhs->kind == EX_BINARY && e->rhs->op == TOKEN_PLUS)
	{
		long long k;
		if (e->rhs->lhs->kind == EX_IDENT && e->rhs->lhs->anno_int > 0
			&& cg_fold_const(cg, e->rhs->rhs, &k) && k >= 0
			&& !cg_is_unroll_const(cg, e->rhs->lhs->anno_int))
		{
			return 1;
		}

		if (e->rhs->rhs->kind == EX_IDENT && e->rhs->rhs->anno_int > 0
			&& cg_fold_const(cg, e->rhs->lhs, &k) && k >= 0
			&& !cg_is_unroll_const(cg, e->rhs->rhs->anno_int))
		{
			return 1;
		}
	}

	return 0;
}

/* True if `e` is one of the strength-reduced accesses for the current loop. */
static int cg_sr_contains(Codegen *cg, Expr *e)
{
	for (int i = 0; i < cg->sr_n; i++)
	{
		if (cg->sr_node[i] == e)
		{
			return 1;
		}
	}

	return 0;
}

/* If `e` is a strength-reduced access, write its direct addressing mode
   "[base + iv*stride]" into buf and return 1, so the element can be loaded or
   stored in a single instruction without first materialising the address. */
static int cg_sr_mode(Codegen *cg, Expr *e, char *buf)
{
	for (int i = 0; i < cg->sr_n; i++)
	{
		if (cg->sr_node[i] == e)
		{
			if (cg->unrolling)
			{
				sprintf(buf, "[%s + %lld]", CG_HOIST_REGS[cg->sr_reg[i]], cg->unroll_iv_val * cg->sr_stride[i]);
			}
			else
			{
				sprintf(buf, "[%s + %s*%d]", CG_HOIST_REGS[cg->sr_reg[i]], cg->sr_ivreg, cg->sr_stride[i]);
			}

			return 1;
		}
	}

	return 0;
}

/* Pure single-mode operand for a BCE-safe array access: when the element
   address folds into one x86 addressing mode from register-resident values
   ("[r8 + r12*8 + 32]"), write it into buf and return 1. Nothing is emitted
   and no scratch register is clobbered, so callers fuse the operand straight
   into an FP load or arithmetic op instead of staging the address in rbx.
   Covers the strength-reduced accesses plus a register-resident IDENT or
   IDENT +/- small-constant index over a register-resident base. */
static int cg_index_opnd(Codegen *cg, Expr *e, char *buf)
{
	if (cg_sr_mode(cg, e, buf))
	{
		return 1;
	}

	if (!e->anno_index_safe || e->lhs->kind != EX_IDENT || e->lhs->anno_int <= 0)
	{
		return 0;
	}

	Expr *idx = NULL;
	long long k = 0;
	if (e->rhs->kind == EX_IDENT)
	{
		idx = e->rhs;
	}
	else if (e->rhs->kind == EX_BINARY
			 && (e->rhs->op == TOKEN_PLUS || e->rhs->op == TOKEN_MINUS)
			 && e->rhs->lhs->kind == EX_IDENT
			 && e->rhs->rhs->kind == EX_INT
			 && e->rhs->rhs->int_val >= 0 && e->rhs->rhs->int_val <= 0x10000000LL)
	{
		idx = e->rhs->lhs;
		k = (e->rhs->op == TOKEN_PLUS) ? e->rhs->rhs->int_val : -e->rhs->rhs->int_val;
	}

	if (!idx || idx->anno_int <= 0
		|| (cg->unrolling && cg_is_unroll_const(cg, idx->anno_int)))
	{
		return 0;
	}

	const char *ir = cg_local_reg(cg, idx->anno_int);
	if (!ir)
	{
		return 0;
	}

	const char *br = cg_local_reg(cg, e->lhs->anno_int);
	if (!br)
	{
		br = cg_hoist_reg(cg, e->lhs->anno_int);
	}

	if (!br)
	{
		return 0;
	}

	int stride = cg_elem_stride(e->type.kind);
	sprintf(buf, "[%s + %s*%d + %lld]", br, ir, stride, 32 + k * stride);
	return 1;
}

/* `x = a[i]` where x is a float-promoted double and the element address folds
   into a single mode: load the element straight into x's XMM home - one movsd,
   no xmm0 bounce, no address staging. Returns 1 when emitted. */
static int cg_fp_load_into_home(Codegen *cg, int off, TypeKind k, Expr *value)
{
	if (k != TY_DOUBLE || !value || value->kind != EX_INDEX || value->type.kind != TY_DOUBLE)
	{
		return 0;
	}

	const char *xr = cg_local_xmm(cg, off);
	if (!xr)
	{
		return 0;
	}

	char iop[64];
	if (!cg_index_opnd(cg, value, iop))
	{
		return 0;
	}

	cg_emit(cg, "    movsd %s, qword %s", xr, iop);
	return 1;
}

/* If `e` is a strength-reduced access arr[INV + iv], its element-0 address is
   already pinned in a register, so the element address is just base + iv*stride -
   a single lea, no base reload, no index reconstruction, no bounds check. Leaves
   the address in rbx like cg_index_addr and returns 1; returns 0 if not matched. */
static int cg_sr_addr(Codegen *cg, Expr *e)
{
	for (int i = 0; i < cg->sr_n; i++)
	{
		if (cg->sr_node[i] == e)
		{
			if (cg->unrolling)
			{
				cg_emit(cg,"    lea rbx, [%s + %lld]", CG_HOIST_REGS[cg->sr_reg[i]], cg->unroll_iv_val * cg->sr_stride[i]);
			}
			else
			{
				cg_emit(cg,"    lea rbx, [%s + %s*%d]", CG_HOIST_REGS[cg->sr_reg[i]], cg->sr_ivreg, cg->sr_stride[i]);
			}

			return 1;
		}
	}

	return 0;
}

/* Leave the address of element a[i] in rbx, bounds-checked. Evaluates the array
   (lhs) then the index (rhs); clobbers rax/rcx/rdx. An out-of-range index calls
   bzy_oob (no return). xmm0 is untouched on the in-range path, so a float/double
   value being stored survives address computation. */
static void cg_index_addr(Codegen *cg, TypeTable *tt, Expr *e)
{
	if (cg_sr_addr(cg, e))
	{
		return;
	}

	/* Unrolled copy with a foldable address: one lea from the direct operand. */
	char imem[64];
	if (cg_index_mem(cg, e, imem, "rcx"))
	{
		cg_emit(cg,"    lea rbx, %s", imem);
		return;
	}

	/* Fast path: a register-resident index (a promoted local, typically a loop
	   induction variable) needs neither a base spill nor a rematerialization into
	   rax - bounds-check and address it straight from its register. The register
	   is callee-saved (r12..r15), so it survives the slow-path bzy_oob call. */
	/* BCE: the analysis proved 0 <= index < length, so the runtime length
	   compare and the bzy_oob slow path are dead. Only the address arithmetic
	   remains. Sound because anno_index_safe is set conservatively. */
	int safe = e->anno_index_safe;

	/* Index of the form `REG +/- CONST` - a promoted int local offset by a small
	   constant, e.g. vx[b + 2] sweeping a flattened array-of-structs, or any
	   arr[base + k] the strength-reducer misses because the index is not the bare
	   loop counter. When the access is BCE-proved safe, the whole element address
	   folds into a single lea: base + ireg*stride + (32 + k*stride). This replaces
	   the materialize-index (load, add, re-extend) + park + reload-base sequence -
	   the dominant per-access cost in the matrix / codec block sweeps - with one
	   instruction. (A non-safe index keeps its bounds check via the paths below.) */
	if (safe && e->rhs->kind==EX_BINARY
		&& (e->rhs->op==TOKEN_PLUS || e->rhs->op==TOKEN_MINUS)
		&& e->rhs->lhs->kind==EX_IDENT && e->rhs->lhs->anno_int > 0
		&& e->rhs->rhs->kind==EX_INT
		&& e->rhs->rhs->int_val >= 0 && e->rhs->rhs->int_val <= 0x10000000LL
		&& !(cg->unrolling && cg_is_unroll_const(cg, e->rhs->lhs->anno_int)))
	{
		const char *ir = cg_local_reg(cg, e->rhs->lhs->anno_int);
		if (ir)
		{
			int stride = cg_elem_stride(e->type.kind);
			long long k = (e->rhs->op==TOKEN_PLUS) ? e->rhs->rhs->int_val : -e->rhs->rhs->int_val;
			long long disp = 32 + k * stride;
			/* The base is loop-invariant, so it may live in a function-promoted
			   register (r12-r15) or this loop's hoist cache (r8-r11); read it from
			   whichever holds it, else materialize it into rax. */
			const char *br = NULL;
			if (e->lhs->kind==EX_IDENT && e->lhs->anno_int > 0)
			{
				br = cg_local_reg(cg, e->lhs->anno_int);
				if (!br)
				{
					br = cg_hoist_reg(cg, e->lhs->anno_int);
				}
			}

			if (br)
			{
				cg_emit(cg,"    lea rbx, [%s + %s*%d + %lld]", br, ir, stride, disp);
			}
			else
			{
				cg_expr(cg,tt,e->lhs);                 /* Base -> rax. */
				cg_emit(cg,"    lea rbx, [rax + %s*%d + %lld]", ir, stride, disp);
			}

			return;
		}
	}

	/* The register-resident-index fast path reads the index register directly; it
	   must not fire for an unrolled loop's induction variable, whose live value is
	   a compile-time constant this copy (the register is not maintained). Falling
	   through materializes that constant via cg_expr. */
	const char *ireg = (e->rhs->kind==EX_IDENT && e->rhs->anno_int > 0
						&& !(cg->unrolling && cg_is_unroll_const(cg, e->rhs->anno_int)))
					   ? cg_local_reg(cg, e->rhs->anno_int) : NULL;
	if (ireg)
	{
		/* If the base pointer is already in a register (promoted r12-r15 or this
		   loop's hoist cache r8-r11), skip the cg_expr round-trip through rax and
		   use the register directly in the bounds check and address computation.
		   On the OOB never-return path, cg_iarg(2/3) may clobber hoist registers
		   (r8/r9 on Win64); that is safe because the in-bounds jb jumps past the
		   OOB code entirely, leaving the hoist register untouched. */
		const char *br = NULL;
		if (e->lhs->kind == EX_IDENT && e->lhs->anno_int > 0)
		{
			br = cg_local_reg(cg, e->lhs->anno_int);
			if (!br)
			{
				br = cg_hoist_reg(cg, e->lhs->anno_int);
			}
		}

		if (!br)
		{
			cg_expr(cg, tt, e->lhs);   /* Base -> rax (fallback when not register-resident). */
		}

		const char *base = br ? br : "rax";
		if (!safe)
		{
			int okf = cg_label(cg);
			int pcf = cg_label(cg);
			char lenop[32];
			if (e->anno_len_const > 0)
			{
				snprintf(lenop, sizeof lenop, "%lld", e->anno_len_const);   /* Constant-length array: immediate, no [base+24] load. */
			}
			else
			{
				snprintf(lenop, sizeof lenop, "[%s + 24]", base);
			}

			cg_emit(cg,"    cmp %s, %s", ireg, lenop);   /* Unsigned: catches negative and >= length. */
			cg_emit(cg,"    jb .L%d", okf);
			cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 0), ireg);       /* index. */
			cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 1), lenop);      /* length. */
			cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 2), pcf);
			cg_emit(cg,".L%d:", pcf);
			cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 3));
			cg_emit(cg,"    call bzy_oob");
			cg_emit(cg,".L%d:", okf);
		}

		cg_emit(cg,"    lea rbx, [%s + %s*%d + 32]", base, ireg, cg_elem_stride(e->type.kind));
		return;
	}

	/* Rematerialized-base path: when the array is a plain local (a single pure
	   load), evaluate the index first, park it, then reload the base. This drops
	   the spill/reload pair the generic path needs to carry the base across the
	   index evaluation - the dominant per-access cost in tight indexing loops. An
	   array-typed local cannot be reassigned by the index expression, and the
	   base load cannot throw, so loading it after the index is order-equivalent. */
	if (e->lhs->kind == EX_IDENT)
	{
		cg_expr(cg,tt,e->rhs);                 /* Index -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));   /* Index parked (rcx). */
		cg_expr(cg,tt,e->lhs);                 /* Base -> rax (rematerialized). */
		if (!safe)
		{
			cg_emit(cg,"    mov %s, [rax + 24]", cg_iarg(cg, 1)); /* Length. */
			int ok = cg_label(cg);
			int pc = cg_label(cg);
			cg_emit(cg,"    cmp %s, %s", cg_iarg(cg, 0), cg_iarg(cg, 1));
			cg_emit(cg,"    jb .L%d", ok);     /* Unsigned: catches negative and >= length. */
			cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 2), pc);
			cg_emit(cg,".L%d:", pc);
			cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 3));
			cg_emit(cg,"    call bzy_oob");
			cg_emit(cg,".L%d:", ok);
		}

		cg_emit(cg,"    lea rbx, [rax + %s*%d + 32]", cg_iarg(cg, 0), cg_elem_stride(e->type.kind));
		return;
	}

	cg_expr(cg,tt,e->lhs);                 /* Base -> rax. */
	cg_temp_push(cg);
	cg_expr(cg,tt,e->rhs);                 /* Index -> rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_temp_pop(cg);             /* base */
	if (!safe)
	{
		cg_emit(cg,"    mov %s, [rax + 24]", cg_iarg(cg, 1)); /* Length. */
		int ok = cg_label(cg);
		int pc = cg_label(cg);
		cg_emit(cg,"    cmp %s, %s", cg_iarg(cg, 0), cg_iarg(cg, 1));
		cg_emit(cg,"    jb .L%d", ok);         /* Unsigned: catches negative and >= length. */
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 2), pc);
		cg_emit(cg,".L%d:", pc);               /* The throw-site PC (within this function/try). */
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 3));
		cg_emit(cg,"    call bzy_oob");        /* rcx=index, rdx=length, r8=pc, r9=rbp; never returns. */
		cg_emit(cg,".L%d:", ok);
	}

	cg_emit(cg,"    lea rbx, [rax + %s*%d + 32]", cg_iarg(cg, 0), cg_elem_stride(e->type.kind));   /* e->type is the element type. */
}

/* Fold a register-resident integer array element straight into a memory operand,
   emitting the bounds check (if any) but NO `lea rbx`. For arr[i] where the base
   and the bare index i both live in registers - the interpreter/hot-loop shape -
   the element address is `[base + i*stride + 32]`, which a load or store can name
   directly; the separate lea-into-rbx the generic path emits is pure overhead Go
   does not pay (it folds slice addressing into the access). Writes the operand to
   `buf` and returns 1, or returns 0 (emitting nothing) when the access is not this
   register-resident bare-index integer shape, leaving the caller on cg_index_addr.
   The bounds check uses the immediate length when statically known, else [base+24];
   its bzy_oob path may clobber rcx/rdx/r8/r9 but never returns, and the in-bounds
   jump skips it, so the base register is intact where the operand is consumed. */
static int cg_index_checked_opnd(Codegen *cg, TypeTable *tt, Expr *e, char *buf, int bufsz)
{
	(void)tt;
	if (e->kind != EX_INDEX || e->lhs->kind != EX_IDENT || e->lhs->anno_int <= 0
		|| ty_is_float(e->type.kind) || ty_is_managed(e->type.kind))
	{
		return 0;
	}

	/* Only the bounds-checked case. A BCE-proved-safe access keeps the existing
	   folded-operand / strength-reduction paths (cg_index_opnd, cg_sr_addr inside
	   cg_index_addr) that the dense-array benchmarks are tuned around - do not
	   divert it. The win here is the data-dependent index that can never be proved
	   safe (an interpreter program counter / stack pointer), which previously paid a
	   lea-into-rbx on every access. */
	if (e->anno_index_safe)
	{
		return 0;
	}

	/* Bare register-resident index only (REG +/- const keeps the generic path: its
	   bounds check is on the computed index, not the bare register). */
	if (e->rhs->kind != EX_IDENT || e->rhs->anno_int <= 0
		|| (cg->unrolling && cg_is_unroll_const(cg, e->rhs->anno_int)))
	{
		return 0;
	}

	const char *ireg = cg_local_reg(cg, e->rhs->anno_int);
	if (!ireg)
	{
		return 0;
	}

	const char *br = cg_local_reg(cg, e->lhs->anno_int);
	if (!br)
	{
		br = cg_hoist_reg(cg, e->lhs->anno_int);
	}

	if (!br)
	{
		return 0;
	}

	int stride = cg_elem_stride(e->type.kind);
	if (!e->anno_index_safe)
	{
		int okf = cg_label(cg);
		int pcf = cg_label(cg);
		char lenop[32];
		if (e->anno_len_const > 0)
		{
			snprintf(lenop, sizeof lenop, "%lld", e->anno_len_const);   /* Constant-length array: immediate, no [base+24] load. */
		}
		else
		{
			snprintf(lenop, sizeof lenop, "[%s + 24]", br);
		}

		cg_emit(cg,"    cmp %s, %s", ireg, lenop);   /* Unsigned: catches negative and >= length. */
		cg_emit(cg,"    jb .L%d", okf);
		cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 0), ireg);
		cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 1), lenop);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 2), pcf);
		cg_emit(cg,".L%d:", pcf);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 3));
		cg_emit(cg,"    call bzy_oob");
		cg_emit(cg,".L%d:", okf);
	}

	snprintf(buf, bufsz, "[%s + %s*%d + 32]", br, ireg, stride);
	return 1;
}

/* Like cg_index_addr, but guarantees the array base survives in rax alongside
   rbx = element address. SHARED-gated managed-element sites need the base's
   gcinfo bit tested after the bounds check, so the folded/strength-reduced
   fast paths (which lose the base) are intentionally skipped - gated sites are
   never hot value loops. */
static void cg_index_addr_based(Codegen *cg, TypeTable *tt, Expr *e)
{
	int safe = e->anno_index_safe;
	if (e->lhs->kind == EX_IDENT)
	{
		/* An array-typed local cannot be reassigned by the index expression and
		   the base load cannot throw, so loading it after the index is
		   order-equivalent (same argument as cg_index_addr's remat path). */
		cg_expr(cg,tt,e->rhs);                 /* Index -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));   /* Index parked. */
		cg_expr(cg,tt,e->lhs);                 /* Base -> rax. */
	}
	else
	{
		cg_expr(cg,tt,e->lhs);                 /* Base -> rax. */
		cg_temp_push(cg);
		cg_expr(cg,tt,e->rhs);                 /* Index -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_temp_pop(cg);                       /* Base back in rax. */
	}

	if (!safe)
	{
		cg_emit(cg,"    mov %s, [rax + 24]", cg_iarg(cg, 1)); /* Length. */
		int ok = cg_label(cg);
		int pc = cg_label(cg);
		cg_emit(cg,"    cmp %s, %s", cg_iarg(cg, 0), cg_iarg(cg, 1));
		cg_emit(cg,"    jb .L%d", ok);         /* Unsigned: catches negative and >= length. */
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 2), pc);
		cg_emit(cg,".L%d:", pc);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 3));
		cg_emit(cg,"    call bzy_oob");        /* Never returns. */
		cg_emit(cg,".L%d:", ok);
	}

	cg_emit(cg,"    lea rbx, [rax + %s*%d + 32]", cg_iarg(cg, 0), cg_elem_stride(e->type.kind));
}


/* Preserve rax in a fixed frame temp slot instead of on the hardware stack, so
   the stack pointer stays static across the calls the caller is about to make.
   Nesting is handled by the depth counter; the frame pass (max_temp_depth) sized
   the region. The overflow trap turns an under-counted analysis into a loud
   compile-time failure rather than silent stack corruption. */
static void cg_temp_push(Codegen *cg)
{
	if (cg->cur_temp_depth >= cg->cur_temp_cap)
	{
		fprintf(stderr,"Codegen: temp-slot overflow (depth %d, cap %d) - frame analysis under-counted.\n",
				cg->cur_temp_depth, cg->cur_temp_cap);
		exit(1);
	}

	cg_emit(cg,"    mov [rbp - %d], rax", cg->temp_base + cg->cur_temp_depth*8);
	cg->cur_temp_depth++;
}

static void cg_temp_pop_reg(Codegen *cg, const char *reg)
{
	cg->cur_temp_depth--;
	cg_emit(cg,"    mov %s, [rbp - %d]", reg, cg->temp_base + cg->cur_temp_depth*8);
}

static void cg_temp_pop(Codegen *cg)
{
	cg_temp_pop_reg(cg,"rax");
}

/* Allocate n bytes from the per-function scratch arena (a software stack that
   replaces sub rsp,N). Returns the rbp offset of the block's byte 0 - the address
   the old code reached via [rsp + 0] - so a former [rsp + k] becomes
   [rbp - (base - k)]. Blocks nest (an arg block stays live while its argument
   expressions, themselves possibly calls, are evaluated), so the cursor is a true
   stack: cg_scratch_free unwinds in reverse. The overflow trap turns an
   under-counted max_scratch_bytes into a loud compile-time failure, not silent
   corruption. n is rounded to 16 to preserve in-arena alignment. */
static int cg_scratch_alloc(Codegen *cg, int n)
{
	int n16 = (n + 15) & ~15;
	cg->cur_scratch += n16;
	if (cg->cur_scratch > cg->cur_scratch_cap)
	{
		fprintf(stderr,"Codegen: scratch-arena overflow (%d > %d) - frame analysis under-counted.\n",
				cg->cur_scratch, cg->cur_scratch_cap);
		exit(1);
	}

	return cg->scratch_base + cg->cur_scratch;
}

static void cg_scratch_free(Codegen *cg, int n)
{
	cg->cur_scratch -= (n + 15) & ~15;
}

/* A runtime call. With the static-rsp frame, rsp is permanently rbp-frame
   (16-aligned) and the callee's 32-byte Win64 shadow space is reserved once at the
   bottom of the frame, so no per-call save / realign / shadow reservation is
   needed - just the bare call. */
static void cg_aligned_call(Codegen *cg, const char *fn)
{
	cg_emit(cg,"    call %s", fn);
}

/* Emit the per-thread pool-base fetch into `reg`, branching to .L<slow> when the
   pool is not reachable inline. Returns the addressing prefix for PoolTLS field
   access: "reg" on Windows (reg holds the PoolTLS pointer), "fs:reg" on Linux (reg
   holds the constant TLS offset, so fields live at fs:[reg + offset]). The returned
   pointer is a static buffer, valid until the next call. Shared by the inline
   allocator (pop) and the inline release (pool_free push). */
static const char *cg_emit_pool_base(Codegen *cg, const char *reg, int slow)
{
	static char base[16];
	if (cg->target == TARGET_WINDOWS)
	{
		cg_emit(cg,"    cmp dword [rel bzy_pool_slot_ready], 0");
		cg_emit(cg,"    je .L%d", slow);                  /* TLS slot not allocated yet. */
		cg_emit(cg,"    mov eax, [rel bzy_pool_slot]");
		cg_emit(cg,"    cmp eax, 64");
		cg_emit(cg,"    jae .L%d", slow);                 /* Slot beyond the inline TEB array. */
		cg_emit(cg,"    mov %s, [gs:0x1480 + rax*8]", reg);  /* PoolTLS* for the current thread. */
		cg_emit(cg,"    test %s, %s", reg, reg);
		cg_emit(cg,"    jz .L%d", slow);                  /* This thread has no pool yet. */
		snprintf(base, sizeof base, "%s", reg);
	}
	else
	{
		cg_emit(cg,"    cmp dword [rel bzy_tpool_off_ready], 0");
		cg_emit(cg,"    je .L%d", slow);                  /* Thread-pointer offset not known yet. */
		cg_emit(cg,"    mov %s, [rel bzy_tpool_off]", reg);  /* &t_pool - thread pointer (constant). */
		snprintf(base, sizeof base, "fs:%s", reg);        /* fs:[reg + N] reaches t_pool field N. */
	}

	return base;
}

/* Release the object pointer currently in the first integer-arg register; rax and
   rdx are clobbered (so is r8 on the inline-free path). The inline path finalizes
   two cases the runtime would otherwise be called for, and defers the rest:
     - a survivor (rc stays > 0) that is a leaf: decrement inline. A survivor with
       managed children might root a dead cycle and must be buffered -> runtime.
     - a leaf with no finalizer whose rc reaches 0: recycle it inline via pool_free
       (push onto its size-class free list). An object with children (free_object
       must release them), a finalizer (must run), an unpooled/oversized class, or a
       full pool -> runtime.
   The descriptor is probed once (rdx) to classify children/finalizer. NULL,
   unmanaged (rc 0), and shared (atomic dec) also defer. Crucially the refcount is
   only ever mutated on a path that completes inline, never before deferring -
   bzy_release always performs the decrement itself. */
static void cg_release_rcx(Codegen *cg)
{
	const char *a0 = cg_iarg(cg, 0);
	int slow = cg_label(cg);
	int nodesc = cg_label(cg);
	int surv = cg_label(cg);
	int freeobj = cg_label(cg);
	int done = cg_label(cg);

	cg_emit(cg,"    test %s, %s", a0, a0);
	cg_emit(cg,"    jz .L%d", done);                  /* NULL: nothing to release. */
	cg_emit(cg,"    mov rax, [%s + 16]", a0);         /* gcinfo. */
	cg_emit(cg,"    test al, 8");                     /* BZY_GCINFO_SHARED (bit 3): cross-core. */
	cg_emit(cg,"    jnz .L%d", slow);                 /* Shared: atomic dec in the runtime. */
	cg_emit(cg,"    mov rax, [%s + 8]", a0);          /* refcount. */
	cg_emit(cg,"    test rax, rax");
	cg_emit(cg,"    jz .L%d", done);                  /* rc==0: unmanaged (stack) object. */
	/* Probe the descriptor once (rdx) to classify children + finalizer; rax keeps rc. */
	cg_emit(cg,"    mov rdx, [%s]", a0);              /* vtable. */
	cg_emit(cg,"    test rdx, rdx");
	cg_emit(cg,"    jz .L%d", nodesc);                /* No vtable: leaf, no finalizer. */
	cg_emit(cg,"    mov rdx, [rdx - 8]");             /* type descriptor. */
	cg_emit(cg,"    test rdx, rdx");
	cg_emit(cg,"    jz .L%d", nodesc);                /* No descriptor: leaf, no finalizer. */
	cg_emit(cg,"    cmp qword [rdx + 8], 0");         /* ti[1]: child count (-1 = array span). */
	cg_emit(cg,"    jne .L%d", slow);                 /* Has children/span: buffer or walk in runtime. */
	cg_emit(cg,"    cmp rax, 1");
	cg_emit(cg,"    jg .L%d", surv);                  /* rc>=2: survivor (leaf). */
	cg_emit(cg,"    cmp qword [rdx], 0");             /* ti[0]: finalizer. */
	cg_emit(cg,"    jne .L%d", slow);                 /* Has finalizer: must run in the runtime. */
	cg_emit(cg,"    jmp .L%d", freeobj);              /* rc==1, leaf, no finalizer: recycle inline. */
	cg_emit(cg,".L%d:", nodesc);                      /* Descriptor-less: no children, no finalizer. */
	cg_emit(cg,"    cmp rax, 1");
	cg_emit(cg,"    jle .L%d", freeobj);              /* rc==1: recycle inline. */
	cg_emit(cg,".L%d:", surv);
	cg_emit(cg,"    sub rax, 1");
	cg_emit(cg,"    mov [%s + 8], rax", a0);          /* rc-- (survivor, leaf): finished inline. */
	cg_emit(cg,"    jmp .L%d", done);
	cg_emit(cg,".L%d:", freeobj);
	/* Inline pool_free(obj): push the block onto its size-class free list. The class
	   nibble lives in gcinfo bits 4-7; r8 holds the pool base (a0 keeps the object so
	   the push can store it). Defers to the runtime for an unpooled class (c==0, freed
	   with free()) or a full pool. */
	{
		const char *base = cg_emit_pool_base(cg, "r8", slow);
		cg_emit(cg,"    mov rax, [%s + 16]", a0);            /* gcinfo. */
		cg_emit(cg,"    shr rax, 4");
		cg_emit(cg,"    and rax, 15");                       /* c = (gcinfo >> 4) & 0xF. */
		cg_emit(cg,"    jz .L%d", slow);                     /* c==0: unpooled (free() in runtime). */
		cg_emit(cg,"    cmp dword [%s + rax*4 + 88], 256", base);  /* n[c] >= POOL_CAP? */
		cg_emit(cg,"    jge .L%d", slow);                    /* Pool full: free() in runtime. */
		cg_emit(cg,"    mov rdx, [%s + rax*8]", base);       /* head[c]. */
		cg_emit(cg,"    mov [%s], rdx", a0);                 /* obj->next = head[c] (link @ offset 0). */
		cg_emit(cg,"    mov [%s + rax*8], %s", base, a0);    /* head[c] = obj. */
		cg_emit(cg,"    inc dword [%s + rax*4 + 88]", base); /* n[c]++. */
		cg_emit(cg,"    dec qword [%s + 136]", base);        /* live--. */
		cg_emit(cg,"    jmp .L%d", done);
	}
	cg_emit(cg,".L%d:", slow);
	cg_aligned_call(cg,"bzy_release");
	cg_emit(cg,".L%d:", done);
}

/* Release every object-typed local of the function, optionally skipping one
   slot so a returned local can transfer ownership; pass except_off = -1 for none. */
static void cg_release_object_locals(Codegen *cg, Func *f, int except_off)
{
	for (int i=0; i<f->obj_local_count; i++)
	{
		int off = f->obj_local_offsets[i];
		if (off == except_off)
		{
			continue;
		}

		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), off);
		cg_release_rcx(cg);
	}
}

/* True if evaluating e leaves an owned (+1) object in rax: new, or a call or
   method call returning an object. Other object reads are borrowed (+0). */
static int expr_is_owned(Expr *e)
{
	if (!ty_is_managed(e->type.kind))
	{
		return 0;
	}

	/* An enum constant access (Color.RED) retains the singleton on read. */
	if (e->kind==EX_FIELD && e->lhs && e->lhs->kind==EX_IDENT && enum_is(e->lhs->name))
	{
		return 1;
	}

	/* A static managed field read (C.field) retains the slot's value. */
	if (e->kind==EX_FIELD && e->anno_int==-1)
	{
		return 1;
	}

	/* A SHARED-gated managed array element read retains on both paths (the
	   retain must be atomic with the load when the array crossed cores). */
	if (e->kind==EX_INDEX && e->anno_shared_gate)
	{
		return 1;
	}

	/* A managed EX_BINARY is a string concat (bzy_str_concat returns +1); an
	   EX_STR literal is +1 from bzy_str_new. */
	return e->kind==EX_NEW || e->kind==EX_CALL || e->kind==EX_METHOD_CALL
		   || e->kind==EX_STR || e->kind==EX_BINARY || e->kind==EX_NEWARRAY
		   || e->kind==EX_NEWMAP || e->kind==EX_NEWGEN || e->kind==EX_NEWCHANNEL
		   || e->kind==EX_LAMBDA;   /* A closure is a fresh +1 (capturing alloc, or singleton retained before return). */
}

/* Retain the object pointer currently in rax; rax is preserved (rdx is clobbered,
   as it would be by the runtime call anyway). The inline fast path handles the
   common case - a non-NULL, non-shared, managed object - with a plain refcount
   increment and a recolor to BLACK. NULL and unmanaged (rc 0) objects need no
   work; a shared object needs an atomic increment, so it defers to bzy_retain. */
static void cg_retain_rax(Codegen *cg)
{
	int slow = cg_label(cg);
	int done = cg_label(cg);

	cg_emit(cg,"    test rax, rax");
	cg_emit(cg,"    jz .L%d", done);                  /* NULL: nothing to retain. */
	cg_emit(cg,"    mov rdx, [rax + 16]");            /* gcinfo. */
	cg_emit(cg,"    test dl, 8");                     /* BZY_GCINFO_SHARED (bit 3): cross-core. */
	cg_emit(cg,"    jnz .L%d", slow);                 /* Shared: atomic inc in the runtime. */
	cg_emit(cg,"    mov rdx, [rax + 8]");             /* refcount. */
	cg_emit(cg,"    test rdx, rdx");
	cg_emit(cg,"    jz .L%d", done);                  /* rc==0: unmanaged (stack) object. */
	cg_emit(cg,"    add rdx, 1");
	cg_emit(cg,"    mov [rax + 8], rdx");             /* rc++. */
	cg_emit(cg,"    and qword [rax + 16], -4");       /* set_color BLACK (clear gcinfo bits 0-1). */
	cg_emit(cg,"    jmp .L%d", done);
	cg_emit(cg,".L%d:", slow);
	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_retain");
	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
	cg_emit(cg,".L%d:", done);
}

/* Evaluate e leaving a +1 owned object in rax, retaining borrowed reads. */
static void cg_expr_owned(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e);
	if (!expr_is_owned(e))
	{
		cg_retain_rax(cg);
	}
}

/* Leave an owned (+1) string in rax for one concat operand: a string is taken
   owned as-is; a scalar is converted to its text via the matching bzy_str_from_*
   helper (which returns +1), so the surrounding concat logic is uniform. */
static void cg_concat_operand(Codegen *cg, TypeTable *tt, Expr *op)
{
	TypeKind k = op->type.kind;
	if (k==TY_STRING)
	{
		cg_expr_owned(cg,tt,op);
		return;
	}

	if (ty_is_float(k))
	{
		cg_expr(cg,tt,op);                           /* Value in xmm0. */
		if (k==TY_FLOAT)
		{
			cg_emit(cg,"    cvtss2sd xmm0, xmm0");   /* Promote to double (matches print). */
		}

		cg_aligned_call(cg,"bzy_str_from_f64");      /* Double arg in xmm0; owned (+1) string in rax. */
		return;
	}

	cg_expr(cg,tt,op);                               /* Integer/bool in rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	const char *fn = k==TY_BOOL ? "bzy_str_from_bool"
					 : ty_is_unsigned(k) ? "bzy_str_from_u64"
					 : "bzy_str_from_i64";
	cg_aligned_call(cg,fn);                          /* Owned (+1) string in rax. */
}

/* The maximum operands a single concat chain flattens into one bzy_str_concat_n
   call. Longer chains (vanishingly rare) fall back to pairwise lowering. */
/* CONCAT_MAX is a degrade-safe bound, NOT a hard cap: a string-concat chain with
   more leaves than this lowers via the pairwise fallback below instead of the
   one-shot n-ary join. It stays fixed because the frame-scratch reservation for
   the n-ary path is sized to it in resolve.c (frame_node_block); growing it would
   require coordinated dynamic frame sizing for negligible benefit (>64 '+' in one
   expression is vanishingly rare). */
#define CONCAT_MAX 64

/* Collect the leaf operands of a maximal string-concat tree into out[], left to
   right. A '+' whose result type is string is a concat node; anything else is a
   leaf. String concat is associative, so a flat left-to-right join is identical
   to the nested evaluation, and the left-to-right walk preserves side-effect
   order. Returns the operand count, or -1 if the chain exceeds CONCAT_MAX. */
static int cg_collect_concat(Expr *e, Expr **out, int n, int cap)
{
	if (n < 0)
	{
		return -1;                       /* Propagate an earlier overflow. */
	}

	if (e->kind==EX_BINARY && e->type.kind==TY_STRING)
	{
		n = cg_collect_concat(e->lhs, out, n, cap);
		n = cg_collect_concat(e->rhs, out, n, cap);
		return n;
	}

	if (n >= cap)
	{
		return -1;                       /* Too many operands: signal the fallback. */
	}

	out[n++] = e;
	return n;
}

/* Fallback pairwise concat for chains longer than CONCAT_MAX: produce an owned
   string for each operand (strings as-is, scalars converted), call bzy_str_concat,
   then release the two operand temporaries. Operands and result are spilled on the
   machine stack so nested concats compose. */
static void cg_str_concat_pair(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_concat_operand(cg,tt,e->lhs);
	int b = cg_scratch_alloc(cg, 32);
	cg_emit(cg,"    mov [rbp - %d], rax", b);          /* lhs. */
	cg_concat_operand(cg,tt,e->rhs);
	cg_emit(cg,"    mov [rbp - %d], rax", b - 8);      /* rhs. */
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
	cg_aligned_call(cg,"bzy_str_concat");      /* Owned (+1) result in rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", b - 16);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);           /* Release the lhs temporary. */
	cg_release_rcx(cg);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);       /* Release the rhs temporary. */
	cg_release_rcx(cg);
	cg_emit(cg,"    mov rax, [rbp - %d]", b - 16);
	cg_scratch_free(cg, 32);
}

/* String concatenation: flatten the whole '+' chain and join it in one
   bzy_str_concat_n call (one allocation, O(n) copying). Each operand is
   evaluated to an owned (+1) string in a reserved stack array; after the join
   every operand temporary is released. Falls back to pairwise lowering only if
   the chain exceeds CONCAT_MAX operands. */
static void cg_str_concat(Codegen *cg, TypeTable *tt, Expr *e)
{
	Expr *ops[CONCAT_MAX];
	int n = cg_collect_concat(e, ops, 0, CONCAT_MAX);
	if (n < 0)
	{
		cg_str_concat_pair(cg,tt,e);
		return;
	}

	/* Reserve an arena region: n operand pointers + 1 result slot. The region is
	   rbp-relative, so it survives operand evaluation (and the inner cg_aligned_call's
	   rsp dance) unconditionally. */
	int slots = ((n + 1) * 8 + 15) & ~15;
	int b = cg_scratch_alloc(cg, slots);
	for (int i = 0; i < n; i++)
	{
		cg_concat_operand(cg,tt,ops[i]);                  /* Owned (+1) string in rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - i*8);
	}

	cg_emit(cg,"    lea %s, [rbp - %d]", cg_iarg(cg, 0), b);   /* parts = &ops[0]. */
	cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), n);      /* count. */
	cg_aligned_call(cg,"bzy_str_concat_n");              /* Owned (+1) result in rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", b - n*8);       /* Stash the result above the operands. */

	for (int i = 0; i < n; i++)                          /* Release each operand temporary. */
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - i*8);
		cg_release_rcx(cg);
	}

	cg_emit(cg,"    mov rax, [rbp - %d]", b - n*8);       /* Result back into rax. */
	cg_scratch_free(cg, slots);
}

/* Widen the operand just evaluated (an integer in rax, or a float/double already
   in xmm0) to the common floating type `ct`, leaving the result in xmm0. */
static void cg_fp_promote(Codegen *cg, TypeKind from, TypeKind ct)
{
	if (ty_is_int(from))
	{
		cg_emit(cg, ct==TY_DOUBLE ? "    cvtsi2sd xmm0, rax" : "    cvtsi2ss xmm0, rax");
	}
	else if (from==TY_FLOAT && ct==TY_DOUBLE)
	{
		cg_emit(cg,"    cvtss2sd xmm0, xmm0");
	}
}

/* If `e` is a same-type FP leaf - a stack local or an array element - format an
   x86 memory operand for it into `buf` and return it; the caller folds that
   straight into the arithmetic op (mulsd xmm0, qword [..]) with no load. Returns
   NULL if `e` is not such a leaf (the caller then spills to compose a nested
   rhs). Array-element addressing uses only the integer registers, so the lhs
   already in xmm0 survives. The leaf's type must equal the op's common type, so
   no cvtss2sd is needed. */
/* Operand string for a same-type FP local leaf: its XMM register if float-promoted,
   else its stack slot ("qword [rbp - off]"). A promoted double is read straight from
   xmm2..5 (caller-saved, not clobbered between its store and this fold). */
static const char *cg_fp_local_opnd(Codegen *cg, Expr *e, TypeKind ct, char *buf, int bufsz)
{
	const char *xr = ct==TY_DOUBLE ? cg_local_xmm(cg, e->anno_int) : NULL;
	if (xr)
	{
		snprintf(buf, bufsz, "%s", xr);
		return buf;
	}

	const char *sz = (ct==TY_DOUBLE) ? "qword" : "dword";
	snprintf(buf, bufsz, "%s [rbp - %d]", sz, e->anno_int);
	return buf;
}

static const char *cg_fp_rhs_leaf(Codegen *cg, TypeTable *tt, Expr *e, TypeKind ct, char *buf, int bufsz)
{
	if (e->type.kind != ct)
	{
		return NULL;
	}

	const char *sz = (ct==TY_DOUBLE) ? "qword" : "dword";
	if (e->kind==EX_IDENT && e->anno_int > 0 && !cg_local_reg(cg, e->anno_int))
	{
		return cg_fp_local_opnd(cg, e, ct, buf, bufsz);   /* FP local: XMM home if promoted, else slot. */
	}

	if (e->kind==EX_INDEX)
	{
		char iop[64];
		if (cg_index_opnd(cg, e, iop))
		{
			snprintf(buf, bufsz, "%s %s", sz, iop);   /* Folded mode: no lea, no rbx. */
			return buf;
		}

		cg_index_addr(cg,tt,e);            /* rbx = element address; xmm0 untouched. */
		snprintf(buf, bufsz, "%s [rbx]", sz);
		return buf;
	}

	return NULL;
}

/* A same-type FP local that lives in its stack slot - the operand a multiply-
   accumulate fast path can read straight from memory without any setup. */
static int cg_fp_simple_local(Codegen *cg, Expr *e, TypeKind ct)
{
	return e->type.kind==ct && e->kind==EX_IDENT && e->anno_int > 0
		   && !cg_local_reg(cg, e->anno_int);
}

/* Floating-point binary op. The left operand evaluates to xmm0 and the right to
   xmm1. A same-type leaf rhs folds straight into the op as a memory operand (no
   round-trip); a `lhs +/- A*B` with same-type local A,B becomes a multiply-
   accumulate into a scratch register (no spill); otherwise the lhs is spilled to
   the machine stack so a nested rhs expression - which would clobber xmm0/xmm1 -
   composes correctly. Both operands promote to the common type: double if either
   side is double, otherwise float (integers convert in with cvtsi2ss/sd). */
static void cg_binary_fp(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind ct = (e->lhs->type.kind==TY_DOUBLE || e->rhs->type.kind==TY_DOUBLE) ? TY_DOUBLE : TY_FLOAT;
	const char *sfx = (ct==TY_DOUBLE) ? "sd" : "ss";

	/* Multiply-accumulate: `lhs +/- (A * B)` for same-type locals A, B. Compute
	   A*B in xmm1 and combine into xmm0 (the evaluated lhs), with no lhs spill.
	   This is a separate mul then add/sub (no FMA contraction), so the result is
	   bit-identical to the generic path. It collapses the dot-product chains a 3D
	   transform or physics step is built from. */
	if ((e->op==TOKEN_PLUS || e->op==TOKEN_MINUS) && e->rhs->kind==EX_BINARY
		&& e->rhs->op==TOKEN_STAR
		&& cg_fp_simple_local(cg, e->rhs->lhs, ct)
		&& cg_fp_simple_local(cg, e->rhs->rhs, ct))
	{
		const char *mov = (ct==TY_DOUBLE) ? "movsd" : "movss";
		char abuf[48], bbuf[48];
		cg_expr(cg,tt,e->lhs);
		cg_fp_promote(cg, e->lhs->type.kind, ct);
		const char *aop = cg_fp_local_opnd(cg, e->rhs->lhs, ct, abuf, sizeof abuf);
		/* A register source takes movaps: the register form of movsd/movss merges
		   into xmm1's upper bits, false-depending on its previous value. */
		cg_emit(cg,"    %s xmm1, %s", strncmp(aop, "xmm", 3)==0 ? "movaps" : mov, aop);
		cg_emit(cg,"    mul%s xmm1, %s", sfx, cg_fp_local_opnd(cg, e->rhs->rhs, ct, bbuf, sizeof bbuf));
		cg_emit(cg,"    %s%s xmm0, xmm1", e->op==TOKEN_PLUS ? "add" : "sub", sfx);
		return;
	}

	cg_expr(cg,tt,e->lhs);
	cg_fp_promote(cg, e->lhs->type.kind, ct);
	char rbuf[48];
	const char *rhs = cg_fp_rhs_leaf(cg, tt, e->rhs, ct, rbuf, sizeof rbuf);
	if (!rhs)
	{
		int b = cg_scratch_alloc(cg, 8);
		cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);   /* Spill lhs (float lives in the low 4 bytes). */
		cg_expr(cg,tt,e->rhs);
		cg_fp_promote(cg, e->rhs->type.kind, ct);
		cg_emit(cg,"    movaps xmm1, xmm0");          /* rhs -> xmm1. */
		cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);   /* lhs -> xmm0. */
		cg_scratch_free(cg, 8);
		rhs = "xmm1";
	}
	switch (e->op)
	{
	case TOKEN_PLUS:
		cg_emit(cg,"    add%s xmm0, %s", sfx, rhs);
		break;
	case TOKEN_MINUS:
		cg_emit(cg,"    sub%s xmm0, %s", sfx, rhs);
		break;
	case TOKEN_STAR:
		cg_emit(cg,"    mul%s xmm0, %s", sfx, rhs);
		break;
	case TOKEN_SLASH:
		cg_emit(cg,"    div%s xmm0, %s", sfx, rhs);
		break;
	default:   /* comparison -> bool in rax (unordered/NaN compares false except !=). */
	{
		const char *set;
		switch (e->op)
		{
		case TOKEN_EQ:
			set="sete";
			break;
		case TOKEN_NEQ:
			set="setne";
			break;
		case TOKEN_LT:
			set="setb";
			break;
		case TOKEN_GT:
			set="seta";
			break;
		case TOKEN_LTE:
			set="setbe";
			break;
		default:
			set="setae";
			break;
		}

		cg_emit(cg,"    ucomi%s xmm0, %s", sfx, rhs);
		cg_emit(cg,"    %s al", set);
		cg_emit(cg,"    movzx rax, al");
		break;
	}
	}
}

/* Set up the rhs operand of an integer binary expression (lhs is already in rax).
   On return *rhsop names the operand for the instruction - a folded immediate in
   immbuf (>= 24 bytes) or the register "rbx" - and *uns is the combined operand
   unsignedness. Factored out of cg_binary so the conditional-branch path lowers
   comparison operands identically. */
/* Ops whose rhs is consumed via *rhsop (so a promoted rhs can be used directly).
   The others (*, /, <<, >>, & | ^) hardcode rbx in cg_binary, so they must keep
   loading rbx. This set matches the immediate-fuse set. */
static int cg_op_uses_rhsop(int op)
{
	return op==TOKEN_PLUS || op==TOKEN_MINUS
		   || op==TOKEN_EQ || op==TOKEN_NEQ || op==TOKEN_LT
		   || op==TOKEN_GT || op==TOKEN_LTE || op==TOKEN_GTE;
}

static void cg_binop_rhs(Codegen *cg, TypeTable *tt, Expr *e, const char **rhsop, char *immbuf, int *uns)
{
	*rhsop = "rbx";
	int fuse = (e->rhs->kind==EX_INT || e->rhs->kind==EX_BOOL)
			   && e->rhs->int_val >= -2147483648LL && e->rhs->int_val <= 2147483647LL
			   && (e->op==TOKEN_PLUS || e->op==TOKEN_MINUS || e->op==TOKEN_EQ
				   || e->op==TOKEN_NEQ || e->op==TOKEN_LT || e->op==TOKEN_GT
				   || e->op==TOKEN_LTE || e->op==TOKEN_GTE);
	if (fuse)
	{
		snprintf(immbuf,24,"%lld", e->rhs->int_val);
		*rhsop = immbuf;
	}
	else if (e->rhs->kind==EX_INT || e->rhs->kind==EX_BOOL)
	{
		cg_emit(cg,"    mov rbx, %lld", e->rhs->int_val);
	}
	else if (e->rhs->kind==EX_NULL)
	{
		cg_emit(cg,"    mov rbx, 0");
	}
	else if (e->rhs->kind==EX_IDENT)
	{
		const char *r = cg_local_reg(cg, e->rhs->anno_int);
		if (!r) r = cg_hoist_reg(cg, e->rhs->anno_int);
		if (r && cg_op_uses_rhsop(e->op))
		{
			*rhsop = r;   /* compare/+/- read the operand straight from its register. */
		}
		else if (r)
		{
			cg_emit(cg,"    mov rbx, %s", r);   /* imul/and/shift path needs rbx. */
		}
		else
		{
			char mem[32];
			sprintf(mem,"[rbp - %d]", e->rhs->anno_int);
			cg_load_scalar_into(cg,e->rhs->type.kind,mem,"rbx","ebx");
		}
	}
	else
	{
		cg_temp_push(cg);            /* Preserve lhs in a frame slot across the rhs evaluation. */
		cg_expr(cg,tt,e->rhs);
		cg_emit(cg,"    mov rbx, rax");
		cg_temp_pop(cg);
	}

	*uns = ty_is_unsigned(e->lhs->type.kind) || ty_is_unsigned(e->rhs->type.kind);
}

/* Peel a non-narrowing integer cast: `(int)b` over a byte/short value is only a
   sign/zero extension, so when the consumer masks the result back to the source
   width (x & 0xFF / 0xFFFF) the cast is irrelevant and the inner value can be
   loaded directly. Returns the inner expr, or `e` unchanged when not such a cast. */
static Expr *cg_peel_widening_cast(Expr *e)
{
	if (e->kind==EX_CAST && !ty_is_float(e->type.kind) && !ty_is_float(e->lhs->type.kind)
		&& ty_bits(e->type.kind) >= ty_bits(e->lhs->type.kind))
	{
		return e->lhs;
	}

	return e;
}

/* An op whose result's low 32 bits depend only on the low 32 bits of its operands:
   the operand may be left zero-extended (the natural state after a 32-bit op) rather
   than sign-extended. True for + - * & | ^ and a left shift's shifted value. False
   for >> (sign bits shift in), / and % (the divide reads the full register), and the
   comparisons (which read the full width). */
static int cg_op_low32_pure(int op)
{
	switch (op)
	{
	case TOKEN_PLUS:
	case TOKEN_MINUS:
	case TOKEN_STAR:
	case TOKEN_AMP:
	case TOKEN_PIPE:
	case TOKEN_CARET:
	case TOKEN_SHL:
		return 1;
	default:
		return 0;
	}
}

/* Emit the trailing re-extension for an integer binary result, unless the consumer
   asked for low-32-only (want_low32) and the result is a 32-bit int: then the 32-bit
   op already left the low 32 bits correct and the high bits are never observed, so
   the movsxd is dead. Non-int widths and full-width consumers re-extend as before. */
static void cg_extend_int_result(Codegen *cg, Expr *e, int want_low32)
{
	if (want_low32 && e->type.kind==TY_INT)
	{
		return;
	}

	cg_extend_reg(cg, e->type.kind);
}

/* Evaluate an operand that lands in rax/eax whose enclosing op (given by `op`) only
   consumes its low 32 bits: set the one-hop low-32 hint first so a nested int binary
   there skips its own trailing extension. A no-op hint for impure ops (the operand
   then re-extends as usual, which / % >> and the comparisons require). */
static void cg_eval_low32_operand(Codegen *cg, TypeTable *tt, Expr *operand, int op)
{
	if (cg_op_low32_pure(op))
	{
		cg->low32_ok = 1;
	}

	cg_expr(cg, tt, operand);
}

static void cg_binary(Codegen *cg, TypeTable *tt, Expr *e, int want_low32)
{
	if (e->type.kind==TY_STRING)
	{
		cg_str_concat(cg,tt,e);
		return;
	}

	if (ty_is_float(e->lhs->type.kind) || ty_is_float(e->rhs->type.kind))
	{
		cg_binary_fp(cg,tt,e);
		return;
	}

	if (e->op==TOKEN_AND || e->op==TOKEN_OR)
	{
		/* Short-circuit: evaluate lhs; if it already decides the result, skip rhs.
		   Bool values are 0/1, so the surviving rhs value is the result as-is. */
		int done = cg_label(cg);
		int shortcut = cg_label(cg);
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg, e->op==TOKEN_AND ? "    je .L%d" : "    jne .L%d", shortcut);
		cg_expr(cg,tt,e->rhs);
		cg_emit(cg,"    jmp .L%d", done);
		cg_emit(cg,".L%d:", shortcut);
		cg_emit(cg, e->op==TOKEN_AND ? "    mov rax, 0" : "    mov rax, 1");
		cg_emit(cg,".L%d:", done);
		return;
	}

	/* Mask to an unsigned octet / halfword: `x & 0xFF` -> `movzx eax, al`,
	   `x & 0xFFFF` -> `movzx eax, ax`. This is the universal unsigned-byte unpack
	   `(int)b & 255` that every parse/codec inner loop runs. It replaces the generic
	   `mov rbx,0xFF; and rax,rbx; movsxd rax,eax` with a single zero-extending move,
	   and the result (0..255 / 0..65535) is already a valid non-negative 64-bit
	   value, so no re-extension follows. When the masked operand is a strength-
	   reduced byte/short array element (the cast over the index is pure widening and
	   is peeled), the load and mask fuse to one `movzx eax, byte/word [base + iv]`. */
	if (e->op==TOKEN_AMP && e->rhs->kind==EX_INT && e->type.kind==TY_INT
		&& (e->rhs->int_val==0xFF || e->rhs->int_val==0xFFFF))
	{
		int w = (e->rhs->int_val==0xFF) ? 8 : 16;
		const char *sz = (w==8) ? "byte" : "word";
		Expr *inner = cg_peel_widening_cast(e->lhs);
		char srm[40];
		if (inner->kind==EX_INDEX && cg_elem_stride(inner->type.kind)==w/8
			&& cg_sr_mode(cg, inner, srm))
		{
			cg_emit(cg, "    movzx eax, %s %s", sz, srm);
			return;
		}

		cg_eval_low32_operand(cg,tt,e->lhs,e->op);   /* lhs -> rax (low byte/word only). */
		cg_emit(cg, w==8 ? "    movzx eax, al" : "    movzx eax, ax");
		return;
	}

	/* Fuse a strength-reduced array element as a memory operand for a 32-bit integer
	   op: `<op> eax, dword [base + iv*stride]`. This is the dense-kernel multiply-
	   accumulate - it keeps both elements out of registers and removes the lhs spill
	   the generic path carries across the rhs load. The other operand is evaluated
	   into eax; for a commutative op either side may be the memory one. */
	{
		int fuse_op = (e->op==TOKEN_PLUS || e->op==TOKEN_MINUS || e->op==TOKEN_STAR
					   || e->op==TOKEN_AMP || e->op==TOKEN_PIPE || e->op==TOKEN_CARET);
		int commutative = (e->op != TOKEN_MINUS);
		const char *opc = NULL;
		switch (e->op)
		{
		case TOKEN_PLUS:  opc = "add";  break;
		case TOKEN_MINUS: opc = "sub";  break;
		case TOKEN_STAR:  opc = "imul"; break;
		case TOKEN_AMP:   opc = "and";  break;
		case TOKEN_PIPE:  opc = "or";   break;
		case TOKEN_CARET: opc = "xor";  break;
		default: break;
		}

		char srm[40];
		if (fuse_op && e->type.kind==TY_INT && e->rhs->type.kind==TY_INT
			&& cg_sr_mode(cg, e->rhs, srm))
		{
			cg_eval_low32_operand(cg,tt,e->lhs,e->op);   /* Other operand -> eax. */
			cg_emit(cg,"    %s eax, dword %s", opc, srm);
			cg_extend_int_result(cg, e, want_low32);
			return;
		}

		if (fuse_op && commutative && e->type.kind==TY_INT && e->lhs->type.kind==TY_INT
			&& cg_sr_mode(cg, e->lhs, srm))
		{
			cg_eval_low32_operand(cg,tt,e->rhs,e->op);   /* Other operand -> eax. */
			cg_emit(cg,"    %s eax, dword %s", opc, srm);
			cg_extend_int_result(cg, e, want_low32);
			return;
		}

		/* The unrolled-copy twin: a safe array element whose address folds via
		   the copy-constant environment fuses the same way. The probe is pure,
		   so the other operand evaluates into eax first; the operand's scratch
		   load (rcx, if its index ident is stack-resident) follows safely. */
		char umem[64];
		if (fuse_op && e->type.kind==TY_INT && e->rhs->type.kind==TY_INT
			&& cg_elem_stride(e->rhs->type.kind)==4 && cg_index_mem_match(cg, e->rhs))
		{
			cg_eval_low32_operand(cg,tt,e->lhs,e->op);   /* Other operand -> eax. */
			cg_index_mem(cg, e->rhs, umem, "rcx");
			cg_emit(cg,"    %s eax, dword %s", opc, umem);
			cg_extend_int_result(cg, e, want_low32);
			return;
		}

		if (fuse_op && commutative && e->type.kind==TY_INT && e->lhs->type.kind==TY_INT
			&& cg_elem_stride(e->lhs->type.kind)==4 && cg_index_mem_match(cg, e->lhs))
		{
			cg_eval_low32_operand(cg,tt,e->rhs,e->op);   /* Other operand -> eax. */
			cg_index_mem(cg, e->lhs, umem, "rcx");
			cg_emit(cg,"    %s eax, dword %s", opc, umem);
			cg_extend_int_result(cg, e, want_low32);
			return;
		}
	}

	cg_eval_low32_operand(cg,tt,e->lhs,e->op);   /* lhs -> rax. */

	/* Strength-reduce '/' or '%' by a positive power-of-two literal: a 64-bit idiv
	   (tens of cycles) collapses to a shift (and a mask/bias). The divisor is bounded
	   to 2^31 so the '%' mask stays an imm32. Signedness follows the operands, exactly
	   as the idiv path below; a signed numerator is biased toward zero before the
	   arithmetic shift. General (non-power-of-two) constants still use idiv. */
	if ((e->op==TOKEN_SLASH || e->op==TOKEN_PERCENT)
		&& e->rhs->kind==EX_INT
		&& e->rhs->int_val >= 2 && e->rhs->int_val <= 0x80000000LL
		&& (e->rhs->int_val & (e->rhs->int_val - 1)) == 0)
	{
		long long d = e->rhs->int_val;
		int k = 0;
		while ((1LL << k) != d)
		{
			k++;        /* d == 2^k. */
		}

		int uns_div = ty_is_unsigned(e->lhs->type.kind) || ty_is_unsigned(e->rhs->type.kind);
		if (e->op==TOKEN_SLASH && uns_div)
		{
			cg_emit(cg,"    shr rax, %d", k);
		}
		else if (e->op==TOKEN_SLASH && e->anno_nonneg)
		{
			/* Dividend proven >= 0: no sign bias needed, a bare arithmetic shift. */
			cg_emit(cg,"    sar rax, %d", k);
		}
		else if (e->op==TOKEN_SLASH)
		{
			/* Signed divide: bias a negative numerator by (2^k - 1) so the shift
			   truncates toward zero, then arithmetic-shift. */
			cg_emit(cg,"    mov rdx, rax");
			cg_emit(cg,"    sar rdx, 63");
			cg_emit(cg,"    shr rdx, %d", 64 - k);
			cg_emit(cg,"    add rax, rdx");
			cg_emit(cg,"    sar rax, %d", k);
		}
		else if (uns_div)
		{
			cg_emit(cg,"    and rax, %lld", d - 1);
		}
		else
		{
			/* Signed remainder = ((n + bias) & (d-1)) - bias, bias = (d-1) if n<0. */
			cg_emit(cg,"    mov rdx, rax");
			cg_emit(cg,"    sar rdx, 63");
			cg_emit(cg,"    shr rdx, %d", 64 - k);
			cg_emit(cg,"    add rax, rdx");
			cg_emit(cg,"    and rax, %lld", d - 1);
			cg_emit(cg,"    sub rax, rdx");
		}

		cg_extend_int_result(cg, e, want_low32);
		return;
	}

	/* Strength-reduce multiply by a positive power-of-two constant to a left shift
	   (lhs is in rax). Sound for both signednesses: the low bits of `x << k` match
	   `x * 2^k`, and cg_extend_reg then narrows to the declared width exactly as the
	   imul path would. This collapses the ubiquitous index arithmetic (i*4, r*8,
	   (v+u)*2, ...) from `mov rbx,C; imul` to a single `shl`. */
	if (e->op==TOKEN_STAR && !ty_is_float(e->type.kind) && e->rhs->kind==EX_INT
		&& e->rhs->int_val > 1 && (e->rhs->int_val & (e->rhs->int_val - 1)) == 0)
	{
		int k = 0;
		long long v = e->rhs->int_val;
		while (v > 1)
		{
			v >>= 1;
			k++;
		}

		cg_emit(cg,"    shl rax, %d", k);
		cg_extend_int_result(cg, e, want_low32);
		return;
	}

	/* Immediate-form multiply / bitwise by a fits-imm32 constant (lhs in rax): one
	   instruction against the literal instead of `mov rbx, CONST` then the op against
	   rbx. `imul` uses its three-operand immediate form. Re-extension narrows to the
	   declared width exactly as the rbx path would, so the result is identical. */
	if (!ty_is_float(e->type.kind) && e->rhs->kind==EX_INT
		&& e->rhs->int_val >= -2147483648LL && e->rhs->int_val <= 2147483647LL
		&& (e->op==TOKEN_STAR || e->op==TOKEN_AMP || e->op==TOKEN_PIPE || e->op==TOKEN_CARET))
	{
		switch (e->op)
		{
		case TOKEN_STAR:  cg_emit(cg,"    imul rax, rax, %lld", e->rhs->int_val); break;
		case TOKEN_AMP:   cg_emit(cg,"    and rax, %lld", e->rhs->int_val);  break;
		case TOKEN_PIPE:  cg_emit(cg,"    or rax, %lld", e->rhs->int_val);   break;
		case TOKEN_CARET: cg_emit(cg,"    xor rax, %lld", e->rhs->int_val);  break;
		}

		cg_extend_int_result(cg, e, want_low32);
		return;
	}

	/* Immediate-form shift by a constant count (lhs in rax): a single `shl/shr/sar
	   rax, imm` instead of `mov rbx, CONST` then `mov rcx, rbx` then the cl-shift.
	   The hardware masks the count to 6 bits exactly as the cl form would, so a count
	   in [0, 63] is identical to the variable path; out-of-range counts fall through.
	   The shift kind follows the LEFT operand (sar for signed >>, shr for unsigned),
	   matching the cl-form case below. */
	if (!ty_is_float(e->type.kind) && e->rhs->kind==EX_INT
		&& e->rhs->int_val >= 0 && e->rhs->int_val <= 63
		&& (e->op==TOKEN_SHL || e->op==TOKEN_SHR))
	{
		if (e->op==TOKEN_SHL)
		{
			cg_emit(cg,"    shl rax, %lld", e->rhs->int_val);
		}
		else if (ty_is_unsigned(e->lhs->type.kind))
		{
			cg_emit(cg,"    shr rax, %lld", e->rhs->int_val);
		}
		else
		{
			cg_emit(cg,"    sar rax, %lld", e->rhs->int_val);
		}

		cg_extend_int_result(cg, e, want_low32);
		return;
	}

	/* lhs is in rax; set up the rhs operand (immediate or rbx) and signedness. */
	const char *rhsop;
	char immbuf[24];
	int uns;
	cg_binop_rhs(cg, tt, e, &rhsop, immbuf, &uns);
	switch (e->op)
	{
	case TOKEN_PLUS:
		cg_emit(cg,"    add rax, %s", rhsop);
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_MINUS:
		cg_emit(cg,"    sub rax, %s", rhsop);
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_STAR:
		cg_emit(cg,"    imul rax, rbx");   /* Low bits agree with mul at any width. */
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_SLASH:
		/* A 32-bit-result divide uses the 32-bit form (cdq/idiv ebx): roughly half
		   the latency of a 64-bit idiv, and the operands' low 32 bits are the int
		   values. A long result keeps the 64-bit divide. */
		if (ty_bits(e->type.kind) <= 32)
		{
			if (uns)
			{
				cg_emit(cg,"    xor edx, edx");
				cg_emit(cg,"    div ebx");
			}
			else
			{
				cg_emit(cg,"    cdq");
				cg_emit(cg,"    idiv ebx");
			}
		}
		else if (uns)
		{
			cg_emit(cg,"    xor edx, edx");
			cg_emit(cg,"    div rbx");
		}
		else
		{
			cg_emit(cg,"    cqo");
			cg_emit(cg,"    idiv rbx");
		}

		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_PERCENT:
		/* Same divide as '/', but the result is the remainder (rdx/edx), not the
		   quotient. */
		if (ty_bits(e->type.kind) <= 32)
		{
			if (uns)
			{
				cg_emit(cg,"    xor edx, edx");
				cg_emit(cg,"    div ebx");
			}
			else
			{
				cg_emit(cg,"    cdq");
				cg_emit(cg,"    idiv ebx");
			}

			cg_emit(cg,"    mov eax, edx");
		}
		else
		{
			if (uns)
			{
				cg_emit(cg,"    xor edx, edx");
				cg_emit(cg,"    div rbx");
			}
			else
			{
				cg_emit(cg,"    cqo");
				cg_emit(cg,"    idiv rbx");
			}

			cg_emit(cg,"    mov rax, rdx");
		}

		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_SHL:
		/* Shift count must be in cl. lhs in rax, rhs (count) in rbx. */
		cg_emit(cg,"    mov rcx, rbx");
		cg_emit(cg,"    shl rax, cl");
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_SHR:
		/* Right shift kind follows the LEFT operand: arithmetic (sar) for a
		   signed lhs, logical (shr) for an unsigned lhs - the combined `uns`
		   is wrong here, so test the lhs alone. */
		cg_emit(cg,"    mov rcx, rbx");
		if (ty_is_unsigned(e->lhs->type.kind))
		{
			cg_emit(cg,"    shr rax, cl");
		}
		else
		{
			cg_emit(cg,"    sar rax, cl");
		}

		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_AMP:
		cg_emit(cg,"    and rax, rbx");
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_PIPE:
		cg_emit(cg,"    or rax, rbx");
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_CARET:
		cg_emit(cg,"    xor rax, rbx");
		cg_extend_int_result(cg, e, want_low32);
		break;
	case TOKEN_XOR:
		/* Logical xor of two 0/1 bool values; the result is already 0/1. */
		cg_emit(cg,"    xor rax, rbx");
		break;
	case TOKEN_EQ:
	case TOKEN_NEQ:
	case TOKEN_LT:
	case TOKEN_GT:
	case TOKEN_LTE:
	case TOKEN_GTE:
	{
		const char *set;
		switch (e->op)
		{
		case TOKEN_EQ:
			set="sete";
			break;
		case TOKEN_NEQ:
			set="setne";
			break;
		case TOKEN_LT:
			set=uns?"setb":"setl";
			break;
		case TOKEN_GT:
			set=uns?"seta":"setg";
			break;
		case TOKEN_LTE:
			set=uns?"setbe":"setle";
			break;
		default:
			set=uns?"setae":"setge";
			break;
		}

		/* int-vs-int comparisons use the 32-bit cmp: correct for the low 32 bits
		   whether or not the upper half is a valid sign-extension. */
		int c32 = !ty_is_float(e->lhs->type.kind) && !ty_is_float(e->rhs->type.kind)
				  && ty_bits(e->lhs->type.kind) <= 32 && ty_bits(e->rhs->type.kind) <= 32;
		cg_emit(cg,"    cmp %s, %s", c32 ? "eax" : "rax", c32 ? cg_reg32(rhsop) : rhsop);
		cg_emit(cg,"    %s al", set);
		cg_emit(cg,"    movzx rax, al");
		break;
	}
	default:
		fprintf(stderr,"Codegen: bad binary op\n");
		exit(1);
	}
}

/* True for the relational and equality operators. */
static int cg_op_is_compare(int op)
{
	return op==TOKEN_EQ || op==TOKEN_NEQ || op==TOKEN_LT
		   || op==TOKEN_GT || op==TOKEN_LTE || op==TOKEN_GTE;
}

/* The integer literal 0. */
static int cg_is_zero_lit(Expr *e)
{
	return e->kind==EX_INT && e->int_val==0;
}

/* `X % 2^k` with a positive power-of-two literal divisor (k>=1, divisor <= 2^31
   so the mask is an imm32). */
static int cg_is_pow2_mod(Expr *e)
{
	return e->kind==EX_BINARY && e->op==TOKEN_PERCENT
		   && e->rhs->kind==EX_INT && e->rhs->int_val >= 2 && e->rhs->int_val <= 0x80000000LL
		   && (e->rhs->int_val & (e->rhs->int_val - 1)) == 0
		   && !ty_is_float(e->lhs->type.kind);
}

/* Branch to .L<label> when `cond` is true (want=1) or false (want=0). An integer
   relational or equality comparison is lowered to a single cmp + conditional jump,
   skipping the setcc/movzx/cmp-against-zero the value path would emit for it.
   Everything else (float comparisons, bool variables, calls, ...) falls back to
   evaluating the condition to 0/1 and testing that. The want=0 emission is
   byte-identical to the original cg_branch_unless. */
static void cg_branch_cond(Codegen *cg, TypeTable *tt, Expr *cond, int label, int want)
{
	/* Divisibility test: (X % 2^k) == 0 / != 0. Only zero-ness is tested, which is
	   sign-independent, so the signed-remainder reconstruction collapses to a single
	   mask that sets ZF directly - no cmp needed. */
	if (cond->kind==EX_BINARY && (cond->op==TOKEN_EQ || cond->op==TOKEN_NEQ))
	{
		Expr *m = NULL;
		if (cg_is_zero_lit(cond->rhs) && cg_is_pow2_mod(cond->lhs))
		{
			m = cond->lhs;
		}
		else if (cg_is_zero_lit(cond->lhs) && cg_is_pow2_mod(cond->rhs))
		{
			m = cond->rhs;
		}

		if (m)
		{
			cg_expr(cg,tt,m->lhs);                          /* X -> rax. */
			cg_emit(cg,"    and rax, %lld", m->rhs->int_val - 1);   /* Sets ZF; low bits == remainder magnitude. */
			/* (X%2^k)==0 true => ZF set => je; !=0 true => jne. want=0 inverts. */
			int eq = (cond->op==TOKEN_EQ);
			cg_emit(cg,"    %s .L%d", (eq==want) ? "je" : "jne", label);
			return;
		}
	}

	if (cond->kind==EX_BINARY && cg_op_is_compare(cond->op)
		&& !ty_is_float(cond->lhs->type.kind) && !ty_is_float(cond->rhs->type.kind))
	{
		const char *lhsop = "rax";
		if (cond->lhs->kind==EX_IDENT && cg_local_reg(cg, cond->lhs->anno_int))
		{
			lhsop = cg_local_reg(cg, cond->lhs->anno_int);   /* Compare straight from the register. */
		}
		else
		{
			cg_expr(cg,tt,cond->lhs);            /* lhs -> rax. */
		}

		const char *rhsop;
		char immbuf[24];
		int uns;
		cg_binop_rhs(cg, tt, cond, &rhsop, immbuf, &uns);
		int c32 = !ty_is_float(cond->lhs->type.kind) && !ty_is_float(cond->rhs->type.kind)
				  && ty_bits(cond->lhs->type.kind) <= 32 && ty_bits(cond->rhs->type.kind) <= 32;
		cg_emit(cg,"    cmp %s, %s", c32 ? cg_reg32(lhsop) : lhsop, c32 ? cg_reg32(rhsop) : rhsop);

		const char *jcc;                     /* want=0: jump when FALSE; want=1: jump when TRUE. */
		switch (cond->op)
		{
		case TOKEN_EQ:
			jcc = want ? "je" : "jne";
			break;
		case TOKEN_NEQ:
			jcc = want ? "jne" : "je";
			break;
		case TOKEN_LT:
			jcc = want ? (uns ? "jb" : "jl") : (uns ? "jae" : "jge");
			break;
		case TOKEN_GT:
			jcc = want ? (uns ? "ja" : "jg") : (uns ? "jbe" : "jle");
			break;
		case TOKEN_LTE:
			jcc = want ? (uns ? "jbe" : "jle") : (uns ? "ja" : "jg");
			break;
		default: /* TOKEN_GTE */
			jcc = want ? (uns ? "jae" : "jge") : (uns ? "jb" : "jl");
			break;
		}

		cg_emit(cg,"    %s .L%d", jcc, label);
		return;
	}

	cg_expr(cg,tt,cond);
	cg_emit(cg,"    cmp rax, 0");
	cg_emit(cg,"    %s .L%d", want ? "jne" : "je", label);
}

/* Branch to .L<label> when `cond` is false (the loop/if exit test). */
static void cg_branch_unless(Codegen *cg, TypeTable *tt, Expr *cond, int label)
{
	cg_branch_cond(cg, tt, cond, label, 0);
}

/* Branch to .L<label> when `cond` is true (the rotated-loop back-edge). */
static void cg_branch_if(Codegen *cg, TypeTable *tt, Expr *cond, int label)
{
	cg_branch_cond(cg, tt, cond, label, 1);
}

/* Emit a Win64 call. Each positional argument is materialized into rcx/rdx/r8/r9
   (integer/object) or xmm0..3 (float/double) by index — arg i uses register slot
   i regardless of class. Arguments are evaluated left-to-right into a 16-aligned
   stack block (so nested calls compose), then loaded into their registers. The
   integer-or-float routing follows the *parameter* type, so an int passed to a
   double parameter is promoted with cvtsi2sd. */
/* Move `total` arguments, already spilled to scratch slots (arg s at
   [rbp-(b-s*8)] with kind slot_kind[s]), into their ABI homes for a call.
   Registers up to the ABI count take args by class; the rest spill to the
   outgoing stack region. Stack args go FIRST (rax/xmm0 scratch) so they never
   clobber an argument register placed in the second pass. Win64: positions 0-3
   in regs, stack at [rsp+32+k*8] (after the 32-byte shadow); System V: 6 int +
   8 fp regs, stack at [rsp+k*8]. The frame reserves this region (outarg_base). */
static void cg_place_args(Codegen *cg, const TypeKind *slot_kind, int total, int b, int marshal_cstr, int variadic)
{
	int win = (cg->target != TARGET_LINUX);

	int int_idx=0, fp_idx=0, stk=0;
	for (int s=0; s<total; s++)
	{
		int is_float=(slot_kind[s]==TY_FLOAT), is_double=(slot_kind[s]==TY_DOUBLE);
		int on_stack;
		if (win)
		{
			on_stack=(s>=4);
		}
		else if (is_float||is_double)
		{
			on_stack=(fp_idx>=8);
			if (!on_stack) { fp_idx++; }
		}
		else
		{
			on_stack=(int_idx>=6);
			if (!on_stack) { int_idx++; }
		}

		if (!on_stack)
		{
			continue;
		}

		int dst = win ? (32+stk*8) : (stk*8);
		stk++;
		int src=b-s*8;
		if (is_float)
		{
			cg_emit(cg,"    movss xmm0, dword [rbp - %d]",src);
			cg_emit(cg,"    movss dword [rsp + %d], xmm0",dst);
		}
		else if (is_double)
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]",src);
			cg_emit(cg,"    movsd qword [rsp + %d], xmm0",dst);
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbp - %d]",src);
			if (marshal_cstr && (slot_kind[s]==TY_STRING || slot_kind[s]==TY_ARRAY)) { cg_emit(cg,"    add rax, 32"); }
			cg_emit(cg,"    mov [rsp + %d], rax",dst);
		}
	}

	int_idx=0; fp_idx=0;
	for (int s=0; s<total; s++)
	{
		int is_float=(slot_kind[s]==TY_FLOAT), is_double=(slot_kind[s]==TY_DOUBLE);
		int src=b-s*8;
		if (win)
		{
			if (s>=4) { continue; }
			if (is_float) { cg_emit(cg,"    movss xmm%d, dword [rbp - %d]",s,src); }
			else if (is_double)
			{
				cg_emit(cg,"    movsd xmm%d, qword [rbp - %d]",s,src);
				if (variadic) { cg_emit(cg,"    movq %s, xmm%d",cg_iarg(cg,s),s); }   /* Win64 varargs: FP also in the GP register. */
			}
			else
			{
				cg_emit(cg,"    mov %s, [rbp - %d]",cg_iarg(cg,s),src);
				if (marshal_cstr && (slot_kind[s]==TY_STRING || slot_kind[s]==TY_ARRAY)) { cg_emit(cg,"    add %s, 32",cg_iarg(cg,s)); }
			}
		}
		else if (is_float||is_double)
		{
			if (fp_idx>=8) { continue; }
			int xi=fp_idx++;
			if (is_float) { cg_emit(cg,"    movss xmm%d, dword [rbp - %d]",xi,src); }
			else { cg_emit(cg,"    movsd xmm%d, qword [rbp - %d]",xi,src); }
		}
		else
		{
			if (int_idx>=6) { continue; }
			int ii=int_idx++;
			cg_emit(cg,"    mov %s, [rbp - %d]",cg_iarg(cg,ii),src);
			if (marshal_cstr && (slot_kind[s]==TY_STRING || slot_kind[s]==TY_ARRAY)) { cg_emit(cg,"    add %s, 32",cg_iarg(cg,ii)); }
		}
	}

	if (variadic && !win)
	{
		/* SysV variadic ABI: AL = number of vector (xmm) registers used to pass
		   arguments. fp_idx now holds that count (0 for integer-only varargs). */
		cg_emit(cg,"    mov al, %d", fp_idx);
	}
}

static void cg_call_with_args(Codegen *cg, TypeTable *tt, const char *target,
							  Expr *self, Expr **args, int argc, int indirect,
							  int result_is_object, int result_is_fp,
							  const TypeRef *params, int param_count, int marshal_cstr)
{
	int total = (self?1:0) + argc;

	/* Result preservation across owned-temp releases keys off result_is_fp alone
	   (rax holds every non-fp result); result_is_object is kept as caller intent. */
	(void)result_is_object;

	if (indirect)
	{
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Callee address (freed by call time). */
	}

	/* Fast path for a single register argument - the common call shape (a one-arg
	   function like fib(n-1), or a zero-arg method whose only operand is `this`).
	   Pass it straight in the first argument register: no spill block, no
	   store-then-reload, and (with the static-rsp frame) a bare call - rsp is already
	   16-aligned and the callee's shadow space is reserved at the frame bottom.
	   Excluded: float args (xmm routing), owned temporaries (their pointer must
	   outlive the call for the release pass), and cstr marshalling - those keep the
	   general path below. */
	{
		Expr *only = self ? self : (argc==1 ? args[0] : NULL);
		if (total==1 && only && !expr_is_owned(only) && !only->is_func_addr)
		{
			TypeKind pk = self ? TY_OBJECT : ((param_count>0) ? params[0].kind : args[0]->type.kind);
			int is_marshalled = marshal_cstr && !self && (args[0]->type.kind==TY_STRING || args[0]->type.kind==TY_ARRAY);
			if (!ty_is_float(pk) && !is_marshalled)
			{
				cg_expr(cg,tt,only);
				if (!self)
				{
					cg_coerce(cg,pk,args[0]->type.kind);
				}

				cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
				if (indirect)
				{
					cg_emit(cg,"    mov r11, [rbp - %d]", cg->val_save);
					cg_emit(cg,"    call r11");
				}
				else
				{
					cg_emit(cg,"    call %s", target);
				}

				return;
			}
		}
	}

	int block = ((total*8 + 15)/16)*16;   /* 16-aligned scratch for spilled args. */
	int b = block ? cg_scratch_alloc(cg, block) : 0;   /* Arena block; a former [rsp+k] becomes [rbp-(b-k)]. */

	TypeKind slot_kind[total > 0 ? total : 1];   /* C99 VLA: one entry per argument (incl. `this`). */
	int owned_tmp[total > 0 ? total : 1];
	int owned_n = 0;
	int slot = 0;

	if (self)
	{
		cg_expr(cg,tt,self);
		slot_kind[slot] = TY_OBJECT;
		if (expr_is_owned(self))
		{
			owned_tmp[owned_n++] = slot;
		}

		cg_emit(cg,"    mov [rbp - %d], rax", b - slot*8);
		slot++;
	}

	for (int i=0; i<argc; i++)
	{
		if (args[i]->is_func_addr)
		{
			/* FFI: pass a Breezy function's address as a C function pointer. */
			FuncInfo *cf = types_find_func(tt, args[i]->name);
			cg_emit(cg,"    lea rax, [rel %s]", cf->asm_label);
			slot_kind[slot] = TY_LONG;
			cg_emit(cg,"    mov [rbp - %d], rax", b - slot*8);
			slot++;
			continue;
		}

		TypeKind pk = (i < param_count) ? params[i].kind : args[i]->type.kind;
		cg_expr(cg,tt,args[i]);
		cg_coerce(cg,pk,args[i]->type.kind);   /* Implicit int->double at a double parameter. */
		slot_kind[slot] = pk;
		if (ty_is_float(pk))
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - slot*8);
		}
		else
		{
			if (expr_is_owned(args[i]))
			{
				owned_tmp[owned_n++] = slot;
			}

			cg_emit(cg,"    mov [rbp - %d], rax", b - slot*8);
		}

		slot++;
	}

	cg_place_args(cg, slot_kind, total, b, marshal_cstr, cg->call_variadic);

	if (indirect)
	{
		cg_emit(cg,"    mov r11, [rbp - %d]", cg->val_save);
		cg_emit(cg,"    call r11");
	}
	else
	{
		cg_emit(cg,"    call %s", target);
	}

	if (owned_n > 0)
	{
		/* Preserve the call result across the owned-temp releases: any non-fp
		   result (object/int/bool) lives in rax, an fp result in xmm0. Both are
		   caller-saved and clobbered by bzy_release, so spill and restore them. */
		if (result_is_fp)
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		}

		for (int i=0; i<owned_n; i++)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - owned_tmp[i]*8);
			cg_release_rcx(cg);
		}

		if (result_is_fp)
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}

	if (block)
	{
		cg_scratch_free(cg, block);
	}
}

/* Blocking ctx-blob layout: argument i sits at byte offset i*8 from the blob base;
   the return value occupies the slot after the last argument. For <=4 arguments the
   result stays at +32 and the blob is 48 bytes, byte-identical to the historical
   layout, so existing blocking calls compile to unchanged code. */
static int cg_blocking_result_off(int argc)
{
	return (argc <= 4) ? 32 : argc * 8;
}

static int cg_blocking_ctx_size(int argc)
{
	int off = cg_blocking_result_off(argc);
	int sz = ((off + 8 + 15) / 16) * 16;
	return sz < 48 ? 48 : sz;
}

/* Lower a call to an `extern blocking` function: build a ctx blob on the stack,
   marshal the args into it, hand it to the offload pool (the breeze parks while a
   worker runs the C call), then read the result back. Args are stored raw (the
   string/array->data marshalling happens worker-side in the thunk), so the
   owned-temp release frees the original object. Any argument count. */
static void cg_blocking_call(Codegen *cg, TypeTable *tt, Expr *e, FuncInfo *fi)
{
	int argc = e->arg_count;
	int sz = cg_blocking_ctx_size(argc);
	int b = cg_scratch_alloc(cg, sz);              /* ctx blob: argc slots + result, 16-aligned. */
	int owned_tmp[argc > 0 ? argc : 1];            /* C99 VLA, mirrors cg_call_with_args. */
	int owned_n = 0;
	for (int i=0; i<argc; i++)
	{
		TypeKind pk = (i < fi->param_count) ? fi->param_types[i].kind : e->args[i]->type.kind;
		cg_expr(cg,tt,e->args[i]);
		cg_coerce(cg,pk,e->args[i]->type.kind);
		if (ty_is_float(pk))
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - i*8);
		}
		else
		{
			if (expr_is_owned(e->args[i]))
			{
				owned_tmp[owned_n++] = i;
			}

			cg_emit(cg,"    mov [rbp - %d], rax", b - i*8);
		}
	}

	cg_emit(cg,"    lea %s, [rel __blocking_%s]", cg_iarg(cg, 0), fi->asm_label);
	cg_emit(cg,"    lea %s, [rbp - %d]", cg_iarg(cg, 1), b);   /* ctx pointer (base of the blob). */
	cg_aligned_call(cg,"bzy_offload_run");         /* Parks the breeze; worker fills the result slot. */

	for (int i=0; i<owned_n; i++)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - owned_tmp[i]*8);
		cg_release_rcx(cg);
	}

	int roff = cg_blocking_result_off(argc);
	if (fi->ret_type.kind==TY_DOUBLE)
	{
		cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b - roff);
	}
	else if (fi->ret_type.kind==TY_FLOAT)
	{
		cg_emit(cg,"    movss xmm0, dword [rbp - %d]", b - roff);
	}
	else if (fi->ret_type.kind!=TY_VOID)
	{
		cg_emit(cg,"    mov rax, [rbp - %d]", b - roff);
	}

	cg_scratch_free(cg, sz);
}

/* Class-hierarchy analysis: true when a call to `method_name` on static type
   `class_name` is monomorphic -- no strict descendant of that class declares its
   own override, so every possible runtime type resolves the method to the same
   implementation. The whole program is compiled at once, so the hierarchy is
   complete. Returns 0 for unknown classes, static methods, or builtins. */
static int method_is_monomorphic(TypeTable *tt, const char *class_name, const char *method_name, int overload_idx)
{
	ClassInfo *base = types_find_class(tt, class_name);
	if (!base)
	{
		return 0;
	}

	MethodInfo *bm = types_find_method_idx(base, method_name, overload_idx);
	if (!bm || bm->vtable_slot < 0)
	{
		return 0;   /* Not found, or a static method (already called directly). */
	}

	char bsig[160];
	overload_encode_types(bsig, sizeof(bsig), bm->param_types, bm->param_count);

	for (int i = 0; i < tt->class_count; i++)
	{
		ClassInfo *d = tt->classes[i];
		if (d == base)
		{
			continue;
		}

		int is_descendant = 0;
		for (ClassInfo *p = d->parent; p; p = p->parent)
		{
			if (p == base)
			{
				is_descendant = 1;
				break;
			}
		}

		if (!is_descendant)
		{
			continue;
		}

		/* A descendant that declares an override of THIS exact overload (same name
		   and signature) makes the call polymorphic. */
		for (int mi = 0; mi < d->method_count; mi++)
		{
			if (strcmp(d->methods[mi].name, method_name) != 0
					|| strcmp(d->methods[mi].owner_class, d->name) != 0)
			{
				continue;
			}

			char dsig[160];
			overload_encode_types(dsig, sizeof(dsig), d->methods[mi].param_types, d->methods[mi].param_count);
			if (strcmp(dsig, bsig) == 0)
			{
				return 0;
			}
		}
	}

	return 1;
}

static void cg_method_call(Codegen *cg, TypeTable *tt, Expr *e)
{
	ClassInfo *c=types_find_class(tt,e->anno_str);

	/* Devirtualization: a monomorphic call site (concrete receiver type, no
	   override below it) needs no vtable lookup. Emit a direct call to the
	   resolved implementation and skip the two indirection loads. */
	if (c)
	{
		MethodInfo *m=types_find_method_idx(c,e->name,e->anno_overload);
		if (m && m->vtable_slot>=0 && method_is_monomorphic(tt,e->anno_str,e->name,e->anno_overload))
		{
			cg_call_with_args(cg,tt,m->asm_label,e->lhs,e->args,e->arg_count,0,
							  ty_is_managed(e->type.kind), ty_is_float(e->type.kind),
							  m->param_types, m->param_count, 0);
			return;
		}
	}

	cg_expr(cg,tt,e->lhs);                       /* Receiver pointer in rax. */
	cg_emit(cg,"    mov rax, [rax]");             /* Vtable pointer. */
	cg_emit(cg,"    mov rax, [rax + %d]", e->anno_int * 8);   /* Method at its (virtual or interface) slot. */
	const TypeRef *params;
	int pcount;
	if (c)
	{
		MethodInfo *m=types_find_method_idx(c,e->name,e->anno_overload);
		params=m->param_types;
		pcount=m->param_count;
	}
	else
	{
		/* Interface-typed receiver: parameter types come from the interface method. */
		InterfaceInfo *itf=types_find_interface(tt,e->anno_str);
		int k=0;
		for (int i=0; i<itf->method_count; i++)
		{
			if (strcmp(itf->methods[i],e->name)==0)
			{
				k=i;
				break;
			}
		}

		params=itf->param_types[k];
		pcount=itf->param_counts[k];
	}

	cg_call_with_args(cg,tt,NULL,e->lhs,e->args,e->arg_count,1, ty_is_managed(e->type.kind),
					  ty_is_float(e->type.kind), params, pcount, 0);
}

/* StringBuilder methods lower to runtime calls (no vtable dispatch). The receiver
   is borrowed; append releases an owned string argument; toString returns +1. */
static void cg_sb_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	if (strcmp(e->name,"append")==0)
	{
		cg_expr(cg,tt,e->lhs);             /* Sb pointer. */
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);         /* String argument. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_aligned_call(cg,"bzy_sb_append");
		if (expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
		}

		cg_scratch_free(cg, 16);
	}
	else   /* toString */
	{
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg,"bzy_sb_to_string");   /* Owned (+1) string in rax. */
	}
}

static int cg_elem_kind(TypeKind k);   /* Defined below; used by containsValue. */

/* Map methods lower to runtime calls (no vtable dispatch). The receiver is
   borrowed; key/value arguments that are owned temporaries are released after
   the call (the runtime retains its own copies). get returns a +1 managed value
   when V is managed; for a value V / containsKey / containsValue the result is a
   plain integer in rax. */
static void cg_map_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	if (strcmp(e->name,"containsValue")==0)
	{
		TypeKind vk = e->lhs->type.elem2->kind;
		cg_expr(cg,tt,e->lhs);                  /* Map. */
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);              /* Value (rax, or xmm0 if FP). */
		if (ty_is_float(vk))
		{
			cg_emit(cg, vk==TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
		}

		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 2), cg_elem_kind(vk));   /* 3 = string -> content eq. */
		cg_aligned_call(cg,"bzy_map_contains_value");
		if (expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", b);            /* Preserve the bool across release. */
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
		}

		cg_scratch_free(cg, 16);
		return;
	}

	if (strcmp(e->name,"getKeys")==0 || strcmp(e->name,"getValues")==0 || strcmp(e->name,"getEntries")==0)
	{
		cg_expr(cg,tt,e->lhs);                  /* Map. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		if (strcmp(e->name,"getEntries")==0)
		{
			cg_aligned_call(cg,"bzy_map_entries");
		}
		else
		{
			/* Pass the element stride so the runtime packs the output array correctly. */
			int stride = cg_elem_stride(e->type.elem->kind);
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), stride);
			const char *fn = strcmp(e->name,"getKeys")==0 ? "bzy_map_keys" : "bzy_map_values";
			cg_aligned_call(cg,fn);
		}

		return;                                 /* Owned array (+1) in rax. */
	}

	if (strcmp(e->name,"put")==0)
	{
		cg_expr(cg,tt,e->lhs);                 /* Map. */
		int b = cg_scratch_alloc(cg, 32);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);             /* Key. */
		cg_extend_reg(cg, e->lhs->type.elem->kind);   /* Canonicalize the key to 64 bits. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_expr(cg,tt,e->args[1]);             /* Value. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 16);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 2), b - 16);
		cg_aligned_call(cg,"bzy_map_put");
		if (expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
		}

		if (expr_is_owned(e->args[1]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 16);
			cg_release_rcx(cg);
		}

		cg_scratch_free(cg, 32);
		return;
	}

	/* putIfAbsent / getOrDefault: receiver + key + value/default, atomic in the
	   runtime; the result is the map's value type (owned when managed), so the
	   marshaling mirrors put and the result handling mirrors get. FP map values
	   are rejected at resolve, so no xmm bridging arises. */
	if (strcmp(e->name,"putIfAbsent")==0 || strcmp(e->name,"getOrDefault")==0)
	{
		const char *cfn = (e->name[0]=='p') ? "bzy_map_put_if_absent" : "bzy_map_get_or_default";
		cg_expr(cg,tt,e->lhs);                 /* Map. */
		int cb = cg_scratch_alloc(cg, 32);
		cg_emit(cg,"    mov [rbp - %d], rax", cb);
		cg_expr(cg,tt,e->args[0]);             /* Key. */
		cg_extend_reg(cg, e->lhs->type.elem->kind);   /* Canonicalize the key to 64 bits. */
		cg_emit(cg,"    mov [rbp - %d], rax", cb - 8);
		cg_expr(cg,tt,e->args[1]);             /* Value / default. */
		cg_emit(cg,"    mov [rbp - %d], rax", cb - 16);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cb);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), cb - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 2), cb - 16);
		cg_aligned_call(cg,cfn);               /* Result in rax (owned when V is managed). */
		if (expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cb);   /* Preserve the result across the release. */
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cb - 8);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", cb);
		}

		if (expr_is_owned(e->args[1]))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cb);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cb - 16);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", cb);
		}

		cg_scratch_free(cg, 32);
		return;
	}

	/* get / has / remove: receiver + one key argument. */
	const char *fn = strcmp(e->name,"get")==0 ? "bzy_map_get"
					 : strcmp(e->name,"containsKey")==0 ? "bzy_map_has"
					 : "bzy_map_remove";
	cg_expr(cg,tt,e->lhs);                      /* Map. */
	int b = cg_scratch_alloc(cg, 16);
	cg_emit(cg,"    mov [rbp - %d], rax", b);
	cg_expr(cg,tt,e->args[0]);                  /* Key. */
	cg_extend_reg(cg, e->lhs->type.elem->kind); /* Canonicalize the key to 64 bits. */
	cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
	cg_aligned_call(cg,fn);                     /* Result (get/has) in rax. */
	if (expr_is_owned(e->args[0]))
	{
		cg_emit(cg,"    mov [rbp - %d], rax", b);        /* Preserve the result across the key release. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
		cg_release_rcx(cg);
		cg_emit(cg,"    mov rax, [rbp - %d]", b);
	}

	cg_scratch_free(cg, 16);
}

/* Entry.getKey()/getValue() lower to bzy_entry_key/val. The result is a +1
   owned managed value when K/V is managed, else a plain integer in rax; for an
   FP K/V the bits are bridged rax -> xmm0 as the consumer expects. */
static void cg_entry_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);                     /* Entry pointer. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg, strcmp(e->name,"getKey")==0 ? "bzy_entry_key" : "bzy_entry_val");
	if (ty_is_float(e->type.kind))
	{
		cg_emit(cg, e->type.kind==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
	}
}

/* channel.send / channel.recv lower to runtime calls. send moves a managed value
   into the channel (the +1 transfers; no release after). recv returns an owned
   value (moved out); an FP element is bridged rax -> xmm0 for the consumer. */
static void cg_channel_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind et = e->lhs->type.elem->kind;
	if (strcmp(e->name,"send")==0)
	{
		cg_expr(cg,tt,e->lhs);                  /* Channel. */
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		if (ty_is_managed(et))
		{
			cg_expr_owned(cg,tt,e->args[0]);    /* +1 owned; moves into the channel. */
		}
		else
		{
			cg_expr(cg,tt,e->args[0]);
		}

		if (ty_is_float(et))
		{
			cg_emit(cg, et==TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
		}

		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_aligned_call(cg,"bzy_channel_send"); /* Channel takes ownership: no release here. */
		cg_scratch_free(cg, 16);
		return;
	}

	/* recv */
	cg_expr(cg,tt,e->lhs);
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_channel_recv");     /* Owned value bits in rax. */
	if (ty_is_float(et))
	{
		cg_emit(cg, et==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
	}
}

/* Encode an element type as the collection runtime's elem_kind: 0 int/bool,
   1 float, 2 double, 3 string (managed, content eq), 4 object (managed, identity). */
static int cg_elem_kind(TypeKind k)
{
	if (k==TY_FLOAT)
	{
		return 1;
	}

	if (k==TY_DOUBLE)
	{
		return 2;
	}

	if (k==TY_STRING)
	{
		return 3;
	}

	if (ty_is_managed(k))
	{
		return 4;
	}

	return 0;   /* int / bool / integer widths. */
}

/* Box<T> methods, specialized inline per T over the length-1 array slot at
   [box+32]. The box is borrowed; set takes overwrite/ARC semantics; get retains
   a managed value (owned); contains bakes equality per T and releases an owned
   managed argument. */
static void cg_box_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind tk = e->lhs->type.elem->kind;
	int managed = ty_is_managed(tk);
	int fp = ty_is_float(tk);

	if (strcmp(e->name,"get")==0)
	{
		cg_expr(cg,tt,e->lhs);                 /* Box ptr -> rax. */
		if (fp)
		{
			cg_load_fp(cg,tk,"[rax + 32]");
		}
		else if (managed)
		{
			cg_emit(cg,"    mov rax, [rax + 32]");
			cg_retain_rax(cg);                 /* Owned (+1), like map.get. */
		}
		else
		{
			cg_load_scalar(cg,tk,"[rax + 32]");
		}

		return;
	}

	if (strcmp(e->name,"set")==0)
	{
		cg_expr(cg,tt,e->lhs);                 /* Box ptr. */
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		if (fp)
		{
			cg_expr(cg,tt,e->args[0]);         /* Value -> xmm0. */
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
			cg_store_fp(cg,tk,"[rax + 32]");
		}
		else if (managed)
		{
			cg_expr(cg,tt,e->args[0]);         /* Value ptr -> rax. */
			cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_aligned_call(cg,"bzy_retain");  /* Retain the new occupant. */
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
			cg_emit(cg,"    mov rdx, [rax + 32]");   /* Old occupant -> rdx (scratch). */
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_emit(cg,"    mov [rax + 32], %s", cg_iarg(cg, 0));    /* Store new. */
			cg_emit(cg,"    mov %s, rdx", cg_iarg(cg, 0));
			cg_release_rcx(cg);                       /* Release the old. */
			if (expr_is_owned(e->args[0]))
			{
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8); /* Release the owned arg temporary. */
				cg_release_rcx(cg);
			}
		}
		else
		{
			cg_expr(cg,tt,e->args[0]);         /* Value -> rax. */
			cg_emit(cg,"    mov rbx, [rbp - %d]", b);
			cg_store_scalar(cg,tk,"[rbx + 32]");      /* Width-correct store. */
		}

		cg_scratch_free(cg, 16);
		return;
	}

	/* contains: bool in rax. */
	cg_expr(cg,tt,e->lhs);                     /* Box ptr. */
	int b = cg_scratch_alloc(cg, 16);
	cg_emit(cg,"    mov [rbp - %d], rax", b);
	if (fp)
	{
		cg_expr(cg,tt,e->args[0]);             /* Arg -> xmm0. */
		cg_emit(cg,"    mov rax, [rbp - %d]", b);
		cg_emit(cg, tk==TY_FLOAT ? "    movss xmm1, dword [rax + 32]" : "    movsd xmm1, qword [rax + 32]");
		cg_emit(cg, tk==TY_FLOAT ? "    ucomiss xmm0, xmm1" : "    ucomisd xmm0, xmm1");
		cg_emit(cg,"    sete al");
		cg_emit(cg,"    movzx rax, al");
	}
	else if (tk==TY_STRING)
	{
		cg_expr(cg,tt,e->args[0]);             /* Arg ptr -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
		cg_emit(cg,"    mov rax, [rbp - %d]", b);
		cg_emit(cg,"    mov %s, [rax + 32]", cg_iarg(cg, 0));
		cg_aligned_call(cg,"bzy_str_eq");      /* rax = 0/1 */
		if (expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", b);          /* Preserve result across release. */
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
		}
	}
	else   /* Scalar int/bool, or object identity. */
	{
		cg_expr(cg,tt,e->args[0]);             /* Arg -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov rbx, rax");
		cg_emit(cg,"    mov rax, [rbp - %d]", b);
		cg_load_scalar(cg,tk,"[rax + 32]");    /* Width-correct load; sign/zero-extends into rax. */
		cg_emit(cg,"    cmp rax, rbx");
		cg_emit(cg,"    sete al");
		cg_emit(cg,"    movzx rax, al");
		if (managed && expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", b);          /* Preserve result. */
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
		}
	}

	cg_scratch_free(cg, 16);
}

/* Set<T> over a BzyMap (keys only). add/remove/contains lower to map put(k,1)/
   remove/has. The receiver is borrowed; an owned managed key temporary is
   released after the call (bzy_map_put retains its own copy). */
static void cg_set_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind tk = e->lhs->type.elem->kind;
	const char *nm = e->name;
	cg_expr(cg,tt,e->lhs);                  /* Set (map) ptr. */
	int b = cg_scratch_alloc(cg, 16);
	cg_emit(cg,"    mov [rbp - %d], rax", b);
	cg_expr(cg,tt,e->args[0]);              /* Key. */
	cg_extend_reg(cg, tk);                  /* Canonicalize an integer key to 64 bits. */
	cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
	if (strcmp(nm,"add")==0)
	{
		cg_emit(cg,"    mov %s, 1", cg_iarg(cg, 2));        /* Dummy value. */
		cg_aligned_call(cg,"bzy_map_put");
	}
	else if (strcmp(nm,"remove")==0)
	{
		cg_aligned_call(cg,"bzy_map_remove");
	}
	else   /* contains */
	{
		cg_aligned_call(cg,"bzy_map_has");
	}

	if (ty_is_managed(tk) && expr_is_owned(e->args[0]))
	{
		cg_emit(cg,"    mov [rbp - %d], rax", b);        /* Preserve a bool result. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
		cg_release_rcx(cg);
		cg_emit(cg,"    mov rax, [rbp - %d]", b);
	}

	cg_scratch_free(cg, 16);
}

/* map / filter / forEach / reduce over a vector-backed collection: evaluate the
   lambda closure once, then loop the receiver's elements calling the closure per
   element through its code pointer (closure is arg0, the element is the next arg,
   with the accumulator before it for reduce). map/filter build a fresh List;
   reduce folds to a scalar; forEach runs for effect. Value elements load inline;
   managed elements come from bzy_vec_get (owned) and are released after the call. */
/* Repoint a cloned lambda-body expression for inline emission inside the
   enclosing function: every reference to a lambda parameter or capture is moved
   from its body-frame slot to a slot in the enclosing frame -- the per-element
   scratch (element / accumulator) for parameters, the captured local's own home
   for captures (so a promoted capture is read straight from its register). */
static void cg_inline_remap(Expr *e, LambdaInfo *lam, int elem_slot, int acc_slot, int is_reduce)
{
	if (!e)
	{
		return;
	}

	if (e->kind == EX_IDENT)
	{
		int done = 0;
		for (int i = 0; i < lam->cap_count && !done; i++)
		{
			if (strcmp(e->name, lam->caps[i].name) == 0)
			{
				e->anno_int = lam->caps[i].src_offset;
				done = 1;
			}
		}

		if (!done && is_reduce && strcmp(e->name, lam->params[0].name) == 0)
		{
			e->anno_int = acc_slot;
			done = 1;
		}

		if (!done && is_reduce && strcmp(e->name, lam->params[1].name) == 0)
		{
			e->anno_int = elem_slot;
			done = 1;
		}

		if (!done && !is_reduce && strcmp(e->name, lam->params[0].name) == 0)
		{
			e->anno_int = elem_slot;
		}
	}

	cg_inline_remap(e->lhs, lam, elem_slot, acc_slot, is_reduce);
	cg_inline_remap(e->rhs, lam, elem_slot, acc_slot, is_reduce);
	for (int i = 0; i < e->arg_count; i++)
	{
		cg_inline_remap(e->args[i], lam, elem_slot, acc_slot, is_reduce);
	}
}

static void cg_combinator(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *nm = e->name;
	int is_map = strcmp(nm,"map")==0;
	int is_filter = strcmp(nm,"filter")==0;
	int is_reduce = strcmp(nm,"reduce")==0;
	int builds = is_map || is_filter;
	TypeKind et = e->lhs->type.elem->kind;
	int emanaged = ty_is_managed(et);
	int map_direct = is_map && !emanaged;   /* Value map: presized indexed fill, no per-element retain. */
	int lam_idx = is_reduce ? 1 : 0;
	TypeKind ut = e->type.kind;   /* reduce: accumulator type U; otherwise unused. */

	/* Monomorphize when the combinator argument is a literal expression lambda
	   (the common case): inline its body into the loop instead of constructing a
	   closure and calling it indirectly per element. A block-body lambda or a
	   function VALUE (a variable, not a literal) keeps the indirect path. */
	Expr *litlam = (e->args[lam_idx]->kind == EX_LAMBDA) ? e->args[lam_idx] : NULL;
	int do_inline = litlam && !litlam->lam->is_block;

	int B = cg_scratch_alloc(cg, 64);
	int s_coll = B, s_clo = B-8, s_res = B-16, s_idx = B-24, s_len = B-32, s_acc = B-40, s_elem = B-48;
	Expr *ibody = NULL;
	if (do_inline)
	{
		ibody = expr_clone(litlam->lam->body_expr);
		cg_inline_remap(ibody, litlam->lam, s_elem, s_acc, is_reduce);
	}

	cg_expr(cg,tt,e->lhs);                              /* Receiver -> rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", s_coll);
	cg_emit(cg,"    mov rax, [rax + 24]");              /* length@24. */
	cg_emit(cg,"    mov [rbp - %d], rax", s_len);

	if (is_reduce)
	{
		cg_expr(cg,tt,e->args[0]);                      /* Seed -> accumulator. */
		if (ty_is_float(ut))
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", s_acc);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", s_acc);
		}
	}

	if (!do_inline)
	{
		cg_expr(cg,tt,e->args[lam_idx]);                /* Closure (owned +1) -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", s_clo);
	}

	if (builds)
	{
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg,0), cg_elem_kind(et));   /* Result holds element kind. */
		cg_aligned_call(cg,"bzy_vec_new");              /* Fresh result List (owned) -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", s_res);
	}

	if (is_map)
	{
		/* map produces exactly one element per source element: pre-size the result to
		   the source length once so the fill never reallocates. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_res);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,1), s_len);
		cg_aligned_call(cg,"bzy_vec_reserve");
		if (map_direct)
		{
			/* Value element: set the length and fill data[i] directly in the loop --
			   no per-element capacity check or push call (a flat array store). The
			   fresh result has head 0, so phys == index. */
			cg_emit(cg,"    mov rax, [rbp - %d]", s_res);
			cg_emit(cg,"    mov rcx, [rbp - %d]", s_len);
			cg_emit(cg,"    mov [rax + 24], rcx");      /* length = source length. */
		}
	}

	/* Optimized value-map fill. The generic loop below reloads the source/result
	   structs and keeps the index in memory every iteration, because the inlined
	   body may clobber any scratch register. When the body is call-free
	   (cg_hoist_expr_ok), it preserves r8-r11, so the source and result data
	   pointers stay hoisted and the index lives in r10 across the whole loop - the
	   same loop quality a desugared foreach gets. A head==0 fast path drops the
	   ring arithmetic (the plain-List case); the rare ring source keeps it. Only
	   for a value source AND value result (rax-carried), un-nested in an outer
	   hoist (r8-r11 free). */
	if (do_inline && map_direct && cg->hoist_n == 0 && ibody
		&& !ty_is_float(et)
		&& e->type.elem && !ty_is_float(e->type.elem->kind) && !ty_is_managed(e->type.elem->kind)
		&& cg_hoist_expr_ok(ibody))
	{
		int floop = cg_label(cg), rloop = cg_label(cg), fend = cg_label(cg);
		cg_emit(cg,"    mov rdx, [rbp - %d]", s_coll);
		cg_emit(cg,"    mov r8, [rdx + 48]");           /* source data ptr (hoisted). */
		cg_emit(cg,"    mov rcx, [rdx + 40]");           /* head. */
		cg_emit(cg,"    mov rdx, [rbp - %d]", s_res);
		cg_emit(cg,"    mov r9, [rdx + 48]");           /* result data ptr (head 0, hoisted). */
		cg_emit(cg,"    xor r10, r10");                  /* index = 0 (hoisted). */
		cg_emit(cg,"    test rcx, rcx");
		cg_emit(cg,"    jnz .L%d", rloop);

		/* head == 0: phys == index, flat source and result. */
		cg_emit(cg,".L%d:", floop);
		cg_emit(cg,"    cmp r10, [rbp - %d]", s_len);
		cg_emit(cg,"    jge .L%d", fend);
		cg_emit(cg,"    mov rax, [r8 + r10*8 + 32]");
		cg_emit(cg,"    mov [rbp - %d], rax", s_elem);
		cg_expr(cg, tt, ibody);                          /* result -> rax; r8/r9/r10 preserved. */
		cg_emit(cg,"    mov [r9 + r10*8 + 32], rax");
		cg_emit(cg,"    inc r10");
		cg_emit(cg,"    jmp .L%d", floop);

		/* head != 0: ring source phys = (head + i) & (cap - 1); result stays flat. */
		cg_emit(cg,".L%d:", rloop);
		cg_emit(cg,"    cmp r10, [rbp - %d]", s_len);
		cg_emit(cg,"    jge .L%d", fend);
		cg_emit(cg,"    mov rdx, [rbp - %d]", s_coll);
		cg_emit(cg,"    mov rcx, r10");
		cg_emit(cg,"    add rcx, [rdx + 40]");           /* + head. */
		cg_emit(cg,"    mov rax, [rdx + 32]");           /* cap. */
		cg_emit(cg,"    dec rax");
		cg_emit(cg,"    and rcx, rax");                  /* phys. */
		cg_emit(cg,"    mov rax, [r8 + rcx*8 + 32]");
		cg_emit(cg,"    mov [rbp - %d], rax", s_elem);
		cg_expr(cg, tt, ibody);
		cg_emit(cg,"    mov [r9 + r10*8 + 32], rax");
		cg_emit(cg,"    inc r10");
		cg_emit(cg,"    jmp .L%d", rloop);

		cg_emit(cg,".L%d:", fend);
		cg_emit(cg,"    mov rax, [rbp - %d]", s_res);    /* Result list (builds). */
		cg_scratch_free(cg, 64);
		return;
	}

	cg_emit(cg,"    mov qword [rbp - %d], 0", s_idx);

	int top = cg_label(cg), end = cg_label(cg);
	cg_emit(cg,".L%d:", top);
	cg_emit(cg,"    mov rcx, [rbp - %d]", s_idx);
	cg_emit(cg,"    cmp rcx, [rbp - %d]", s_len);
	cg_emit(cg,"    jge .L%d", end);

	if (emanaged)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_coll);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,1), s_idx);
		cg_aligned_call(cg,"bzy_vec_get");              /* Owned (+1) element -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", s_elem);
	}
	else
	{
		/* Inline ring load: phys = (head+i) & (cap-1), with the head==0 (plain List)
		   fast path skipping the ring arithmetic. rcx = index from the bounds check. */
		int phys_done = cg_label(cg);
		cg_emit(cg,"    mov rdx, [rbp - %d]", s_coll);
		cg_emit(cg,"    mov rax, [rdx + 48]");          /* data array ptr. */
		cg_emit(cg,"    mov r8, [rdx + 40]");           /* head. */
		cg_emit(cg,"    test r8, r8");
		cg_emit(cg,"    jz .L%d", phys_done);
		cg_emit(cg,"    add rcx, r8");
		cg_emit(cg,"    mov r8, [rdx + 32]");           /* cap. */
		cg_emit(cg,"    dec r8");
		cg_emit(cg,"    and rcx, r8");
		cg_emit(cg,".L%d:", phys_done);
		cg_emit(cg,"    mov rax, [rax + rcx*8 + 32]");  /* slot value. */
		cg_emit(cg,"    mov [rbp - %d], rax", s_elem);
	}

	if (do_inline)
	{
		/* Inlined lambda body: reads the element (and accumulator) from their
		   scratch slots and captures from their enclosing homes; result in
		   rax / xmm0, exactly where the indirect call would leave it. */
		cg_expr(cg,tt,ibody);
	}
	else
	{
		/* Call the closure: arg0 = closure (env), then the element (preceded by the
		   accumulator for reduce). cg_place_args marshals int/float into the right
		   registers from the staged block. */
		int ab = cg_scratch_alloc(cg, 32);
		int total = is_reduce ? 3 : 2;
		TypeKind slot_kind[3];
		slot_kind[0] = TY_OBJECT;
		cg_emit(cg,"    mov rax, [rbp - %d]", s_clo);
		cg_emit(cg,"    mov [rbp - %d], rax", ab);
		if (is_reduce)
		{
			slot_kind[1] = ut;
			slot_kind[2] = et;
			cg_emit(cg,"    mov rax, [rbp - %d]", s_acc);
			cg_emit(cg,"    mov [rbp - %d], rax", ab-8);
			cg_emit(cg,"    mov rax, [rbp - %d]", s_elem);
			cg_emit(cg,"    mov [rbp - %d], rax", ab-16);
		}
		else
		{
			slot_kind[1] = et;
			cg_emit(cg,"    mov rax, [rbp - %d]", s_elem);
			cg_emit(cg,"    mov [rbp - %d], rax", ab-8);
		}

		cg_place_args(cg, slot_kind, total, ab, 0, 0);
		cg_emit(cg,"    mov rax, [rbp - %d]", ab);
		cg_emit(cg,"    mov rax, [rax + 24]");          /* Code pointer. */
		cg_emit(cg,"    call rax");                     /* Result -> rax / xmm0. */
		cg_scratch_free(cg, 32);
	}

	if (map_direct)
	{
		if (ty_is_float(et))
		{
			cg_emit(cg,"    movq rax, xmm0");
		}

		/* Direct indexed store into the pre-sized result (head 0, phys == index):
		   data[index] = value, no capacity check, no length bump. */
		cg_emit(cg,"    mov rcx, [rbp - %d]", s_res);
		cg_emit(cg,"    mov rcx, [rcx + 48]");             /* data array ptr. */
		cg_emit(cg,"    mov r8, [rbp - %d]", s_idx);
		cg_emit(cg,"    mov [rcx + r8*8 + 32], rax");      /* data[index] = value. */
	}
	else if (is_map)
	{
		/* Managed-element map: bzy_vec_push_back retains the value into the result
		   (the capacity is pre-reserved, so this never grows). An owned body result
		   is released after the retain so its temporary +1 does not leak. */
		int result_owned = do_inline ? expr_is_owned(litlam->lam->body_expr) : 1;
		if (result_owned)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", s_acc);   /* Preserve for release. */
		}

		cg_emit(cg,"    mov %s, rax", cg_iarg(cg,1));
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_res);
		cg_aligned_call(cg,"bzy_vec_push_back");
		if (result_owned)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_acc);
			cg_release_rcx(cg);
		}
	}
	else if (is_filter)
	{
		int skip = cg_label(cg);
		cg_emit(cg,"    test al, al");
		cg_emit(cg,"    jz .L%d", skip);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,1), s_elem);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_res);
		cg_aligned_call(cg,"bzy_vec_push_back");
		cg_emit(cg,".L%d:", skip);
	}
	else if (is_reduce)
	{
		if (ty_is_float(ut))
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", s_acc);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", s_acc);
		}
	}

	if (emanaged)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_elem);
		cg_release_rcx(cg);
	}

	cg_emit(cg,"    inc qword [rbp - %d]", s_idx);
	cg_emit(cg,"    jmp .L%d", top);
	cg_emit(cg,".L%d:", end);

	if (!do_inline)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg,0), s_clo);   /* Release the closure. */
		cg_release_rcx(cg);
	}

	if (builds)
	{
		cg_emit(cg,"    mov rax, [rbp - %d]", s_res);
	}
	else if (is_reduce)
	{
		if (ty_is_float(ut))
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", s_acc);
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", s_acc);
		}
	}

	cg_scratch_free(cg, 64);
}

/* List / Stack / Queue / Deque / ArrayDeque methods over the vector runtime.
   The receiver is borrowed; an owned managed argument is released after the call
   (the runtime retains its own copy); fp element values are reinterpreted between
   rax/eax and xmm0 around the call. get/peek return owned; pop/dequeue/remove*
   transfer the element out. */
static void cg_collection_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind tk = e->lhs->type.elem->kind;
	int fp = ty_is_float(tk);
	const char *nm = e->name;

	if (strcmp(nm,"map")==0 || strcmp(nm,"filter")==0
			|| strcmp(nm,"forEach")==0 || strcmp(nm,"reduce")==0)
	{
		cg_combinator(cg,tt,e);
		return;
	}

	if (strcmp(nm,"reserve")==0)
	{
		cg_expr(cg,tt,e->lhs);                       /* Receiver -> rax. */
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);                   /* Capacity -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_aligned_call(cg,"bzy_vec_reserve");
		cg_scratch_free(cg, 16);
		return;
	}

	const char *fn = NULL;
	if (strcmp(nm,"add")==0 || strcmp(nm,"push")==0 || strcmp(nm,"enqueue")==0 || strcmp(nm,"addLast")==0)
	{
		fn="bzy_vec_push_back";
	}
	else if (strcmp(nm,"addFirst")==0)
	{
		fn="bzy_vec_push_front";
	}
	else if (strcmp(nm,"pop")==0 || strcmp(nm,"removeLast")==0)
	{
		fn="bzy_vec_pop_back";
	}
	else if (strcmp(nm,"dequeue")==0 || strcmp(nm,"removeFirst")==0)
	{
		fn="bzy_vec_pop_front";
	}
	else if (strcmp(nm,"peekLast")==0)
	{
		fn="bzy_vec_peek_back";
	}
	else if (strcmp(nm,"peekFirst")==0)
	{
		fn="bzy_vec_peek_front";
	}
	else if (strcmp(nm,"peek")==0)
	{
		fn = strcmp(e->lhs->type.class_name,"Queue")==0 ? "bzy_vec_peek_front" : "bzy_vec_peek_back";   /* Stack: top (back); Queue: front. */
	}
	else if (strcmp(nm,"get")==0)
	{
		fn="bzy_vec_get";
	}
	else if (strcmp(nm,"set")==0)
	{
		fn="bzy_vec_set";
	}
	else if (strcmp(nm,"removeAt")==0)
	{
		fn="bzy_vec_remove_at";
	}
	else if (strcmp(nm,"indexOf")==0)
	{
		fn="bzy_vec_index_of";
	}
	else
	{
		fn="bzy_vec_contains";
	}

	/* Zero-argument, returns T: pop / peek / dequeue / removeFirst|Last / peekFirst|Last. */
	int zero_ret = strcmp(nm,"pop")==0 || strcmp(nm,"peek")==0 || strcmp(nm,"dequeue")==0
				   || strcmp(nm,"removeFirst")==0 || strcmp(nm,"removeLast")==0
				   || strcmp(nm,"peekFirst")==0 || strcmp(nm,"peekLast")==0;
	if (zero_ret)
	{
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg,fn);                     /* Result int64 in rax. */
		if (fp)
		{
			cg_emit(cg, tk==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
		}

		return;
	}

	/* get(index) -> T, one integer arg. */
	if (strcmp(nm,"get")==0)
	{
		cg_expr(cg,tt,e->lhs);
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);                       /* Index -> rax. */
		if (ty_is_managed(tk))
		{
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
			cg_aligned_call(cg,"bzy_vec_get");           /* Element (owned -> retained) in rax. */
		}
		else
		{
			/* Value element: inline the bounds check + ring load, skipping the
			   bzy_vec_get call and the value-case no-op retain. Out-of-range
			   still calls bzy_oob_abort(index, length) -- behaviour-identical to
			   bzy_vec_get (a hard abort, not the catchable array-subscript
			   throw). Vector layout: length@24, cap@32, head@40, data array@48
			   (its 8-byte slots at +32); phys = (head+i) & (cap-1). A receiver
			   whose static type may cross cores first tests its SHARED bit and
			   routes to the (shared-aware) runtime call; a confined receiver
			   falls through to the unchanged inline load. */
			int ok = cg_label(cg);
			int gated = types_typeref_maybe_shared(tt,&e->lhs->type);
			int sh = 0, join = 0;
			cg_emit(cg,"    mov rcx, rax");               /* index */
			cg_emit(cg,"    mov rdx, [rbp - %d]", b);     /* receiver */
			if (gated)
			{
				sh = cg_label(cg);
				join = cg_label(cg);
				cg_emit(cg,"    test qword [rdx + 16], 8");   /* BZY_GCINFO_SHARED. */
				cg_emit(cg,"    jnz .L%d", sh);
			}

			cg_emit(cg,"    mov r8, [rdx + 24]");         /* length */
			cg_emit(cg,"    cmp rcx, r8");
			cg_emit(cg,"    jb .L%d", ok);                /* unsigned: catches <0 and >=length */
			cg_emit(cg,"    mov %s, rcx", cg_iarg(cg, 0));/* OOB: bzy_oob_abort(index, length) */
			cg_emit(cg,"    mov %s, r8", cg_iarg(cg, 1));
			cg_aligned_call(cg,"bzy_oob_abort");         /* Never returns. */
			cg_emit(cg,".L%d:", ok);
			cg_emit(cg,"    mov rax, [rdx + 48]");        /* data array ptr */
			cg_emit(cg,"    mov r8, [rdx + 40]");         /* head */
			cg_emit(cg,"    add rcx, r8");                /* head + index */
			cg_emit(cg,"    mov r8, [rdx + 32]");         /* cap */
			cg_emit(cg,"    dec r8");                     /* cap - 1 */
			cg_emit(cg,"    and rcx, r8");                /* phys = (head+i) & (cap-1) */
			cg_emit(cg,"    mov rax, [rax + rcx*8 + 32]");/* slot value -> rax */
			if (gated)
			{
				cg_emit(cg,"    jmp .L%d", join);
				cg_emit(cg,".L%d:", sh);
				cg_emit(cg,"    mov %s, rcx", cg_iarg(cg, 1));        /* Index (before arg0 clobbers rcx on Win64). */
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
				cg_aligned_call(cg,"bzy_vec_get");        /* Shared receiver: locked in the runtime. */
				cg_emit(cg,".L%d:", join);
			}
		}
		if (fp)
		{
			cg_emit(cg, tk==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
		}

		cg_scratch_free(cg, 16);
		return;
	}

	/* removeAt(index) / indexOf(value) / contains(value): receiver + one arg. */
	if (strcmp(nm,"removeAt")==0 || strcmp(nm,"indexOf")==0 || strcmp(nm,"contains")==0)
	{
		cg_expr(cg,tt,e->lhs);
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		if (fp)
		{
			cg_expr(cg,tt,e->args[0]);
			cg_emit(cg, tk==TY_FLOAT ? "    movd edx, xmm0" : "    movq rdx, xmm0");
		}
		else
		{
			cg_expr(cg,tt,e->args[0]);
			cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
		}

		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_aligned_call(cg,fn);
		if (!fp && ty_is_managed(tk) && expr_is_owned(e->args[0]))   /* indexOf/contains arg temp. */
		{
			cg_emit(cg,"    mov [rbp - %d], rax", b);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
		}

		cg_scratch_free(cg, 16);
		return;
	}

	/* set(index, value): receiver + index + value, void. */
	if (strcmp(nm,"set")==0)
	{
		cg_expr(cg,tt,e->lhs);
		int b = cg_scratch_alloc(cg, 32);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);                  /* Index. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		if (fp)
		{
			cg_expr(cg,tt,e->args[1]);
			cg_emit(cg, tk==TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
		}
		else
		{
			cg_expr(cg,tt,e->args[1]);
		}

		cg_emit(cg,"    mov [rbp - %d], rax", b - 16);
		if (ty_is_managed(tk))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 2), b - 16);
			cg_aligned_call(cg,"bzy_vec_set");       /* Retains new, releases old occupant. */
			if (expr_is_owned(e->args[1]))
			{
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 16);
				cg_release_rcx(cg);
			}
		}
		else
		{
			/* Value element: inline the bounds check + ring store, skipping the
			   bzy_vec_set call (value sets have no retain/release). Out-of-range
			   still calls bzy_oob_abort(index, length) -- behaviour-identical to
			   bzy_vec_set. No call on the in-range path, so r8/r9 scratch is free.
			   A maybe-shared receiver tests its SHARED bit first and routes to the
			   (shared-aware) runtime call. */
			int ok = cg_label(cg);
			int gated = types_typeref_maybe_shared(tt,&e->lhs->type);
			int sh = 0, join = 0;
			cg_emit(cg,"    mov rdx, [rbp - %d]", b);      /* receiver */
			cg_emit(cg,"    mov rcx, [rbp - %d]", b - 8);  /* index */
			if (gated)
			{
				sh = cg_label(cg);
				join = cg_label(cg);
				cg_emit(cg,"    test qword [rdx + 16], 8");    /* BZY_GCINFO_SHARED. */
				cg_emit(cg,"    jnz .L%d", sh);
			}

			cg_emit(cg,"    mov r8, [rdx + 24]");          /* length */
			cg_emit(cg,"    cmp rcx, r8");
			cg_emit(cg,"    jb .L%d", ok);                 /* unsigned: catches <0 and >=length */
			cg_emit(cg,"    mov %s, rcx", cg_iarg(cg, 0)); /* OOB: bzy_oob_abort(index, length) */
			cg_emit(cg,"    mov %s, r8", cg_iarg(cg, 1));
			cg_aligned_call(cg,"bzy_oob_abort");          /* Never returns. */
			cg_emit(cg,".L%d:", ok);
			cg_emit(cg,"    mov rax, [rdx + 48]");         /* data array ptr */
			cg_emit(cg,"    mov r8, [rdx + 40]");          /* head */
			cg_emit(cg,"    add rcx, r8");                 /* head + index */
			cg_emit(cg,"    mov r8, [rdx + 32]");          /* cap */
			cg_emit(cg,"    dec r8");                      /* cap - 1 */
			cg_emit(cg,"    and rcx, r8");                 /* phys = (head+i) & (cap-1) */
			cg_emit(cg,"    mov r9, [rbp - %d]", b - 16);  /* value bits */
			cg_emit(cg,"    mov [rax + rcx*8 + 32], r9");  /* store slot */
			if (gated)
			{
				cg_emit(cg,"    jmp .L%d", join);
				cg_emit(cg,".L%d:", sh);
				cg_emit(cg,"    mov %s, rcx", cg_iarg(cg, 1));         /* Index (before arg0 clobbers rcx on Win64). */
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 2), b - 16);   /* Value bits. */
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);        /* Receiver. */
				cg_aligned_call(cg,"bzy_vec_set");         /* Shared receiver: locked in the runtime. */
				cg_emit(cg,".L%d:", join);
			}
		}

		cg_scratch_free(cg, 32);
		return;
	}

	/* push-shape: add / push / enqueue / addFirst / addLast — one value arg, void. */
	cg_expr(cg,tt,e->lhs);
	int b = cg_scratch_alloc(cg, 16);
	cg_emit(cg,"    mov [rbp - %d], rax", b);
	if (fp)
	{
		cg_expr(cg,tt,e->args[0]);
		cg_emit(cg, tk==TY_FLOAT ? "    movd edx, xmm0" : "    movq rdx, xmm0");
	}
	else
	{
		cg_expr(cg,tt,e->args[0]);
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
	}

	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);

	/* Inline the back-insertion fast path for value (non-managed) elements: when
	   len < cap, store at phys = (head+len) & (cap-1) and bump len with no call;
	   only the grow case falls through to bzy_vec_push_back (which re-checks, then
	   grows + stores). Managed elements (need a retain), front insertion
	   (addFirst -> push_front), and fp values keep the call. The fast path
	   clobbers only rax/r8/r9 -- never arg0/arg1 (rcx/rdx on Win64, rdi/rsi on
	   SysV) -- so the receiver/value stay live for the slow path's call. */
	if (strcmp(fn,"bzy_vec_push_back")==0 && !fp && !ty_is_managed(tk))
	{
		int slow = cg_label(cg), done = cg_label(cg);
		const char *recv = cg_iarg(cg, 0), *val = cg_iarg(cg, 1);
		if (types_typeref_maybe_shared(tt,&e->lhs->type))
		{
			/* A receiver that crossed cores must take the (shared-aware) runtime
			   call - the existing grow fallback is exactly that call. */
			cg_emit(cg,"    test qword [%s + 16], 8", recv);   /* BZY_GCINFO_SHARED. */
			cg_emit(cg,"    jnz .L%d", slow);
		}

		cg_emit(cg,"    mov rax, [%s + 24]", recv);        /* length */
		cg_emit(cg,"    cmp rax, [%s + 32]", recv);        /* vs cap */
		cg_emit(cg,"    jge .L%d", slow);                  /* full -> grow via call */
		cg_emit(cg,"    mov r8, [%s + 40]", recv);         /* head */
		cg_emit(cg,"    add r8, rax");                     /* head + length */
		cg_emit(cg,"    mov r9, [%s + 32]", recv);         /* cap */
		cg_emit(cg,"    dec r9");                          /* cap - 1 */
		cg_emit(cg,"    and r8, r9");                      /* phys = (head+len) & (cap-1) */
		cg_emit(cg,"    mov r9, [%s + 48]", recv);         /* data array ptr */
		cg_emit(cg,"    mov [r9 + r8*8 + 32], %s", val);   /* store value into slot */
		cg_emit(cg,"    inc qword [%s + 24]", recv);       /* length++ */
		cg_emit(cg,"    jmp .L%d", done);
		cg_emit(cg,".L%d:", slow);
		cg_aligned_call(cg,fn);
		cg_emit(cg,".L%d:", done);
		cg_scratch_free(cg, 16);
		return;
	}

	cg_aligned_call(cg,fn);
	if (!fp && ty_is_managed(tk) && expr_is_owned(e->args[0]))
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
		cg_release_rcx(cg);
	}

	cg_scratch_free(cg, 16);
}

/* PriorityQueue<T> (binary min-heap, runtime/pqueue.c). add takes one value
   (the runtime retains; an owned temporary is released after); poll/peek return
   an owned T (FP results come back as bits in rax -> moved to xmm0); size/isEmpty
   return an int. The element value is passed in a GP register even for FP keys
   (the runtime stores the bit pattern and the comparator reinterprets it). */
static void cg_pqueue_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind tk = e->lhs->type.elem->kind;
	int fp = ty_is_float(tk);
	const char *nm = e->name;

	if (strcmp(nm,"add")==0)
	{
		cg_expr(cg,tt,e->lhs);
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);          /* Receiver. */
		cg_expr(cg,tt,e->args[0]);                         /* Value -> rax (xmm0 if fp). */
		if (fp)
		{
			cg_emit(cg,"    movq rax, xmm0");              /* Bits -> GP (low 32 valid for float). */
		}

		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);      /* Owned-temp slot. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_aligned_call(cg,"bzy_pq_add");
		if (!fp && ty_is_managed(tk) && expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
		}

		cg_scratch_free(cg, 16);
		return;
	}

	if (strcmp(nm,"poll")==0 || strcmp(nm,"peek")==0)
	{
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg, strcmp(nm,"poll")==0 ? "bzy_pq_poll" : "bzy_pq_peek");
		if (fp)
		{
			cg_emit(cg, tk==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
		}

		return;
	}

	/* size() / isEmpty(). */
	cg_expr(cg,tt,e->lhs);
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_pq_size");
	if (strcmp(nm,"isEmpty")==0)
	{
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    sete al");
		cg_emit(cg,"    movzx rax, al");
	}
}

/* TreeMap<K,V> / TreeSet<T> (B-tree, runtime/btree.c). Keys and values pass as
   int64 bit patterns in GP registers; FP keys/values are bridged xmm<->GP, and
   object keys order through the compareTo vtable slot baked in at construction.
   TreeSet is a value-less TreeMap: add(k) lowers to put(k,0); its first/last/
   floor/ceiling share the firstKey/lastKey/floorKey/ceilingKey runtime entries. */
static void cg_btree_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	int is_set = strcmp(e->lhs->type.class_name,"TreeSet")==0;
	TypeKind kk = e->lhs->type.elem->kind;     /* Key type. */
	const char *nm = e->name;

	/* size(). */
	if (strcmp(nm,"size")==0)
	{
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg,"bzy_btree_size");
		return;
	}

	/* getKeys / getValues / getEntries -> owned array (+1). keys/values pack at the
	   result element stride (entries are always 8-byte Entry pointers). */
	if (strcmp(nm,"getKeys")==0 || strcmp(nm,"getValues")==0 || strcmp(nm,"getEntries")==0)
	{
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		if (strcmp(nm,"getEntries")==0)
		{
			cg_aligned_call(cg,"bzy_btree_entries");
		}
		else
		{
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), cg_elem_stride(e->type.elem->kind));
			cg_aligned_call(cg, strcmp(nm,"getKeys")==0 ? "bzy_btree_keys" : "bzy_btree_values");
		}

		return;
	}

	/* first/last (TreeSet) and firstKey/lastKey (TreeMap): no argument, return key T. */
	if (strcmp(nm,"first")==0 || strcmp(nm,"last")==0
			|| strcmp(nm,"firstKey")==0 || strcmp(nm,"lastKey")==0)
	{
		int last = (nm[0]=='l');               /* "last" / "lastKey" both start 'l'. */
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg, last ? "bzy_btree_last" : "bzy_btree_first");
		if (ty_is_float(kk))
		{
			cg_emit(cg, kk==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
		}

		return;
	}

	/* put(k,v) (TreeMap) / add(k) (TreeSet -> put(k,0)). */
	if (strcmp(nm,"put")==0 || strcmp(nm,"add")==0)
	{
		cg_expr(cg,tt,e->lhs);
		int b = cg_scratch_alloc(cg, 32);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[0]);             /* Key. */
		if (ty_is_float(kk))
		{
			cg_emit(cg, kk==TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
		}
		else
		{
			cg_extend_reg(cg, kk);             /* Canonicalize an int key to 64 bits. */
		}

		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		TypeKind vk = TY_VOID;
		if (is_set)
		{
			cg_emit(cg,"    mov qword [rbp - %d], 0", b - 16);   /* No value column. */
		}
		else
		{
			vk = e->lhs->type.elem2->kind;
			cg_expr(cg,tt,e->args[1]);         /* Value. */
			if (ty_is_float(vk))
			{
				cg_emit(cg, vk==TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
			}

			cg_emit(cg,"    mov [rbp - %d], rax", b - 16);
		}

		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 2), b - 16);
		cg_aligned_call(cg,"bzy_btree_put");   /* Runtime retains managed key/value. */
		if (ty_is_managed(kk) && expr_is_owned(e->args[0]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
		}

		if (!is_set && ty_is_managed(vk) && expr_is_owned(e->args[1]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 16);
			cg_release_rcx(cg);
		}

		cg_scratch_free(cg, 32);
		return;
	}

	/* Single-key-arg group: get / containsKey / remove / floor(Key) / ceiling(Key).
	   get returns V; floor/ceiling return the matched key T; containsKey a bool;
	   remove is void. bridge_fp names an FP result needing a GP->xmm move. */
	const char *fn;
	TypeKind bridge_fp = TY_VOID;
	if (strcmp(nm,"get")==0)
	{
		fn = "bzy_btree_get";
		TypeKind vk = e->lhs->type.elem2->kind;
		if (ty_is_float(vk))
		{
			bridge_fp = vk;
		}
	}
	else if (strcmp(nm,"containsKey")==0)
	{
		fn = "bzy_btree_has";
	}
	else if (strcmp(nm,"remove")==0)
	{
		fn = "bzy_btree_remove";
	}
	else if (strcmp(nm,"floor")==0 || strcmp(nm,"floorKey")==0)
	{
		fn = "bzy_btree_floor";
		if (ty_is_float(kk))
		{
			bridge_fp = kk;
		}
	}
	else   /* ceiling / ceilingKey. */
	{
		fn = "bzy_btree_ceiling";
		if (ty_is_float(kk))
		{
			bridge_fp = kk;
		}
	}

	cg_expr(cg,tt,e->lhs);
	int b = cg_scratch_alloc(cg, 16);
	cg_emit(cg,"    mov [rbp - %d], rax", b);
	cg_expr(cg,tt,e->args[0]);                  /* Key. */
	if (ty_is_float(kk))
	{
		cg_emit(cg, kk==TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
	}
	else
	{
		cg_extend_reg(cg, kk);
	}

	cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
	cg_aligned_call(cg,fn);                     /* Result (get/has/floor/ceiling) in rax. */
	if (ty_is_managed(kk) && expr_is_owned(e->args[0]))
	{
		cg_emit(cg,"    mov [rbp - %d], rax", b);        /* Preserve the result across the key release. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
		cg_release_rcx(cg);
		cg_emit(cg,"    mov rax, [rbp - %d]", b);
	}

	if (bridge_fp != TY_VOID)
	{
		cg_emit(cg, bridge_fp==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
	}

	cg_scratch_free(cg, 16);
}

/* Run a constructor on a freshly-built object: the object is in rax on entry and
   becomes arg slot 0 (this, borrowed), the user args follow. Spills this first so
   arg evaluation can't clobber it (mirrors cg_call_with_args' self handling). The
   object is left in rax as the result of `new`. */
static void cg_ctor_call(Codegen *cg, TypeTable *tt, const char *label,
						 Expr **args, int argc, const TypeRef *params, int param_count)
{
	int total = 1 + argc;

	int block = ((total*8 + 15)/16)*16;
	int b = cg_scratch_alloc(cg, block);
	cg_emit(cg,"    mov [rbp - %d], rax", b);     /* this (borrowed). */

	TypeKind slot_kind[total];   /* C99 VLA: `this` + ctor args. */
	int owned_tmp[total];
	int owned_n = 0;
	slot_kind[0] = TY_OBJECT;
	int slot = 1;
	for (int i=0; i<argc; i++)
	{
		TypeKind pk = (i < param_count) ? params[i].kind : args[i]->type.kind;
		cg_expr(cg,tt,args[i]);
		cg_coerce(cg,pk,args[i]->type.kind);
		slot_kind[slot] = pk;
		if (ty_is_float(pk))
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - slot*8);
		}
		else
		{
			if (expr_is_owned(args[i]))
			{
				owned_tmp[owned_n++] = slot;
			}

			cg_emit(cg,"    mov [rbp - %d], rax", b - slot*8);
		}

		slot++;
	}

	cg_place_args(cg, slot_kind, total, b, 0, 0);

	cg_emit(cg,"    call %s", label);

	for (int i=0; i<owned_n; i++)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - owned_tmp[i]*8);
		cg_release_rcx(cg);
	}

	cg_emit(cg,"    mov rax, [rbp - %d]", b);     /* The object is the result of `new`. */
	cg_scratch_free(cg, block);
}

/* Indirect call of a function value (closure): the closure object is arg0 (its
   environment), and the code pointer at [closure+24] is the call target. e->anno_int
   is the stack slot of the closure local; the user args follow in arg1.. Mirrors
   cg_ctor_call's spill-and-place, then calls through the loaded code pointer. */
static void cg_closure_call(Codegen *cg, TypeTable *tt, Expr *e)
{
	int argc = e->arg_count;
	int total = 1 + argc;
	int block = ((total*8 + 15)/16)*16;
	if (block < 16)
	{
		block = 16;
	}

	int b = cg_scratch_alloc(cg, block);
	cg_emit(cg,"    mov rax, [rbp - %d]", e->anno_int);   /* Closure pointer (env / arg0). */
	cg_emit(cg,"    mov [rbp - %d], rax", b);

	TypeKind slot_kind[total];   /* C99 VLA: closure + user args. */
	int owned_tmp[total];
	int owned_n = 0;
	slot_kind[0] = TY_OBJECT;
	int sl = 1;
	for (int i=0; i<argc; i++)
	{
		TypeKind pk = e->args[i]->type.kind;
		cg_expr(cg,tt,e->args[i]);
		slot_kind[sl] = pk;
		if (ty_is_float(pk))
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - sl*8);
		}
		else
		{
			if (expr_is_owned(e->args[i]))
			{
				owned_tmp[owned_n++] = sl;
			}

			cg_emit(cg,"    mov [rbp - %d], rax", b - sl*8);
		}

		sl++;
	}

	cg_place_args(cg, slot_kind, total, b, 0, 0);
	cg_emit(cg,"    mov rax, [rbp - %d]", b);   /* Reload closure for the indirect target. */
	cg_emit(cg,"    mov rax, [rax + 24]");      /* Code pointer. */
	cg_emit(cg,"    call rax");

	if (owned_n > 0)
	{
		int fp = ty_is_float(e->type.kind);
		if (fp)
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);   /* Preserve result across releases. */
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", b);
		}

		for (int i=0; i<owned_n; i++)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - owned_tmp[i]*8);
			cg_release_rcx(cg);
		}

		if (fp)
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
		}
	}

	cg_scratch_free(cg, block);
}

/* Smallest pool class (1..10) whose block holds object_size, or 0 if unpooled
   (> 256). Mirrors runtime/alloc.c g_class_size; the inline allocator uses it to
   pick the free-list index and block size at compile time. */
static int cg_pool_class(int object_size, int *block_size)
{
	static const int sz[11] = { 0, 32, 48, 64, 80, 96, 128, 160, 192, 224, 256 };
	if (object_size > 256)
	{
		return 0;
	}

	for (int pc = 1; pc <= 10; pc++)
	{
		if (object_size <= sz[pc])
		{
			*block_size = sz[pc];
			return pc;
		}
	}

	return 0;
}

/* Emit an allocation leaving an owned (+1) object of object_size bytes in rax with
   refcount 1 and zeroed fields - the contract of bzy_alloc, which the caller then
   finishes by storing the vtable. For a poolable size, inline bzy_alloc's hot path:
   reach the current thread's PoolTLS and pop the size-class free list, with the size
   class resolved at compile time. The two targets differ only in how the pool is
   reached - Windows reads a pointer from its TEB slot (gs:[0x1480 + slot*8]); Linux
   addresses the struct directly in TLS at fs:[bzy_tpool_off + ...] - so the pop and
   header init are shared via `base`. The path falls back to a bzy_alloc call when
   its readiness gate fails or the free list is empty. The baked-in PoolTLS offsets
   (head @0, n @88, live @136) and the TEB slot base (0x1480) are pinned by
   _Static_asserts in runtime/alloc.c. rax, rcx and rdx are clobbered. */
static void cg_emit_alloc(Codegen *cg, int object_size)
{
	int block_size = 0;
	int pc = cg_pool_class(object_size, &block_size);
	if (!pc)
	{
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), object_size);
		cg_emit(cg,"    call bzy_alloc");
		return;
	}

	int slow = cg_label(cg);
	int done = cg_label(cg);
	const char *base = cg_emit_pool_base(cg, "rcx", slow);

	cg_emit(cg,"    mov rax, [%s + %d]", base, pc * 8);   /* head[pc]  (head @ offset 0). */
	cg_emit(cg,"    test rax, rax");
	cg_emit(cg,"    jz .L%d", slow);                      /* Free list empty (also a fresh thread). */
	cg_emit(cg,"    mov rdx, [rax]");                     /* next block. */
	cg_emit(cg,"    mov [%s + %d], rdx", base, pc * 8);   /* head[pc] = next. */
	cg_emit(cg,"    dec dword [%s + %d]", base, 88 + pc * 4);  /* n[pc]--. */
	cg_emit(cg,"    inc qword [%s + 136]", base);         /* live++. */
	cg_emit(cg,"    mov qword [rax + 8], 1");             /* refcount = 1. */
	cg_emit(cg,"    mov qword [rax + 16], %d", pc << 4);  /* gcinfo: class nibble, BLACK, unshared. */
	for (int off = 24; off < object_size; off += 8)
	{
		cg_emit(cg,"    mov qword [rax + %d], 0", off);  /* Zero the fields (calloc semantics). */
	}

	cg_emit(cg,"    jmp .L%d", done);
	cg_emit(cg,".L%d:", slow);
	cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), object_size);
	cg_emit(cg,"    call bzy_alloc");
	cg_emit(cg,".L%d:", done);
}

static void cg_new(Codegen *cg, TypeTable *tt, Expr *e)
{
	if (strcmp(e->name,"StringBuilder")==0)
	{
		cg_aligned_call(cg,"bzy_sb_new");   /* Owned (+1) StringBuilder in rax. */
		return;
	}

	ClassInfo *c=types_find_class(tt,e->name);
	if (e->anno_stack)
	{
		/* The object lives in the frame at rbp - anno_stack_off: no refcount
		   traffic, no free. A refcount of zero marks it unmanaged for the runtime. */
		cg_emit(cg,"    lea rax, [rbp - %d]", e->anno_stack_off);
		cg_emit(cg,"    lea rbx, [rel __vtable_%s]", c->name);
		cg_emit(cg,"    mov [rax], rbx");
		cg_emit(cg,"    mov qword [rax + 8], 0");
		for (int off=16; off<c->object_size; off+=8)
		{
			cg_emit(cg,"    mov qword [rax + %d], 0", off);
		}
	}
	else
	{
		cg_emit_alloc(cg, c->object_size);
		cg_emit(cg,"    lea rbx, [rel __vtable_%s]", c->name);
		cg_emit(cg,"    mov [rax], rbx");
		/* The refcount and fields are zeroed by the allocation, so rax holds an owned reference. */
		if (c->is_shared)
		{
			/* Channel-reachable class: mark gcinfo so retain/release go atomic.
			   8 = BZY_GCINFO_SHARED (bit 3) in runtime/breezy.h. */
			cg_emit(cg,"    or qword [rax + 16], 8");
		}
	}

	if (c->has_ctor)
	{
		MethodInfo *ct=&c->ctors[e->anno_overload];
		cg_ctor_call(cg,tt,ct->asm_label,e->args,e->arg_count,ct->param_types,ct->param_count);
	}
}

/* Static enum calls (the receiver is the enum type name): Enum.values() builds a
   fresh owned array of the singletons; Enum.valueOf(s) returns the matching
   constant (retained) or throws via bzy_enum_no_constant. Emitted inline in the
   caller's frame so the throw site is covered by its exception record. */
static void cg_enum_static(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *en=e->lhs->name;
	int n=enum_count_of(en);
	/* assign_save holds the value that must survive cg_retain_rax (which uses
	   val_save as its own scratch). */
	if (strcmp(e->name,"values")==0)
	{
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), n);
		cg_emit(cg,"    mov %s, 1", cg_iarg(cg, 1));   /* Managed elements. */
		cg_aligned_call(cg,"bzy_array_new");           /* Owned array -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);
		for (int i=0; i<n; i++)
		{
			cg_emit(cg,"    mov rax, [rel __enum_%s_%s]", en, enum_const_name(en,i));
			cg_retain_rax(cg);
			cg_emit(cg,"    mov rbx, [rbp - %d]", cg->assign_save);
			cg_emit(cg,"    mov [rbx + %d], rax", 32 + i*8);
		}

		cg_emit(cg,"    mov rax, [rbp - %d]", cg->assign_save);
		return;
	}

	/* valueOf(string): query string in assign_save (survives str_eq + retain). */
	cg_expr(cg,tt,e->args[0]);
	int owned=expr_is_owned(e->args[0]);
	cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);
	int done=cg_label(cg);
	for (int i=0; i<n; i++)
	{
		int next=cg_label(cg);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->assign_save);
		cg_emit(cg,"    mov rbx, [rel __enum_%s_%s]", en, enum_const_name(en,i));
		cg_emit(cg,"    mov %s, [rbx + 32]", cg_iarg(cg, 1));   /* The constant's __name. */
		cg_aligned_call(cg,"bzy_str_eq");
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d", next);
		cg_emit(cg,"    mov rax, [rel __enum_%s_%s]", en, enum_const_name(en,i));
		cg_retain_rax(cg);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Result (retain done). */
		cg_emit(cg,"    jmp .L%d", done);
		cg_emit(cg,".L%d:", next);
	}

	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->assign_save);   /* The bad name. */
	int pc=cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 1), pc);
	cg_emit(cg,".L%d:", pc);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 2));
	cg_aligned_call(cg,"bzy_enum_no_constant");   /* Never returns. */
	cg_emit(cg,".L%d:", done);
	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->assign_save);
		cg_release_rcx(cg);   /* Clobbers rax; the result is parked in val_save. */
	}

	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
}

/* Enum instance built-ins: name() reads the hidden __name field (offset 32, an
   owned string), ordinal() reads __ordinal (offset 24, an int). */
static void cg_enum_instance(Codegen *cg, TypeTable *tt, Expr *e)
{
	int isname = strcmp(e->name,"name")==0;
	cg_expr(cg,tt,e->lhs);
	int owned=expr_is_owned(e->lhs);
	if (owned)
	{
		cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);   /* Receiver (survives retain). */
	}

	cg_emit(cg,"    mov rax, [rax + %d]", isname ? 32 : 24);
	if (isname)
	{
		cg_retain_rax(cg);                                       /* Uses val_save as scratch. */
	}

	if (owned)
	{
		cg_temp_push(cg);                              /* Preserve the result. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->assign_save);
		cg_release_rcx(cg);
		cg_temp_pop(cg);
	}
}

static void cg_print(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->args[0]);
	TypeKind k=e->args[0]->type.kind;
	if (k==TY_STRING)
	{
		int owned = expr_is_owned(e->args[0]);
		if (owned)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		}

		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg,"bzy_print_str");
		if (owned)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
			cg_release_rcx(cg);
		}

		return;
	}

	if (ty_is_float(k))
	{
		if (k==TY_FLOAT)
		{
			cg_emit(cg,"    cvtss2sd xmm0, xmm0");   /* Promote to double for printing. */
		}

		cg_aligned_call(cg,"bzy_print_f64");         /* Double arg already in xmm0. */
		return;
	}

	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	const char *fn = k==TY_BOOL ? "bzy_print_bool"
					 : ty_is_unsigned(k) ? "bzy_print_u64"
					 : "bzy_print_i64";
	cg_aligned_call(cg,fn);
}

/* Evaluate an argument and leave it in xmm0 as a double (promoting int/float). */
static void cg_to_double(Codegen *cg, TypeTable *tt, Expr *a)
{
	cg_expr(cg,tt,a);
	if (ty_is_int(a->type.kind))
	{
		cg_emit(cg,"    cvtsi2sd xmm0, rax");
	}
	else if (a->type.kind==TY_FLOAT)
	{
		cg_emit(cg,"    cvtss2sd xmm0, xmm0");
	}
}

/* Math.* builtins. Integer results land in rax; double results in xmm0. */
static void cg_math(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 5;   /* After "Math.". */
	if (strcmp(m,"sqrt")==0)
	{
		cg_to_double(cg,tt,e->args[0]);
		cg_emit(cg,"    sqrtsd xmm0, xmm0");
		return;
	}

	if (strcmp(m,"abs")==0)
	{
		if (e->type.kind==TY_DOUBLE)
		{
			cg_to_double(cg,tt,e->args[0]);
			cg_emit(cg,"    movq rax, xmm0");
			cg_emit(cg,"    btr rax, 63");      /* Clear the sign bit. */
			cg_emit(cg,"    movq xmm0, rax");
		}
		else
		{
			cg_expr(cg,tt,e->args[0]);          /* Integer in rax. */
			cg_emit(cg,"    mov rcx, rax");     /* rcx: scratch (no call here); same on both ABIs. */
			cg_emit(cg,"    neg rcx");
			cg_emit(cg,"    test rax, rax");
			cg_emit(cg,"    cmovs rax, rcx");   /* If x < 0, take -x. */
		}

		return;
	}

	if (strcmp(m,"min")==0 || strcmp(m,"max")==0)
	{
		int ismin = strcmp(m,"min")==0;
		if (e->type.kind==TY_DOUBLE)
		{
			cg_to_double(cg,tt,e->args[0]);
			int b = cg_scratch_alloc(cg, 16);
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);
			cg_to_double(cg,tt,e->args[1]);
			cg_emit(cg,"    movaps xmm1, xmm0");
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);
			cg_scratch_free(cg, 16);
			cg_emit(cg, ismin ? "    minsd xmm0, xmm1" : "    maxsd xmm0, xmm1");
		}
		else
		{
			cg_expr(cg,tt,e->args[0]);
			cg_temp_push(cg);
			cg_expr(cg,tt,e->args[1]);
			cg_emit(cg,"    mov rbx, rax");
			cg_temp_pop(cg);
			cg_emit(cg,"    cmp rax, rbx");
			cg_emit(cg, ismin ? "    cmovg rax, rbx" : "    cmovl rax, rbx");
		}

		return;
	}

	if (strcmp(m,"clamp")==0)
	{
		if (e->type.kind==TY_DOUBLE)
		{
			int b = cg_scratch_alloc(cg, 32);
			cg_to_double(cg,tt,e->args[0]);
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);        /* x */
			cg_to_double(cg,tt,e->args[1]);
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - 8);    /* lo */
			cg_to_double(cg,tt,e->args[2]);
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - 16);   /* hi */
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);
			cg_emit(cg,"    movsd xmm1, qword [rbp - %d]", b - 16);
			cg_emit(cg,"    minsd xmm0, xmm1");               /* min(x, hi) */
			cg_emit(cg,"    movsd xmm1, qword [rbp - %d]", b - 8);
			cg_emit(cg,"    maxsd xmm0, xmm1");               /* max(., lo) */
			cg_scratch_free(cg, 32);
		}
		else
		{
			int b = cg_scratch_alloc(cg, 32);
			cg_expr(cg,tt,e->args[0]);
			cg_emit(cg,"    mov [rbp - %d], rax", b);
			cg_expr(cg,tt,e->args[1]);
			cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
			cg_expr(cg,tt,e->args[2]);
			cg_emit(cg,"    mov [rbp - %d], rax", b - 16);
			cg_emit(cg,"    mov rax, [rbp - %d]", b);
			cg_emit(cg,"    mov rbx, [rbp - %d]", b - 16);
			cg_emit(cg,"    cmp rax, rbx");
			cg_emit(cg,"    cmovg rax, rbx");                 /* min(x, hi) */
			cg_emit(cg,"    mov rbx, [rbp - %d]", b - 8);
			cg_emit(cg,"    cmp rax, rbx");
			cg_emit(cg,"    cmovl rax, rbx");                 /* max(., lo) */
			cg_scratch_free(cg, 32);
		}

		return;
	}

	if (strcmp(m,"floor")==0 || strcmp(m,"ceil")==0 || strcmp(m,"round")==0)
	{
		int mode = strcmp(m,"floor")==0 ? 1 : strcmp(m,"ceil")==0 ? 2 : 0;   /* 1 floor, 2 ceil, 0 nearest */
		cg_to_double(cg,tt,e->args[0]);
		cg_emit(cg,"    roundsd xmm0, xmm0, %d", mode);
		return;
	}

	if (strcmp(m,"toRadians")==0)
	{
		cg_to_double(cg,tt,e->args[0]);
		cg_emit(cg,"    movsd xmm1, qword [rel __deg2rad]");
		cg_emit(cg,"    mulsd xmm0, xmm1");
		return;
	}

	if (strcmp(m,"sin")==0 || strcmp(m,"cos")==0 || strcmp(m,"tan")==0 || strcmp(m,"exp")==0)
	{
		cg_to_double(cg,tt,e->args[0]);
		cg_aligned_call(cg,m);              /* Arg already in xmm0; result in xmm0. */
		return;
	}

	if (strcmp(m,"pow")==0)
	{
		cg_to_double(cg,tt,e->args[0]);
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);
		cg_to_double(cg,tt,e->args[1]);
		cg_emit(cg,"    movaps xmm1, xmm0");
		cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);
		cg_scratch_free(cg, 16);
		cg_aligned_call(cg,"pow");          /* base xmm0, exp xmm1; result xmm0. */
		return;
	}

	fprintf(stderr,"Codegen: unsupported Math method '%s'\n", m);
	exit(1);
}

/* string.method(...) -> bzy_str_* (receiver passed as self; bool/int or owned result). */
static void cg_string_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *nm = e->name;
	const char *fn =
		strcmp(nm,"length")==0      ? "bzy_str_len" :
		strcmp(nm,"contains")==0    ? "bzy_str_contains" :
		strcmp(nm,"startsWith")==0  ? "bzy_str_starts_with" :
		strcmp(nm,"endsWith")==0    ? "bzy_str_ends_with" :
		strcmp(nm,"substring")==0   ? "bzy_str_substring" :
		strcmp(nm,"replace")==0     ? "bzy_str_replace" :
		strcmp(nm,"trim")==0        ? "bzy_str_trim" :
		strcmp(nm,"toUpper")==0     ? "bzy_str_to_upper" :
		strcmp(nm,"toLower")==0     ? "bzy_str_to_lower" :
		strcmp(nm,"isEmpty")==0          ? "bzy_str_is_empty" :
		strcmp(nm,"isNumeric")==0        ? "bzy_str_is_numeric" :
		strcmp(nm,"isAlphaNumeric")==0   ? "bzy_str_is_alphanumeric" :
		strcmp(nm,"equals")==0           ? "bzy_str_eq" :
		strcmp(nm,"equalsIgnoreCase")==0 ? "bzy_str_equals_ignore_case" :
		strcmp(nm,"lastIndexOf")==0      ? "bzy_str_last_index_of" :
		strcmp(nm,"charAt")==0           ? "bzy_str_char_at" :
		strcmp(nm,"repeat")==0           ? "bzy_str_repeat" :
		strcmp(nm,"split")==0            ? "bzy_str_split" :
		strcmp(nm,"toBytes")==0          ? "bzy_str_to_bytes" :
		strcmp(nm,"toInt")==0            ? "bzy_str_to_int" :
		strcmp(nm,"toLong")==0           ? "bzy_str_to_long" :
		strcmp(nm,"toByte")==0           ? "bzy_str_to_byte" :
		strcmp(nm,"toShort")==0          ? "bzy_str_to_short" :
		strcmp(nm,"toFloat")==0          ? "bzy_str_to_float" :
		strcmp(nm,"toDouble")==0         ? "bzy_str_to_double" :
		strcmp(nm,"toBool")==0           ? "bzy_str_to_bool" :
		"bzy_str_index_of";
	TypeRef ps[2];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), 0, ps, e->arg_count, 0);

	/* The parse methods throw NumberFormatException on malformed input: emit the
	   post-call check, preserving the result (int/bool in rax, fp in xmm0). */
	int is_parse = strcmp(nm,"toInt")==0 || strcmp(nm,"toLong")==0 || strcmp(nm,"toByte")==0
				   || strcmp(nm,"toShort")==0 || strcmp(nm,"toFloat")==0
				   || strcmp(nm,"toDouble")==0 || strcmp(nm,"toBool")==0;
	if (is_parse)
	{
		int fp = ty_is_float(e->type.kind);
		if (fp)
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		}

		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_number_check");
		if (fp)
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
}

/* scheduleAfter(f, delayMs) / scheduleEvery(f, delayMs, periodMs): pass the
   target's address (a named zero-arg void function) plus the integer delays to
   the timer runtime. Returns an owned (+1) Timer in rax. */
static void cg_schedule(Codegen *cg, TypeTable *tt, Expr *e)
{
	int periodic = (strcmp(e->name,"scheduleEvery")==0);
	FuncInfo *fi = types_find_func(tt, e->args[0]->name);
	if (periodic)
	{
		int b = cg_scratch_alloc(cg, 16);
		cg_expr(cg,tt,e->args[1]);          /* delayMs -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr(cg,tt,e->args[2]);          /* periodMs -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 2));      /* period -> 3rd arg. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b);   /* delay  -> 2nd arg. */
		cg_scratch_free(cg, 16);
		cg_emit(cg,"    lea %s, [rel %s]", cg_iarg(cg, 0), fi->asm_label);   /* entry -> 1st arg (after the cg_exprs). */
		cg_aligned_call(cg,"bzy_timer_every");
	}
	else
	{
		cg_expr(cg,tt,e->args[1]);          /* delayMs -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));     /* delay -> 2nd arg. */
		cg_emit(cg,"    lea %s, [rel %s]", cg_iarg(cg, 0), fi->asm_label);   /* entry -> 1st arg. */
		cg_aligned_call(cg,"bzy_timer_after");
	}
	/* Owned (+1) Timer in rax. */
}

/* Timer.cancel(): mark the timer dead (the heap evicts it lazily). */
static void cg_timer_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);             /* Timer handle -> rax (borrowed, not owned). */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_timer_cancel");
}

/* Network.* constructors: listen/connect/udp -> owned handle. No receiver; args
   (port, or host+port) lower through cg_call_with_args (owned host temp released). */
static void cg_ffi(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 4;   /* After "Ffi.". */
	if (strcmp(m,"bind")==0)
	{
		/* bzy_ffi_bind(path) -> bool; non-fallible (returns false on load failure). */
		TypeRef ps[1];
		ps[0]=e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_ffi_bind",NULL,e->args,e->arg_count,0, 0, 0, ps, e->arg_count, 0);
		return;
	}
}

/* A dynamic extern call: resolve the C symbol through the active resolver on the
   first call (slow path -> bzy_dynsym + io_check), cache it in a per-extern static
   slot, then marshal args exactly as a normal extern and call indirectly through the
   slot. The slot/name statics are emitted once in cg_program. Keyed on the bare name
   (e->name), NOT fi->asm_label -- the latter carries the NASM '$' escape. */
static void cg_dynamic_extern_call(Codegen *cg, TypeTable *tt, Expr *e, FuncInfo *fi)
{
	int khave = cg_label(cg);
	cg_emit(cg,"    cmp qword [rel __dynslot_%s], 0", e->name);
	cg_emit(cg,"    jne .L%d", khave);
	cg_emit(cg,"    lea %s, [rel __dynname_%s]", cg_iarg(cg, 0), e->name);
	cg_aligned_call(cg,"bzy_dynsym");
	cg_emit(cg,"    mov [rel __dynslot_%s], rax", e->name);
	int kpc = cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), kpc);
	cg_emit(cg,".L%d:", kpc);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
	cg_aligned_call(cg,"bzy_io_check");   /* Throws IOException if unresolved (slow path only). */
	cg_emit(cg,".L%d:", khave);

	char target[160];
	snprintf(target, sizeof(target), "qword [rel __dynslot_%s]", e->name);
	cg->call_variadic = fi->is_variadic;
	cg_call_with_args(cg,tt,target,NULL,e->args,e->arg_count,0, ty_is_managed(e->type.kind),
					  ty_is_float(e->type.kind), fi->param_types, fi->param_count, fi->is_extern);
	cg->call_variadic = 0;
}

static void cg_graphics(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 9;   /* After "Graphics.". */
	if (strcmp(m,"open")==0)
	{
		/* Open a window -> owned Surface handle; fallible (no SDL / no display),
		   so a post-call bzy_io_check throws IOException with the handle preserved. */
		TypeRef ps[3];
		for (int i=0; i<e->arg_count; i++) { ps[i]=e->args[i]->type; }
		cg_call_with_args(cg,tt,"bzy_surface_open",NULL,e->args,e->arg_count,0, 1, 0, ps, e->arg_count, 0);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	if (strcmp(m,"openGL")==0)
	{
		/* Open a GL window -> owned GlSurface handle; fallible (no SDL / no display),
		   so a post-call bzy_io_check throws IOException with the handle preserved. */
		TypeRef ps[3];
		for (int i=0; i<e->arg_count; i++) { ps[i]=e->args[i]->type; }
		cg_call_with_args(cg,tt,"bzy_glsurface_open",NULL,e->args,e->arg_count,0, 1, 0, ps, e->arg_count, 0);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}
}

static void cg_network(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 8;   /* After "Network.". */
	if (strcmp(m,"readUrl")==0)
	{
		/* Fetch (http/https) -> owned body string; fallible, so a post-call
		   bzy_io_check throws IOException with the body preserved across it. */
		TypeRef ps0[1];
		ps0[0]=e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_net_read_url",NULL,e->args,e->arg_count,0, 1, 0, ps0, e->arg_count, 0);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the body across io_check. */
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	if (strcmp(m,"rawSocket")==0)
	{
		/* Open a raw socket -> owned Socket handle; fallible (privilege denial),
		   so a post-call bzy_io_check throws IOException with the handle preserved. */
		TypeRef psr[1];
		psr[0]=e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_raw_socket",NULL,e->args,e->arg_count,0, 1, 0, psr, e->arg_count, 0);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the handle across io_check. */
		int kr = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), kr);
		cg_emit(cg,".L%d:", kr);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	if (strcmp(m,"tlsConnect")==0)
	{
		/* 2-arg -> system-default CAs; 3-arg -> explicit CA bundle. Fallible. */
		const char *tfn = (e->arg_count==3) ? "bzy_tls_connect_ca" : "bzy_tls_connect";
		TypeRef pst[3];
		for (int i=0; i<e->arg_count; i++) { pst[i]=e->args[i]->type; }
		cg_call_with_args(cg,tt,tfn,NULL,e->args,e->arg_count,0, 1, 0, pst, e->arg_count, 0);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		int kc = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), kc);
		cg_emit(cg,".L%d:", kc);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	if (strcmp(m,"tlsListen")==0)
	{
		TypeRef psl[3];
		for (int i=0; i<e->arg_count; i++) { psl[i]=e->args[i]->type; }
		cg_call_with_args(cg,tt,"bzy_tls_listen",NULL,e->args,e->arg_count,0, 1, 0, psl, e->arg_count, 0);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		int kl = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), kl);
		cg_emit(cg,".L%d:", kl);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	const char *fn;
	if (strcmp(m,"listen")==0)
	{
		fn = "bzy_listener_new";
	}
	else if (strcmp(m,"connect")==0)
	{
		fn = "bzy_socket_connect";
	}
	else if (strcmp(m,"udp")==0)
	{
		fn = "bzy_udp_new";
	}
	else
	{
		fprintf(stderr,"Codegen: unknown Network method '%s'\n", m);
		exit(1);
	}

	TypeRef ps[2];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,NULL,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), 0, ps, e->arg_count, 0);
}

/* Log.open(path) -> owned Logger. Single string arg; fallible (the FileWriter open
   can fail) so a post-call bzy_io_check throws IOException, result preserved. */
/* XmlNode methods: attr(name)/hasAttr(name) take a string, attrNameAt(index)
   takes an int. attr/attrNameAt return an owned string; hasAttr returns a bool.
   The receiver is borrowed; an owned argument or receiver is released after the
   call, preserving the result across the releases. */
static void cg_xml_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;

	if (strcmp(n,"descendants")==0)               /* Zero-arg; owned List<XmlNode>. */
	{
		int b = cg_scratch_alloc(cg, 16);
		cg_expr(cg,tt,e->lhs);                     /* Receiver -> rax. */
		int owned = expr_is_owned(e->lhs);
		cg_emit(cg,"    mov [rbp - %d], rax", b);  /* Receiver (for a possible release). */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg,"bzy_xml_descendants"); /* Owned List -> rax. */
		if (owned)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", b - 8);   /* Preserve the result. */
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", b - 8);
		}

		cg_scratch_free(cg, 16);
		return;
	}

	const char *fn = strcmp(n,"attr")==0 ? "bzy_xml_attr"
					 : strcmp(n,"hasAttr")==0 ? "bzy_xml_has_attr"
					 : "bzy_xml_attr_name_at";

	cg_expr(cg,tt,e->lhs);                        /* Receiver -> rax. */
	int b = cg_scratch_alloc(cg, 24);
	cg_emit(cg,"    mov [rbp - %d], rax", b);     /* receiver. */
	cg_expr(cg,tt,e->args[0]);                    /* name string / index -> rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", b - 8); /* owned-arg slot. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
	cg_aligned_call(cg,fn);                       /* Owned string, or bool, in rax. */

	int rel_arg = expr_is_owned(e->args[0]);
	int rel_recv = expr_is_owned(e->lhs);
	if (rel_arg || rel_recv)
	{
		cg_emit(cg,"    mov [rbp - %d], rax", b - 16);   /* Preserve the result. */
		if (rel_arg)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
			cg_release_rcx(cg);
		}

		if (rel_recv)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
			cg_release_rcx(cg);
		}

		cg_emit(cg,"    mov rax, [rbp - %d]", b - 16);
	}

	cg_scratch_free(cg, 24);
}

/* Xml.parse(text) -> XmlNode. Lowers to bzy_xml_parse(text); a post-call
   bzy_xml_check throws a catchable XmlException on malformed input (the
   bzy_io_check / bzy_number_check pattern), preserving the node result. */
static void cg_xml(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 4;   /* After "Xml.". */
	if (strcmp(m,"parse")!=0)
	{
		fprintf(stderr,"Codegen: unknown Xml method '%s'\n", m);
		exit(1);
	}

	cg_expr(cg,tt,e->args[0]);                   /* text -> rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	int owned = expr_is_owned(e->args[0]);
	if (owned)
	{
		cg_emit(cg,"    mov [rbp - %d], %s", cg->val_save, cg_iarg(cg, 0));   /* Save text for release. */
	}

	cg_aligned_call(cg,"bzy_xml_parse");         /* Owned root XmlNode -> rax (NULL on error). */
	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
		cg_temp_push(cg);
		cg_release_rcx(cg);
		cg_temp_pop(cg);
	}

	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the node across the check. */
	int xk = cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), xk);
	cg_emit(cg,".L%d:", xk);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
	cg_aligned_call(cg,"bzy_xml_check");
	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
}

/* Json.parse(text) -> JsonValue. Lowers to bzy_json_parse(text); a post-call
   bzy_json_check throws a catchable JsonException on malformed input (the
   bzy_xml_check copy), preserving the node result. */
static void cg_json(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 5;   /* After "Json.". */

	if (strcmp(m,"ofNull")==0)                   /* Zero-arg; owned null JsonValue. */
	{
		cg_aligned_call(cg,"bzy_json_null");
		return;
	}

	if (strcmp(m,"of")==0)                        /* Lift one Breezy value (overload on arg type). */
	{
		TypeKind ak = e->args[0]->type.kind;
		const char *fn = ty_is_int(ak)                  ? "bzy_json_of_long" :
						 (ak==TY_DOUBLE || ak==TY_FLOAT) ? "bzy_json_of_double" :
						 ak==TY_STRING                   ? "bzy_json_of_string" :
						 ak==TY_BOOL                     ? "bzy_json_of_bool" :
						 ak==TY_GENERIC                  ? "bzy_json_of_array" :
						 "bzy_json_of_object";          /* TY_MAP. */
		TypeRef ps[1];
		memset(&ps[0],0,sizeof(ps[0]));
		ps[0].kind = ty_is_int(ak) ? TY_LONG : (ak==TY_FLOAT ? TY_DOUBLE : ak);
		cg_call_with_args(cg,tt,fn,NULL,e->args,1,0,1,0,ps,1,0);   /* Owned JsonValue -> rax. */
		return;
	}

	if (strcmp(m,"stringify")==0)                 /* JsonValue -> compact string; non-finite throws. */
	{
		TypeRef ps[1];
		ps[0]=e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_json_stringify",NULL,e->args,1,0,1,0,ps,1,0);   /* Owned string -> rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve across the check. */
		int sk = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), sk);
		cg_emit(cg,".L%d:", sk);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_json_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	if (strcmp(m,"parse")!=0)
	{
		fprintf(stderr,"Codegen: unknown Json method '%s'\n", m);
		exit(1);
	}

	cg_expr(cg,tt,e->args[0]);                   /* text -> rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	int owned = expr_is_owned(e->args[0]);
	if (owned)
	{
		cg_emit(cg,"    mov [rbp - %d], %s", cg->val_save, cg_iarg(cg, 0));   /* Save text for release. */
	}

	cg_aligned_call(cg,"bzy_json_parse");        /* Owned root JsonValue -> rax (NULL on error). */
	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
		cg_temp_push(cg);
		cg_release_rcx(cg);
		cg_temp_pop(cg);
	}

	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the node across the check. */
	int jk = cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), jk);
	cg_emit(cg,".L%d:", jk);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
	cg_aligned_call(cg,"bzy_json_check");
	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
}

/* JsonValue accessors. Kind tests + type() never throw; the strict asX()
   extractors set the shared error on a wrong kind, so a post-call bzy_json_check
   throws JsonException (the str.toX number-check pattern). All current methods
   are zero-arg on the receiver. */
static void cg_json_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn =
		strcmp(n,"isNull")==0   ? "bzy_json_is_null" :
		strcmp(n,"isBool")==0   ? "bzy_json_is_bool" :
		strcmp(n,"isNumber")==0 ? "bzy_json_is_number" :
		strcmp(n,"isString")==0 ? "bzy_json_is_string" :
		strcmp(n,"isArray")==0  ? "bzy_json_is_array" :
		strcmp(n,"isObject")==0 ? "bzy_json_is_object" :
		strcmp(n,"type")==0     ? "bzy_json_type_name" :
		strcmp(n,"asString")==0 ? "bzy_json_as_string" :
		strcmp(n,"asLong")==0   ? "bzy_json_as_long" :
		strcmp(n,"asDouble")==0 ? "bzy_json_as_double" :
		strcmp(n,"asBool")==0   ? "bzy_json_as_bool" :
		strcmp(n,"get")==0      ? "bzy_json_get" :
		strcmp(n,"has")==0      ? "bzy_json_has" :
		strcmp(n,"keys")==0     ? "bzy_json_keys" :
		strcmp(n,"items")==0    ? "bzy_json_items" :
		strcmp(n,"at")==0       ? "bzy_json_at" :
		NULL;
	if (!fn)
	{
		fprintf(stderr,"Codegen: unknown JsonValue method '%s'\n", n);
		exit(1);
	}

	TypeRef ps[1];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), ty_is_float(e->type.kind), ps, e->arg_count, 0);

	int is_extract = strcmp(n,"asString")==0 || strcmp(n,"asLong")==0
					 || strcmp(n,"asDouble")==0 || strcmp(n,"asBool")==0
					 || strcmp(n,"at")==0;
	if (is_extract)
	{
		int fp = ty_is_float(e->type.kind);
		if (fp)
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		}

		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_json_check");
		if (fp)
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
}

/* Http.readRequest(socket) -> HttpRequest. Lowers to bzy_http_read_request(sock);
   a post-call bzy_http_check throws a catchable HttpException on a malformed
   message (the bzy_json_check copy), preserving the node (NULL at a clean EOF). */
static void cg_http(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 5;   /* After "Http.". */

	if (strcmp(m,"respond")==0)                  /* (Socket, status, body) -> void. */
	{
		TypeRef ps[3];
		ps[0]=e->args[0]->type;
		memset(&ps[1],0,sizeof(ps[1]));
		ps[1].kind=TY_LONG;
		ps[2]=e->args[2]->type;
		cg_call_with_args(cg,tt,"bzy_http_respond",NULL,e->args,3,0,0,0,ps,3,0);
		return;
	}

	if (strcmp(m,"response")==0)                  /* (status) -> HttpResponse. */
	{
		TypeRef ps[1];
		memset(&ps[0],0,sizeof(ps[0]));
		ps[0].kind=TY_LONG;
		cg_call_with_args(cg,tt,"bzy_http_response",NULL,e->args,1,0,1,0,ps,1,0);
		return;
	}

	if (strcmp(m,"request")==0)                   /* (method, path) -> HttpRequest. */
	{
		TypeRef ps[2];
		ps[0]=e->args[0]->type;
		ps[1]=e->args[1]->type;
		cg_call_with_args(cg,tt,"bzy_http_request",NULL,e->args,2,0,1,0,ps,2,0);
		return;
	}

	int is_resp = strcmp(m,"readResponse")==0;
	if (!is_resp && strcmp(m,"readRequest")!=0)
	{
		fprintf(stderr,"Codegen: unknown Http method '%s'\n", m);
		exit(1);
	}

	cg_expr(cg,tt,e->args[0]);                   /* socket -> rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	int owned = expr_is_owned(e->args[0]);
	if (owned)
	{
		cg_emit(cg,"    mov [rbp - %d], %s", cg->val_save, cg_iarg(cg, 0));
	}

	cg_aligned_call(cg, is_resp ? "bzy_http_read_response" : "bzy_http_read_request"); /* Owned message -> rax (NULL at EOF). */
	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
		cg_temp_push(cg);
		cg_release_rcx(cg);
		cg_temp_pop(cg);
	}

	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve across the check. */
	int hk = cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), hk);
	cg_emit(cg,".L%d:", hk);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
	cg_aligned_call(cg,"bzy_http_check");
	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
}

/* HttpRequest/HttpResponse accessor methods (header/hasHeader/headerNames/
   bodyBytes; the builder setHeader/setBody/send arrive in Tasks 5-7). None throw. */
static void cg_http_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn =
		strcmp(n,"header")==0      ? "bzy_http_header" :
		strcmp(n,"hasHeader")==0   ? "bzy_http_has_header" :
		strcmp(n,"headerNames")==0 ? "bzy_http_header_names" :
		strcmp(n,"bodyBytes")==0   ? "bzy_http_body_bytes" :
		strcmp(n,"setHeader")==0   ? "bzy_http_set_header" :
		strcmp(n,"setBody")==0     ? "bzy_http_set_body" :
		strcmp(n,"send")==0        ? (e->lhs->type.kind==TY_HTTPRESPONSE ? "bzy_http_send_response" : "bzy_http_send_request") :
		NULL;
	if (!fn)
	{
		fprintf(stderr,"Codegen: unknown HTTP method '%s'\n", n);
		exit(1);
	}

	TypeRef ps[2];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), 0, ps, e->arg_count, 0);
}

static void cg_log(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 4;   /* After "Log.". */
	if (strcmp(m,"open")!=0)
	{
		fprintf(stderr,"Codegen: unknown Log method '%s'\n", m);
		exit(1);
	}

	cg_expr(cg,tt,e->args[0]);                   /* path -> rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	int owned = expr_is_owned(e->args[0]);
	if (owned)
	{
		cg_emit(cg,"    mov [rbp - %d], %s", cg->val_save, cg_iarg(cg, 0));   /* Save path for release. */
	}

	cg_aligned_call(cg,"bzy_logger_open");       /* Owned Logger -> rax. */
	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
		cg_temp_push(cg);
		cg_release_rcx(cg);
		cg_temp_pop(cg);
	}

	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the Logger across io_check. */
	int k = cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
	cg_emit(cg,".L%d:", k);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
	cg_aligned_call(cg,"bzy_io_check");
	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
}

/* Listener/Socket/UdpSocket/Datagram methods. The receiver (e->lhs) is the first
   arg (rcx); the runtime symbol is chosen by receiver kind + method name, and the
   timeout overloads select the _timeout symbol when the optional arg is present.
   cg_call_with_args releases owned arg temporaries and preserves the result. */
static void cg_net_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind rk = e->lhs->type.kind;
	const char *n = e->name;
	const char *fn = NULL;
	if (rk==TY_LISTENER)
	{
		if (strcmp(n,"accept")==0)
		{
			fn = e->arg_count==1 ? "bzy_listener_accept_timeout" : "bzy_listener_accept";
		}
		else if (strcmp(n,"tryAccept")==0)
		{
			fn = "bzy_listener_try_accept";
		}
		else if (strcmp(n,"port")==0)
		{
			fn = "bzy_listener_port";
		}
		else
		{
			fn = "bzy_listener_close";
		}
	}
	else if (rk==TY_SOCKET)
	{
		if (strcmp(n,"read")==0)
		{
			fn = e->arg_count==2 ? "bzy_socket_read_timeout" : "bzy_socket_read";
		}
		else if (strcmp(n,"tryRead")==0)
		{
			fn = "bzy_socket_try_read";
		}
		else if (strcmp(n,"readText")==0)
		{
			fn = e->arg_count==2 ? "bzy_socket_read_text_timeout" : "bzy_socket_read_text";
		}
		else if (strcmp(n,"tryReadText")==0)
		{
			fn = "bzy_socket_try_read_text";
		}
		else if (strcmp(n,"write")==0)
		{
			fn = "bzy_socket_write";
		}
		else if (strcmp(n,"writeText")==0)
		{
			fn = "bzy_socket_write_text";
		}
		else
		{
			fn = "bzy_socket_close";
		}
	}
	else if (rk==TY_UDPSOCKET)
	{
		if (strcmp(n,"sendTo")==0)
		{
			fn = "bzy_udp_send_to";
		}
		else if (strcmp(n,"sendTextTo")==0)
		{
			fn = "bzy_udp_send_text_to";
		}
		else if (strcmp(n,"receive")==0)
		{
			fn = e->arg_count==1 ? "bzy_udp_receive_timeout" : "bzy_udp_receive";
		}
		else if (strcmp(n,"tryReceive")==0)
		{
			fn = "bzy_udp_try_receive";
		}
		else if (strcmp(n,"port")==0)
		{
			fn = "bzy_udp_port";
		}
		else
		{
			fn = "bzy_udp_close";
		}
	}
	else   /* TY_DATAGRAM. */
	{
		if (strcmp(n,"data")==0)
		{
			fn = "bzy_dgram_data";
		}
		else if (strcmp(n,"text")==0)
		{
			fn = "bzy_dgram_text";
		}
		else if (strcmp(n,"host")==0)
		{
			fn = "bzy_dgram_host";
		}
		else
		{
			fn = "bzy_dgram_port";
		}
	}

	TypeRef ps[3];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), 0, ps, e->arg_count, 0);
}

/* FileChannel methods: receiver (e->lhs) in rcx, args in rdx/r8. readAt returns an
   owned byte[]; writeAt/size return int/long; truncate/sync/close return void.
   readAt/writeAt/truncate/sync are fallible -> post-call bzy_io_check. */
static void cg_filechannel_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	int fallible = 1;
	if (strcmp(n,"readAt")==0)
	{
		fn = "bzy_filechannel_read_at";
	}
	else if (strcmp(n,"writeAt")==0)
	{
		fn = "bzy_filechannel_write_at";
	}
	else if (strcmp(n,"readInto")==0)
	{
		fn = "bzy_filechannel_read_into";
	}
	else if (strcmp(n,"truncate")==0)
	{
		fn = "bzy_filechannel_truncate";
	}
	else if (strcmp(n,"sync")==0)
	{
		fn = "bzy_filechannel_sync";
	}
	else if (strcmp(n,"size")==0)
	{
		fn = "bzy_filechannel_size";
		fallible = 0;
	}
	else if (strcmp(n,"lock")==0)
	{
		fn = "bzy_filechannel_lock";   /* Returns bool in rax; not fallible (no throw). */
		fallible = 0;
	}
	else if (strcmp(n,"unlock")==0)
	{
		fn = "bzy_filechannel_unlock";   /* void, best-effort release; not fallible. */
		fallible = 0;
	}
	else if (strcmp(n,"mmap")==0)
	{
		fn = "bzy_mmap_map";   /* Fallible (default), returns an owned MappedFile (managed). */
	}
	else
	{
		fn = "bzy_filechannel_close";
		fallible = 0;
	}

	TypeRef ps[3];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	int obj = ty_is_managed(e->type.kind);   /* readAt -> byte[] (owned); others scalar/void. */
	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, obj, 0, ps, e->arg_count, 0);

	if (fallible)
	{
		if (e->type.kind != TY_VOID)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the return value (byte[] or count) across the check. */
		}

		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		if (e->type.kind != TY_VOID)
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
}

static void cg_mappedfile_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	int fallible = 0;
	if (strcmp(n,"size")==0)            { fn = "bzy_mmap_size"; }
	else if (strcmp(n,"getByte")==0)    { fn = "bzy_mmap_get_byte"; }
	else if (strcmp(n,"getInt")==0)     { fn = "bzy_mmap_get_int"; }
	else if (strcmp(n,"getLong")==0)    { fn = "bzy_mmap_get_long"; }
	else if (strcmp(n,"putByte")==0)    { fn = "bzy_mmap_put_byte"; }
	else if (strcmp(n,"putInt")==0)     { fn = "bzy_mmap_put_int"; }
	else if (strcmp(n,"putLong")==0)    { fn = "bzy_mmap_put_long"; }
	else if (strcmp(n,"copyInto")==0)   { fn = "bzy_mmap_copy_into"; }
	else if (strcmp(n,"copyFrom")==0)   { fn = "bzy_mmap_copy_from"; }
	else if (strcmp(n,"flush")==0)      { fn = "bzy_mmap_flush"; fallible = 1; }
	else                                { fn = "bzy_mmap_close"; }

	TypeRef ps[3];
	for (int i=0; i<e->arg_count; i++) { ps[i]=e->args[i]->type; }

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, 0, 0, ps, e->arg_count, 0);

	if (fallible)
	{
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
	}
}

/* TlsListener.accept() -> owned TlsSocket (fallible: handshake can fail);
   close() -> void (not fallible). */
static void cg_tls_listener_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	if (strcmp(n,"accept")==0)
	{
		fn = "bzy_tls_accept";
	}
	else if (strcmp(n,"port")==0)
	{
		fn = "bzy_tls_listener_port";
	}
	else
	{
		fn = "bzy_tls_close_listener";
	}
	int fallible = (strcmp(n,"accept")==0);

	int obj = ty_is_managed(e->type.kind);
	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, obj, 0, NULL, 0, 0);

	if (fallible)
	{
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
	}
}

/* TlsSocket.read(max)->byte[] / write(byte[])->int are fallible; close()->void is not. */
static void cg_tls_socket_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	int fallible = 1;
	if (strcmp(n,"read")==0)
	{
		fn = "bzy_tls_read";
	}
	else if (strcmp(n,"write")==0)
	{
		fn = "bzy_tls_write";
	}
	else
	{
		fn = "bzy_tls_close";
		fallible = 0;
	}

	TypeRef ps[1];
	for (int i=0; i<e->arg_count; i++) { ps[i]=e->args[i]->type; }

	int obj = ty_is_managed(e->type.kind);   /* read -> byte[] (owned); others scalar/void. */
	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, obj, 0, ps, e->arg_count, 0);

	if (fallible)
	{
		if (e->type.kind != TY_VOID)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		}

		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		if (e->type.kind != TY_VOID)
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
}

/* Surface methods: receiver (e->lhs) in the first arg register. present is fallible
   (length mismatch / lost device) -> post-call bzy_io_check; pollEvent (long),
   isOpen (bool), close (void) return directly. */
static void cg_surface_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	int fallible = 0;
	if (strcmp(n,"present")==0)
	{
		fn = "bzy_surface_present";
		fallible = 1;
	}
	else if (strcmp(n,"pollEvent")==0)
	{
		fn = "bzy_surface_poll_event";
	}
	else if (strcmp(n,"isOpen")==0)
	{
		fn = "bzy_surface_is_open";
	}
	else
	{
		fn = "bzy_surface_close";
	}

	TypeRef ps[1];
	for (int i=0; i<e->arg_count; i++) { ps[i]=e->args[i]->type; }

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, 0, 0, ps, e->arg_count, 0);

	if (fallible)
	{
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
	}
}

static void cg_glsurface_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	if (strcmp(n,"pollEvent")==0)
	{
		fn = "bzy_glsurface_poll";
	}
	else if (strcmp(n,"swapBuffers")==0)
	{
		fn = "bzy_glsurface_swap";
	}
	else if (strcmp(n,"isOpen")==0)
	{
		fn = "bzy_glsurface_isopen";
	}
	else
	{
		fn = "bzy_glsurface_close";
	}

	TypeRef ps[1];
	for (int i=0; i<e->arg_count; i++) { ps[i]=e->args[i]->type; }

	/* GlSurface methods are non-fallible: swapBuffers aborts on misuse, never throws. */
	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, 0, 0, ps, e->arg_count, 0);
}

/* FileWriter methods: receiver (e->lhs) in rcx, the one string/byte[] arg in rdx.
   All return void and are fallible -- an offloaded flush can fail -- so each emits a
   post-call bzy_io_check that throws IOException on a write error. */
static void cg_filewriter_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *n = e->name;
	const char *fn;
	if (strcmp(n,"write")==0)
	{
		fn = "bzy_filewriter_write";
	}
	else if (strcmp(n,"writeLine")==0)
	{
		fn = "bzy_filewriter_write_line";
	}
	else if (strcmp(n,"writeBytes")==0)
	{
		fn = "bzy_filewriter_write_bytes";
	}
	else if (strcmp(n,"flush")==0)
	{
		fn = "bzy_filewriter_flush";
	}
	else
	{
		fn = "bzy_filewriter_close";
	}

	TypeRef ps[1];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, 0, 0, ps, e->arg_count, 0);

	int k = cg_label(cg);
	cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);   /* pc = the call site. */
	cg_emit(cg,".L%d:", k);
	cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));             /* frame. */
	cg_aligned_call(cg,"bzy_io_check");
}

/* Logger methods. log(string): receiver in rcx, the string moved (+1, owned) into
   rdx and NOT released after -- the channel takes ownership. close(): receiver only. */
static void cg_logger_method(Codegen *cg, TypeTable *tt, Expr *e)
{
	if (strcmp(e->name,"log")==0)
	{
		cg_expr(cg,tt,e->lhs);                  /* Logger. */
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_expr_owned(cg,tt,e->args[0]);        /* +1 owned string; moves into the channel. */
		cg_emit(cg,"    mov [rbp - %d], rax", b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_aligned_call(cg,"bzy_logger_log");   /* Channel takes ownership: no release here. */
		cg_scratch_free(cg, 16);
		return;
	}

	/* close */
	cg_expr(cg,tt,e->lhs);
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_logger_close");
}

/* Clock.* builtins: zero-arg time reads, or getDateString (owned-string result). */
/* System.shell(command[, wait]) -> bzy_system_shell(command, wait). command in
   rcx, wait in rdx (0 when the optional bool is absent). The command string is
   only read by the runtime, so an owned temporary is released after the call. */
static void cg_system(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 7;   /* After "System.". */
	if (strcmp(m,"args")==0)
	{
		cg_aligned_call(cg,"bzy_sys_args");   /* Owned (+1) string[] in rax. */
		return;
	}

	if (strcmp(m,"getenv")==0)
	{
		/* System.getenv(name) -> owned string (null when unset). Route through the
		   general call path: releases the owned name temp, preserves the result. */
		TypeRef ps[1] = {0};
		ps[0].kind = TY_STRING;
		cg_call_with_args(cg,tt,"bzy_sys_getenv",NULL,e->args,e->arg_count,0,
						  1 /* owned string result */, 0,
						  ps, e->arg_count, 0 /* pass the string object, no marshal */);
		return;
	}

	if (strcmp(m,"awaitShutdown")==0)
	{
		cg_aligned_call(cg,"bzy_await_shutdown");   /* Parks the breeze; void result. */
		return;
	}

	if (strcmp(m,"sleep")==0)
	{
		/* System.sleep(ms) -> bzy_sys_sleep(ms). Scalar arg, void result. */
		TypeRef ps[1];
		ps[0] = e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_sys_sleep",NULL,e->args,e->arg_count,0,
						  0 /* result not owned */, 0, ps, e->arg_count, 0);
		return;
	}

	if (strcmp(m,"rawMode")==0)
	{
		/* System.rawMode(on) -> bzy_sys_raw_mode(on). Scalar bool, void result. */
		TypeRef ps[1];
		ps[0] = e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_sys_raw_mode",NULL,e->args,e->arg_count,0,
						  0 /* result not owned */, 0, ps, e->arg_count, 0);
		return;
	}

	if (strcmp(m,"pollKey")==0)
	{
		cg_aligned_call(cg,"bzy_sys_poll_key");   /* Next byte or -1 in rax. */
		return;
	}

	if (strcmp(m,"mouseMode")==0)
	{
		/* System.mouseMode(on) -> bzy_sys_mouse_mode(on). Scalar bool, void result. */
		TypeRef ps[1];
		ps[0] = e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_sys_mouse_mode",NULL,e->args,e->arg_count,0,
						  0 /* result not owned */, 0, ps, e->arg_count, 0);
		return;
	}

	if (strcmp(m,"pollMouse")==0)
	{
		cg_aligned_call(cg,"bzy_sys_poll_mouse");   /* Packed long or -1 in rax. */
		return;
	}

	if (strcmp(m,"cpuCount")==0)
	{
		cg_aligned_call(cg,"bzy_sys_cpu_count");   /* Core count in rax. */
		return;
	}

	if (strcmp(m,"affinity")==0)
	{
		/* System.affinity(mask) -> bzy_sys_affinity(mask). Scalar long, bool result. */
		TypeRef ps[1];
		ps[0] = e->args[0]->type;
		cg_call_with_args(cg,tt,"bzy_sys_affinity",NULL,e->args,e->arg_count,0,
						  0 /* result not owned */, 0, ps, e->arg_count, 0);
		return;
	}

	if (strcmp(m,"shell")!=0)
	{
		fprintf(stderr,"Codegen: unknown System method '%s'\n", m);
		exit(1);
	}

	if (e->arg_count==2)
	{
		cg_expr(cg,tt,e->args[1]);            /* wait (bool) -> rax. */
		cg_temp_push(cg);
		cg_expr(cg,tt,e->args[0]);            /* command (string) -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_temp_pop_reg(cg,"rdx");
	}
	else
	{
		cg_expr(cg,tt,e->args[0]);            /* command -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_emit(cg,"    mov %s, 0", cg_iarg(cg, 1));          /* wait = 0 (async). */
	}

	int owned = expr_is_owned(e->args[0]);
	if (owned)
	{
		cg_emit(cg,"    mov [rbp - %d], %s", cg->val_save, cg_iarg(cg, 0));   /* Save command for release. */
	}

	cg_aligned_call(cg,"bzy_system_shell");   /* Result (pid / exit code) in rax. */
	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
		cg_temp_push(cg);            /* Preserve the int result across the release. */
		cg_release_rcx(cg);
		cg_temp_pop(cg);
	}
}

static void cg_memory(Codegen *cg, TypeTable *tt, Expr *e)
{
	/* Memory.map(bytes) -> owned anonymous MappedFile region in rax. */
	cg_expr(cg,tt,e->args[0]);
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_memory_map");
}

/* True for a packed-value operand whose evaluation leaves xmm0 untouched, so it
   can be loaded straight into a chosen xmm register without spilling the other
   operand: a `Simd.load` (its address math uses only integer registers) or a
   plain f64x2 local (a single movupd from its slot). */
static int cg_simd_xmm0_safe(Expr *e)
{
	return (e->kind==EX_CALL && strncmp(e->name,"Simd.",5)==0 && strcmp(e->name+5,"load")==0)
		   || (e->kind==EX_IDENT && ty_is_simd(e->type.kind) && e->anno_int > 0);
}

/* The packed-op instruction suffix for a SIMD kind: "pd" for f64x2, "ps" for
   f32x4. So addpd vs addps, mulpd vs mulps, etc. */
static const char *cg_simd_sfx(TypeKind k)
{
	return k==TY_F32X4 ? "ps" : "pd";
}

/* Element kind a Simd.load/store addresses for its array (double, float, or int),
   which sets the index stride (8 for double, 4 for float/int). */
static TypeKind cg_simd_elem_kind(Expr *arr)
{
	return (arr->type.elem) ? arr->type.elem->kind : TY_DOUBLE;
}

static const char *cg_simd_op(const char *m, TypeKind k, char *buf);

/* In-place packed accumulate: `T = Simd.<op>(T, P)` (or the commutative
   `Simd.add/mul(P, T)`) where T is a register-promoted f64x2 local. Emits the
   other operand into xmm0 and combines straight into T's xmm home - no shuffle
   through xmm0 and no store-back, so a dot/saxpy reduction carries the same
   tight in-place chain Go's scalar accumulator does. Returns 1 if it emitted. */
static int cg_try_simd_inplace(Codegen *cg, TypeTable *tt, Expr *target, Expr *value)
{
	if (target->kind != EX_IDENT || !ty_is_simd(target->type.kind) || target->anno_int <= 0)
	{
		return 0;
	}

	const char *home = cg_local_xmm(cg, target->anno_int);
	if (!home || value->kind != EX_CALL || strncmp(value->name, "Simd.", 5) != 0)
	{
		return 0;
	}

	const char *m = value->name + 5;
	if (value->arg_count != 2
		|| !(strcmp(m,"add")==0 || strcmp(m,"sub")==0 || strcmp(m,"mul")==0
			 || strcmp(m,"div")==0 || strcmp(m,"min")==0 || strcmp(m,"max")==0))
	{
		return 0;
	}

	char op[8];
	cg_simd_op(m, target->type.kind, op);
	int commutative = strcmp(m,"add")==0 || strcmp(m,"mul")==0 || strcmp(m,"min")==0 || strcmp(m,"max")==0;
	int lhs_is_t = value->args[0]->kind==EX_IDENT && value->args[0]->anno_int==target->anno_int;
	int rhs_is_t = value->args[1]->kind==EX_IDENT && value->args[1]->anno_int==target->anno_int;

	Expr *other;
	if (lhs_is_t)
	{
		other = value->args[1];          /* T op P  ->  home op= P. */
	}
	else if (rhs_is_t && commutative)
	{
		other = value->args[0];          /* P op T, commutative -> home op= P. */
	}
	else
	{
		return 0;
	}

	cg_expr(cg,tt,other);                /* P -> reg 0 (Simd ops touch only reg 0/1). */
	if (ty_simd_bytes(target->type.kind) == 32)
	{
		char yb[8];
		const char *yhome = cg_ymm_alias(home, yb, sizeof yb);
		cg_emit(cg,"    %s %s, %s, ymm0", op, yhome, yhome);   /* vop ymmHome, ymmHome, ymm0. */
	}
	else
	{
		cg_emit(cg,"    %s %s, xmm0", op, home);
	}

	return 1;
}

/* Load such an operand into reg 1 or 2 (xmm for SSE widths, ymm for f64x4).
   Assumes cg_simd_xmm0_safe(e). */
static void cg_simd_load_into(Codegen *cg, TypeTable *tt, Expr *e, int reg)
{
	if (e->kind==EX_IDENT)
	{
		cg_load_local_simd(cg, e->anno_int, reg, e->type.kind);   /* Register home if promoted, else slot. */
		return;
	}

	int w256 = (ty_simd_bytes(e->type.kind) == 32);
	const char *rw = w256 ? "ymm" : "xmm";
	const char *mov = w256 ? "vmovups" : "movups";

	/* Simd.load(a, i): packed load. The fabricated index uses the array element
	   kind so the stride is right. When the base is hoisted and the index
	   register-resident, fold the whole address into the load operand; else
	   compute it into rbx first. */
	Expr ie = {0};
	ie.kind = EX_INDEX;
	ie.lhs = e->args[0];
	ie.rhs = e->args[1];
	ie.type.kind = cg_simd_elem_kind(e->args[0]);
	ie.anno_index_safe = 1;   /* Raw packed primitive: caller guarantees the lane group is in range. */

	char opnd[64];
	if (cg_index_opnd(cg, &ie, opnd))
	{
		cg_emit(cg,"    %s %s%d, %s", mov, rw, reg, opnd);
		return;
	}

	cg_index_addr(cg,tt,&ie);
	cg_emit(cg,"    %s %s%d, [rbx]", mov, rw, reg);
}

/* Combine two packed operands with `op`, leaving the result vector in reg 0. SSE
   widths use the two-operand form (op xmm0, xmm1); f64x4 uses the AVX three-
   operand form (vop ymm0, ymm0, ymm1). The spill-free fast paths apply when one
   operand is xmm0-safe; otherwise the lhs is spilled so a nested rhs can reuse
   reg 0/1. `vk` is the (shared) operand vector kind. */
static void cg_simd_combine(Codegen *cg, TypeTable *tt, Expr *a, Expr *b, const char *op, int commutative, TypeKind vk)
{
	int w256 = (ty_simd_bytes(vk) == 32);
	const char *r0 = w256 ? "ymm0" : "xmm0";
	const char *r1 = w256 ? "ymm1" : "xmm1";

	if (cg_simd_xmm0_safe(b))
	{
		cg_expr(cg,tt,a);                       /* lhs -> reg 0. */
		cg_simd_load_into(cg,tt,b,1);           /* rhs -> reg 1 (reg 0 preserved). */
	}
	else if (commutative && cg_simd_xmm0_safe(a))
	{
		cg_expr(cg,tt,b);                       /* rhs -> reg 0. */
		cg_simd_load_into(cg,tt,a,1);           /* lhs -> reg 1; op is commutative. */
	}
	else
	{
		int bytes = ty_simd_bytes(vk);
		int sb = cg_scratch_alloc(cg, bytes);
		cg_expr(cg,tt,a);                       /* a -> reg 0. */
		cg_emit(cg, w256 ? "    vmovups [rbp - %d], ymm0" : "    movups [rbp - %d], xmm0", sb);
		cg_expr(cg,tt,b);                       /* b -> reg 0. */
		cg_emit(cg, w256 ? "    vmovaps ymm1, ymm0" : "    movaps xmm1, xmm0");
		cg_emit(cg, w256 ? "    vmovups ymm0, [rbp - %d]" : "    movups xmm0, [rbp - %d]", sb);
		cg_scratch_free(cg, bytes);
	}

	if (w256)
	{
		cg_emit(cg,"    %s %s, %s, %s", op, r0, r0, r1);   /* vop ymm0, ymm0, ymm1. */
	}
	else
	{
		cg_emit(cg,"    %s %s, %s", op, r0, r1);
	}
}

/* Horizontally reduce the packed vector in reg 0 to a scalar. Float results land
   in the low lane (x+y for f64x2, x+y+z+w for f32x4/f64x4); the i32x4 sum lands
   in eax. */
static void cg_simd_hreduce(Codegen *cg, TypeKind k)
{
	if (k==TY_I32X4)
	{
		cg_emit(cg,"    phaddd xmm0, xmm0");    /* (x+y, z+w, x+y, z+w). */
		cg_emit(cg,"    phaddd xmm0, xmm0");    /* low lane = x+y+z+w. */
		cg_emit(cg,"    movd eax, xmm0");       /* Scalar int result in eax. */
		return;
	}

	if (k==TY_F32X4)
	{
		cg_emit(cg,"    haddps xmm0, xmm0");    /* (x+y, z+w, x+y, z+w). */
		cg_emit(cg,"    haddps xmm0, xmm0");    /* low lane = x+y+z+w. */
		return;
	}

	if (k==TY_F64X4)
	{
		cg_emit(cg,"    vextractf128 xmm1, ymm0, 1");   /* xmm1 = high pair (z, w). */
		cg_emit(cg,"    vzeroupper");                    /* Done with the upper half. */
		cg_emit(cg,"    addpd xmm0, xmm1");              /* (x+z, y+w). */
		cg_emit(cg,"    movaps xmm1, xmm0");
		cg_emit(cg,"    unpckhpd xmm1, xmm1");           /* xmm1 low = y+w. */
		cg_emit(cg,"    addsd xmm0, xmm1");              /* low lane = x+y+z+w. */
		return;
	}

	cg_emit(cg,"    movaps xmm1, xmm0");
	cg_emit(cg,"    unpckhpd xmm1, xmm1");   /* xmm1 low = y. */
	cg_emit(cg,"    addsd xmm0, xmm1");      /* low lane = x+y. */
}

/* The packed mnemonic for a Simd op name + a vector kind. Float vectors get the
   ss/sd-style suffix (addps/mulpd/…); i32x4 gets the integer packed ops
   (paddd/psubd/pmulld/pminsd/pmaxsd). Caller passes a buffer of at least 8 bytes. */
static const char *cg_simd_op(const char *m, TypeKind k, char *buf)
{
	if (k==TY_I32X4)
	{
		const char *iop = strcmp(m,"add")==0 ? "paddd"
						  : strcmp(m,"sub")==0 ? "psubd"
						  : strcmp(m,"mul")==0 ? "pmulld"
						  : strcmp(m,"min")==0 ? "pminsd" : "pmaxsd";
		snprintf(buf, 8, "%s", iop);
		return buf;
	}

	const char *base = strcmp(m,"add")==0 ? "add"
					   : strcmp(m,"sub")==0 ? "sub"
					   : strcmp(m,"mul")==0 ? "mul"
					   : strcmp(m,"div")==0 ? "div"
					   : strcmp(m,"min")==0 ? "min" : "max";
	/* f64x4 uses the AVX VEX-encoded form (vaddpd…); SSE widths the legacy form. */
	snprintf(buf, 8, "%s%s%s", k==TY_F64X4 ? "v" : "", base, cg_simd_sfx(k));
	return buf;
}

/* Simd.* builtins. A packed f64x2 result lives in the full xmm0 (low lane = x,
   high lane = y); a lane-extract result is a scalar double in xmm0. */
static void cg_simd(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 5;   /* After "Simd.". */
	if (strcmp(m,"pack")==0)
	{
		if (e->type.kind==TY_F64X2)
		{
			int b = cg_scratch_alloc(cg, 16);
			cg_to_double(cg,tt,e->args[0]);                  /* x -> xmm0. */
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);
			cg_to_double(cg,tt,e->args[1]);                  /* y -> xmm0. */
			cg_emit(cg,"    movaps xmm1, xmm0");             /* xmm1 low = y. */
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);
			cg_scratch_free(cg, 16);
			cg_emit(cg,"    unpcklpd xmm0, xmm1");           /* xmm0 = [x | y]. */
			return;
		}

		if (e->type.kind==TY_I32X4)
		{
			/* i32x4: lay four int lanes into a 16-byte scratch, then one load. */
			int b = cg_scratch_alloc(cg, 16);
			for (int i=0; i<4; i++)
			{
				cg_expr(cg,tt,e->args[i]);                   /* lane i (int) -> eax. */
				cg_emit(cg,"    mov dword [rbp - %d], eax", b - i*4);
			}

			cg_emit(cg,"    movups xmm0, [rbp - %d]", b);
			cg_scratch_free(cg, 16);
			return;
		}

		/* f32x4: lay the four float lanes into a 16-byte scratch, then one load. */
		int b = cg_scratch_alloc(cg, 16);
		for (int i=0; i<4; i++)
		{
			cg_expr(cg,tt,e->args[i]);                       /* lane i -> xmm0 low. */
			if (e->args[i]->type.kind==TY_DOUBLE)
			{
				cg_emit(cg,"    cvtsd2ss xmm0, xmm0");        /* Narrow a double lane to float. */
			}

			cg_emit(cg,"    movss dword [rbp - %d], xmm0", b - i*4);
		}

		cg_emit(cg,"    movups xmm0, [rbp - %d]", b);
		cg_scratch_free(cg, 16);
		return;
	}

	if (strcmp(m,"pack256")==0)
	{
		/* f64x4: lay four double lanes into a 32-byte scratch, then one ymm load. */
		int b = cg_scratch_alloc(cg, 32);
		for (int i=0; i<4; i++)
		{
			cg_to_double(cg,tt,e->args[i]);              /* lane i -> xmm0 low. */
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b - i*8);
		}

		cg_emit(cg,"    vmovups ymm0, [rbp - %d]", b);
		cg_scratch_free(cg, 32);
		return;
	}

	if (strcmp(m,"x")==0 || strcmp(m,"y")==0 || strcmp(m,"z")==0 || strcmp(m,"w")==0)
	{
		cg_expr(cg,tt,e->args[0]);                       /* Packed value -> reg 0. */
		int lane = strcmp(m,"x")==0 ? 0 : strcmp(m,"y")==0 ? 1 : strcmp(m,"z")==0 ? 2 : 3;

		if (e->args[0]->type.kind==TY_F64X4)
		{
			if (lane >= 2)
			{
				cg_emit(cg,"    vextractf128 xmm0, ymm0, 1");   /* xmm0 = (lane2, lane3). */
			}

			cg_emit(cg,"    vzeroupper");                        /* Back to SSE; avoid the transition penalty. */
			if (lane & 1)
			{
				cg_emit(cg,"    unpckhpd xmm0, xmm0");           /* Odd lane: high of the pair into low. */
			}

			return;
		}

		if (e->args[0]->type.kind==TY_I32X4)
		{
			if (lane != 0)
			{
				cg_emit(cg,"    pshufd xmm0, xmm0, %d", lane);   /* Bring int lane into lane 0. */
			}

			cg_emit(cg,"    movd eax, xmm0");                    /* Scalar int lane -> eax. */
			return;
		}

		if (e->args[0]->type.kind==TY_F64X2)
		{
			if (lane==1)
			{
				cg_emit(cg,"    unpckhpd xmm0, xmm0");           /* High f64 lane into the low half. */
			}
		}
		else if (lane != 0)
		{
			cg_emit(cg,"    shufps xmm0, xmm0, %d", lane);       /* Bring f32 lane into lane 0. */
		}

		return;
	}

	if (strcmp(m,"load")==0)
	{
		/* Address of element i via a fabricated arr[i] index. The packed load reads
		   the whole lane group; the caller guarantees it is in range (raw primitive,
		   so anno_index_safe skips the bounds check). */
		Expr ie = {0};
		ie.kind = EX_INDEX;
		ie.lhs = e->args[0];
		ie.rhs = e->args[1];
		ie.type.kind = cg_simd_elem_kind(e->args[0]);
		ie.anno_index_safe = 1;
		char lopnd[64];
		if (cg_index_opnd(cg, &ie, lopnd))
		{
			cg_emit(cg,"    movups xmm0, %s", lopnd);    /* Folded address. */
		}
		else
		{
			cg_index_addr(cg,tt,&ie);                    /* rbx = &a[i]. */
			cg_emit(cg,"    movups xmm0, [rbx]");
		}

		return;
	}

	if (strcmp(m,"store")==0)
	{
		Expr ie = {0};
		ie.kind = EX_INDEX;
		ie.lhs = e->args[0];
		ie.rhs = e->args[1];
		ie.type.kind = cg_simd_elem_kind(e->args[0]);
		ie.anno_index_safe = 1;

		/* If the address folds, no spill is needed: evaluate v into xmm0 and store
		   it straight to the folded operand. Otherwise park v while computing the
		   address into rbx. */
		char sopnd[64];
		if (cg_index_opnd(cg, &ie, sopnd))
		{
			cg_expr(cg,tt,e->args[2]);                   /* v -> xmm0. */
			cg_emit(cg,"    movups %s, xmm0", sopnd);
			return;
		}

		int b = cg_scratch_alloc(cg, 16);
		cg_expr(cg,tt,e->args[2]);                       /* v -> xmm0. */
		cg_emit(cg,"    movups [rbp - %d], xmm0", b);     /* Park v across the address computation. */
		cg_index_addr(cg,tt,&ie);                        /* rbx = &a[i]. */
		cg_emit(cg,"    movups xmm1, [rbp - %d]", b);
		cg_scratch_free(cg, 16);
		cg_emit(cg,"    movups [rbx], xmm1");
		return;
	}

	if (strcmp(m,"load256")==0)
	{
		/* f64x4 load: four double lanes (stride 8), one ymm move, folded when the
		   base is hoisted. */
		Expr ie = {0};
		ie.kind = EX_INDEX;
		ie.lhs = e->args[0];
		ie.rhs = e->args[1];
		ie.type.kind = TY_DOUBLE;
		ie.anno_index_safe = 1;
		char lopnd[64];
		if (cg_index_opnd(cg, &ie, lopnd))
		{
			cg_emit(cg,"    vmovups ymm0, %s", lopnd);
		}
		else
		{
			cg_index_addr(cg,tt,&ie);
			cg_emit(cg,"    vmovups ymm0, [rbx]");
		}

		return;
	}

	if (strcmp(m,"store256")==0)
	{
		Expr ie = {0};
		ie.kind = EX_INDEX;
		ie.lhs = e->args[0];
		ie.rhs = e->args[1];
		ie.type.kind = TY_DOUBLE;
		ie.anno_index_safe = 1;
		char sopnd[64];
		if (cg_index_opnd(cg, &ie, sopnd))
		{
			cg_expr(cg,tt,e->args[2]);                   /* v -> ymm0. */
			cg_emit(cg,"    vmovups %s, ymm0", sopnd);
			return;
		}

		int b = cg_scratch_alloc(cg, 32);
		cg_expr(cg,tt,e->args[2]);                       /* v -> ymm0. */
		cg_emit(cg,"    vmovups [rbp - %d], ymm0", b);
		cg_index_addr(cg,tt,&ie);                        /* rbx = &a[i]. */
		cg_emit(cg,"    vmovups ymm1, [rbp - %d]", b);
		cg_scratch_free(cg, 32);
		cg_emit(cg,"    vmovups [rbx], ymm1");
		return;
	}

	if (strcmp(m,"sum")==0)
	{
		cg_expr(cg,tt,e->args[0]);                       /* Vector -> reg 0. */
		cg_simd_hreduce(cg, e->args[0]->type.kind);
		return;
	}

	if (strcmp(m,"dot")==0)
	{
		TypeKind vk = e->args[0]->type.kind;
		char mul[8];
		cg_simd_op("mul", vk, mul);                              /* mulpd/mulps/pmulld/vmulpd. */
		cg_simd_combine(cg,tt,e->args[0],e->args[1],mul,1,vk);   /* a*b vector -> reg 0. */
		cg_simd_hreduce(cg, vk);
		return;
	}

	/* Element-wise add/sub/mul/div/min/max on two packed vectors of e->type.kind.
	   add/mul/min/max are commutative; the in-place accumulate (cg_try_simd_inplace)
	   already handled the hot reduction case before reaching here. */
	char op[8];
	cg_simd_op(m, e->type.kind, op);
	int commutative = strcmp(m,"add")==0 || strcmp(m,"mul")==0 || strcmp(m,"min")==0 || strcmp(m,"max")==0;
	cg_simd_combine(cg,tt,e->args[0],e->args[1],op,commutative,e->type.kind);
}

static void cg_clock(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 6;   /* After "Clock.". */
	if (strcmp(m,"getDateString")==0)
	{
		const char *fn = e->arg_count==2 ? "bzy_clock_date_fmt" : "bzy_clock_date";
		TypeRef ps[2];
		for (int i=0; i<e->arg_count; i++)
		{
			ps[i]=e->args[i]->type;
		}

		cg_call_with_args(cg,tt,fn,NULL,e->args,e->arg_count,0, 1, 0, ps, e->arg_count, 0);
		return;
	}

	cg_aligned_call(cg, strcmp(m,"currentTimeNanos")==0 ? "bzy_clock_nanos" : "bzy_clock_millis");
}

/* Regex.* builtins -> bzy_regex_* (string args, bool or owned-string result). */
static void cg_regex(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 6;   /* After "Regex.". */
	const char *fn =
		strcmp(m,"matches")==0 ? "bzy_regex_matches" :
		strcmp(m,"test")==0    ? "bzy_regex_test" :
		strcmp(m,"find")==0    ? "bzy_regex_find" :
		"bzy_regex_replace";
	TypeRef ps[3];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	cg_call_with_args(cg,tt,fn,NULL,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), 0, ps, e->arg_count, 0);
}

/* File.* builtins. Selects the bzy_file_* symbol, lowers via cg_call_with_args,
   and for fallible ops emits a post-call bzy_io_check(pc, frame) that throws an
   IOException if the op set the runtime error (the value result, if any, is
   preserved across the check). Predicates never fail and get no check. */
static void cg_file(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 5;   /* After "File.". */

	/* openWrite/openAppend carry an implicit append flag + an optional bufferBytes,
	   so they don't fit the generic cg_call_with_args path; lower them by hand
	   (mirrors cg_system) then emit the fallible io_check (owned-object result). */
	if (strcmp(m,"openWrite")==0 || strcmp(m,"openAppend")==0)
	{
		long long append = (strcmp(m,"openAppend")==0) ? 1 : 0;
		if (e->arg_count==2)
		{
			cg_expr(cg,tt,e->args[1]);            /* bufferBytes -> rax. */
			cg_temp_push(cg);
			cg_expr(cg,tt,e->args[0]);            /* path -> rax. */
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_temp_pop_reg(cg,"r8");
		}
		else
		{
			cg_expr(cg,tt,e->args[0]);            /* path -> rax. */
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_emit(cg,"    mov %s, 0", cg_iarg(cg, 2));           /* bufferBytes = 0 -> runtime default. */
		}

		cg_emit(cg,"    mov %s, %lld", cg_iarg(cg, 1), append);
		int owned = expr_is_owned(e->args[0]);
		if (owned)
		{
			cg_emit(cg,"    mov [rbp - %d], %s", cg->val_save, cg_iarg(cg, 0));   /* Save path for release (rcx dies in the call). */
		}

		cg_aligned_call(cg,"bzy_filewriter_open");   /* Owned FileWriter -> rax. */
		if (owned)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
			cg_temp_push(cg);                 /* Preserve the writer across the path release. */
			cg_release_rcx(cg);
			cg_temp_pop(cg);
		}

		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the writer across io_check. */
		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		return;
	}

	const char *fn;
	int fallible;
	if (strcmp(m,"exists")==0)
	{
		fn="bzy_file_exists";
		fallible=0;
	}
	else if (strcmp(m,"isFile")==0)
	{
		fn="bzy_file_is_file";
		fallible=0;
	}
	else if (strcmp(m,"isFolder")==0)
	{
		fn="bzy_file_is_folder";
		fallible=0;
	}
	else if (strcmp(m,"createFile")==0)
	{
		fn="bzy_file_create_file";
		fallible=1;
	}
	else if (strcmp(m,"createFolder")==0)
	{
		fn="bzy_file_create_folder";
		fallible=1;
	}
	else if (strcmp(m,"delete")==0)
	{
		fn="bzy_file_delete";
		fallible=1;
	}
	else if (strcmp(m,"deleteRecursive")==0)
	{
		fn="bzy_file_delete_recursive";
		fallible=1;
	}
	else if (strcmp(m,"readText")==0)
	{
		fn="bzy_file_read_text";
		fallible=1;
	}
	else if (strcmp(m,"readLines")==0)
	{
		fn="bzy_file_read_lines";
		fallible=1;
	}
	else if (strcmp(m,"writeText")==0)
	{
		fn="bzy_file_write_text";
		fallible=1;
	}
	else if (strcmp(m,"appendText")==0)
	{
		fn="bzy_file_append_text";
		fallible=1;
	}
	else if (strcmp(m,"readBytes")==0)
	{
		fn="bzy_file_read_bytes";
		fallible=1;
	}
	else if (strcmp(m,"openChannel")==0)
	{
		fn="bzy_filechannel_open";
		fallible=1;
	}
	else if (strcmp(m,"writeBytes")==0)
	{
		fn="bzy_file_write_bytes";
		fallible=1;
	}
	else if (strcmp(m,"list")==0)
	{
		fn="bzy_file_list";
		fallible=1;
	}
	else if (strcmp(m,"search")==0)
	{
		fn="bzy_file_search";
		fallible=1;
	}
	else if (strcmp(m,"searchRecursive")==0)
	{
		fn="bzy_file_search_recursive";
		fallible=1;
	}
	else if (strcmp(m,"setAttribute")==0)
	{
		fn="bzy_file_set_attribute";
		fallible=1;
	}
	else
	{
		fn="bzy_file_has_attribute";
		fallible=0;
	}

	TypeRef ps[4];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	int obj = ty_is_managed(e->type.kind);
	cg_call_with_args(cg,tt,fn,NULL,e->args,e->arg_count,0, obj, 0, ps, e->arg_count, 0);

	if (fallible)
	{
		if (obj)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the result. */
		}

		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);                  /* pc = the call site. */
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));                            /* frame. */
		cg_aligned_call(cg,"bzy_io_check");
		if (obj)
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
}

/* Random.* builtins. Selects the typed bzy_rnd_* symbol from the method + arg
   types and delegates to cg_call_with_args (int/fp routing + owned-temp release). */
static void cg_random(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *m = e->name + 7;   /* After "Random.". */
	const char *fn;
	TypeRef ps[2];
	int np = 0;

	if (strcmp(m,"nextBool")==0)
	{
		fn="bzy_rnd_bool";
	}
	else if (strcmp(m,"nextInt")==0)
	{
		fn="bzy_rnd_int";
	}
	else if (strcmp(m,"nextLong")==0)
	{
		fn="bzy_rnd_long";
	}
	else if (strcmp(m,"nextFloat")==0)
	{
		fn="bzy_rnd_float";
	}
	else if (strcmp(m,"nextDouble")==0)
	{
		fn="bzy_rnd_double";
	}
	else if (strcmp(m,"nextGaussian")==0)
	{
		fn="bzy_rnd_gaussian";
	}
	else if (strcmp(m,"nextBytes")==0)
	{
		fn="bzy_rnd_bytes";
		ps[0]=e->args[0]->type;            /* The byte[]. */
		np=1;
	}
	else   /* get */
	{
		TypeKind k=e->args[0]->type.kind;
		const char *suffix =
			k==TY_LONG   ? (e->arg_count==2 ? "ll" : "l") :
			k==TY_FLOAT  ? (e->arg_count==2 ? "ff" : "f") :
			k==TY_DOUBLE ? (e->arg_count==2 ? "dd" : "d") :
			(e->arg_count==2 ? "ii" : "i");
		static char buf[24];
		snprintf(buf,sizeof buf,"bzy_rnd_get_%s",suffix);
		fn=buf;
		for (int i=0; i<e->arg_count; i++)
		{
			ps[i]=e->args[i]->type;
		}

		np=e->arg_count;
	}

	cg_call_with_args(cg,tt,fn,NULL,e->args,e->arg_count,0,
					  ty_is_managed(e->type.kind), ty_is_float(e->type.kind), ps, np, 0);
}

static void cg_expr(Codegen *cg, TypeTable *tt, Expr *e)
{
	/* Capture and clear the one-hop low-32 hint a parent may have set for this
	   operand, so it reaches this node only - never a grandchild (which would
	   re-consume rax at full width). Only the binary case acts on it. */
	int want_low32 = cg->low32_ok;
	cg->low32_ok = 0;

	/* Inside an unrolled copy, any integer arithmetic over copy-constant locals
	   is itself a constant: materialize it instead of computing it (cb * 100,
	   cbase + xp, ...). Literals and bare ident reads already have fast paths. */
	if (cg->unrolling && (e->kind == EX_BINARY || e->kind == EX_CAST))
	{
		long long ucv;
		if (cg_fold_const(cg, e, &ucv))
		{
			cg_emit(cg,"    mov rax, %lld", ucv);
			return;
		}
	}

	switch (e->kind)
	{
	case EX_INT:
		cg_emit(cg,"    mov rax, %lld", e->int_val);
		break;
	case EX_BOOL:
		cg_emit(cg,"    mov rax, %lld", e->int_val);
		break;
	case EX_NULL:
		cg_emit(cg,"    mov rax, 0");   /* null is a bare 0 pointer (never retained/released). */
		break;
	case EX_NEWARRAY:
		cg_expr(cg,tt,e->lhs);             /* Count -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), cg_elem_stride(e->type.elem->kind));
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 2), ty_is_managed(e->type.elem->kind) ? 1 : 0);
		cg_aligned_call(cg,"bzy_array_new_sized");   /* Owned (+1) array in rax. */
		break;
	case EX_NEWMAP:
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_map_key_kind(tt, e->type.elem));
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), ty_is_managed(e->type.elem2->kind) ? 1 : 0);
		cg_aligned_call(cg,"bzy_map_new");   /* Owned (+1) map in rax. */
		break;
	case EX_NEWCHANNEL:
		cg_expr(cg,tt,e->args[0]);           /* Capacity -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), ty_is_managed(e->type.elem->kind) ? 1 : 0);
		cg_aligned_call(cg,"bzy_channel_new");   /* Owned (+1) channel in rax. */
		break;
	case EX_NEWGEN:
		if (strcmp(e->type.class_name,"Box")==0)
		{
			cg_emit(cg,"    mov %s, 1", cg_iarg(cg, 0));
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), cg_elem_stride(e->type.elem->kind));
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 2), ty_is_managed(e->type.elem->kind) ? 1 : 0);
			cg_aligned_call(cg,"bzy_array_new_sized");   /* Box = length-1 array. */
		}
		else if (strcmp(e->type.class_name,"Set")==0)
		{
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_map_key_kind(tt, e->type.elem));        /* key_kind. */
			cg_emit(cg,"    mov %s, 0", cg_iarg(cg, 1));                                            /* Values unmanaged. */
			cg_aligned_call(cg,"bzy_map_new");
		}
		else if (strcmp(e->type.class_name,"PriorityQueue")==0)
		{
			int slot = -1;   /* compareTo vtable slot for object keys; -1 for primitives. */
			if (e->type.elem->kind==TY_OBJECT)
			{
				ClassInfo *ci = types_find_class(tt, e->type.elem->class_name);
				MethodInfo *cm = ci ? types_find_method(ci, "compareTo") : (MethodInfo*)0;
				slot = cm ? cm->vtable_slot : -1;
			}

			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_elem_kind(e->type.elem->kind));
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), slot);
			cg_aligned_call(cg,"bzy_pq_new");
		}
		else if (strcmp(e->type.class_name,"TreeSet")==0 || strcmp(e->type.class_name,"TreeMap")==0)
		{
			int is_map = strcmp(e->type.class_name,"TreeMap")==0;
			int slot = -1;   /* compareTo vtable slot for object keys; -1 otherwise. */
			if (e->type.elem->kind==TY_OBJECT)
			{
				ClassInfo *ci = types_find_class(tt, e->type.elem->class_name);
				MethodInfo *cm = ci ? types_find_method(ci, "compareTo") : (MethodInfo*)0;
				slot = cm ? cm->vtable_slot : -1;
			}

			int vman = is_map && ty_is_managed(e->type.elem2->kind) ? 1 : 0;
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_elem_kind(e->type.elem->kind));   /* kkind. */
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), is_map ? 1 : 0);                     /* has_values. */
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 2), vman);                               /* vman. */
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 3), slot);                               /* obj_slot. */
			cg_aligned_call(cg,"bzy_btree_new");
		}
		else   /* List / Stack / Queue / Deque / ArrayDeque -> vector. */
		{
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_elem_kind(e->type.elem->kind));
			cg_aligned_call(cg,"bzy_vec_new");
		}

		break;
	case EX_LAMBDA:
	{
		/* Build the closure object: [vtable | rc | gcinfo | code_ptr@24 | caps@32+].
		   bzy_alloc zeroes it and sets rc=1 + the gcinfo class nibble; the captures
		   are snapshotted from the enclosing frame (managed ones retained). */
		LambdaInfo *lam = e->lam;
		int cc = lam->cap_count;
		if (cc == 0)
		{
			/* No captures -> the closure is stateless: allocate it once and cache it
			   in a static slot, so re-evaluating the lambda (e.g. each loop turn)
			   costs only a load, not an allocation. */
			int done = cg_label(cg);
			cg_emit(cg,"    mov rax, [rel __%s_single]", lam->label);
			cg_emit(cg,"    test rax, rax");
			cg_emit(cg,"    jnz .L%d", done);
			cg_emit(cg,"    mov %s, 32", cg_iarg(cg, 0));
			cg_aligned_call(cg,"bzy_alloc");
			cg_emit(cg,"    lea rcx, [rel __%s_vt]", lam->label);
			cg_emit(cg,"    mov [rax], rcx");
			cg_emit(cg,"    lea rcx, [rel %s]", lam->label);
			cg_emit(cg,"    mov [rax + 24], rcx");
			cg_emit(cg,"    mov [rel __%s_single], rax", lam->label);
			cg_emit(cg,".L%d:", done);
			cg_retain_rax(cg);   /* Return +1; the cache keeps the alloc's original ref, so rc never hits 0. */
			break;
		}

		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), 32 + cc * 8);
		cg_aligned_call(cg,"bzy_alloc");
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_emit(cg,"    lea rcx, [rel __%s_vt]", lam->label);
		cg_emit(cg,"    mov [rax], rcx");                  /* vtable -> typeinfo descriptor. */
		cg_emit(cg,"    lea rcx, [rel %s]", lam->label);
		cg_emit(cg,"    mov [rax + 24], rcx");             /* code pointer. */
		for (int i = 0; i < cc; i++)
		{
			LambdaCap *c = &lam->caps[i];
			if (ty_is_float(c->type.kind))
			{
				const char *xr = (c->type.kind==TY_DOUBLE) ? cg_local_xmm(cg, c->src_offset) : NULL;
				if (xr)
				{
					cg_emit(cg,"    movq rax, %s", xr);
				}
				else
				{
					cg_emit(cg,"    mov rax, [rbp - %d]", c->src_offset);
				}
			}
			else
			{
				const char *r = cg_local_reg(cg, c->src_offset);
				if (r)
				{
					cg_emit(cg,"    mov rax, %s", r);
				}
				else
				{
					cg_emit(cg,"    mov rax, [rbp - %d]", c->src_offset);
				}
			}

			if (c->is_managed)
			{
				cg_retain_rax(cg);                         /* The env owns a reference. */
			}

			cg_emit(cg,"    mov rdx, [rbp - %d]", b);       /* Reload closure (retain may clobber). */
			cg_emit(cg,"    mov [rdx + %d], rax", c->env_offset);
		}

		cg_emit(cg,"    mov rax, [rbp - %d]", b);           /* Result: the closure object. */
		cg_scratch_free(cg, 16);
		break;
	}
	case EX_INDEX:
	{
		if (e->anno_shared_gate)
		{
			/* Managed element of a maybe-shared array: the load and its retain
			   must be atomic against a concurrent overwrite's release when the
			   array crossed cores. Both paths produce an OWNED element (see
			   expr_is_owned), so consumers treat this read like a call. */
			cg_index_addr_based(cg,tt,e);                /* rax = base, rbx = slot. */
			int sh = cg_label(cg), join = cg_label(cg);
			cg_emit(cg,"    test qword [rax + 16], 8");  /* BZY_GCINFO_SHARED. */
			cg_emit(cg,"    jnz .L%d", sh);
			cg_emit(cg,"    mov rax, [rbx]");            /* Confined: plain load... */
			cg_retain_rax(cg);                           /* ...and plain retain. */
			cg_emit(cg,"    jmp .L%d", join);
			cg_emit(cg,".L%d:", sh);
			cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
			cg_aligned_call(cg,"bzy_array_get_shared");  /* Locked load+retain (owned). */
			cg_emit(cg,".L%d:", join);
			break;
		}

		char mode[40];
		if (cg_sr_mode(cg, e, mode))   /* Strength-reduced: load straight from [base + iv*stride], no lea. */
		{
			if (ty_is_float(e->type.kind))
			{
				cg_load_fp(cg,e->type.kind,mode);
			}
			else
			{
				cg_load_scalar(cg,e->type.kind,mode);
			}

			break;
		}

		/* Unrolled copy with a foldable address: load straight from a direct
		   memory operand - no lea, no index materialization. */
		char imem[64];
		if (!ty_is_float(e->type.kind) && cg_index_mem(cg, e, imem, "rcx"))
		{
			cg_load_scalar(cg,e->type.kind,imem);
			break;
		}

		/* FP element whose address is a single register-built mode: fold base,
		   scaled index, and header displacement into the movsd itself. */
		if (ty_is_float(e->type.kind) && cg_index_opnd(cg, e, imem))
		{
			cg_load_fp(cg,e->type.kind,imem);
			break;
		}

		/* Integer element, register-resident base + bare index: emit the bounds
		   check and fold the address into the load - no lea-into-rbx. */
		if (!ty_is_float(e->type.kind) && cg_index_checked_opnd(cg, tt, e, imem, sizeof imem))
		{
			cg_load_scalar(cg,e->type.kind,imem);
			break;
		}

		cg_index_addr(cg,tt,e);
		if (ty_is_float(e->type.kind))
		{
			cg_load_fp(cg,e->type.kind,"[rbx]");
		}
		else
		{
			cg_load_scalar(cg,e->type.kind,"[rbx]");   /* Reads the element's natural width, sign/zero-extended. */
		}

		break;
	}
	case EX_STR:
	{
		int id=cg_str_const(cg,e);
		cg_emit(cg,"    lea %s, [rel __str%d]", cg_iarg(cg, 0), id);
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), cg->strk[id].len);
		cg_aligned_call(cg,"bzy_str_new");   /* Owned (+1) string in rax. */
		break;
	}
	case EX_FLOAT:
	{
		int id=cg_fp_const(cg,e);
		char mem[40];
		sprintf(mem,"[rel __fpk%d]", id);
		cg_load_fp(cg,e->type.kind,mem);
		break;
	}
	case EX_CAST:
	{
		cg_expr(cg,tt,e->lhs);
		TypeKind from=e->lhs->type.kind, to=e->type.kind;
		int ff=ty_is_float(from), tf=ty_is_float(to);
		if (!ff && !tf)
		{
			cg_extend_reg(cg,to);                  /* int -> int (truncate/re-extend). */
		}
		else if (!ff && tf)
		{
			cg_emit(cg, to==TY_FLOAT ? "    cvtsi2ss xmm0, rax" : "    cvtsi2sd xmm0, rax");
		}
		else if (ff && !tf)
		{
			cg_emit(cg, from==TY_FLOAT ? "    cvttss2si rax, xmm0" : "    cvttsd2si rax, xmm0");
			cg_extend_reg(cg,to);                  /* Narrow the truncated integer to its width. */
		}
		else if (from==TY_FLOAT && to==TY_DOUBLE)
		{
			cg_emit(cg,"    cvtss2sd xmm0, xmm0");
		}
		else if (from==TY_DOUBLE && to==TY_FLOAT)
		{
			cg_emit(cg,"    cvtsd2ss xmm0, xmm0");
		}

		break;
	}
	case EX_THIS:
		cg_emit(cg,"    mov rax, [rbp - 8]");
		break;
	case EX_IDENT:
	{
		char mem[32];
		sprintf(mem,"[rbp - %d]", e->anno_int);
		if (ty_is_simd(e->type.kind))
		{
			cg_load_local_simd(cg, e->anno_int, 0, e->type.kind);   /* XMM home if promoted, else 16-byte slot. */
		}
		else if (ty_is_float(e->type.kind))
		{
			const char *xr = e->type.kind==TY_DOUBLE ? cg_local_xmm(cg, e->anno_int) : NULL;
			if (xr)
			{
				cg_emit(cg,"    movaps xmm0, %s", xr);   /* Promoted double: read from its XMM home (movaps: no upper-half merge dependency). */
			}
			else
			{
				cg_load_fp(cg,e->type.kind,mem);
			}
		}
		else
		{
			const char *r = cg_local_reg(cg, e->anno_int);
			const char *h = r ? NULL : cg_hoist_reg(cg, e->anno_int);
			long long ucv;
			if (cg->unrolling && e->anno_int > 0 && cg_unroll_const(cg, e->anno_int, &ucv))
			{
				cg_emit(cg,"    mov rax, %lld", ucv);   /* Unrolled loop: a copy-constant local (induction variable or derived). */
			}
			else if (r)
			{
				cg_emit(cg,"    mov rax, %s", r);   /* Promoted local: read from its register. */
			}
			else if (h)
			{
				cg_emit(cg,"    mov rax, %s", h);   /* Loop-invariant local: read from its hoist register. */
			}
			else
			{
				cg_load_scalar(cg,e->type.kind,mem);
			}
		}

		break;
	}
	case EX_FIELD:
	{
		/* Enum constant (Color.RED): load the singleton slot and retain (owned).
		   Preserve rax across the call via the stack, NOT val_save -- this load can
		   appear as a method-call receiver, where cg_call_with_args parks the callee
		   address in val_save while evaluating the receiver. */
		if (e->lhs->kind==EX_IDENT && enum_is(e->lhs->name))
		{
			cg_emit(cg,"    mov rax, [rel __enum_%s_%s]", e->lhs->name, e->name);
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_temp_push(cg);
			cg_aligned_call(cg,"bzy_retain");
			cg_temp_pop(cg);
			break;
		}

		/* Static field (C.total): load from the global slot; retain if managed
		   (owned, stack-preserved like the enum load). anno_str = class. */
		if (e->anno_int == -1)
		{
			char smem[192];
			snprintf(smem,sizeof(smem),"[rel __static_%s_%s]", e->anno_str, e->name);
			if (ty_is_float(e->type.kind))
			{
				cg_load_fp(cg,e->type.kind,smem);
			}
			else
			{
				cg_load_scalar(cg,e->type.kind,smem);
			}

			if (ty_is_managed(e->type.kind))
			{
				cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
				cg_temp_push(cg);
				cg_aligned_call(cg,"bzy_retain");
				cg_temp_pop(cg);
			}

			break;
		}

		/* JsonValue.size is kind-dependent (array length / object key count), so it
		   lowers to a runtime call rather than a raw offset load. */
		if (e->lhs->type.kind==TY_JSONVALUE)
		{
			cg_call_with_args(cg,tt,"bzy_json_size",e->lhs,NULL,0,0,0,0,NULL,0,0);
			break;
		}

		cg_expr(cg,tt,e->lhs);
		char mem[32];
		sprintf(mem,"[rax + %d]", e->anno_int);
		if (ty_is_float(e->type.kind))
		{
			cg_load_fp(cg,e->type.kind,mem);
		}
		else
		{
			cg_load_scalar(cg,e->type.kind,mem);
		}

		break;
	}
	case EX_UNARY:
		cg_expr(cg,tt,e->lhs);
		if (e->op==TOKEN_NOT)
		{
			cg_emit(cg,"    cmp rax, 0");
			cg_emit(cg,"    sete al");
			cg_emit(cg,"    movzx rax, al");
			break;
		}

		if (ty_is_float(e->type.kind))
		{
			const char *sfx = e->type.kind==TY_FLOAT ? "ss" : "sd";
			cg_emit(cg,"    xorps xmm1, xmm1");
			cg_emit(cg,"    sub%s xmm1, xmm0", sfx);   /* 0 - x = -x. */
			cg_emit(cg,"    movaps xmm0, xmm1");
		}
		else
		{
			cg_emit(cg, e->op==TOKEN_TILDE ? "    not rax" : "    neg rax");
			cg_extend_reg(cg,e->type.kind);
		}

		break;
	case EX_BINARY:
		cg_binary(cg,tt,e,want_low32);
		break;
	case EX_INCDEC:
		cg_expr(cg,tt,e->lhs);                 /* Current value -> rax. */
		cg_emit(cg, e->op==TOKEN_PLUSPLUS ? "    add rax, 1" : "    sub rax, 1");
		cg_extend_reg(cg,e->type.kind);        /* Re-extend to the declared width. */
		cg_store_local_off(cg, e->lhs->anno_int, e->lhs->type.kind);   /* Register home if promoted, else slot. */
		break;
	case EX_NEW:
		cg_new(cg,tt,e);
		break;
	case EX_METHOD_CALL:
		if (e->anno_int==-1)   /* Static method call (C.method(args)): no `this`. */
		{
			MethodInfo *sm=types_find_method_idx(types_find_class(tt,e->anno_str),e->name,e->anno_overload);
			cg_call_with_args(cg,tt,sm->asm_label,NULL,e->args,e->arg_count,0,
							  ty_is_managed(e->type.kind), ty_is_float(e->type.kind),
							  sm->param_types, sm->param_count, 0);
			break;
		}

		if (e->lhs->kind==EX_IDENT && enum_is(e->lhs->name))
		{
			cg_enum_static(cg,tt,e);   /* Enum.values() / Enum.valueOf(s). */
			break;
		}

		if (e->lhs->type.kind==TY_OBJECT && enum_is(e->lhs->type.class_name)
				&& (strcmp(e->name,"name")==0 || strcmp(e->name,"ordinal")==0))
		{
			cg_enum_instance(cg,tt,e);
			break;
		}

		if (e->lhs->type.kind==TY_GENERIC)
		{
			if (strcmp(e->lhs->type.class_name,"Box")==0)
			{
				cg_box_method(cg,tt,e);
			}
			else if (strcmp(e->lhs->type.class_name,"Set")==0)
			{
				cg_set_method(cg,tt,e);
			}
			else if (strcmp(e->lhs->type.class_name,"PriorityQueue")==0)
			{
				cg_pqueue_method(cg,tt,e);
			}
			else if (strcmp(e->lhs->type.class_name,"TreeSet")==0
					 || strcmp(e->lhs->type.class_name,"TreeMap")==0)
			{
				cg_btree_method(cg,tt,e);
			}
			else
			{
				cg_collection_method(cg,tt,e);
			}
		}
		else if (e->lhs->type.kind==TY_MAP)
		{
			cg_map_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_ENTRY)
		{
			cg_entry_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_CHANNEL)
		{
			cg_channel_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_TIMER)
		{
			cg_timer_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_LISTENER || e->lhs->type.kind==TY_SOCKET
				 || e->lhs->type.kind==TY_UDPSOCKET || e->lhs->type.kind==TY_DATAGRAM)
		{
			cg_net_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_TLSLISTENER)
		{
			cg_tls_listener_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_TLSSOCKET)
		{
			cg_tls_socket_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_SURFACE)
		{
			cg_surface_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_GLSURFACE)
		{
			cg_glsurface_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_FILECHANNEL)
		{
			cg_filechannel_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_XMLNODE)
		{
			cg_xml_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_JSONVALUE)
		{
			cg_json_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_HTTPREQUEST || e->lhs->type.kind==TY_HTTPRESPONSE)
		{
			cg_http_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_MAPPEDFILE)
		{
			cg_mappedfile_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_FILEWRITER)
		{
			cg_filewriter_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_LOGGER)
		{
			cg_logger_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_OBJECT && strcmp(e->lhs->type.class_name,"StringBuilder")==0)
		{
			cg_sb_method(cg,tt,e);
		}
		else if (e->lhs->type.kind==TY_STRING)
		{
			cg_string_method(cg,tt,e);
		}
		else if (strcmp(e->name,"getClassName")==0
				 && !types_find_method(types_find_class(tt,e->lhs->type.class_name),"getClassName"))
		{
			/* Builtin: dynamic class name. Read it from the receiver's actual vtable. */
			cg_expr(cg,tt,e->lhs);                       /* Receiver -> rax. */
			int owned = expr_is_owned(e->lhs);
			if (owned)
			{
				cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
			}

			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_aligned_call(cg,"bzy_class_name");        /* Owned (+1) string in rax. */
			if (owned)
			{
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
				cg_temp_push(cg);               /* Preserve the string across the release. */
				cg_release_rcx(cg);
				cg_temp_pop(cg);
			}
		}
		else
		{
			cg_method_call(cg,tt,e);
		}

		break;
	case EX_CALL:
		if (e->anno_indirect)
		{
			cg_closure_call(cg,tt,e);   /* Calling a function value through its code pointer. */
		}
		else if (strcmp(e->name,"print")==0)
		{
			cg_print(cg,tt,e);
		}
		else if (strcmp(e->name,"input")==0)
		{
			cg_aligned_call(cg,"bzy_input_line");   /* Owned (+1) string in rax. */
		}
		else if (strcmp(e->name,"liveCount")==0)
		{
			cg_emit(cg,"    call bzy_live_count");
		}
		else if (strcmp(e->name,"collectCycles")==0)
		{
			cg_emit(cg,"    call bzy_collect_cycles");
		}
		else if (strcmp(e->name,"yield")==0)
		{
			cg_aligned_call(cg,"bzy_yield");
		}
		else if (strcmp(e->name,"length")==0)
		{
			cg_expr(cg,tt,e->args[0]);
			int owned = expr_is_owned(e->args[0]);
			if (owned)
			{
				cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Save the string pointer. */
			}

			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_aligned_call(cg,"bzy_str_len");   /* Length (int) in rax. */
			if (owned)
			{
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
				cg_temp_push(cg);        /* Preserve the length across the release. */
				cg_release_rcx(cg);
				cg_temp_pop(cg);
			}
		}
		else if (strcmp(e->name,"fromCString")==0 || strcmp(e->name,"fromCBytes")==0)
		{
			/* Copy a C char* into an owned Breezy string. Route through the general
			   call path: it spills/places the scalar args correctly and preserves the
			   owned-string result. The args are raw pointers/lengths (no marshalling). */
			const char *sym = (strcmp(e->name,"fromCBytes")==0) ? "bzy_str_from_cbytes" : "bzy_str_from_cstring";
			TypeRef ps[2] = {0};                   /* Only .kind is read by cg_call_with_args. */
			ps[0].kind = TY_LONG;
			ps[1].kind = TY_LONG;                  /* len widened to 64-bit by cg_coerce. */
			cg_call_with_args(cg,tt,sym,NULL,e->args,e->arg_count,0,
							  1 /* result_is_object: owned string */, 0,
							  ps, e->arg_count, 0 /* marshal_cstr: args are raw scalars */);
		}
		else if (strcmp(e->name,"fromBytes")==0)
		{
			/* Build an owned string from a byte[]/ubyte[]. The array object pointer is
			   passed as-is (the helper reads its length+data); the result is an owned
			   string. No C-string marshalling. */
			TypeRef ps[1] = {0};
			ps[0].kind = TY_ARRAY;
			cg_call_with_args(cg,tt,"bzy_str_from_bytes",NULL,e->args,e->arg_count,0,
							  1 /* result_is_object: owned string */, 0,
							  ps, e->arg_count, 0 /* marshal_cstr: pass the object pointer */);
		}
		else if (strncmp(e->name,"Math.",5)==0)
		{
			cg_math(cg,tt,e);
		}
		else if (strncmp(e->name,"Clock.",6)==0)
		{
			cg_clock(cg,tt,e);
		}
		else if (strcmp(e->name,"scheduleAfter")==0 || strcmp(e->name,"scheduleEvery")==0)
		{
			cg_schedule(cg,tt,e);
		}
		else if (strncmp(e->name,"Random.",7)==0)
		{
			cg_random(cg,tt,e);
		}
		else if (strncmp(e->name,"Regex.",6)==0)
		{
			cg_regex(cg,tt,e);
		}
		else if (strncmp(e->name,"File.",5)==0)
		{
			cg_file(cg,tt,e);
		}
		else if (strncmp(e->name,"System.",7)==0)
		{
			cg_system(cg,tt,e);
		}
		else if (strncmp(e->name,"Network.",8)==0)
		{
			cg_network(cg,tt,e);
		}
		else if (strncmp(e->name,"Memory.",7)==0)
		{
			cg_memory(cg,tt,e);
		}
		else if (strncmp(e->name,"Simd.",5)==0)
		{
			cg_simd(cg,tt,e);
		}
		else if (strncmp(e->name,"Graphics.",9)==0)
		{
			cg_graphics(cg,tt,e);
		}
		else if (strncmp(e->name,"Ffi.",4)==0)
		{
			cg_ffi(cg,tt,e);
		}
		else if (strncmp(e->name,"Log.",4)==0)
		{
			cg_log(cg,tt,e);
		}
		else if (strncmp(e->name,"Xml.",4)==0)
		{
			cg_xml(cg,tt,e);
		}
		else if (strncmp(e->name,"Json.",5)==0)
		{
			cg_json(cg,tt,e);
		}
		else if (strncmp(e->name,"Http.",5)==0)
		{
			cg_http(cg,tt,e);
		}
		else
		{
			FuncInfo *fi=types_find_func_idx(tt,e->name,e->anno_overload);
			if (fi->is_dynamic)
			{
				cg_dynamic_extern_call(cg,tt,e,fi);
			}
			else if (fi->is_blocking)
			{
				cg_request_blocking_thunk(cg, fi);
				cg_blocking_call(cg,tt,e,fi);
			}
			else
			{
				/* If this extern receives a Breezy function pointer, bracket the call
				   with the in-callback guard: a callback may fire (synchronously, on
				   this stack) during the C call, and it must not park/throw. */
				int has_cb = 0;
				for (int ci = 0; ci < e->arg_count; ci++)
				{
					if (e->args[ci]->is_func_addr) { has_cb = 1; break; }
				}

				if (has_cb)
				{
					cg_aligned_call(cg, "bzy_callback_enter");
				}

				cg->call_variadic = fi->is_variadic;
				cg_call_with_args(cg,tt,fi->asm_label,NULL,e->args,e->arg_count,0, ty_is_managed(e->type.kind),
								  ty_is_float(e->type.kind), fi->param_types, fi->param_count, fi->is_extern);
				cg->call_variadic = 0;

				if (has_cb)
				{
					/* Preserve the call result across the leave call. */
					if (ty_is_float(e->type.kind))
					{
						cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);
						cg_aligned_call(cg, "bzy_callback_leave");
						cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);
					}
					else
					{
						cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
						cg_aligned_call(cg, "bzy_callback_leave");
						cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
					}
				}
			}
		}
		break;
	}
}

static void cg_block(Codegen *cg, TypeTable *tt, Func *f, Block *b, int in_main);
static void cg_stmt(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main);

/* IR loop regions (Plan 4): lookup/emission for loops the pre-scan recorded
   (defined with the other region helpers ahead of cg_emit_func). */
static int  cg_region_find(Codegen *cg, const Stmt *s);
static void cg_emit_region_stmt(Codegen *cg, Func *f, int r);
static int cg_tt_has_statics(TypeTable *tt);
static void cg_accum_loop(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main);   /* P5. */
static void cg_accum_append(Codegen *cg, TypeTable *tt, Stmt *a);                       /* P5. */

static void cg_store(Codegen *cg, TypeTable *tt, Expr *target)
{
	int fp = ty_is_float(target->type.kind);
	if (target->kind==EX_INDEX)
	{
		if (fp)
		{
			char iop[64];
			if (cg_index_opnd(cg, target, iop))
			{
				cg_store_fp(cg,target->type.kind,iop);   /* Folded mode: no lea, no rbx. */
				return;
			}

			cg_index_addr(cg,tt,target);   /* rbx = element address; xmm0 preserved. */
			cg_store_fp(cg,target->type.kind,"[rbx]");
		}
		else
		{
			/* Register-resident base + bare index: emit the bounds check and fold
			   the address into the store. The value stays in rax (the bounds-check
			   fast path and its skipped never-return oob stub leave rax untouched),
			   so no temp-slot round-trip is needed. */
			char imem[64];
			if (cg_index_checked_opnd(cg,tt,target,imem,sizeof imem))
			{
				cg_store_scalar(cg,target->type.kind,imem);
			}
			else
			{
				cg_temp_push(cg);      /* The integer value. */
				cg_index_addr(cg,tt,target);
				cg_temp_pop(cg);
				cg_store_scalar(cg,target->type.kind,"[rbx]");   /* target->type is the element type. */
			}
		}

		return;
	}
	if (target->kind==EX_IDENT)
	{
		if (fp)
		{
			cg_store_local_fp(cg, target->anno_int, target->type.kind);   /* XMM home if promoted, else slot. */
		}
		else
		{
			cg_store_local_off(cg, target->anno_int, target->type.kind);   /* Register home if promoted, else slot. */
		}
	}
	else if (target->anno_int==-1)   /* Static field: a global slot (no receiver). */
	{
		char mem[192];
		snprintf(mem,sizeof(mem),"[rel __static_%s_%s]", target->anno_str, target->name);
		if (fp)
		{
			cg_store_fp(cg,target->type.kind,mem);
		}
		else
		{
			cg_emit(cg,"    mov %s, rax", mem);
		}
	}
	else if (fp)     /* EX_FIELD, float value in xmm0. */
	{
		cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);   /* Spill value. */
		cg_expr(cg,tt,target->lhs);
		cg_emit(cg,"    mov rbx, rax");
		cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);   /* Reload value. */
		char mem[32];
		sprintf(mem,"[rbx + %d]", target->anno_int);
		cg_store_fp(cg,target->type.kind,mem);
	}
	else             /* EX_FIELD, integer/object value in rax. */
	{
		cg_temp_push(cg);
		cg_expr(cg,tt,target->lhs);
		cg_emit(cg,"    mov rbx, rax");
		cg_temp_pop(cg);
		cg_emit(cg,"    mov [rbx + %d], rax", target->anno_int);
	}
}

/* Structural AST equality (mirrors irlower.c's expr_eq), for the indexed-RMW fuse. */
static int cg_expr_eq(const Expr *a, const Expr *b)
{
	if (!a || !b)
	{
		return a == b;
	}

	if (a->kind != b->kind || a->type.kind != b->type.kind)
	{
		return 0;
	}

	switch (a->kind)
	{
	case EX_INT:
	case EX_BOOL:
		return a->int_val == b->int_val;
	case EX_IDENT:
		return a->anno_int == b->anno_int;
	case EX_CAST:
		return cg_expr_eq(a->lhs, b->lhs);
	case EX_UNARY:
		return a->op == b->op && cg_expr_eq(a->lhs, b->lhs);
	case EX_BINARY:
		return a->op == b->op && cg_expr_eq(a->lhs, b->lhs) && cg_expr_eq(a->rhs, b->rhs);
	case EX_INDEX:
		return cg_expr_eq(a->lhs, b->lhs) && cg_expr_eq(a->rhs, b->rhs);
	case EX_FIELD:
		return strcmp(a->name, b->name) == 0 && cg_expr_eq(a->lhs, b->lhs);
	default:
		return 0;
	}
}

/* The two-operand instruction for an in-place arithmetic op, or NULL. */
static const char *cg_inplace_mnem(int op)
{
	switch (op)
	{
	case TOKEN_PLUS:  return "add";
	case TOKEN_MINUS: return "sub";
	case TOKEN_STAR:  return "imul";
	case TOKEN_AMP:   return "and";
	case TOKEN_PIPE:  return "or";
	case TOKEN_CARET: return "xor";
	default:          return NULL;
	}
}

/* Fuse `arr[X] = arr[X] op V` (or `V op arr[X]` for a commutative op) into a single
   indexed address with ONE bounds check, then load/op/store - instead of computing
   the index and bounds-checking twice (once for the element read inside the value,
   once for the store). Integer elements only; returns 1 if it emitted the store.
   Mirrors the IR backend's RMW lowering so the emitter path (e.g. a loop with calls,
   which is not IR-region-eligible) gets the same fusion. */
static int cg_try_rmw_index(Codegen *cg, TypeTable *tt, Expr *target, Expr *value)
{
	if (target->kind != EX_INDEX || value->kind != EX_BINARY)
	{
		return 0;
	}

	TypeKind k = target->type.kind;
	if (ty_is_managed(k) || ty_is_float(k) || value->type.kind != k)
	{
		return 0;   /* Integer elements, no width coercion between the op and the store. */
	}

	const char *mnem = cg_inplace_mnem(value->op);
	if (!mnem)
	{
		return 0;
	}

	const Expr *inner = NULL;
	if (cg_expr_eq(value->lhs, target))
	{
		inner = value->lhs;                          /* arr[X] op V (any of the six ops). */
	}
	else if ((value->op == TOKEN_PLUS || value->op == TOKEN_STAR || value->op == TOKEN_AMP
			  || value->op == TOKEN_PIPE || value->op == TOKEN_CARET)
			 && cg_expr_eq(value->rhs, target))
	{
		inner = value->rhs;                          /* V op arr[X], commutative -> arr[X] op V. */
	}

	if (!inner)
	{
		return 0;
	}

	Expr *other = (inner == value->lhs) ? value->rhs : value->lhs;
	cg_expr(cg, tt, other);                  /* rax = V (the non-element operand). */
	cg_temp_push(cg);                        /* Save V across the address computation. */
	cg_index_addr(cg, tt, target);           /* rbx = &elem; ONE bounds check. */
	cg_temp_pop_reg(cg, "rcx");              /* rcx = V. */
	cg_load_scalar(cg, k, "[rbx]");          /* rax = elem (width-correct). */
	cg_emit(cg, "    %s rax, rcx", mnem);    /* rax = elem op V. */
	cg_extend_reg(cg, k);                    /* Re-narrow to the element width. */
	cg_store_scalar(cg, k, "[rbx]");
	return 1;
}

/* Store a +1 object into an object-typed target, releasing the previous occupant
   and any owned receiver temporary. */
static void cg_assign_object(Codegen *cg, TypeTable *tt, Expr *target, Expr *value)
{
	if (target->kind==EX_INDEX)
	{
		if (target->anno_shared_gate)
		{
			/* Maybe-shared managed element store: when the array crossed cores,
			   the swap must run under the slot stripe and the new element must be
			   deep-shared (insert barrier) - the locked helper does both and
			   takes ownership of the +1. The confined path is the unchanged
			   inline swap. The bit test happens while the base is still live;
			   cg_temp_pop is a plain mov, so the flags survive to the jnz. */
			cg_expr_owned(cg,tt,value);              /* +1 new element -> rax. */
			cg_temp_push(cg);
			cg_index_addr_based(cg,tt,target);       /* rax = base, rbx = slot. */
			cg_emit(cg,"    test qword [rax + 16], 8");   /* BZY_GCINFO_SHARED. */
			cg_temp_pop(cg);                          /* Value back in rax. */
			int sh = cg_label(cg), join = cg_label(cg);
			cg_emit(cg,"    jnz .L%d", sh);
			cg_emit(cg,"    mov rdx, [rbx]");         /* Old element. */
			cg_emit(cg,"    mov [rbx], rax");         /* Store new (transfers the +1). */
			cg_emit(cg,"    mov %s, rdx", cg_iarg(cg, 0));
			cg_release_rcx(cg);                       /* Release old. */
			cg_emit(cg,"    jmp .L%d", join);
			cg_emit(cg,".L%d:", sh);
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 1));
			cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
			cg_aligned_call(cg,"bzy_array_set_shared");   /* Locked swap; consumes the +1; releases old. */
			cg_emit(cg,".L%d:", join);
			return;
		}

		cg_expr_owned(cg,tt,value);          /* +1 new element -> rax. */
		cg_temp_push(cg);
		cg_index_addr(cg,tt,target);         /* rbx = element address. */
		cg_temp_pop(cg);
		cg_emit(cg,"    mov rdx, [rbx]");     /* Old element. */
		cg_emit(cg,"    mov [rbx], rax");     /* Store new (transfers the +1). */
		cg_emit(cg,"    mov %s, rdx", cg_iarg(cg, 0));
		cg_release_rcx(cg);                   /* Release old. */
		return;
	}
	if (target->kind==EX_IDENT)
	{
		cg_expr_owned(cg,tt,value);
		cg_emit(cg,"    mov rbx, [rbp - %d]", target->anno_int);
		cg_emit(cg,"    mov [rbp - %d], rax", target->anno_int);
		cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
		cg_release_rcx(cg);
	}
	else if (target->anno_int==-1)   /* Static managed field: a global slot. */
	{
		char mem[192];
		snprintf(mem,sizeof(mem),"[rel __static_%s_%s]", target->anno_str, target->name);
		cg_expr_owned(cg,tt,value);            /* +1 new value -> rax. */
		/* Static slots are reachable from every breeze with no handoff point:
		   deep-share the value before publishing it (a share point, like a
		   channel send). The call preserves nothing, so park the value first. */
		cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_aligned_call(cg,"bzy_share_crosscore");
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->assign_save);
		cg_emit(cg,"    mov rbx, %s", mem);    /* Old occupant. */
		cg_emit(cg,"    mov %s, rax", mem);    /* Store new (transfers the +1). */
		cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
		cg_release_rcx(cg);                    /* Release old. */
	}
	else
	{
		cg_expr_owned(cg,tt,value);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);
		/* A field store on a shared-class instance publishes the value to every
		   breeze that can reach the object: instances of is_shared classes are
		   born SHARED, so deep-share the incoming value unconditionally here
		   (the value is parked in assign_save across the call). */
		if (target->lhs->type.kind==TY_OBJECT)
		{
			ClassInfo *rc = types_find_class(tt,target->lhs->type.class_name);
			if (rc && rc->is_shared)
			{
				cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
				cg_aligned_call(cg,"bzy_share_crosscore");
			}
		}

		cg_expr(cg,tt,target->lhs);
		cg_emit(cg,"    mov rbx, rax");
		cg_emit(cg,"    mov rdx, [rbx + %d]", target->anno_int);
		cg_emit(cg,"    mov rax, [rbp - %d]", cg->assign_save);
		cg_emit(cg,"    mov [rbx + %d], rax", target->anno_int);
		cg_emit(cg,"    mov %s, rdx", cg_iarg(cg, 0));
		cg_release_rcx(cg);
		if (expr_is_owned(target->lhs))
		{
			cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
			cg_release_rcx(cg);
		}
	}
}

/* C-style for: init once, then test/body/post, with continue landing on the
   post step so the increment still runs. break -> end. Reuses the shared
   loop-label fields on Codegen. */
/* ---- Loop-invariant register caching (LICM-lite) for innermost loops ----

   A tight indexing loop reloads its array base pointers and invariant index
   offsets from the stack every iteration; that memory traffic, not the bounds
   check, is what makes such loops trail a tracing-GC native compiler. When a
   loop body is provably call-free and indexes only bounds-check-eliminated
   arrays, the caller-saved registers r8..r11 are free for its whole duration, so
   we pin up to four of its hottest loop-invariant locals there and read them
   from the register instead of memory. The slot is never written, so this is a
   pure caching transform - correctness does not depend on every read using it. */

static int cg_hoist_expr_ok(Expr *e)
{
	if (!e)
	{
		return 1;
	}

	/* Simd.* intrinsics lower entirely inline (no call instruction); a Simd.load/
	   store fabricates an already-safe index, so it emits no bounds check either.
	   They are call-free as long as their argument expressions are - so a loop
	   doing packed work can still hoist its invariant array bases into r8-r11. */
	if (e->kind == EX_CALL && strncmp(e->name, "Simd.", 5) == 0)
	{
		for (int i = 0; i < e->arg_count; i++)
		{
			if (!cg_hoist_expr_ok(e->args[i]))
			{
				return 0;
			}
		}

		return 1;
	}

	switch (e->kind)
	{
	case EX_INT:
	case EX_BOOL:
	case EX_FLOAT:
	case EX_IDENT:
	case EX_THIS:
		return 1;
	case EX_UNARY:
	case EX_INCDEC:
	case EX_CAST:
		return cg_hoist_expr_ok(e->lhs) && cg_hoist_expr_ok(e->rhs);
	case EX_BINARY:
		if (e->type.kind == TY_STRING)
		{
			return 0;   /* String concatenation calls into the runtime. */
		}

		return cg_hoist_expr_ok(e->lhs) && cg_hoist_expr_ok(e->rhs);
	case EX_INDEX:
		/* A map/string index or managed element (ARC retain/release) is not call-free.
		   bzy_oob is reachable on out-of-bounds but NEVER RETURNS, so the hoist registers
		   are only clobbered on a path that terminates; the in-bounds path is safe. */
		if (e->lhs->kind != EX_IDENT || e->lhs->type.kind != TY_ARRAY
			|| ty_is_managed(e->type.kind))
		{
			return 0;
		}

		return cg_hoist_expr_ok(e->rhs);
	default:
		return 0;   /* EX_CALL / EX_METHOD_CALL / EX_NEW* / ... may emit a call. */
	}
}

static int cg_hoist_block_ok(Block *b);

static int cg_hoist_stmt_ok(Stmt *s)
{
	if (!s)
	{
		return 1;
	}

	switch (s->kind)
	{
	case ST_VARDECL:
		if (ty_is_managed(s->decl_type.kind))
		{
			return 0;
		}

		return cg_hoist_expr_ok(s->decl_init);
	case ST_ASSIGN:
		if (s->target && s->target->kind == EX_IDENT)
		{
			if (ty_is_managed(s->target->type.kind))
			{
				return 0;
			}
		}
		else if (!s->target || !cg_hoist_expr_ok(s->target))   /* Array element store: must be a safe, non-managed index. */
		{
			return 0;
		}

		return cg_hoist_expr_ok(s->value);
	case ST_EXPR:
		return cg_hoist_expr_ok(s->expr);
	case ST_IF:
		return cg_hoist_expr_ok(s->cond) && cg_hoist_block_ok(s->then_blk)
			   && cg_hoist_block_ok(s->else_blk);
	case ST_WHILE:
		return cg_hoist_expr_ok(s->cond) && cg_hoist_block_ok(s->then_blk);
	case ST_FOR:
		return cg_hoist_block_ok(s->then_blk);
	case ST_BREAK:
	case ST_CONTINUE:
		return 1;
	case ST_SWITCH:
		/* Dispatch on an integer subject is call-free: it lowers to a jump table or
		   a compare chain using rax/rcx/rdx scratch, never the r8-r11 hoist cache.
		   The case bodies live flat in then_blk, so vetting it as a block reaches
		   them (and the ST_CASE/ST_DEFAULT markers below). String and enum subjects
		   route through the runtime (string compare; owned-enum release), so restrict
		   to a non-managed scalar subject. */
		if (ty_is_managed(s->cond->type.kind))
		{
			return 0;
		}

		return cg_hoist_expr_ok(s->cond) && cg_hoist_block_ok(s->then_blk);
	case ST_CASE:
		return cg_hoist_expr_ok(s->value);   /* Marker only; the label value is a compile-time constant. */
	case ST_DEFAULT:
		return 1;                            /* Marker only; emits a label. */
	default:
		return 0;   /* Return, throw, try, spawn, foreach. */
	}
}

static int cg_hoist_block_ok(Block *b)
{
	if (!b)
	{
		return 1;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (!cg_hoist_stmt_ok(b->stmts[i]))
		{
			return 0;
		}
	}

	return 1;
}

/* --- Loop-scoped float promotion (xmm6..xmm11). ----------------------------------
   Function-scope float promotion (xmm2..5) refuses any double whose live range
   crosses a call: those registers are caller-saved. A call-free innermost loop can
   still home such doubles in callee-saved XMM registers for just the loop's span -
   the per-frame accumulator of a hot FP kernel is the canonical win. Each chosen
   double is loaded from its slot BEFORE the entry guard and stored back after the
   end label, so the guard-fail path round-trips the values unchanged and break
   exits (which jump to the end label) pass through the stores. The body allowlist
   is the same one that protects the r8..r11 caches (call-free, BCE-safe, no
   return/throw/try/foreach), extended to the loop's own condition and any nested
   for headers, which the hoist predicate never vets. On Win64 xmm6+ belong to the
   caller and are preserved in scratch slots around the loop; SysV leaves them
   volatile. */

/* Nested for/while headers (cond, init, post) are emitted per-entry or per-
   iteration inside the armed region, so they must satisfy the same call-free
   allowlist as the body statements. */
static int cg_lpromo_headers_ok_block(Block *b);

static int cg_lpromo_headers_ok_stmt(Stmt *s)
{
	if (!s)
	{
		return 1;
	}

	if (s->kind == ST_FOR
		&& (!cg_hoist_expr_ok(s->cond) || !cg_hoist_stmt_ok(s->for_init) || !cg_hoist_stmt_ok(s->for_post)))
	{
		return 0;
	}

	return cg_lpromo_headers_ok_stmt(s->for_init) && cg_lpromo_headers_ok_stmt(s->for_post)
		   && cg_lpromo_headers_ok_block(s->then_blk) && cg_lpromo_headers_ok_block(s->else_blk);
}

static int cg_lpromo_headers_ok_block(Block *b)
{
	if (!b)
	{
		return 1;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (!cg_lpromo_headers_ok_stmt(b->stmts[i]))
		{
			return 0;
		}
	}

	return 1;
}

/* Use-count collection for double locals that hold no caller-saved XMM home. */
typedef struct
{
	int off;
	int count;
} LpCand;

static void cg_lpromo_note(Codegen *cg, int off, LpCand **cand, int *n, int *cap)
{
	if (off <= 0 || cg_local_xmm(cg, off))
	{
		return;   /* Already homed function-wide (xmm2..5), or not a local slot. */
	}

	for (int i = 0; i < *n; i++)
	{
		if ((*cand)[i].off == off)
		{
			(*cand)[i].count++;
			return;
		}
	}

	*cand = grow_ensure(*cand, *n, cap, sizeof(**cand));
	(*cand)[*n].off = off;
	(*cand)[*n].count = 1;
	(*n)++;
}

static void cg_lpromo_uses_expr(Codegen *cg, Expr *e, LpCand **cand, int *n, int *cap)
{
	if (!e)
	{
		return;
	}

	if (e->kind == EX_IDENT && e->type.kind == TY_DOUBLE && e->anno_int > 0)
	{
		cg_lpromo_note(cg, e->anno_int, cand, n, cap);
	}

	cg_lpromo_uses_expr(cg, e->lhs, cand, n, cap);
	cg_lpromo_uses_expr(cg, e->rhs, cand, n, cap);
	for (int i = 0; i < e->arg_count; i++)
	{
		cg_lpromo_uses_expr(cg, e->args[i], cand, n, cap);
	}
}

static void cg_lpromo_uses_block(Codegen *cg, Block *b, LpCand **cand, int *n, int *cap);

static void cg_lpromo_uses_stmt(Codegen *cg, Stmt *s, LpCand **cand, int *n, int *cap)
{
	if (!s)
	{
		return;
	}

	if (s->kind == ST_VARDECL && s->decl_type.kind == TY_DOUBLE && s->decl_offset > 0)
	{
		cg_lpromo_note(cg, s->decl_offset, cand, n, cap);
	}

	cg_lpromo_uses_expr(cg, s->target, cand, n, cap);
	cg_lpromo_uses_expr(cg, s->cond, cand, n, cap);
	cg_lpromo_uses_expr(cg, s->expr, cand, n, cap);
	cg_lpromo_uses_expr(cg, s->value, cand, n, cap);
	cg_lpromo_uses_expr(cg, s->decl_init, cand, n, cap);
	cg_lpromo_uses_stmt(cg, s->for_init, cand, n, cap);
	cg_lpromo_uses_stmt(cg, s->for_post, cand, n, cap);
	cg_lpromo_uses_block(cg, s->then_blk, cand, n, cap);
	cg_lpromo_uses_block(cg, s->else_blk, cand, n, cap);
}

static void cg_lpromo_uses_block(Codegen *cg, Block *b, LpCand **cand, int *n, int *cap)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		cg_lpromo_uses_stmt(cg, b->stmts[i], cand, n, cap);
	}
}

/* Arm loop-scoped float promotion for `loop` if eligible: pick the hottest
   slot-resident doubles used in it, preserve the caller's xmm6.. (Win64), and
   load each candidate into its register. Must be emitted BEFORE the loop's
   entry guard; cg_loop_hoist_end emits the matching stores and restores. */
static void cg_lpromo_begin(Codegen *cg, TypeTable *tt, Stmt *loop)
{
	(void)tt;
	Block *body = loop->then_blk;
	if (!cg->cur_func || cg->hoist_depth > 0 || cg->lpromo_n > 0 || cg->unrolling
		|| !body || !cg_hoist_block_ok(body) || !cg_lpromo_headers_ok_block(body)
		|| !cg_hoist_expr_ok(loop->cond)
		|| (loop->kind == ST_FOR && !cg_hoist_stmt_ok(loop->for_post)))
	{
		return;
	}

	LpCand *cand = NULL;
	int nc = 0;
	int cap = 0;
	cg_lpromo_uses_block(cg, body, &cand, &nc, &cap);
	cg_lpromo_uses_expr(cg, loop->cond, &cand, &nc, &cap);
	cg_lpromo_uses_stmt(cg, loop->for_post, &cand, &nc, &cap);

	while (cg->lpromo_n < LPROMO_NREGS)
	{
		int best = -1;
		for (int i = 0; i < nc; i++)
		{
			if (cand[i].count >= 2 && (best < 0 || cand[i].count > cand[best].count))
			{
				best = i;
			}
		}

		if (best < 0)
		{
			break;
		}

		int k = cg->lpromo_n;
		if (cg->target == TARGET_WINDOWS)
		{
			cg_emit(cg, "    movups [rbp - %d], %s", cg->lpromo_save_base + k*16, CG_LPROMO_REGS[k]);
		}

		cg_emit(cg, "    movsd %s, qword [rbp - %d]", CG_LPROMO_REGS[k], cand[best].off);
		cg->lpromo_off[k] = cand[best].off;
		cg->lpromo_n++;
		cand[best].count = 0;
	}
}

/* Collect, into w[0..*wn), the slot offsets a region writes (the values that are
   therefore NOT loop-invariant). */
static void cg_hoist_writes_expr(Expr *e, int *w, int *wn)
{
	if (!e)
	{
		return;
	}

	if (e->kind == EX_INCDEC && e->lhs && e->lhs->kind == EX_IDENT && e->lhs->anno_int > 0)
	{
		if (*wn < 128)
		{
			w[(*wn)++] = e->lhs->anno_int;
		}
	}

	cg_hoist_writes_expr(e->lhs, w, wn);
	cg_hoist_writes_expr(e->rhs, w, wn);
	for (int i = 0; i < e->arg_count; i++)
	{
		cg_hoist_writes_expr(e->args[i], w, wn);
	}
}

static void cg_hoist_writes_block(Block *b, int *w, int *wn);

static void cg_hoist_writes_stmt(Stmt *s, int *w, int *wn)
{
	if (!s)
	{
		return;
	}

	if (s->kind == ST_VARDECL && s->decl_offset > 0 && *wn < 128)
	{
		w[(*wn)++] = s->decl_offset;
	}

	if (s->kind == ST_ASSIGN && s->target && s->target->kind == EX_IDENT
		&& s->target->anno_int > 0 && *wn < 128)
	{
		w[(*wn)++] = s->target->anno_int;
	}

	cg_hoist_writes_expr(s->cond, w, wn);
	cg_hoist_writes_expr(s->decl_init, w, wn);
	cg_hoist_writes_expr(s->value, w, wn);
	cg_hoist_writes_expr(s->target, w, wn);
	cg_hoist_writes_expr(s->expr, w, wn);
	cg_hoist_writes_stmt(s->for_post, w, wn);
	cg_hoist_writes_block(s->then_blk, w, wn);
	cg_hoist_writes_block(s->else_blk, w, wn);
}

static void cg_hoist_writes_block(Block *b, int *w, int *wn)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		cg_hoist_writes_stmt(b->stmts[i], w, wn);
	}
}

static int cg_off_in(int *w, int wn, int off)
{
	for (int i = 0; i < wn; i++)
	{
		if (w[i] == off)
		{
			return 1;
		}
	}

	return 0;
}

typedef struct
{
	int off;
	int count;
	TypeKind k;
} HoistCand;

/* Tally reads of loop-invariant integer/array locals (the candidates to cache).
   Floats use xmm and a different load path; managed values other than array bases
   are excluded by the block predicate already. */
static void cg_hoist_reads_expr(Codegen *cg, Expr *e, int *w, int wn, HoistCand *c, int *nc)
{
	if (!e)
	{
		return;
	}

	if (cg_sr_contains(cg, e))
	{
		return;   /* Folded into a strength-reduced base pointer; not loaded via its idents. */
	}

	if (e->kind == EX_IDENT && e->anno_int > 0
		&& (ty_is_int(e->type.kind) || e->type.kind == TY_ARRAY)
		&& !cg_local_reg(cg, e->anno_int) && !cg_off_in(w, wn, e->anno_int))
	{
		int found = 0;
		for (int i = 0; i < *nc; i++)
		{
			if (c[i].off == e->anno_int)
			{
				c[i].count++;
				found = 1;
				break;
			}
		}

		if (!found && *nc < 64)
		{
			c[*nc].off = e->anno_int;
			c[*nc].count = 1;
			c[*nc].k = e->type.kind;
			(*nc)++;
		}
	}

	cg_hoist_reads_expr(cg, e->lhs, w, wn, c, nc);
	cg_hoist_reads_expr(cg, e->rhs, w, wn, c, nc);
	for (int i = 0; i < e->arg_count; i++)
	{
		cg_hoist_reads_expr(cg, e->args[i], w, wn, c, nc);
	}
}

static void cg_hoist_reads_block(Codegen *cg, Block *b, int *w, int wn, HoistCand *c, int *nc);

static void cg_hoist_reads_stmt(Codegen *cg, Stmt *s, int *w, int wn, HoistCand *c, int *nc)
{
	if (!s)
	{
		return;
	}

	/* Nested while/for: count the body 4× so inner-loop-resident arrays
	   outrank outer-body-only arrays of equal static frequency. */
	if (s->kind == ST_WHILE || s->kind == ST_FOR)
	{
		cg_hoist_reads_expr(cg, s->cond, w, wn, c, nc);
		for (int j = 0; j < 4; j++)
		{
			cg_hoist_reads_block(cg, s->then_blk, w, wn, c, nc);
		}
		return;
	}

	cg_hoist_reads_expr(cg, s->cond, w, wn, c, nc);
	cg_hoist_reads_expr(cg, s->decl_init, w, wn, c, nc);
	cg_hoist_reads_expr(cg, s->value, w, wn, c, nc);
	cg_hoist_reads_expr(cg, s->expr, w, wn, c, nc);
	if (s->target && s->target->kind == EX_INDEX)   /* An index store reads its base/index idents. */
	{
		cg_hoist_reads_expr(cg, s->target, w, wn, c, nc);
	}

	cg_hoist_reads_block(cg, s->then_blk, w, wn, c, nc);
	cg_hoist_reads_block(cg, s->else_blk, w, wn, c, nc);
}

static void cg_hoist_reads_block(Codegen *cg, Block *b, int *w, int wn, HoistCand *c, int *nc)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		cg_hoist_reads_stmt(cg, b->stmts[i], w, wn, c, nc);
	}
}

/* The induction variable's register for a `for (iv = LO; iv </<= HI; iv = iv+1)`
   (or iv++) loop whose counter is promoted, or NULL. Only the unit-step form is
   recognised, because strength reduction folds a coefficient-1 index term into a
   scaled address [base + iv*stride]. */
static const char *cg_loop_induction(Codegen *cg, Stmt *loop, int *iv_off)
{
	if (loop->kind != ST_FOR || !loop->for_init || !loop->cond || !loop->for_post)
	{
		return NULL;
	}

	int io = 0;
	if (loop->for_init->kind == ST_VARDECL)
	{
		io = loop->for_init->decl_offset;
	}
	else if (loop->for_init->kind == ST_ASSIGN && loop->for_init->target
			 && loop->for_init->target->kind == EX_IDENT)
	{
		io = loop->for_init->target->anno_int;
	}
	else
	{
		return NULL;
	}

	Expr *c = loop->cond;
	if (!(c->kind == EX_BINARY && (c->op == TOKEN_LT || c->op == TOKEN_LTE)
		  && c->lhs->kind == EX_IDENT && c->lhs->anno_int == io && io != 0))
	{
		return NULL;
	}

	Stmt *p = loop->for_post;
	int step_ok = 0;
	if (p->kind == ST_ASSIGN && p->target && p->target->kind == EX_IDENT
		&& p->target->anno_int == io && p->value && p->value->kind == EX_BINARY
		&& p->value->op == TOKEN_PLUS && p->value->lhs->kind == EX_IDENT
		&& p->value->lhs->anno_int == io && p->value->rhs->kind == EX_INT
		&& p->value->rhs->int_val == 1)
	{
		step_ok = 1;
	}
	else if (p->kind == ST_EXPR && p->expr && p->expr->kind == EX_INCDEC
			 && p->expr->op == TOKEN_PLUSPLUS && p->expr->lhs
			 && p->expr->lhs->kind == EX_IDENT && p->expr->lhs->anno_int == io)
	{
		step_ok = 1;
	}

	if (!step_ok)
	{
		return NULL;
	}

	const char *r = cg_local_reg(cg, io);   /* Must be register-resident to serve as a scaled index. */
	if (!r)
	{
		return NULL;
	}

	*iv_off = io;
	return r;
}

/* True if `e` reads any local in the written set w (so it is not loop-invariant). */
static int cg_expr_reads_written(Expr *e, int *w, int wn)
{
	if (!e)
	{
		return 0;
	}

	if (e->kind == EX_IDENT && cg_off_in(w, wn, e->anno_int))
	{
		return 1;
	}

	if (cg_expr_reads_written(e->lhs, w, wn) || cg_expr_reads_written(e->rhs, w, wn))
	{
		return 1;
	}

	for (int i = 0; i < e->arg_count; i++)
	{
		if (cg_expr_reads_written(e->args[i], w, wn))
		{
			return 1;
		}
	}

	return 0;
}

/* If `e` is a strength-reducible access arr[INV + iv] (arr and INV loop-invariant,
   iv the unit-step induction variable, legal element scale), return its invariant
   offset expression INV via E_out; else NULL. */
static Expr *cg_sr_eligible(Expr *e, int iv_off, int *w, int wn)
{
	if (e->kind != EX_INDEX || !e->anno_index_safe)
	{
		return NULL;
	}

	if (e->lhs->kind != EX_IDENT || e->lhs->type.kind != TY_ARRAY
		|| cg_off_in(w, wn, e->lhs->anno_int))
	{
		return NULL;   /* Base must be an invariant array local. */
	}

	int stride = cg_elem_stride(e->type.kind);
	if (stride != 1 && stride != 2 && stride != 4 && stride != 8)
	{
		return NULL;
	}

	Expr *idx = e->rhs;
	if (idx->kind != EX_BINARY || idx->op != TOKEN_PLUS)
	{
		return NULL;
	}

	Expr *E = NULL;
	if (idx->rhs->kind == EX_IDENT && idx->rhs->anno_int == iv_off)
	{
		E = idx->lhs;
	}
	else if (idx->lhs->kind == EX_IDENT && idx->lhs->anno_int == iv_off)
	{
		E = idx->rhs;
	}
	else
	{
		return NULL;
	}

	if (cg_expr_reads_written(E, w, wn))
	{
		return NULL;   /* The offset must be loop-invariant. */
	}

	return E;
}

static void cg_sr_collect_expr(Expr *e, int iv_off, int *w, int wn, Expr **nodes, Expr **offs, int *n)
{
	if (!e)
	{
		return;
	}

	Expr *E = cg_sr_eligible(e, iv_off, w, wn);
	if (E)
	{
		int dup = 0;
		for (int i = 0; i < *n; i++)
		{
			if (nodes[i] == e)
			{
				dup = 1;
				break;
			}
		}

		if (!dup && *n < 4)
		{
			nodes[*n] = e;
			offs[*n] = E;
			(*n)++;
		}

		return;   /* Do not descend into a matched access (its iv term is handled). */
	}

	cg_sr_collect_expr(e->lhs, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_expr(e->rhs, iv_off, w, wn, nodes, offs, n);
	for (int i = 0; i < e->arg_count; i++)
	{
		cg_sr_collect_expr(e->args[i], iv_off, w, wn, nodes, offs, n);
	}
}

static void cg_sr_collect_block(Block *b, int iv_off, int *w, int wn, Expr **nodes, Expr **offs, int *n);

static void cg_sr_collect_stmt(Stmt *s, int iv_off, int *w, int wn, Expr **nodes, Expr **offs, int *n)
{
	if (!s)
	{
		return;
	}

	cg_sr_collect_expr(s->cond, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_expr(s->decl_init, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_expr(s->value, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_expr(s->target, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_expr(s->expr, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_block(s->then_blk, iv_off, w, wn, nodes, offs, n);
	cg_sr_collect_block(s->else_blk, iv_off, w, wn, nodes, offs, n);
}

static void cg_sr_collect_block(Block *b, int iv_off, int *w, int wn, Expr **nodes, Expr **offs, int *n)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		cg_sr_collect_stmt(b->stmts[i], iv_off, w, wn, nodes, offs, n);
	}
}

/* If `loop` (a for/while) qualifies, pin its hottest loop-invariant locals into
   r8..r11 and record them in the cg hoist/sr caches so reads use the register.
   Strength-reduced array bases take registers first (each removes a base reload,
   an index reconstruction, and a bounds check), then the remaining registers
   cache plain invariant locals. Emits the one-time setup; the caller must place
   this after the entry guard, before the loop top, and call cg_loop_hoist_end
   after the loop. */
/* True if `e` reads slot `off` anywhere within it. */
static int cg_expr_refs_off(Expr *e, int off)
{
	if (!e)
	{
		return 0;
	}

	if (e->kind == EX_IDENT && e->anno_int == off)
	{
		return 1;
	}

	if (cg_expr_refs_off(e->lhs, off) || cg_expr_refs_off(e->rhs, off))
	{
		return 1;
	}

	for (int i = 0; i < e->arg_count; i++)
	{
		if (cg_expr_refs_off(e->args[i], off))
		{
			return 1;
		}
	}

	return 0;
}

static int cg_acc_deferrable_block(Block *b, int off);

/* True if every appearance of int slot `off` in this statement is as the target
   and one operand of a self-accumulation `off = off <op> EXPR` (EXPR free of off)
   for an op whose low 32 bits depend only on the operands' low 32 bits: +, -, *,
   &, |, ^. Each lowers to a 32-bit in-place op, so the register's low half stays
   correct every iteration and only a single sign-extension after the loop is
   needed. Shifts and divide/remainder are excluded - they read the full 64-bit
   value. Any other read (a comparison, an index, a different assignment form) would
   observe the full register and thus disqualifies deferral. */
static int cg_acc_deferrable_stmt(Stmt *s, int off)
{
	if (!s)
	{
		return 1;
	}

	if (s->kind == ST_ASSIGN && s->target && s->target->kind == EX_IDENT
		&& s->target->anno_int == off)
	{
		Expr *v = s->value;
		if (v && v->kind == EX_BINARY && s->target->type.kind == TY_INT)
		{
			int op = v->op;
			int safe_op = (op==TOKEN_PLUS || op==TOKEN_MINUS || op==TOKEN_STAR
						   || op==TOKEN_AMP || op==TOKEN_PIPE || op==TOKEN_CARET);
			int commutative = (op != TOKEN_MINUS);   /* all but subtraction. */
			if (safe_op && v->lhs->kind==EX_IDENT && v->lhs->anno_int==off
				&& !cg_expr_refs_off(v->rhs, off))
			{
				return 1;   /* off = off <op> EXPR. */
			}

			if (safe_op && commutative && v->rhs->kind==EX_IDENT && v->rhs->anno_int==off
				&& !cg_expr_refs_off(v->lhs, off))
			{
				return 1;   /* off = EXPR <op> off (commutative). */
			}
		}

		return 0;       /* Assignment to off in a form that is not a 32-bit self-accumulate. */
	}

	if (cg_expr_refs_off(s->cond, off) || cg_expr_refs_off(s->decl_init, off)
		|| cg_expr_refs_off(s->value, off) || cg_expr_refs_off(s->target, off)
		|| cg_expr_refs_off(s->expr, off) || cg_expr_refs_off(s->ret_val, off))
	{
		return 0;
	}

	if (!cg_acc_deferrable_block(s->then_blk, off) || !cg_acc_deferrable_block(s->else_blk, off))
	{
		return 0;
	}

	return 1;
}

static int cg_acc_deferrable_block(Block *b, int off)
{
	if (!b)
	{
		return 1;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (!cg_acc_deferrable_stmt(b->stmts[i], off))
		{
			return 0;
		}
	}

	return 1;
}

static void cg_loop_hoist_begin(Codegen *cg, TypeTable *tt, Stmt *loop)
{
	if (cg->hoist_depth > 0)
	{
		/* An outer loop already owns the hoist registers; skip a nested begin so
		   the outer hoist state is preserved for the entire outer loop body. */
		cg->hoist_depth++;
		return;
	}

	cg->hoist_n = 0;
	cg->sr_n = 0;
	cg->sr_ivreg = NULL;
	cg->defer_n = 0;
	Block *body = loop->then_blk;
	if (!body || !cg_hoist_block_ok(body))
	{
		return;
	}

	cg->hoist_depth = 1;

	int w[128];
	int wn = 0;
	cg_hoist_writes_block(body, w, &wn);
	cg_hoist_writes_stmt(loop->for_post, w, &wn);

	/* Deferred-extension accumulators: a promoted int local written in the loop
	   solely by `acc = acc +/- EXPR` keeps a valid low 32 bits every iteration (the
	   in-place add is 32-bit); its sign-extension is needed only for reads after
	   the loop, so defer it to the exit (cg_loop_hoist_end) and drop the per-
	   iteration movsxd from the carried dependency. */
	for (int i = 0; i < wn && cg->defer_n < 8; i++)
	{
		int off = w[i];
		if (off != 0 && off != cg->cur_counter_off && cg_local_reg(cg, off)
			&& cg_acc_deferrable_block(body, off))
		{
			cg->defer_off[cg->defer_n++] = off;
		}
	}

	int next_reg = 0;

	/* Strength reduction: fold arr[INV + iv] into a base register = arr + INV*stride + 32. */
	int iv_off = 0;
	const char *ivreg = cg_loop_induction(cg, loop, &iv_off);
	if (ivreg)
	{
		Expr *nodes[4];
		Expr *offs[4];
		int sn = 0;
		cg_sr_collect_block(body, iv_off, w, wn, nodes, offs, &sn);
		cg->sr_ivreg = ivreg;
		for (int i = 0; i < sn && next_reg < 4; i++)
		{
			int stride = cg_elem_stride(nodes[i]->type.kind);
			char mem[32];
			sprintf(mem, "[rbp - %d]", nodes[i]->lhs->anno_int);
			cg_expr(cg, tt, offs[i]);                 /* INV -> rax (invariant, call-free). */
			cg_emit(cg, "    mov rdx, %s", mem);      /* Array base pointer. */
			cg_emit(cg, "    lea %s, [rdx + rax*%d + 32]", CG_HOIST_REGS[next_reg], stride);
			cg->sr_node[cg->sr_n] = nodes[i];
			cg->sr_reg[cg->sr_n] = next_reg;
			cg->sr_stride[cg->sr_n] = stride;
			cg->sr_n++;
			next_reg++;
		}
	}

	/* Cache remaining hot loop-invariant locals (those not folded into an SR base). */
	HoistCand cand[64];
	int nc = 0;
	cg_hoist_reads_block(cg, body, w, wn, cand, &nc);
	while (next_reg < 4)
	{
		int best = -1;
		for (int i = 0; i < nc; i++)
		{
			if (cand[i].count > 0 && (best < 0 || cand[i].count > cand[best].count))
			{
				best = i;
			}
		}

		if (best < 0)
		{
			break;
		}

		char mem[32];
		sprintf(mem, "[rbp - %d]", cand[best].off);
		cg_load_scalar_into(cg, cand[best].k, mem, CG_HOIST_REGS[next_reg], CG_HOIST_REGS32[next_reg]);
		cg->hoist_off[cg->hoist_n] = cand[best].off;
		cg->hoist_reg[cg->hoist_n] = next_reg;
		cg->hoist_n++;
		cand[best].count = 0;
		next_reg++;
	}
}

static void cg_loop_hoist_end(Codegen *cg)
{
	if (cg->hoist_depth > 1)
	{
		/* This is a nested end; the outer loop still needs the hoist state. */
		cg->hoist_depth--;
		return;
	}

	cg->hoist_depth = 0;

	/* Re-extend each deferred accumulator once, now that the loop has exited, so
	   later 64-bit reads of it see a valid sign-extended register. Emitted after
	   the loop's end label, so it covers both the fall-through and break exits. */
	for (int i = 0; i < cg->defer_n; i++)
	{
		const char *R = cg_local_reg(cg, cg->defer_off[i]);
		if (R)
		{
			cg_emit(cg, "    movsxd %s, %sd", R, R);
		}
	}

	/* Loop-scoped float promotion: write each promoted double back to its slot
	   (on the guard-fail path this stores the values just loaded - a harmless
	   round trip) and, on Win64, restore the caller's xmm6.. from scratch. */
	for (int i = 0; i < cg->lpromo_n; i++)
	{
		cg_emit(cg, "    movsd qword [rbp - %d], %s", cg->lpromo_off[i], CG_LPROMO_REGS[i]);
	}

	if (cg->target == TARGET_WINDOWS)
	{
		for (int i = 0; i < cg->lpromo_n; i++)
		{
			cg_emit(cg, "    movups %s, [rbp - %d]", CG_LPROMO_REGS[i], cg->lpromo_save_base + i*16);
		}
	}

	cg->lpromo_n = 0;
	cg->hoist_n = 0;
	cg->sr_n = 0;
	cg->sr_ivreg = NULL;
	cg->defer_n = 0;
}

/* True if a block contains a break/continue (which an unrolled body cannot host -
   there is no enclosing loop label to target). */
static int cg_block_has_breakcont(Block *b);

static int cg_stmt_has_breakcont(Stmt *s)
{
	if (!s)
	{
		return 0;
	}

	if (s->kind == ST_BREAK || s->kind == ST_CONTINUE)
	{
		return 1;
	}

	return cg_block_has_breakcont(s->then_blk) || cg_block_has_breakcont(s->else_blk);
}

static int cg_block_has_breakcont(Block *b)
{
	if (!b)
	{
		return 0;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (cg_stmt_has_breakcont(b->stmts[i]))
		{
			return 1;
		}
	}

	return 0;
}

/* Fully unroll a fixed, small-trip `for (int i = LO; i </<= HI; i = i + STEP)` loop
   whose body is the clean arithmetic the hoist predicate accepts: emit the body
   once per iteration with the induction variable bound to a compile-time constant,
   so there is no counter, no branch, and strength-reduced accesses become constant
   offsets [base + k*stride]. Returns 1 if it unrolled, 0 to fall back to a loop. */
#define CG_UNROLL_MAX 8
static int cg_try_unroll(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	Stmt *fi = s->for_init;
	if (!fi || fi->kind != ST_VARDECL || !fi->decl_init || fi->decl_init->kind != EX_INT)
	{
		return 0;   /* Loop-scoped counter only, so nothing reads it after the loop. */
	}

	int io = fi->decl_offset;
	long long lo = fi->decl_init->int_val;
	if (io == 0)
	{
		return 0;
	}

	Expr *c = s->cond;
	if (!(c && c->kind == EX_BINARY && (c->op == TOKEN_LT || c->op == TOKEN_LTE)
		  && c->lhs->kind == EX_IDENT && c->lhs->anno_int == io && c->rhs->kind == EX_INT))
	{
		return 0;
	}

	long long hi = c->rhs->int_val;

	long long step = 0;
	Stmt *p = s->for_post;
	if (p && p->kind == ST_ASSIGN && p->target && p->target->kind == EX_IDENT
		&& p->target->anno_int == io && p->value && p->value->kind == EX_BINARY
		&& p->value->op == TOKEN_PLUS && p->value->lhs->kind == EX_IDENT
		&& p->value->lhs->anno_int == io && p->value->rhs->kind == EX_INT)
	{
		step = p->value->rhs->int_val;
	}
	else if (p && p->kind == ST_EXPR && p->expr && p->expr->kind == EX_INCDEC
			 && p->expr->op == TOKEN_PLUSPLUS && p->expr->lhs
			 && p->expr->lhs->kind == EX_IDENT && p->expr->lhs->anno_int == io)
	{
		step = 1;
	}

	if (step <= 0)
	{
		return 0;
	}

	long long trip = (c->op == TOKEN_LT)
					 ? ((hi > lo) ? (hi - lo + step - 1) / step : 0)
					 : ((hi >= lo) ? (hi - lo) / step + 1 : 0);
	if (trip <= 0 || trip > CG_UNROLL_MAX)
	{
		return 0;
	}

	Block *body = s->then_blk;
	if (!body || !cg_hoist_block_ok(body) || cg_block_has_breakcont(body))
	{
		return 0;
	}

	int w[128];
	int wn = 0;
	cg_hoist_writes_block(body, w, &wn);
	if (cg_off_in(w, wn, io))
	{
		return 0;   /* Body reassigns the induction variable: not a clean count. */
	}

	cg_loop_hoist_begin(cg, tt, s);   /* Pin SR bases / invariants / deferred accumulators. */
	int saved_iv_off = cg->unroll_iv_off;
	long long saved_iv_val = cg->unroll_iv_val;
	int saved_uc_n = cg->uc_n;
	cg->unrolling++;
	cg->unroll_iv_off = io;
	const char *ivreg = cg_local_reg(cg, io);
	for (long long k = lo; (c->op == TOKEN_LT) ? (k < hi) : (k <= hi); k += step)
	{
		cg->unroll_iv_val = k;
		cg_unroll_const_set(cg, io, k);
		/* Materialize the induction value in its home so any read that bypasses the
		   unroll-constant fast paths (a register operand, a slot load) still sees it.
		   Strength-reduced accesses use the constant offset directly and ignore this. */
		if (ivreg)
		{
			cg_emit(cg, "    mov %s, %lld", ivreg, k);
		}
		else
		{
			cg_emit(cg, "    mov dword [rbp - %d], %lld", io, k);
		}

		cg_block(cg, tt, f, body, in_main);
	}

	cg->unrolling--;
	cg->unroll_iv_off = saved_iv_off;   /* An enclosing unroll keeps its state (this used to be zeroed, stranding the outer copy without its constant). */
	cg->unroll_iv_val = saved_iv_val;
	cg->uc_n = saved_uc_n;              /* Drop this loop's iv and derived constants. */
	cg_loop_hoist_end(cg);              /* Re-extend deferred accumulators once. */
	return 1;
}

static void cg_for(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	if (cg_try_unroll(cg, tt, f, s, in_main))
	{
		return;
	}

	if (cg->unrolling)
	{
		/* A real (non-unrolled) loop inside an unrolled copy: anything it
		   writes varies per iteration, so it is no longer a copy-constant. */
		int w[128];
		int wn = 0;
		cg_hoist_writes_block(s->then_blk, w, &wn);
		cg_hoist_writes_stmt(s->for_init, w, &wn);
		cg_hoist_writes_stmt(s->for_post, w, &wn);
		for (int i = 0; i < wn; i++)
		{
			cg_unroll_const_kill(cg, w[i]);
		}
	}

	int top=cg_label(cg), end=cg_label(cg), cont=cg_label(cg);
	int sb=cg->cur_break_label, sc=cg->cur_continue_label;
	cg_stmt(cg,tt,f,s->for_init,in_main);

	/* A promoted, unit-step counter starting at a non-negative constant stays in
	   [0, 2^31) for the loop's life, so its `i=i+1` need not re-extend (a 32-bit
	   add zero-extends, which equals the sign-extension for a non-negative value).
	   cg_try_inplace consults cur_counter_off to skip the movsxd. */
	int saved_counter = cg->cur_counter_off;
	cg->cur_counter_off = 0;
	int civ = 0;
	if (cg_loop_induction(cg, s, &civ))
	{
		Stmt *fi = s->for_init;
		Expr *lo = (fi && fi->kind==ST_VARDECL) ? fi->decl_init
				   : (fi && fi->kind==ST_ASSIGN) ? fi->value : NULL;
		if (lo && lo->kind==EX_INT && lo->int_val >= 0)
		{
			cg->cur_counter_off = civ;
		}
	}

	cg_lpromo_begin(cg, tt, s);           /* Home hot doubles in xmm6.. before the guard (see cg_lpromo_begin). */
	cg_branch_unless(cg,tt,s->cond,end);  /* Entry guard: skip the loop if false up front. */
	cg_loop_hoist_begin(cg, tt, s);       /* Pin loop-invariant locals into r8..r11 if the body allows. */
	cg_emit(cg,".L%d:", top);
	cg->cur_break_label=end;
	cg->cur_continue_label=cont;
	cg_block(cg,tt,f,s->then_blk,in_main);
	cg->cur_break_label=sb;
	cg->cur_continue_label=sc;
	cg_emit(cg,".L%d:", cont);            /* Continue lands here -> post runs, then re-test. */
	cg_stmt(cg,tt,f,s->for_post,in_main);
	cg_branch_if(cg,tt,s->cond,top);      /* Bottom test = back-edge; no unconditional jmp. */
	cg_emit(cg,".L%d:", end);
	cg_loop_hoist_end(cg);
	cg->cur_counter_off = saved_counter;
}

/* Emit the per-iteration tail shared by both arms of an unswitched hoisted-list
   foreach: store the element (in rax / xmm0) to the loop variable, run the user
   body (break -> end, continue -> cont), then advance the register cursor and
   loop. Used for the flat (head==0) and ring (head!=0) loops, which duplicate
   this tail so the hot flat loop carries no per-element ring test. */
static void cg_foreach_vec_tail(Codegen *cg, TypeTable *tt, Func *f, Stmt *s,
                                int in_main, TypeKind et, const char *curreg,
                                int top, int cont, int end)
{
	if (ty_is_float(et))
	{
		char mem[32];
		cg_emit(cg, et==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
		sprintf(mem,"[rbp - %d]", s->decl_offset);
		cg_store_fp(cg,et,mem);
	}
	else
	{
		cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
	}

	int sb = cg->cur_break_label, sc = cg->cur_continue_label;
	cg->cur_break_label = end;
	cg->cur_continue_label = cont;
	cg_block(cg,tt,f,s->then_blk,in_main);
	cg->cur_break_label = sb;
	cg->cur_continue_label = sc;

	cg_emit(cg,".L%d:", cont);
	cg_emit(cg,"    add %s, 1", curreg);
	cg_emit(cg,"    jmp .L%d", top);
}

/* foreach over an array (index loop), string (byte loop), or map (control-byte
   slot scan). The loop variable receives each element/key borrowed (no retain);
   continue lands on the cursor advance, break on the end. An owned iterable
   temporary is released at loop exit. Reuses the shared loop-label fields. */
static void cg_foreach(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	TypeKind ik = s->expr->type.kind;          /* iterable: TY_ARRAY / TY_STRING / TY_MAP */
	int top = cg_label(cg), end = cg_label(cg), cont = cg_label(cg);
	int owned = expr_is_owned(s->expr);
	int sb = cg->cur_break_label, sc = cg->cur_continue_label;   /* Save the enclosing loop labels. */
	int gen_set = (ik==TY_GENERIC && strcmp(s->expr->type.class_name,"Set")==0);   /* Set is a map. */
	int gen_vec = (ik==TY_GENERIC && !gen_set);                                    /* List/Stack/Queue/Deque. */

	/* A value-element foreach with a call-free, nested-loop-free, mutation-free body
	   (cg_hoist_block_ok) has a loop-invariant collection: its data base, length,
	   head and cap are hoisted into r8..r11 once before the loop instead of being
	   reloaded through the collection pointer every iteration. cg_hoist_block_ok
	   guarantees no nested loop (which would reclaim r8..r11 via the loop-hoist pool),
	   no call (which would clobber them or free the list), and no managed store (so
	   the collection reference and its front cannot change mid-loop). */
	int hoist_vec = gen_vec && s->expr->type.elem && !ty_is_managed(s->expr->type.elem->kind)
					&& cg_hoist_block_ok(s->then_blk);

	/* The loop cursor may be promoted to a register (see promote.c); `cur` is the
	   operand to read/write it - either a register name or its stack slot. */
	const char *curreg = cg_local_reg(cg, s->fe_index_offset);
	char cur[24];
	if (curreg)
	{
		snprintf(cur, sizeof cur, "%s", curreg);
	}
	else
	{
		snprintf(cur, sizeof cur, "[rbp - %d]", s->fe_index_offset);
	}

	cg_expr(cg,tt,s->expr);                     /* Container pointer -> rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", s->fe_coll_offset);

	if (ik==TY_MAP || gen_set)
	{
		/* Iterate through an owned snapshot handle: the map itself (retained) when
		   confined - two instructions in the runtime - or a frozen clone when
		   shared, so a concurrent rehash can never invalidate the slot cursor.
		   The handle replaces the container in fe_coll; an owned original is
		   released right here (the handle holds its own reference), and the
		   handle itself is released at loop exit. fe_aux is free for maps
		   (string foreach only), so it stashes the handle across the release. */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_aligned_call(cg,"bzy_map_iter_snapshot");   /* Owned handle in rax. */
		if (owned)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", s->fe_aux_offset);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, [rbp - %d]", s->fe_aux_offset);
		}

		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_coll_offset);
	}

	if (curreg)
	{
		cg_emit(cg,"    mov %s, 0", curreg);
	}
	else
	{
		cg_emit(cg,"    mov qword [rbp - %d], 0", s->fe_index_offset);
	}

	if (ik==TY_STRING)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_aligned_call(cg,"bzy_str_len");
		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_len_offset);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_aligned_call(cg,"bzy_str_data");
		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_aux_offset);
	}

	if (hoist_vec)
	{
		/* Loop-invariant collection fields -> r8..r11 once (see hoist_vec note). */
		cg_emit(cg,"    mov rdx, [rbp - %d]", s->fe_coll_offset);
		cg_emit(cg,"    mov r9, [rdx + 48]");    /* data array ptr */
		cg_emit(cg,"    mov r10, [rdx + 24]");   /* length */
		cg_emit(cg,"    mov r11, [rdx + 40]");   /* head */
		cg_emit(cg,"    mov r8, [rdx + 32]");    /* cap */
	}

	/* Unswitch the hoisted list loop on the loop-invariant head when the cursor
	   is register-resident: head==0 (any list that never had a front
	   insertion/removal - the common case) runs a flat loop that indexes the
	   data array directly, carrying no per-element ring test or wrap, matching a
	   plain contiguous array sweep. head!=0 runs the ring loop. The body is
	   duplicated (cheap: hoist_vec requires a call-free, nested-loop-free body),
	   each arm exits to the shared end. */
	if (hoist_vec && curreg)
	{
		TypeKind et = s->expr->type.elem->kind;
		int flat_top = cg_label(cg), flat_cont = cg_label(cg);
		int ring_top = cg_label(cg), ring_cont = cg_label(cg);

		cg_emit(cg,"    test r11, r11");
		cg_emit(cg,"    jnz .L%d", ring_top);

		/* Flat loop: head == 0, so phys == index; index the data base directly. */
		cg_emit(cg,".L%d:", flat_top);
		cg_emit(cg,"    cmp %s, r10", curreg);
		cg_emit(cg,"    jge .L%d", end);
		cg_emit(cg,"    mov rax, [r9 + %s*8 + 32]", curreg);
		cg_foreach_vec_tail(cg,tt,f,s,in_main,et,curreg,flat_top,flat_cont,end);

		/* Ring loop: head != 0, phys = (head + index) & (cap - 1). */
		cg_emit(cg,".L%d:", ring_top);
		cg_emit(cg,"    mov rcx, %s", curreg);
		cg_emit(cg,"    cmp rcx, r10");
		cg_emit(cg,"    jge .L%d", end);
		cg_emit(cg,"    add rcx, r11");
		cg_emit(cg,"    mov rax, r8");
		cg_emit(cg,"    dec rax");
		cg_emit(cg,"    and rcx, rax");
		cg_emit(cg,"    mov rax, [r9 + rcx*8 + 32]");
		cg_foreach_vec_tail(cg,tt,f,s,in_main,et,curreg,ring_top,ring_cont,end);

		cg_emit(cg,".L%d:", end);
		if (owned)   /* An owned iterable temporary is released here. */
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
			cg_release_rcx(cg);
		}

		return;
	}

	cg_emit(cg,".L%d:", top);
	if (ik==TY_MAP || gen_set)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 1), cur);
		cg_aligned_call(cg,"bzy_map_iter");      /* Next full slot or -1 in rax. */
		cg_emit(cg,"    mov %s, rax", cur);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    jl .L%d", end);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 1), cur);
		cg_aligned_call(cg,"bzy_map_key_at");    /* Key (borrowed) in rax. */
		cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
		if (s->fe_val_type.kind != TY_VOID)
		{
			TypeKind vt = s->fe_val_type.kind;
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
			cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 1), cur);
			cg_aligned_call(cg,"bzy_map_val_at");   /* Value bits (borrowed) in rax. */
			if (ty_is_float(vt))
			{
				char mem[32];
				cg_emit(cg, vt==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
				sprintf(mem,"[rbp - %d]", s->fe_val_offset);
				cg_store_fp(cg,vt,mem);
			}
			else
			{
				cg_store_local_off(cg, s->fe_val_offset, vt);   /* Register home if promoted, else slot. */
			}
		}
	}
	else if (ik==TY_STRING)
	{
		cg_emit(cg,"    mov rcx, %s", cur);
		cg_emit(cg,"    cmp rcx, [rbp - %d]", s->fe_len_offset);
		cg_emit(cg,"    jge .L%d", end);
		cg_emit(cg,"    mov rax, [rbp - %d]", s->fe_aux_offset);
		cg_emit(cg,"    movzx eax, byte [rax + rcx]");   /* byte -> int (zero-extended). */
		cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
	}
	else if (gen_vec)
	{
		TypeKind et = s->expr->type.elem->kind;
		int managed = ty_is_managed(et);
		if (hoist_vec)
		{
			/* Fully hoisted value path: data/length/head/cap live in r9/r10/r11/r8
			   across the whole loop (see hoist_vec note), so the body reloads
			   nothing through the collection pointer. phys = (head+i) & (cap-1),
			   with the head==0 common case skipping the ring arithmetic. */
			int phys_done = cg_label(cg);
			cg_emit(cg,"    mov rcx, %s", cur);
			cg_emit(cg,"    cmp rcx, r10");               /* index vs hoisted length */
			cg_emit(cg,"    jge .L%d", end);
			cg_emit(cg,"    test r11, r11");              /* hoisted head */
			cg_emit(cg,"    jz .L%d", phys_done);         /* head == 0 -> phys = index */
			cg_emit(cg,"    add rcx, r11");               /* head + index */
			cg_emit(cg,"    mov rax, r8");                /* cap (hoisted) */
			cg_emit(cg,"    dec rax");                    /* cap - 1 */
			cg_emit(cg,"    and rcx, rax");               /* phys = (head+i) & (cap-1) */
			cg_emit(cg,".L%d:", phys_done);
			cg_emit(cg,"    mov rax, [r9 + rcx*8 + 32]"); /* slot value via hoisted data base */
		}
		else
		{
			cg_emit(cg,"    mov rcx, %s", cur);
			cg_emit(cg,"    mov rdx, [rbp - %d]", s->fe_coll_offset);
			cg_emit(cg,"    cmp rcx, [rdx + 24]");           /* index vs length@24 */
			cg_emit(cg,"    jge .L%d", end);
			if (managed)
			{
				cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
				cg_emit(cg,"    mov %s, %s", cg_iarg(cg, 1), cur);
				cg_aligned_call(cg,"bzy_vec_get");           /* Element (owned -> retained) in rax. */
			}
			else
			{
				/* Value element: inline the ring load, skipping the bzy_vec_get
				   call, its redundant bounds check, and the value-case no-op
				   retain. rcx=index, rdx=coll survive from the bounds check above.
				   Vector layout: cap@32, head@40, data array@48; the data array
				   holds its 8-byte slots at +32. phys = (head+i) & (cap-1) (cap is
				   a power of two). No call here, so r8/rax/rcx scratch is safe.

				   Every list that has never had a front insertion/removal keeps
				   head == 0 (the common case - plain List), and then phys == i, so
				   skip the ring arithmetic (a cap load plus add/dec/and) entirely.
				   head is re-tested each iteration, so a body that mutates the front
				   mid-loop stays correct. */
				int phys_done = cg_label(cg);
				cg_emit(cg,"    mov rax, [rdx + 48]");        /* data array ptr */
				cg_emit(cg,"    mov r8, [rdx + 40]");         /* head */
				cg_emit(cg,"    test r8, r8");
				cg_emit(cg,"    jz .L%d", phys_done);         /* head == 0 -> phys = index (rcx unchanged) */
				cg_emit(cg,"    add rcx, r8");                /* head + index */
				cg_emit(cg,"    mov r8, [rdx + 32]");         /* cap */
				cg_emit(cg,"    dec r8");                     /* cap - 1 */
				cg_emit(cg,"    and rcx, r8");                /* phys = (head+i) & (cap-1) */
				cg_emit(cg,".L%d:", phys_done);
				cg_emit(cg,"    mov rax, [rax + rcx*8 + 32]");/* slot value -> rax */
			}
		}
		if (ty_is_float(et))
		{
			char mem[32];
			cg_emit(cg, et==TY_FLOAT ? "    movd xmm0, eax" : "    movq xmm0, rax");
			sprintf(mem,"[rbp - %d]", s->decl_offset);
			cg_store_fp(cg,et,mem);
		}
		else
		{
			cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
		}

		if (managed)   /* bzy_vec_get returned owned; the loop var is borrowed. */
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->decl_offset);
			cg_release_rcx(cg);
		}
	}
	else   /* TY_ARRAY */
	{
		TypeKind et = s->expr->type.elem->kind;
		cg_emit(cg,"    mov rax, [rbp - %d]", s->fe_coll_offset);
		cg_emit(cg,"    mov rcx, %s", cur);
		cg_emit(cg,"    cmp rcx, [rax + 24]");           /* index vs length */
		cg_emit(cg,"    jge .L%d", end);
		cg_emit(cg,"    lea rbx, [rax + rcx*%d + 32]", cg_elem_stride(et));    /* element address */
		if (ty_is_float(et))
		{
			char mem[32];
			sprintf(mem,"[rbp - %d]", s->decl_offset);
			cg_load_fp(cg,et,"[rbx]");
			cg_store_fp(cg,et,mem);
		}
		else
		{
			cg_load_scalar(cg,et,"[rbx]");
			cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
		}
	}

	cg->cur_break_label = end;            /* break -> .Lend; continue -> .Lcont (the increment). */
	cg->cur_continue_label = cont;
	cg_block(cg,tt,f,s->then_blk,in_main);
	cg->cur_break_label = sb;
	cg->cur_continue_label = sc;

	cg_emit(cg,".L%d:", cont);            /* continue lands here, then the cursor advances. */
	if (curreg)
	{
		cg_emit(cg,"    add %s, 1", curreg);   /* Cursor in a register (map slot scan or 0..len index). */
	}
	else
	{
		cg_emit(cg,"    inc qword [rbp - %d]", s->fe_index_offset);
	}

	cg_emit(cg,"    jmp .L%d", top);
	cg_emit(cg,".L%d:", end);

	if (ik==TY_MAP || gen_set)
	{
		/* The snapshot handle is always owned (an owned original was already
		   released at setup, after the handle took its own reference). */
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_release_rcx(cg);
	}
	else if (owned)   /* An owned iterable temporary (e.g. `new int[3]`) is released here. */
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_release_rcx(cg);
	}
}

/* C-style switch: case/default are label statements in the body block, so
   fallthrough is automatic (bodies emit contiguously) and only break exits.
   A pre-pass assigns a label per case/default; dispatch is a compare-chain;
   break targets the switch end. */
/* String switch: a compare-chain via bzy_str_eq (no jump table on pointers). The
   operand persists in assign_save; the matched target label is computed into
   fp_save (a stack slot survives the calls), then a single indirect jmp dispatches
   so an owned operand is released exactly once regardless of which case matched. */
static void cg_switch_string(Codegen *cg, TypeTable *tt, Stmt *s, Block *b, int default_lbl)
{
	cg_expr(cg,tt,s->cond);                                  /* Operand string -> rax. */
	int owned = expr_is_owned(s->cond);
	cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);  /* Operand (persists). */
	cg_emit(cg,"    lea rax, [rel .L%d]", default_lbl);
	cg_emit(cg,"    mov [rbp - %d], rax", cg->fp_save);      /* Matched target := default. */
	for (int i=0; i<b->count; i++)
	{
		Stmt *c=b->stmts[i];
		if (c->kind!=ST_CASE)
		{
			continue;
		}

		int skip=cg_label(cg);
		cg_expr(cg,tt,c->value);                             /* Case literal -> rax (owned). */
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save); /* Case string (to release). */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), cg->assign_save);
		cg_aligned_call(cg,"bzy_str_eq");                    /* rax = 0/1. */
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d", skip);
		cg_emit(cg,"    lea rax, [rel .L%d]", c->decl_offset);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->fp_save);  /* Matched target := this case. */
		cg_emit(cg,".L%d:", skip);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->val_save);
		cg_release_rcx(cg);                                  /* Release the case literal. */
	}

	if (owned)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), cg->assign_save);
		cg_release_rcx(cg);
	}

	cg_emit(cg,"    mov rax, [rbp - %d]", cg->fp_save);
	cg_emit(cg,"    jmp rax");
}

/* Select: try each arm in order via the channel try-primitives; the first ready
   arm runs. With a default arm the select is non-blocking (the default runs when
   nothing is ready); without one it blocks, cooperatively yielding and retrying
   until an arm becomes ready. One scratch slot is shared across the (sequential)
   arm attempts: [rbp-b] holds the channel, [rbp-(b-8)] the received/sent value.
   The dispatch chain jumps to per-arm body labels; bodies are emitted after the
   scratch is freed. */
static void cg_select(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	int blocking = (s->else_blk == NULL);
	int end = cg_label(cg);
	int retry = blocking ? cg_label(cg) : end;
	int *armlbl = malloc(sizeof(int) * (size_t)s->sel_arm_count);
	for (int i = 0; i < s->sel_arm_count; i++)
	{
		armlbl[i] = cg_label(cg);
	}

	int b = cg_scratch_alloc(cg, 16);
	if (blocking)
	{
		cg_emit(cg, ".L%d:", retry);          /* Re-poll point: loop back here after yielding. */
	}

	for (int i = 0; i < s->sel_arm_count; i++)
	{
		SelectArm *a = &s->sel_arms[i];
		TypeKind et = a->chan->type.elem->kind;
		int managed = ty_is_managed(et);

		if (!a->is_send)
		{
			cg_expr(cg, tt, a->chan);                        /* Channel -> rax. */
			cg_emit(cg, "    mov [rbp - %d], rax", b);
			cg_emit(cg, "    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
			cg_emit(cg, "    lea %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
			cg_aligned_call(cg, "bzy_channel_try_recv");     /* rax = 1/0, value at [rbp-(b-8)]. */
			cg_emit(cg, "    cmp rax, 0");
			int next = cg_label(cg);
			cg_emit(cg, "    je .L%d", next);
			if (a->bind[0])
			{
				cg_emit(cg, "    mov rax, [rbp - %d]", b - 8);
				cg_emit(cg, "    mov [rbp - %d], rax", a->bind_offset);   /* Bind (raw bits; owned value moves in). */
			}
			else if (managed)
			{
				cg_emit(cg, "    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
				cg_release_rcx(cg);                          /* Discarded owned value. */
			}

			cg_emit(cg, "    jmp .L%d", armlbl[i]);
			cg_emit(cg, ".L%d:", next);
		}
		else
		{
			cg_expr(cg, tt, a->chan);                        /* Channel -> rax. */
			cg_emit(cg, "    mov [rbp - %d], rax", b);
			if (managed)
			{
				cg_expr_owned(cg, tt, a->send_val);          /* +1; moves in on success. */
			}
			else
			{
				cg_expr(cg, tt, a->send_val);
			}

			if (ty_is_float(et))
			{
				cg_emit(cg, et == TY_FLOAT ? "    movd eax, xmm0" : "    movq rax, xmm0");
			}

			cg_emit(cg, "    mov [rbp - %d], rax", b - 8);
			cg_emit(cg, "    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
			cg_emit(cg, "    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
			cg_aligned_call(cg, "bzy_channel_try_send");     /* rax = 1/0. */
			cg_emit(cg, "    cmp rax, 0");
			cg_emit(cg, "    jne .L%d", armlbl[i]);           /* Sent: channel owns the value. */
			if (managed)
			{
				cg_emit(cg, "    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
				cg_release_rcx(cg);                          /* Full: reclaim the +1 we took. */
			}
		}
	}

	cg_scratch_free(cg, 16);

	if (blocking)
	{
		cg_aligned_call(cg, "bzy_yield");           /* Nothing ready: yield, then re-poll. */
		cg_emit(cg, "    jmp .L%d", retry);
	}
	else
	{
		cg_block(cg, tt, f, s->else_blk, in_main);  /* Default arm: no arm was ready. */
		cg_emit(cg, "    jmp .L%d", end);
	}

	for (int i = 0; i < s->sel_arm_count; i++)
	{
		cg_emit(cg, ".L%d:", armlbl[i]);
		cg_block(cg, tt, f, s->sel_arms[i].body, in_main);
		cg_emit(cg, "    jmp .L%d", end);
	}

	cg_emit(cg, ".L%d:", end);
	free(armlbl);
}

static void cg_switch(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	Block *b = s->then_blk;
	int end = cg_label(cg);
	int default_lbl = end;                 /* No-match target; overridden if a default exists. */

	for (int i=0; i<b->count; i++)         /* Pre-pass: one label per case/default position. */
	{
		Stmt *c=b->stmts[i];
		if (c->kind==ST_CASE || c->kind==ST_DEFAULT)
		{
			c->decl_offset = cg_label(cg);
			if (c->kind==ST_DEFAULT)
			{
				default_lbl = c->decl_offset;
			}
		}
	}

	if (s->cond->type.kind==TY_STRING)
	{
		cg_switch_string(cg,tt,s,b,default_lbl);
		goto bodies;
	}

	/* Collect case-value extent for the dense/sparse choice. */
	long long lo=0, hi=0;
	int ncases=0;
	for (int i=0; i<b->count; i++)
	{
		if (b->stmts[i]->kind==ST_CASE)
		{
			long long v=b->stmts[i]->value->int_val;
			if (ncases==0 || v<lo)
			{
				lo=v;
			}

			if (ncases==0 || v>hi)
			{
				hi=v;
			}

			ncases++;
		}
	}

	long long range = (ncases>0) ? (hi - lo + 1) : 0;
	int dense = ncases>=4 && range<=4*ncases && range<=4096;

	cg_expr(cg,tt,s->cond);                 /* operand -> rax */
	if (s->cond->type.kind==TY_OBJECT && enum_is(s->cond->type.class_name))
	{
		/* Enum operand: switch on the hidden __ordinal (offset 24). Release the
		   operand object first if it was an owned read (rbx is callee-saved, so it
		   survives bzy_release and carries the ordinal across). */
		if (expr_is_owned(s->cond))
		{
			cg_emit(cg,"    mov rbx, [rax + 24]");
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_release_rcx(cg);
			cg_emit(cg,"    mov rax, rbx");
		}
		else
		{
			cg_emit(cg,"    mov rax, [rax + 24]");
		}
	}

	if (dense)
	{
		int tab = cg_label(cg);
		cg_emit(cg,"    mov rcx, rax");     /* rcx/rdx: jump-table scratch (no call); same on both ABIs. */
		if (lo != 0)
		{
			cg_emit(cg,"    sub rcx, %lld", lo);   /* Rebase to 0; skipped when the lowest case is already 0. */
		}

		cg_emit(cg,"    cmp rcx, %lld", range);
		cg_emit(cg,"    jae .L%d", default_lbl);     /* unsigned: outside [lo,hi] -> default/end */
		cg_emit(cg,"    lea rdx, [rel .L%d]", tab);
		cg_emit(cg,"    jmp [rdx + rcx*8]");
		cg_emit(cg,".L%d:", tab);                    /* Inline table (jumped over; never executed). */
		for (long long v=lo; v<=hi; v++)
		{
			int target = default_lbl;
			for (int i=0; i<b->count; i++)
			{
				if (b->stmts[i]->kind==ST_CASE && b->stmts[i]->value->int_val==v)
				{
					target = b->stmts[i]->decl_offset;
					break;
				}
			}

			cg_emit(cg,"    dq .L%d", target);       /* Gaps -> default/end. */
		}
	}
	else
	{
		for (int i=0; i<b->count; i++)          /* Compare-chain dispatch. */
		{
			Stmt *c=b->stmts[i];
			if (c->kind==ST_CASE)
			{
				cg_emit(cg,"    cmp rax, %lld", c->value->int_val);
				cg_emit(cg,"    je .L%d", c->decl_offset);
			}
		}

		cg_emit(cg,"    jmp .L%d", default_lbl);
	}

bodies:
	;
	int sb = cg->cur_break_label;
	cg->cur_break_label = end;              /* break -> switch end; continue unchanged. */
	for (int i=0; i<b->count; i++)          /* Body in source order; labels emit contiguously. */
	{
		Stmt *c=b->stmts[i];
		if (c->kind==ST_CASE || c->kind==ST_DEFAULT)
		{
			cg_emit(cg,".L%d:", c->decl_offset);
		}
		else
		{
			cg_stmt(cg,tt,f,c,in_main);
		}
	}

	cg->cur_break_label = sb;
	cg_emit(cg,".L%d:", end);
}

/* try/catch (5e-2): the body is emitted inline between two file-unique labels;
   normal completion jmps over the inline landing pad. The pad is reached only
   by the unwinder (which sets rax = the caught exception), binds it into the
   catch variable's slot (transferring the owned +1), and runs the catch body.
   The region is recorded for this function's try-table (emitted in the exception record). */
static void cg_try(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	int k = cg->exception_try_count++;
	int after = cg_label(cg);
	cg_emit(cg,"..@exceptiontry%d_s:", k);                /* `..@` labels are file-global yet don't reset .L scope. */
	cg_block(cg,tt,f,s->then_blk,in_main);
	cg_emit(cg,"..@exceptiontry%d_e:", k);
	cg_emit(cg,"    jmp .L%d", after);             /* Normal path: skip all landing pads. */
	for (int c=0; c<s->else_blk->count; c++)
	{
		Stmt *cl = s->else_blk->stmts[c];
		cg_emit(cg,"..@exceptiontry%d_p%d:", k, c);       /* Landing pad: rax = caught exception. */
		cg_emit(cg,"    mov [rbp - %d], rax", cl->decl_offset);   /* Bind (transfer owned). */
		cg_block(cg,tt,f,cl->then_blk,in_main);
		cg_emit(cg,"    jmp .L%d", after);         /* After the handler, leave the try. */
		if (cg->cur_try_count == cg->cur_try_cap)
		{
			cg->cur_try_cap = cg->cur_try_cap ? cg->cur_try_cap * 2 : 8;
			cg->cur_try_k  = realloc(cg->cur_try_k,  (size_t)cg->cur_try_cap * sizeof(*cg->cur_try_k));
			cg->cur_try_c  = realloc(cg->cur_try_c,  (size_t)cg->cur_try_cap * sizeof(*cg->cur_try_c));
			cg->cur_try_vt = realloc(cg->cur_try_vt, (size_t)cg->cur_try_cap * sizeof(*cg->cur_try_vt));
		}

		cg->cur_try_k[cg->cur_try_count] = k;
		cg->cur_try_c[cg->cur_try_count] = c;
		strcpy(cg->cur_try_vt[cg->cur_try_count], cl->decl_type.class_name);
		cg->cur_try_count++;
	}

	cg_emit(cg,".L%d:", after);
}

/* Record that target fi needs a __breeze_<label> thunk (deduped); emitted once
   in cg_program after the function bodies. */
static void cg_request_breeze_thunk(Codegen *cg, FuncInfo *fi)
{
	for (int i=0; i<cg->breeze_thunk_count; i++)
	{
		if (cg->breeze_thunks[i] == fi)
		{
			return;
		}
	}

	cg->breeze_thunks = grow_ensure(cg->breeze_thunks, cg->breeze_thunk_count, &cg->breeze_thunk_cap, sizeof(*cg->breeze_thunks));
	cg->breeze_thunks[cg->breeze_thunk_count++] = fi;
}

/* The per-target spawn thunk: rcx = arg buffer (the breeze's inline argbuf). Loads
   each arg into its Win64 parameter register by class (int -> rcx/rdx/r8/r9, fp ->
   xmm0..3 by position), calls the target, releases managed args (the target borrowed
   them), and returns to breeze_run. The buffer is not freed - it is recycled with the
   pooled breeze. */
static void cg_emit_breeze_thunk(Codegen *cg, FuncInfo *fi)
{
	cg->cur_func = NULL;   /* Synthesized frame: promotion helpers must fall back to slots. */
	const char *a0 = cg_iarg(cg, 0);   /* The thunk's incoming block ptr + every 1-arg call's arg0. */
	cg_emit(cg,"__breeze_%s:", fi->asm_label);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, 48");                 /* 16-aligned: block save at [rbp-8] + shadow. */
	cg_emit(cg,"    mov [rbp - 8], %s", a0);       /* Save the block pointer. */
	int int_idx = 0, fp_idx = 0;
	for (int i=0; i<fi->param_count; i++)
	{
		TypeKind k = fi->param_types[i].kind;
		cg_emit(cg,"    mov rax, [rbp - 8]");
		if (ty_is_float(k))
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : i;
			cg_emit(cg, k==TY_FLOAT ? "    movd xmm%d, [rax + %d]" : "    movq xmm%d, [rax + %d]", xi, i*8);
		}
		else
		{
			int ii = (cg->target==TARGET_LINUX) ? int_idx++ : i;
			cg_emit(cg,"    mov %s, [rax + %d]", cg_iarg(cg, ii), i*8);
		}
	}

	/* rsp is 16-aligned at rbp-48 throughout; each call reserves its own 32-byte
	   shadow manually (cg_aligned_call can't be used -- it keys on the per-function
	   sp_save slot, which this hand-rolled frame does not own). */
	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call %s", fi->asm_label);
	cg_emit(cg,"    add rsp, 32");
	for (int i=0; i<fi->param_count; i++)
	{
		if (ty_is_managed(fi->param_types[i].kind))
		{
			cg_emit(cg,"    mov %s, [rbp - 8]", a0);
			cg_emit(cg,"    mov %s, [%s + %d]", a0, a0, i*8);
			cg_emit(cg,"    sub rsp, 32");
			cg_emit(cg,"    call bzy_release");
			cg_emit(cg,"    add rsp, 32");
		}
	}

	/* No free: the arg buffer is the pooled Breeze's inline argbuf, recycled with it. */
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
}

static void cg_request_blocking_thunk(Codegen *cg, FuncInfo *fi)
{
	for (int i=0; i<cg->blocking_thunk_count; i++)
	{
		if (cg->blocking_thunks[i] == fi)
		{
			return;
		}
	}

	cg->blocking_thunks = grow_ensure(cg->blocking_thunks, cg->blocking_thunk_count, &cg->blocking_thunk_cap, sizeof(*cg->blocking_thunks));
	cg->blocking_thunks[cg->blocking_thunk_count++] = fi;
}

/* The per-target blocking thunk, run on an offload worker: arg0 = ctx blob.
   ctx[i*8] holds arg i (raw object pointer for a string/array); the result slot
   (cg_blocking_result_off) receives the return value. Places register arguments,
   spills any overflow arguments to the outgoing stack area, marshals a string or
   value array to its data pointer (+32), calls the raw C symbol, stores the result.
   With no overflow arguments the emitted code is byte-identical to the historical
   <=4-argument thunk (sub rsp,32 / call / result at +32). */
static void cg_emit_blocking_thunk(Codegen *cg, FuncInfo *fi)
{
	cg->cur_func = NULL;   /* Synthesized frame: promotion helpers must fall back to slots. */
	int win = (cg->target != TARGET_LINUX);
	const char *a0 = cg_iarg(cg, 0);   /* The thunk's incoming ctx pointer (arg0). */
	cg_emit(cg,"__blocking_%s:", fi->asm_label);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, 48");                 /* 16-aligned: ctx save at [rbp-8] + scratch. */
	cg_emit(cg,"    mov [rbp - 8], %s", a0);       /* Save the ctx pointer. */

	/* First pass: place the arguments that fit in registers. */
	int int_idx = 0, fp_idx = 0, stk = 0;
	for (int i=0; i<fi->param_count; i++)
	{
		TypeKind k = fi->param_types[i].kind;
		int is_fp = ty_is_float(k);
		int on_stack;
		if (win)
		{
			on_stack = (i >= 4);
		}
		else if (is_fp)
		{
			on_stack = (fp_idx >= 8);
			if (!on_stack) { fp_idx++; }
		}
		else
		{
			on_stack = (int_idx >= 6);
			if (!on_stack) { int_idx++; }
		}

		if (on_stack)
		{
			stk++;
			continue;
		}

		cg_emit(cg,"    mov rax, [rbp - 8]");
		if (is_fp)
		{
			int xi = win ? i : (fp_idx - 1);
			cg_emit(cg, k==TY_FLOAT ? "    movd xmm%d, [rax + %d]" : "    movq xmm%d, [rax + %d]", xi, i*8);
		}
		else
		{
			int ii = win ? i : (int_idx - 1);
			const char *r = cg_iarg(cg, ii);
			cg_emit(cg,"    mov %s, [rax + %d]", r, i*8);
			if (k==TY_STRING || k==TY_ARRAY)
			{
				cg_emit(cg,"    add %s, 32", r);   /* string/value-array object -> data pointer. */
			}
		}
	}

	/* Outgoing area: Win64 keeps a 32-byte shadow; both targets append 8 bytes per
	   stack-spilled argument, 16-aligned. With no stack args this is 32 on both,
	   preserving the historical sub rsp,32 / add rsp,32. */
	int outgoing = ((32 + stk*8) + 15) & ~15;
	cg_emit(cg,"    sub rsp, %d", outgoing);

	/* Second pass: spill overflow arguments to [rsp + dst], via r10 (never an
	   argument register on either ABI). */
	int_idx = 0; fp_idx = 0; stk = 0;
	for (int i=0; i<fi->param_count; i++)
	{
		TypeKind k = fi->param_types[i].kind;
		int is_fp = ty_is_float(k);
		int on_stack;
		if (win)
		{
			on_stack = (i >= 4);
		}
		else if (is_fp)
		{
			on_stack = (fp_idx >= 8);
			if (!on_stack) { fp_idx++; }
		}
		else
		{
			on_stack = (int_idx >= 6);
			if (!on_stack) { int_idx++; }
		}

		if (!on_stack)
		{
			continue;
		}

		int dst = win ? (32 + stk*8) : (stk*8);
		stk++;
		cg_emit(cg,"    mov rax, [rbp - 8]");
		cg_emit(cg,"    mov r10, [rax + %d]", i*8);
		if (!is_fp && (k==TY_STRING || k==TY_ARRAY))
		{
			cg_emit(cg,"    add r10, 32");
		}

		cg_emit(cg,"    mov [rsp + %d], r10", dst);
	}

	cg_emit(cg,"    call %s", fi->asm_label);      /* The raw ($-escaped) C symbol. */
	cg_emit(cg,"    add rsp, %d", outgoing);
	cg_emit(cg,"    mov %s, [rbp - 8]", a0);       /* Reload ctx pointer (a0 was an arg reg). */

	int roff = cg_blocking_result_off(fi->param_count);
	if (fi->ret_type.kind==TY_DOUBLE)
	{
		cg_emit(cg,"    movq [%s + %d], xmm0", a0, roff);
	}
	else if (fi->ret_type.kind==TY_FLOAT)
	{
		cg_emit(cg,"    movd [%s + %d], xmm0", a0, roff);
	}
	else if (fi->ret_type.kind!=TY_VOID)
	{
		cg_emit(cg,"    mov [%s + %d], rax", a0, roff);
	}

	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
}

static void cg_stmt(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	/* No expression is mid-evaluation at a statement boundary, so every temp slot
	   pushed during the previous statement must have been popped. A non-zero depth
	   here means a converted push/pop pair is unbalanced. */
	if (cg->cur_temp_depth != 0)
	{
		fprintf(stderr,"Codegen: temp depth %d at a statement boundary (unbalanced push/pop).\n", cg->cur_temp_depth);
		exit(1);
	}

	/* Likewise every scratch-arena block allocated during the previous statement
	   must have been freed; a non-zero cursor means a converted block is unbalanced. */
	if (cg->cur_scratch != 0)
	{
		fprintf(stderr,"Codegen: scratch cursor %d at a statement boundary (unbalanced arena alloc/free).\n", cg->cur_scratch);
		exit(1);
	}

	switch (s->kind)
	{
	case ST_VARDECL:
		if (s->decl_init)
		{
			long long dcv;
			if (cg->unrolling && s->decl_offset > 0
				&& (s->decl_type.kind == TY_INT || s->decl_type.kind == TY_LONG)
				&& cg_fold_const(cg, s->decl_init, &dcv))
			{
				/* Inside an unrolled copy, a local assigned a foldable
				   expression of the induction constants is itself a constant
				   for this copy (cb = u * 8): record it so reads fold, and
				   store the immediate to keep the slot truthful. */
				cg_unroll_const_set(cg, s->decl_offset, dcv);
				cg_emit(cg,"    mov rax, %lld", dcv);
				cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
				break;
			}

			if (cg->unrolling && s->decl_offset > 0)
			{
				cg_unroll_const_kill(cg, s->decl_offset);   /* Reassigned non-foldably. */
			}

			if (ty_is_managed(s->decl_type.kind))
			{
				cg_expr_owned(cg,tt,s->decl_init);
				cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
			}
			else if (ty_is_simd(s->decl_type.kind))
			{
				cg_expr(cg,tt,s->decl_init);                       /* Packed value -> xmm0. */
				cg_store_local_simd(cg, s->decl_offset, s->decl_type.kind);           /* XMM home if promoted, else 16-byte slot. */
			}
			else if (!cg_fp_load_into_home(cg, s->decl_offset, s->decl_type.kind, s->decl_init))
			{
				cg_expr(cg,tt,s->decl_init);
				cg_coerce(cg,s->decl_type.kind,s->decl_init->type.kind);
				if (ty_is_float(s->decl_type.kind))
				{
					cg_store_local_fp(cg, s->decl_offset, s->decl_type.kind);   /* XMM home if promoted, else slot. */
				}
				else
				{
					cg_store_local_off(cg, s->decl_offset, s->decl_type.kind);
				}
			}
		}
		break;
	case ST_ASSIGN:
		if (cg->unrolling && s->target->kind == EX_IDENT && s->target->anno_int > 0
			&& s != cg->cur_accum_stmt && !ty_is_managed(s->target->type.kind))
		{
			long long acv;
			if ((s->target->type.kind == TY_INT || s->target->type.kind == TY_LONG)
				&& cg_fold_const(cg, s->value, &acv))
			{
				/* Copy-constant reassignment: record and store the immediate. */
				cg_unroll_const_set(cg, s->target->anno_int, acv);
				cg_emit(cg,"    mov rax, %lld", acv);
				cg_store_local_off(cg, s->target->anno_int, s->target->type.kind);
				break;
			}

			cg_unroll_const_kill(cg, s->target->anno_int);   /* Reassigned non-foldably. */
		}

		if (s == cg->cur_accum_stmt)   /* P5: lower this iteration's accumulation to sb appends. */
		{
			cg_accum_append(cg,tt,s);
		}
		else if (ty_is_managed(s->target->type.kind))
		{
			cg_assign_object(cg,tt,s->target,s->value);
		}
		else if (cg_try_inplace(cg,tt,s->target,s->value))
		{
			/* Emitted in place on the target's promoted register. */
		}
		else if (s->target->kind == EX_IDENT && s->target->anno_int > 0
				 && cg_fp_load_into_home(cg, s->target->anno_int, s->target->type.kind, s->value))
		{
			/* Element loaded straight into the target's XMM home. */
		}
		else if (cg_try_rmw_index(cg,tt,s->target,s->value))
		{
			/* arr[X] = arr[X] op V fused: one indexed address, one bounds check. */
		}
		else if (ty_is_simd(s->target->type.kind) && cg_try_simd_inplace(cg,tt,s->target,s->value))
		{
			/* Packed accumulate fused straight into the target's xmm home. */
		}
		else if (ty_is_simd(s->target->type.kind) && s->target->kind == EX_IDENT && s->target->anno_int > 0)
		{
			cg_expr(cg,tt,s->value);                               /* Packed value -> xmm0. */
			cg_store_local_simd(cg, s->target->anno_int, s->target->type.kind);          /* XMM home if promoted, else slot. */
		}
		else
		{
			cg_expr(cg,tt,s->value);
			cg_coerce(cg,s->target->type.kind,s->value->type.kind);
			cg_store(cg,tt,s->target);
		}
		break;
	case ST_EXPR:
		if (cg->unrolling && s->expr && s->expr->kind == EX_INCDEC
			&& s->expr->lhs && s->expr->lhs->kind == EX_IDENT
			&& s->expr->lhs->anno_int > 0)
		{
			cg_unroll_const_kill(cg, s->expr->lhs->anno_int);   /* i++ rewrites it. */
		}

		cg_expr(cg,tt,s->expr);
		if (ty_is_managed(s->expr->type.kind) && expr_is_owned(s->expr))
		{
			cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
			cg_release_rcx(cg);
		}
		break;
	case ST_RETURN:
		if (s->ret_val && ty_is_managed(s->ret_val->type.kind))
		{
			if (s->ret_val->kind==EX_IDENT)
			{
				/* Transfer the returned local's reference out; release the rest. */
				cg_expr(cg,tt,s->ret_val);
				cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
				cg_release_object_locals(cg, f, s->ret_val->anno_int);
				cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
			}
			else
			{
				cg_expr_owned(cg,tt,s->ret_val);
				cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
				cg_release_object_locals(cg, f, -1);
				cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
			}
		}
		else if (s->ret_val && ty_is_float(f->ret_type.kind))
		{
			/* FP return value lives in xmm0; spill it across local releases
			   (bzy_release may clobber xmm registers). */
			cg_expr(cg,tt,s->ret_val);
			cg_coerce(cg,f->ret_type.kind,s->ret_val->type.kind);
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);
			cg_release_object_locals(cg, f, -1);
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);
		}
		else
		{
			if (s->ret_val)
			{
				cg_expr(cg,tt,s->ret_val);
				if (f->obj_local_count > 0)
				{
					/* Preserve the scalar return value (rax) across the local
					   releases: bzy_release clobbers rax. */
					cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
					cg_release_object_locals(cg, f, -1);
					cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
				}
				else
				{
					cg_release_object_locals(cg, f, -1);
				}
			}
			else
			{
				cg_release_object_locals(cg, f, -1);
			}
		}

		if (in_main)
		{
			cg_emit(cg,"    xor eax, eax");
		}

		int promo_restored[4] = { 0, 0, 0, 0 };   /* Each physical register is restored once. */
		for (int i = 0; i < f->promo_count; i++)   /* Restore the caller's r12..r15. */
		{
			int ri = f->promo_reg[i];
			if (promo_restored[ri])
			{
				continue;   /* Register shared by several locals across disjoint live ranges. */
			}

			promo_restored[ri] = 1;
			cg_emit(cg,"    mov %s, [rbp - %d]", CG_PROMO_REGS[ri], cg->callee_save[ri]);
		}
		cg_emit(cg,"    mov rbx, [rbp - %d]", cg->rbx_save);   /* Restore the caller's rbx. */
		cg_emit(cg,"    mov rsp, rbp");
		cg_emit(cg,"    pop rbp");
		cg_emit(cg,"    ret");
		break;
	case ST_IF:
	{
		if (cg->unrolling)
		{
			/* Conditional writes: a branch may or may not assign, so any local
			   either branch writes is no longer a copy-constant. */
			int w[128];
			int wn = 0;
			cg_hoist_writes_block(s->then_blk, w, &wn);
			cg_hoist_writes_block(s->else_blk, w, &wn);
			for (int i = 0; i < wn; i++)
			{
				cg_unroll_const_kill(cg, w[i]);
			}
		}

		int else_l=cg_label(cg), end_l=cg_label(cg);
		cg_branch_unless(cg,tt,s->cond, s->else_blk?else_l:end_l);
		cg_block(cg,tt,f,s->then_blk,in_main);
		if (s->else_blk)
		{
			cg_emit(cg,"    jmp .L%d",end_l);
			cg_emit(cg,".L%d:",else_l);
			cg_block(cg,tt,f,s->else_blk,in_main);
		}

		cg_emit(cg,".L%d:",end_l);
		break;
	}
	case ST_WHILE:
	{
		/* IR loop region. Fires when no enclosing emitter loop holds register
		   state across this statement: hoisted invariants / strength-reduction
		   bases live in r8..r11 (hoist_n/sr_n), deferred accumulators hold dirty
		   high bits in r12..r15 (defer_n), and an unrolled body's induction
		   variable has a stale slot (unrolling). All idle -> every register the
		   region touches is dead or handed off via home slots. */
		if (cg->hoist_n == 0 && cg->sr_n == 0 && cg->defer_n == 0 && !cg->unrolling)
		{
			int r = cg_region_find(cg, s);
			if (r >= 0)
			{
				cg_emit_region_stmt(cg, f, r);
				break;
			}
		}

		if (s->accum_sb_offset)   /* P5: string self-accumulation -> StringBuilder. */
		{
			cg_accum_loop(cg,tt,f,s,in_main);
			break;
		}

		if (cg->unrolling)
		{
			/* A real loop inside an unrolled copy: anything it writes varies
			   per iteration, so it is no longer a copy-constant. */
			int w[128];
			int wn = 0;
			cg_hoist_writes_block(s->then_blk, w, &wn);
			for (int i = 0; i < wn; i++)
			{
				cg_unroll_const_kill(cg, w[i]);
			}
		}

		int top=cg_label(cg), cont=cg_label(cg), end=cg_label(cg);
		int sb=cg->cur_break_label, sc=cg->cur_continue_label;
		cg->cur_break_label=end;
		cg->cur_continue_label=cont;          /* continue re-tests the condition at the bottom. */
		cg_lpromo_begin(cg, tt, s);           /* Home hot doubles in xmm6.. before the guard (see cg_lpromo_begin). */
		cg_branch_unless(cg,tt,s->cond,end);  /* Entry guard: skip the loop if false up front. */
		cg_loop_hoist_begin(cg, tt, s);       /* Pin loop-invariant locals into r8..r11 if the body allows. */
		cg_emit(cg,".L%d:",top);
		cg_block(cg,tt,f,s->then_blk,in_main);
		cg_emit(cg,".L%d:",cont);
		cg_branch_if(cg,tt,s->cond,top);      /* Bottom test = back-edge; no unconditional jmp. */
		cg_emit(cg,".L%d:",end);
		cg_loop_hoist_end(cg);
		cg->cur_break_label=sb;
		cg->cur_continue_label=sc;
		break;
	}
	case ST_FOREACH:
		cg_foreach(cg,tt,f,s,in_main);
		break;
	case ST_BREAK:
		cg_emit(cg,"    jmp .L%d", cg->cur_break_label);
		break;
	case ST_CONTINUE:
		cg_emit(cg,"    jmp .L%d", cg->cur_continue_label);
		break;
	case ST_FOR:
		/* IR loop region. Fires when no enclosing emitter loop holds register
		   state across this statement: hoisted invariants / strength-reduction
		   bases live in r8..r11 (hoist_n/sr_n), deferred accumulators hold dirty
		   high bits in r12..r15 (defer_n), and an unrolled body's induction
		   variable has a stale slot (unrolling). All idle -> every register the
		   region touches is dead or handed off via home slots. */
		if (cg->hoist_n == 0 && cg->sr_n == 0 && cg->defer_n == 0 && !cg->unrolling)
		{
			int r = cg_region_find(cg, s);
			if (r >= 0)
			{
				cg_emit_region_stmt(cg, f, r);
				break;
			}
		}

		if (s->accum_sb_offset)   /* P5: string self-accumulation -> StringBuilder. */
		{
			cg_accum_loop(cg,tt,f,s,in_main);
		}
		else
		{
			cg_for(cg,tt,f,s,in_main);
		}
		break;
	case ST_SWITCH:
		cg_switch(cg,tt,f,s,in_main);
		break;
	case ST_SELECT:
		cg_select(cg,tt,f,s,in_main);
		break;
	case ST_ASM:
		for (int i=0; i<s->asm_line_count; i++)
		{
			cg_emit(cg,"    %s", s->asm_lines[i]);   /* Verbatim escape hatch. */
		}

		break;
	case ST_CASE:
	case ST_DEFAULT:
		break;   /* Emitted by cg_switch, never reached here. */
	case ST_TRY:
		cg_try(cg,tt,f,s,in_main);
		break;
	case ST_CATCH:
		break;   /* Emitted by cg_try, never reached here. */
	case ST_THROW:
	{
		cg_expr_owned(cg,tt,s->expr);            /* Owned (+1) exception pointer -> rax. */
		cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
		int pc=cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 1), pc);
		cg_emit(cg,".L%d:", pc);                  /* The throw-site PC (within this function). */
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 2));
		cg_aligned_call(cg,"bzy_throw");          /* bzy_throw(exc, pc, rbp) -- never returns. */
		break;
	}
	case ST_SPAWN:
	{
		FuncInfo *fi = types_find_func(tt, s->expr->name);
		if (s->expr->arg_count == 0)
		{
			cg_emit(cg,"    lea %s, [rel %s]", cg_iarg(cg, 0), fi->asm_label);   /* The breeze entry function. */
			cg_aligned_call(cg,"bzy_spawn");
			break;
		}

		/* Arg'd spawn: take the pooled Breeze's inline argbuf (no malloc), fill it with
		   the args (managed args owned by the breeze), then commit. The buffer pointer
		   lives on the native stack so it survives the arg evaluations (which clobber
		   rax and use val_save). */
		int n = s->expr->arg_count;
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    lea %s, [rel __breeze_%s]", cg_iarg(cg, 0), fi->asm_label);
		cg_aligned_call(cg,"bzy_spawn_args_begin");
		cg_emit(cg,"    mov [rbp - %d], rax", b);           /* Save the argbuf pointer. */
		for (int i=0; i<n; i++)
		{
			TypeKind k = s->expr->args[i]->type.kind;
			if (ty_is_managed(k))
			{
				cg_expr_owned(cg,tt,s->expr->args[i]);     /* +1 owned: the breeze owns the arg. */
			}
			else
			{
				cg_expr(cg,tt,s->expr->args[i]);           /* Plain value (no retain). */
			}

			cg_emit(cg,"    mov rdx, [rbp - %d]", b);   /* rdx: block base for the stores below (scratch, no call). */
			if (ty_is_float(k))
			{
				cg_emit(cg, k==TY_FLOAT ? "    movd [rdx + %d], xmm0" : "    movq [rdx + %d], xmm0", i*8);
			}
			else
			{
				cg_emit(cg,"    mov [rdx + %d], rax", i*8);
			}
		}

		/* Each managed arg crosses to the spawned breeze (possibly another core);
		   deep-share it before the breeze can run (bzy_share_crosscore walks the
		   whole reachable graph). The block still holds each arg. */
		for (int i=0; i<n; i++)
		{
			if (ty_is_managed(s->expr->args[i]->type.kind))
			{
				cg_emit(cg,"    mov rdx, [rbp - %d]", b);
				cg_emit(cg,"    mov %s, [rdx + %d]", cg_iarg(cg, 0), i*8);
				cg_aligned_call(cg,"bzy_share_crosscore");
			}
		}

		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);   /* argbuf -> arg0 */
		cg_aligned_call(cg,"bzy_spawn_args_commit");
		cg_scratch_free(cg, 16);
		cg_request_breeze_thunk(cg, fi);
		break;
	}
	}
}

static void cg_block(Codegen *cg, TypeTable *tt, Func *f, Block *b, int in_main)
{
	for (int i=0; i<b->count; i++)
	{
		cg_stmt(cg,tt,f,b->stmts[i],in_main);
	}
}

/* P5: emit the body's recognized `s = s + ...` as appends to the active builder.
   The chain's leftmost leaf is s (the accumulator, already in the builder), so
   only the leaves after it are appended. Each non-s leaf is evaluated to an owned
   (+1) string, appended (bzy_sb_append copies the bytes), then released. The
   recognizer guaranteed the chain fits the flattener (<= CONCAT_MAX leaves). */
static void cg_accum_append(Codegen *cg, TypeTable *tt, Stmt *a)
{
	int off = cg->cur_accum_sb_off;
	Expr *ops[CONCAT_MAX];
	int n = cg_collect_concat(a->value, ops, 0, CONCAT_MAX);
	for (int i=1; i<n; i++)   /* Skip ops[0] = the leftmost leaf s. */
	{
		int b = cg_scratch_alloc(cg, 16);
		cg_concat_operand(cg,tt,ops[i]);                  /* Owned (+1) string in rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), off);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b);
		cg_aligned_call(cg,"bzy_sb_append");
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_release_rcx(cg);                               /* Release the owned leaf. */
		cg_scratch_free(cg, 16);
	}
}

/* P5: lower a recognized string self-accumulation loop to an O(n) StringBuilder.
   Prologue: sb = bzy_sb_new(); seed it with s's pre-loop value. Body: the
   recognized accumulation statement is replaced (via cur_accum_stmt) by appends
   of the non-s leaves. Epilogue: s = bzy_sb_to_string(sb), releasing the old s
   and the builder. The loop skeleton mirrors the normal ST_FOR / ST_WHILE paths
   exactly (same labels and continue target); break is impossible here (the
   recognizer rejects any early exit). */
static void cg_accum_loop(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	int off = s->accum_sb_offset;
	int soff = s->accum_stmt->target->anno_int;

	cg_aligned_call(cg,"bzy_sb_new");                 /* Owned (+1) builder in rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", off);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), off);
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), soff);   /* Seed with the borrowed initial s. */
	cg_aligned_call(cg,"bzy_sb_append");

	int top=cg_label(cg), end=cg_label(cg), cont=cg_label(cg);
	int sbk=cg->cur_break_label, sc=cg->cur_continue_label;
	Stmt *sa=cg->cur_accum_stmt;
	int saoff=cg->cur_accum_sb_off;
	cg->cur_accum_stmt = s->accum_stmt;
	cg->cur_accum_sb_off = off;

	if (s->kind==ST_FOR)
	{
		cg_stmt(cg,tt,f,s->for_init,in_main);
		cg_emit(cg,".L%d:", top);
		cg_expr(cg,tt,s->cond);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d", end);
		cg->cur_break_label=end;
		cg->cur_continue_label=cont;
		cg_block(cg,tt,f,s->then_blk,in_main);
		cg->cur_break_label=sbk;
		cg->cur_continue_label=sc;
		cg_emit(cg,".L%d:", cont);
		cg_stmt(cg,tt,f,s->for_post,in_main);
		cg_emit(cg,"    jmp .L%d", top);
		cg_emit(cg,".L%d:", end);
	}
	else   /* ST_WHILE. */
	{
		cg->cur_break_label=end;
		cg->cur_continue_label=top;
		cg_emit(cg,".L%d:", top);
		cg_expr(cg,tt,s->cond);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d", end);
		cg_block(cg,tt,f,s->then_blk,in_main);
		cg_emit(cg,"    jmp .L%d", top);
		cg_emit(cg,".L%d:", end);
		cg->cur_break_label=sbk;
		cg->cur_continue_label=sc;
	}

	cg->cur_accum_stmt=sa;
	cg->cur_accum_sb_off=saoff;

	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), off);
	cg_aligned_call(cg,"bzy_sb_to_string");           /* Owned (+1) result string in rax. */
	cg_emit(cg,"    mov rbx, [rbp - %d]", soff);       /* Old accumulator. */
	cg_emit(cg,"    mov [rbp - %d], rax", soff);       /* Store the materialized result (transfers +1). */
	cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
	cg_release_rcx(cg);                                /* Release old s. */
	cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), off);
	cg_release_rcx(cg);                                /* Release the builder. */
}

/* Emits one per-function exception record into .data (PC range, frame size, name,
   object-local offsets, and the try-region table). The end label is placed in
   .text just past the function; the record is appended in .data. */
void cg_emit_exception_record(Codegen *cg, const char *label, int frame, Func *f)
{
	int i = cg->exception_fn_count++;
	cg_emit(cg,"__exceptionend%d:", i);                 /* In .text, just past the function. */
	cg_emit(cg,"section .data");
	fprintf(cg->out, "__exceptionname%d: db ", i);
	for (const char *p=f->name; *p; p++)
	{
		fprintf(cg->out, "%d,", (unsigned char)*p);
	}

	fprintf(cg->out, "0\n");
	if (f->obj_local_count > 0)
	{
		fprintf(cg->out, "__exceptionobjs%d: dq ", i);
		for (int j=0; j<f->obj_local_count; j++)
		{
			fprintf(cg->out, "%d%s", f->obj_local_offsets[j], j+1<f->obj_local_count ? "," : "");
		}

		fprintf(cg->out, "\n");
	}

	if (cg->cur_try_count > 0)
	{
		cg_emit(cg,"__exceptiontrytab%d:", i);          /* BzyExceptionTry[]: start, end, catch-vtable, pad. */
		for (int t=0; t<cg->cur_try_count; t++)
		{
			int k = cg->cur_try_k[t];
			int c = cg->cur_try_c[t];
			cg_emit(cg,"    dq ..@exceptiontry%d_s", k);
			cg_emit(cg,"    dq ..@exceptiontry%d_e", k);
			cg_emit(cg,"    dq __vtable_%s", cg->cur_try_vt[t]);
			cg_emit(cg,"    dq ..@exceptiontry%d_p%d", k, c);
		}
	}

	cg_emit(cg,"__exceptionfn%d:", i);
	cg_emit(cg,"    dq %s", label);
	cg_emit(cg,"    dq __exceptionend%d", i);
	cg_emit(cg,"    dq %d", frame);
	cg_emit(cg,"    dq __exceptionname%d", i);
	cg_emit(cg,"    dq %d", f->obj_local_count);
	if (f->obj_local_count > 0)
	{
		cg_emit(cg,"    dq __exceptionobjs%d", i);
	}
	else
	{
		cg_emit(cg,"    dq 0");
	}

	cg_emit(cg,"    dq %d", cg->cur_try_count);   /* Try-region count. */
	if (cg->cur_try_count > 0)
	{
		cg_emit(cg,"    dq __exceptiontrytab%d", i);
	}
	else
	{
		cg_emit(cg,"    dq 0");
	}

	cg_emit(cg,"section .text");
}

/* IR loop regions (Plan 4): any try statement anywhere in the body disables
   regions for the whole function - a catch resuming in this frame would read
   scalar home slots the region holds in registers. */
static int block_has_try(const Block *b);
static int stmt_has_try(const Stmt *s)
{
	if (!s)
	{
		return 0;
	}

	if (s->kind == ST_TRY)
	{
		return 1;
	}

	return stmt_has_try(s->for_init) || stmt_has_try(s->for_post)
		   || block_has_try(s->then_blk) || block_has_try(s->else_blk);
}

static int block_has_try(const Block *b)
{
	if (!b)
	{
		return 0;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (stmt_has_try(b->stmts[i]))
		{
			return 1;
		}
	}

	return 0;
}

/* Diagnostics / prototype overrides for the region pre-scan, read once.
   BZY_IR_DEBUG=1 prints each candidate loop's verdict to stderr;
   BZY_IR_HOTSPILL=1 accepts hot-spill regions anyway (a measurement override
   for gate experiments, not a supported mode). */
static int cg_region_debug(void)
{
	static int cached = -1;
	if (cached < 0)
	{
		const char *v = getenv("BZY_IR_DEBUG");
		cached = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
	}

	return cached;
}

static int cg_region_hotspill_override(void)
{
	static int cached = -1;
	if (cached < 0)
	{
		const char *v = getenv("BZY_IR_HOTSPILL");
		cached = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
	}

	return cached;
}

/* Debug-only A/B filter: BZY_IR_REGION_LINES="58,96" restricts regions to loops
   whose source line is in the comma-separated list (any function). Unset or
   empty = all lines allowed. For per-region benchmark bisection. */
static int cg_region_line_allowed(int line)
{
	static const char *list = NULL;
	static int init = 0;
	if (!init)
	{
		init = 1;
		list = getenv("BZY_IR_REGION_LINES");
		if (list && !list[0])
		{
			list = NULL;
		}
	}

	if (!list)
	{
		return 1;
	}

	const char *p = list;
	while (*p)
	{
		long v = strtol(p, (char **)&p, 10);
		if ((int)v == line)
		{
			return 1;
		}

		while (*p && *p != ',')
		{
			p++;
		}

		if (*p == ',')
		{
			p++;
		}
	}

	return 0;
}

/* Collect eligible loops as regions, lowering and allocating each. An outermost
   eligible loop is taken whole; a loop that does NOT become a region (ineligible
   or hot-spill) has its body scanned so eligible inner loops can fire on their
   own - the dispatch-time guard in cg_stmt re-checks that the enclosing emitter
   loop holds no register state before emitting a nested region, so recording
   here is always safe (a skipped region just falls back to the emitter). Bodies
   of ST_FOREACH (pins the container base in a register across the body, outside
   the hoist accounting) and ST_SWITCH are never descended into. A hot-spill
   allocation (a value in the deepest loop would spill, which the emitter's
   tuned heuristics tend to handle better) keeps the loop on the emitter. */
static void cg_scan_regions(Codegen *cg, Func *f, const Block *b)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		Stmt *s = b->stmts[i];
		if (s->kind == ST_FOR || s->kind == ST_WHILE)
		{
			const char *verdict = "ineligible";
			if (ir_region_eligible(s)
				&& cg_region_line_allowed(s->line))
			{
				IRFunc *irf = ir_lower_region(f, s);
				if (irf)
				{
					IRAlloc *a = ra_run(irf);
					int vec_override = a->hot_spill && ir_region_vectorizable(f, s, a);
					if (!a->hot_spill || cg_region_hotspill_override() || vec_override)
					{
						if (cg->region_count == cg->region_cap)
						{
							cg->region_cap = cg->region_cap ? cg->region_cap * 2 : 8;
							cg->region_stmt  = realloc(cg->region_stmt,  (size_t)cg->region_cap * sizeof(*cg->region_stmt));
							cg->region_irf   = realloc(cg->region_irf,   (size_t)cg->region_cap * sizeof(*cg->region_irf));
							cg->region_alloc = realloc(cg->region_alloc, (size_t)cg->region_cap * sizeof(*cg->region_alloc));
						}

						cg->region_stmt[cg->region_count] = s;
						cg->region_irf[cg->region_count] = irf;
						cg->region_alloc[cg->region_count] = a;
						cg->region_count++;
						if (cg_region_debug())
						{
							fprintf(stderr, "ir-region: %s line %d: REGION (%d locals, %d spill bytes%s)\n",
									f->name, s->line, a->nlocal, a->spill_bytes,
									vec_override ? ", VECTORIZE OVERRIDE"
									: a->hot_spill ? ", HOT-SPILL OVERRIDE" : "");
						}

						continue;
					}

					if (cg_region_debug())
					{
						fprintf(stderr, "ir-region: %s line %d: hot-spill (%d deep temp spills)\n",
								f->name, s->line, a->hot_spill_count);
					}

					verdict = NULL;
					ra_free(a);
					ir_func_free(irf);
				}
			}

			if (cg_region_debug() && verdict)
			{
				fprintf(stderr, "ir-region: %s line %d: %s\n", f->name, s->line, verdict);
			}

			cg_scan_regions(cg, f, s->then_blk);   /* Inner loops may region on their own. */
			continue;
		}

		if (s->kind == ST_IF)
		{
			cg_scan_regions(cg, f, s->then_blk);
			cg_scan_regions(cg, f, s->else_blk);
		}
	}
}

/* The recorded region index for statement s, or -1. */
static int cg_region_find(Codegen *cg, const Stmt *s)
{
	for (int i = 0; i < cg->region_count; i++)
	{
		if (cg->region_stmt[i] == s)
		{
			return i;
		}
	}

	return -1;
}

/* Emit a recorded region, handing promoted locals across the boundary: their
   home slots are stale while promoted, and the region reads/writes locals only
   through home slots, so flush before and reload after. Only promoted locals the
   region references need this; live ranges sharing a physical register are
   disjoint, so at most one of them spans any given region. */
static void cg_emit_region_stmt(Codegen *cg, Func *f, int r)
{
	IRAlloc *a = cg->region_alloc[r];
	for (int i = 0; i < f->promo_count; i++)
	{
		for (int k = 0; k < a->nlocal; k++)
		{
			if (a->local_disp[k] == f->promo_off[i])
			{
				cg_emit(cg, "    mov [rbp - %d], %s", f->promo_off[i], CG_PROMO_REGS[f->promo_reg[i]]);
			}
		}
	}

	/* Float-promoted locals (xmm2-5) need the same treatment: their home slots are
	   stale while promoted, and the region reads/writes locals only through home
	   slots. Without this flush a region reads a stale 0 for a promoted double. */
	for (int i = 0; i < f->fpromo_count; i++)
	{
		for (int k = 0; k < a->nlocal; k++)
		{
			if (a->local_disp[k] == f->fpromo_off[i])
			{
				cg_emit(cg, "    movsd [rbp - %d], %s", f->fpromo_off[i], CG_FPROMO_REGS[f->fpromo_reg[i]]);
			}
		}
	}

	ir_emit_region(cg, cg->region_irf[r], a, cg->region_base, cg->region_stmt[r]);

	for (int i = 0; i < f->promo_count; i++)
	{
		for (int k = 0; k < a->nlocal; k++)
		{
			if (a->local_disp[k] == f->promo_off[i])
			{
				cg_emit(cg, "    mov %s, [rbp - %d]", CG_PROMO_REGS[f->promo_reg[i]], f->promo_off[i]);
			}
		}
	}

	for (int i = 0; i < f->fpromo_count; i++)
	{
		for (int k = 0; k < a->nlocal; k++)
		{
			if (a->local_disp[k] == f->fpromo_off[i])
			{
				cg_emit(cg, "    movsd %s, [rbp - %d]", CG_FPROMO_REGS[f->fpromo_reg[i]], f->fpromo_off[i]);
			}
		}
	}
}

static void cg_emit_func(Codegen *cg, TypeTable *tt, const char *label, Func *f, const char *this_class)
{
	/* Experimental IR backend: a free (non-method) function that lowers cleanly
	   is emitted from IR instead of the syntax-directed path. Gated by BZY_IR and
	   guarded by ir_eligible; a NULL lowering falls through to the emitter. */
	if (bzy_ir_enabled() && this_class == NULL && !f->is_lambda && ir_eligible(f))
	{
		IRFunc *irf = ir_lower_func(f, tt);
		if (irf)
		{
			/* Safety gate: if the allocation would spill a value used in the deepest
			   loop, the emitter's tuned promotion/spill handling tends to do better -
			   keep this function on the emitter. Otherwise emit from the IR. */
			if (!ra_hot_spill(irf))
			{
				ir_emit_func(cg, irf, label);
				ir_func_free(irf);
				return;
			}

			ir_func_free(irf);
		}
	}

	/* IR loop regions (Plan 4): pre-scan the body for outermost loops whose whole
	   subtree lowers; reserve one shared spill/callee-save area sized for the
	   largest region (regions never execute concurrently). cg_stmt emits each
	   recorded loop from its IR instead of the syntax-directed path. */
	cg->region_count = 0;
	cg->region_base = 0;
	int region_area = 0;
	if (bzy_ir_regions_enabled() && this_class == NULL && !block_has_try(f->body))
	{
		cg_scan_regions(cg, f, f->body);
		for (int i = 0; i < cg->region_count; i++)
		{
			IRAlloc *a = cg->region_alloc[i];
			int ncs = 0;
			for (int j = 0; j < RA_NREGS; j++)
			{
				if (a->used_reg[j] && ra_is_callee_saved(j, cg->target == TARGET_LINUX))
				{
					ncs++;
				}
			}

			int need = a->spill_bytes + ncs * 8;
			if (need > region_area)
			{
				region_area = need;
			}
		}

		region_area = (region_area + 15) & ~15;
	}

	int is_main = (this_class==NULL && strcmp(f->name,"main")==0);
	cg->cur_func = f;            /* Promotion helpers consult this; hand-rolled frames clear it. */
	cg->cur_try_count = 0;
	int locals = f->frame_size;
	if (locals < 16)
	{
		locals = 16;
	}

	cg->sp_save     = locals + 8;
	cg->val_save    = locals + 16;
	cg->argtmp_base = locals + 24;
	cg->assign_save = locals + 56;
	cg->fp_save     = locals + 64;
	cg->rbx_save    = locals + 72;
	/* Register promotion reserves four more callee-save slots (r12..r15) just below
	   rbx_save, but only when this function actually promotes locals, so a
	   non-promoting frame is byte-identical to before. */
	int promo_save  = (f->promo_count > 0) ? 32 : 0;
	cg->callee_save[0] = locals + 80;    /* r12 */
	cg->callee_save[1] = locals + 88;    /* r13 */
	cg->callee_save[2] = locals + 96;    /* r14 */
	cg->callee_save[3] = locals + 104;   /* r15 */
	int scratch = 80 + promo_save;   /* sp_save, val_save, four arg temps, assign_save, fp_save, rbx_save, [+r12..r15]. */
	int stack_objs = f->stack_alloc_bytes;
	if (stack_objs % 16 != 0)
	{
		stack_objs = (stack_objs/16 + 1)*16;   /* Keep the frame 16-byte aligned. */
	}

	/* Fixed-rsp frame regions (reserved here, consumed by later tasks). The temp
	   region replaces expression push/pop; the outgoing region (rounded arg slots
	   plus 32 bytes of Win64 shadow) sits at the bottom of the frame, at rsp, so
	   calls need no per-call rsp arithmetic. Existing locals/scratch/stack-object
	   offsets are unchanged - the new regions only extend the frame downward. */
	int temps   = f->max_temp_depth * 8;
	int arena   = (f->max_scratch_bytes + 15) & ~15;   /* 16-aligned scratch byte-stack region. */
	int outargs = ((f->max_outgoing_args*8 + 15)/16)*16 + 32;
	cg->temp_base      = locals + scratch + stack_objs + 8;
	cg->cur_temp_depth = 0;
	cg->cur_temp_cap   = f->max_temp_depth;

	/* Scratch arena: a software byte-stack that replaces the body's sub rsp,N
	   blocks so rsp stays static. It sits between the temp region and the
	   outgoing-arg/shadow region; cg_scratch_alloc grows the cursor downward and
	   returns the rbp offset of the block's byte 0 (its deepest byte). */
	cg->scratch_base    = locals + scratch + stack_objs + temps;
	cg->cur_scratch     = 0;
	cg->cur_scratch_cap = arena;
	cg->lpromo_n        = 0;   /* No loop-scoped XMM homes are armed at function entry. */

	/* Loop-scoped float promotion preserves the caller's xmm6..xmm11 around an
	   armed loop on Win64 (callee-saved there); the save area sits after the
	   region area. SysV leaves those registers volatile and reserves nothing. */
	int lpromo_area = (cg->target == TARGET_WINDOWS) ? LPROMO_NREGS * 16 : 0;
	cg->lpromo_save_base = locals + scratch + stack_objs + temps + arena + region_area + 16;

	/* The region spill/callee-save area sits between the scratch arena and the
	   outgoing-arg region; region spill slot 0 is at [rbp - region_base]. */
	int frame = locals + scratch + stack_objs + temps + arena + region_area + lpromo_area + outargs;
	if (frame % 16 != 0)
	{
		frame = (frame/16 + 1)*16;   /* Keep rsp 16-aligned after the prologue so calls are aligned. */
	}

	if (cg->region_count > 0)
	{
		cg->region_base = locals + scratch + stack_objs + temps + arena + 8;
	}

	cg->outarg_base = frame;   /* rsp = rbp - frame; shadow [rsp,rsp+32), outgoing args at [rsp+32+i*8]. */

	cg_emit(cg,"global %s", label);
	cg_emit(cg,"%s:", label);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, %d", frame);
	cg_emit(cg,"    mov [rbp - %d], rbx", cg->rbx_save);   /* Preserve the caller's callee-saved rbx. */
	int promo_saved[4] = { 0, 0, 0, 0 };                   /* Each physical register is preserved once. */
	for (int i = 0; i < f->promo_count; i++)               /* Preserve the caller's r12..r15 we will use. */
	{
		int ri = f->promo_reg[i];
		if (promo_saved[ri])
		{
			continue;   /* Register shared by several locals across disjoint live ranges. */
		}

		promo_saved[ri] = 1;
		cg_emit(cg,"    mov [rbp - %d], %s", cg->callee_save[ri], CG_PROMO_REGS[ri]);
	}

	/* Spill incoming args: this at [rbp-8], then params at [rbp-16], [rbp-24], and so on.
	   Win64 numbers both register classes by argument position; System V numbers
	   integer and FP registers independently. Arguments beyond the ABI's register
	   count arrive on the caller's stack and are read back from [rbp+off]: Win64 at
	   48 + k*8 (8 ret + 8 saved rbp + 32 shadow), System V at 16 + k*8. */
	int win = (cg->target != TARGET_LINUX);
	int int_idx = 0, fp_idx = 0, stk = 0;
	if (this_class)
	{
		cg_emit(cg,"    mov [rbp - 8], %s", cg_iarg(cg, 0));
		int_idx++;
	}

	for (int i=0; i<f->param_count; i++)
	{
		int slot = this_class ? (16 + i*8) : (8 + i*8);
		int pos = (this_class ? 1 : 0) + i;   /* Win64 by-position index. */
		TypeKind pk = f->params[i].type.kind;
		int is_float=(pk==TY_FLOAT), is_double=(pk==TY_DOUBLE);

		int on_stack;
		if (win) { on_stack = (pos >= 4); }
		else if (is_float||is_double) { on_stack = (fp_idx >= 8); }
		else { on_stack = (int_idx >= 6); }

		if (on_stack)
		{
			int off = win ? (48 + stk*8) : (16 + stk*8);
			stk++;
			const char *pr = cg_local_reg(cg, slot);
			if (is_float)
			{
				cg_emit(cg,"    movss xmm0, dword [rbp + %d]", off);
				cg_emit(cg,"    movss dword [rbp - %d], xmm0", slot);
			}
			else if (is_double)
			{
				cg_emit(cg,"    movsd xmm0, qword [rbp + %d]", off);
				cg_emit(cg,"    movsd qword [rbp - %d], xmm0", slot);
			}
			else if (pr)
			{
				cg_emit(cg,"    mov %s, [rbp + %d]", pr, off);
				if (pk == TY_INT) { cg_emit(cg,"    movsxd %s, %sd", pr, pr); }
			}
			else
			{
				cg_emit(cg,"    mov rax, [rbp + %d]", off);
				cg_emit(cg,"    mov [rbp - %d], rax", slot);
			}
			continue;
		}

		if (is_float)
		{
			int xi = win ? pos : fp_idx++;
			cg_emit(cg,"    movss dword [rbp - %d], xmm%d", slot, xi);
		}
		else if (is_double)
		{
			int xi = win ? pos : fp_idx++;
			cg_emit(cg,"    movsd qword [rbp - %d], xmm%d", slot, xi);
		}
		else
		{
			int ii = win ? pos : int_idx++;
			const char *pr = cg_local_reg(cg, slot);
			if (pr)
			{
				cg_emit(cg,"    mov %s, %s", pr, cg_iarg(cg, ii));   /* Seed a promoted param straight into its register. */
				if (pk == TY_INT)
				{
					cg_emit(cg,"    movsxd %s, %sd", pr, pr);   /* Sign-extend a 32-bit int param (r12 -> r12d). */
				}
			}
			else
			{
				cg_emit(cg,"    mov [rbp - %d], %s", slot, cg_iarg(cg, ii));
			}
		}
	}

	/* Object locals must be NULL before any release; bzy_alloc only zeroes heap
	   objects, not stack slots. */
	for (int i=0; i<f->obj_local_count; i++)
	{
		cg_emit(cg,"    mov qword [rbp - %d], 0", f->obj_local_offsets[i]);
	}

	/* Lambda body: seed each captured local from the closure environment (arg0,
	   spilled to [rbp-8]) before any user statement reads it. */
	if (f->is_lambda)
	{
		for (int i=0; i<f->cap_count; i++)
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov rax, [rax + %d]", f->cap_env_off[i]);
			cg_emit(cg,"    mov [rbp - %d], rax", f->cap_local_off[i]);
		}
	}

	/* Class-load: construct the enum singletons once, before any user statement. */
	if (is_main && enum_total()>0)
	{
		cg_emit(cg,"    call __enum_init");
	}

	if (is_main && cg_tt_has_statics(tt))
	{
		cg_emit(cg,"    call __static_init");
	}

	cg_block(cg, tt, f, f->body, is_main);

	cg_release_object_locals(cg, f, -1);
	if (is_main)
	{
		cg_emit(cg,"    xor eax, eax");
	}

	int promo_restored[4] = { 0, 0, 0, 0 };   /* Each physical register is restored once. */
	for (int i = 0; i < f->promo_count; i++)   /* Restore the caller's r12..r15. */
	{
		int ri = f->promo_reg[i];
		if (promo_restored[ri])
		{
			continue;   /* Register shared by several locals across disjoint live ranges. */
		}

		promo_restored[ri] = 1;
		cg_emit(cg,"    mov %s, [rbp - %d]", CG_PROMO_REGS[ri], cg->callee_save[ri]);
	}
	cg_emit(cg,"    mov rbx, [rbp - %d]", cg->rbx_save);   /* Restore the caller's rbx. */
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
	cg_emit_exception_record(cg, label, frame, f);

	for (int i = 0; i < cg->region_count; i++)
	{
		ra_free(cg->region_alloc[i]);
		ir_func_free(cg->region_irf[i]);
	}

	cg->region_count = 0;
	cg->region_base = 0;
}

static void cg_emit_vtable(Codegen *cg, ClassInfo *c)
{
	cg_emit(cg,"__typeinfo_%s:", c->name);
	cg_emit(cg,"    dq 0");   /* Finalizer slot (vtable-16): none for user classes. */
	int nobj=0;
	for (int i=0; i<c->field_count; i++)
	{
		if (ty_is_managed(c->fields[i].type.kind) && !c->fields[i].is_static)
		{
			nobj++;
		}
	}

	cg_emit(cg,"    dq %d", nobj);
	for (int i=0; i<c->field_count; i++)
	{
		if (ty_is_managed(c->fields[i].type.kind) && !c->fields[i].is_static)
		{
			cg_emit(cg,"    dq %d", c->fields[i].offset);   /* Instance managed fields only; statics are global. */
		}
	}

	cg_emit(cg,"    dq __classname_%s", c->name);  /* Class name pointer at descriptor[2+nobj]. */
	cg_emit(cg,"    dq __typeinfo_%s", c->name);   /* This word lands at the vtable label minus eight. */
	cg_emit(cg,"__vtable_%s:", c->name);
	for (int slot=0; slot<c->vtable_size; slot++)
	{
		const char *label=NULL;
		for (int i=0; i<c->method_count; i++)
		{
			if (c->methods[i].vtable_slot==slot)
			{
				label=c->methods[i].asm_label;
				break;
			}
		}

		/* Fill gaps (reserved interface slots this class does not implement) with 0
		   so every slot keeps its fixed index; the type checker ensures a gap is
		   never actually dispatched. */
		cg_emit(cg,"    dq %s", label ? label : "0");
	}

	/* Reflection name: a per-constant enum subclass (Enum$CONST) reports the base
	   enum, so getClassName() on Color.RED returns "Color", not "Color$RED". */
	char refl[64];
	strncpy(refl,c->name,sizeof(refl)-1);
	refl[sizeof(refl)-1]='\0';
	const char *dollar=strchr(c->name,'$');
	if (dollar)
	{
		char base[64];
		size_t bl=(size_t)(dollar-c->name);
		if (bl<sizeof(base))
		{
			memcpy(base,c->name,bl);
			base[bl]='\0';
			if (enum_is(base) && enum_ordinal(base,dollar+1)>=0)
			{
				strcpy(refl,base);
			}
		}
	}

	cg_emit(cg,"__classname_%s: db \"%s\", 0", c->name, refl);   /* NUL-terminated dynamic class name. */
}

/* True if field type 'f' is a record (value-equality) object type. */
static int cg_field_is_record(Codegen *cg, TypeTable *tt, FieldInfo *f)
{
	(void)cg;
	if (f->type.kind != TY_OBJECT)
	{
		return 0;
	}

	ClassInfo *ci = types_find_class(tt, f->type.class_name);
	return ci && ci->is_record;
}

/* Indirect call to the function pointer in r11 (used for a record field's recursive
   hashCode/equals via the callee's vtable). The record-method frames keep rsp static
   and 16-aligned with a reserved bottom shadow region, so this is a bare call. */
static void cg_aligned_call_r11(Codegen *cg)
{
	cg_emit(cg,"    call r11");
}

/* Synthesize the bodies of a record's hashCode (vtable slot 0) and equals (slot 1).
   Frame: [rbp-8]=this, [rbp-16]=other, [rbp-24]=aligned-call sp save (cg->sp_save),
   [rbp-32]=h accumulator, [rbp-40]=field contribution, [rbp-48]=record-field temp.
   Field rules mirror equals<->hashCode so equal records always hash equal:
   int/bool width-canonical; float/double by bits; string by content; record by
   recursion; any other object/collection by identity. */
static void cg_emit_record_methods(Codegen *cg, TypeTable *tt, ClassInfo *c)
{
	cg->cur_func = NULL;   /* Synthesized frames: promotion helpers must fall back to slots. */
	int saved_sp = cg->sp_save;
	cg->sp_save = 24;
	InterfaceInfo *h = types_find_interface(tt,"__Hashable");
	int hc_slot = h->vslot[0], eq_slot = h->vslot[1];
	char mem[64];

	/* ---------- equals(this, other) -> rax in {0,1} ---------- */
	cg_emit(cg,"global __rec_equals_%s", c->name);
	cg_emit(cg,"__rec_equals_%s:", c->name);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, 96");   /* 64 of frame slots + 32 of bottom shadow for the now-bare calls. */
	cg_emit(cg,"    mov [rbp - 8], %s", cg_iarg(cg,0));
	cg_emit(cg,"    mov [rbp - 16], %s", cg_iarg(cg,1));
	int eq_ret0 = cg_label(cg), eq_done = cg_label(cg);
	int lne = cg_label(cg);
	cg_emit(cg,"    mov rax, [rbp - 8]");
	cg_emit(cg,"    cmp rax, [rbp - 16]");
	cg_emit(cg,"    jne .L%d", lne);            /* this == other -> equal. */
	cg_emit(cg,"    mov eax, 1");
	cg_emit(cg,"    jmp .L%d", eq_done);
	cg_emit(cg,".L%d:", lne);
	cg_emit(cg,"    cmp qword [rbp - 16], 0");  /* other == null -> not equal. */
	cg_emit(cg,"    je .L%d", eq_ret0);
	for (int i=0; i<c->field_count; i++)
	{
		FieldInfo *f=&c->fields[i];
		if (f->is_static)
		{
			continue;
		}

		int off=f->offset;
		TypeKind tk=f->type.kind;
		if (tk==TY_STRING)
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov %s, [rax + %d]", cg_iarg(cg,0), off);
			cg_emit(cg,"    mov rax, [rbp - 16]");
			cg_emit(cg,"    mov %s, [rax + %d]", cg_iarg(cg,1), off);
			cg_aligned_call(cg,"bzy_str_eq");
			cg_emit(cg,"    test rax, rax");
			cg_emit(cg,"    jz .L%d", eq_ret0);
		}
		else if (cg_field_is_record(cg,tt,f))
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov rax, [rax + %d]", off);
			cg_emit(cg,"    mov [rbp - 32], rax");          /* thisF */
			cg_emit(cg,"    mov rax, [rbp - 16]");
			cg_emit(cg,"    mov rax, [rax + %d]", off);
			cg_emit(cg,"    mov [rbp - 40], rax");          /* otherF */
			int fok=cg_label(cg);
			cg_emit(cg,"    mov rax, [rbp - 32]");
			cg_emit(cg,"    cmp rax, [rbp - 40]");
			cg_emit(cg,"    je .L%d", fok);                 /* same ref / both null. */
			cg_emit(cg,"    cmp qword [rbp - 32], 0");
			cg_emit(cg,"    je .L%d", eq_ret0);             /* thisF null, otherF not. */
			cg_emit(cg,"    mov %s, [rbp - 32]", cg_iarg(cg,0));
			cg_emit(cg,"    mov %s, [rbp - 40]", cg_iarg(cg,1));
			cg_emit(cg,"    mov rax, [rbp - 32]");
			cg_emit(cg,"    mov rax, [rax]");               /* thisF vtable. */
			cg_emit(cg,"    mov r11, [rax + %d]", eq_slot*8);
			cg_aligned_call_r11(cg);
			cg_emit(cg,"    test rax, rax");
			cg_emit(cg,"    jz .L%d", eq_ret0);
			cg_emit(cg,".L%d:", fok);
		}
		else if (ty_is_int(tk) || tk==TY_BOOL)
		{
			snprintf(mem,sizeof(mem),"[rax + %d]",off);
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_load_scalar(cg,tk,mem);                      /* width-canonical. */
			cg_emit(cg,"    mov [rbp - 32], rax");
			cg_emit(cg,"    mov rax, [rbp - 16]");
			cg_load_scalar(cg,tk,mem);
			cg_emit(cg,"    cmp rax, [rbp - 32]");
			cg_emit(cg,"    jne .L%d", eq_ret0);
		}
		else   /* float/double by bits; any other object/collection by identity. */
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov rax, [rax + %d]", off);
			cg_emit(cg,"    mov [rbp - 32], rax");
			cg_emit(cg,"    mov rax, [rbp - 16]");
			cg_emit(cg,"    mov rax, [rax + %d]", off);
			cg_emit(cg,"    cmp rax, [rbp - 32]");
			cg_emit(cg,"    jne .L%d", eq_ret0);
		}
	}

	cg_emit(cg,"    mov eax, 1");
	cg_emit(cg,"    jmp .L%d", eq_done);
	cg_emit(cg,".L%d:", eq_ret0);
	cg_emit(cg,"    xor eax, eax");
	cg_emit(cg,".L%d:", eq_done);
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");

	/* ---------- hashCode(this) -> rax (int) ---------- */
	cg_emit(cg,"global __rec_hashCode_%s", c->name);
	cg_emit(cg,"__rec_hashCode_%s:", c->name);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, 96");   /* 64 of frame slots + 32 of bottom shadow for the now-bare calls. */
	cg_emit(cg,"    mov [rbp - 8], %s", cg_iarg(cg,0));
	cg_emit(cg,"    mov qword [rbp - 32], 17");             /* h = 17 */
	for (int i=0; i<c->field_count; i++)
	{
		FieldInfo *f=&c->fields[i];
		if (f->is_static)
		{
			continue;
		}

		int off=f->offset;
		TypeKind tk=f->type.kind;
		if (tk==TY_STRING)
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov %s, [rax + %d]", cg_iarg(cg,0), off);
			cg_aligned_call(cg,"bzy_str_hashcode");
			cg_emit(cg,"    mov [rbp - 40], rax");
		}
		else if (cg_field_is_record(cg,tt,f))
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov rax, [rax + %d]", off);
			cg_emit(cg,"    mov [rbp - 48], rax");          /* thisF */
			cg_emit(cg,"    mov qword [rbp - 40], 0");
			int hskip=cg_label(cg);
			cg_emit(cg,"    cmp qword [rbp - 48], 0");
			cg_emit(cg,"    je .L%d", hskip);
			cg_emit(cg,"    mov %s, [rbp - 48]", cg_iarg(cg,0));
			cg_emit(cg,"    mov rax, [rbp - 48]");
			cg_emit(cg,"    mov rax, [rax]");
			cg_emit(cg,"    mov r11, [rax + %d]", hc_slot*8);
			cg_aligned_call_r11(cg);
			cg_emit(cg,"    mov [rbp - 40], rax");
			cg_emit(cg,".L%d:", hskip);
		}
		else if (ty_is_int(tk) || tk==TY_BOOL)
		{
			snprintf(mem,sizeof(mem),"[rax + %d]",off);
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_load_scalar(cg,tk,mem);                      /* width-canonical, matches equals. */
			cg_emit(cg,"    mov [rbp - 40], rax");
		}
		else if (ty_is_float(tk))
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov rax, [rax + %d]", off);      /* raw bits. */
			cg_emit(cg,"    mov [rbp - 40], rax");
		}
		else   /* any other object/collection: identity hash of the pointer. */
		{
			cg_emit(cg,"    mov rax, [rbp - 8]");
			cg_emit(cg,"    mov %s, [rax + %d]", cg_iarg(cg,0), off);
			cg_aligned_call(cg,"bzy_ptr_hash");
			cg_emit(cg,"    mov [rbp - 40], rax");
		}

		cg_emit(cg,"    mov rax, [rbp - 32]");              /* h = h*31 + contribution. */
		cg_emit(cg,"    imul rax, rax, 31");
		cg_emit(cg,"    add rax, [rbp - 40]");
		cg_emit(cg,"    mov [rbp - 32], rax");
	}

	cg_emit(cg,"    mov rax, [rbp - 32]");
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
	cg->sp_save = saved_sp;
}

/* Class-load init: construct each enum constant's singleton via the ordinary
   new + ctor path, stamp its hidden __ordinal/__name, and store it in its data
   slot (which holds the one permanent reference). Called at main's entry. */
static void cg_emit_enum_init(Codegen *cg, TypeTable *tt)
{
	cg->cur_func = NULL;   /* Synthesized frame: promotion helpers must fall back to slots. */
	if (enum_total()==0)
	{
		return;
	}

	int locals=16;
	cg->sp_save=locals+8;
	cg->val_save=locals+16;
	cg->argtmp_base=locals+24;
	cg->assign_save=locals+56;
	cg->fp_save=locals+64;
	cg->rbx_save=locals+72;
	int scratch=80;
	/* This hand-rolled frame runs each enum constant's constructor via cg_new ->
	   cg_ctor_call, which now preserves temporaries in frame temp slots and marshals
	   args through the scratch arena. cg_emit_func sets those regions up per function;
	   this synthetic frame must do the same or the arena addresses land outside it
	   (the cause of a hard crash). Size both regions generously - __enum_init runs
	   once at startup - and arm the overflow traps as the backstop for an unusually
	   complex constant argument. No fixed outgoing-arg region is needed: the inner
	   calls still reserve their shadow space dynamically. */
	int temps=32*8;
	int arena=512;
	cg->temp_base=locals+scratch+8;
	cg->cur_temp_depth=0;
	cg->cur_temp_cap=32;
	cg->scratch_base=locals+scratch+temps;
	cg->cur_scratch=0;
	cg->cur_scratch_cap=arena;
	int frame=locals+scratch+temps+arena+32;   /* +32: the now-bare calls use a reserved bottom shadow region. */
	if (frame%16!=0)
	{
		frame=(frame/16+1)*16;
	}

	cg_emit(cg,"global __enum_init");
	cg_emit(cg,"__enum_init:");
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, %d", frame);
	cg_emit(cg,"    mov [rbp - %d], rbx", cg->rbx_save);   /* Preserve the caller's callee-saved rbx. */
	for (int i=0; i<enum_total(); i++)
	{
		const EnumInfo *e=enum_at(i);
		for (int k=0; k<e->constant_count; k++)
		{
			if (e->const_payload && e->const_payload[k])
			{
				continue;   /* Payload variant: constructed per call, not a startup singleton. */
			}

			Expr tmp;
			memset(&tmp,0,sizeof(tmp));
			tmp.kind=EX_NEW;
			strcpy(tmp.name,e->const_class[k]);
			tmp.type.kind=TY_OBJECT;
			strcpy(tmp.type.class_name,e->const_class[k]);
			for (int a=0; a<e->const_argc[k]; a++)
			{
				expr_add_arg(&tmp, e->const_args[k][a]);
			}

			cg_new(cg,tt,&tmp);                                /* Owned object -> rax. */
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
			cg_emit(cg,"    mov qword [rax + 24], %d", k);      /* __ordinal. */
			cg_emit(cg,"    lea %s, [rel __enumname_%s_%s]", cg_iarg(cg, 0), e->name, e->const_name[k]);
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), (int)strlen(e->const_name[k]));
			cg_aligned_call(cg,"bzy_str_new");                 /* Owned string -> rax. */
			cg_emit(cg,"    mov rbx, [rbp - %d]", cg->val_save);
			cg_emit(cg,"    mov [rbx + 32], rax");              /* __name. */
			cg_emit(cg,"    mov [rel __enum_%s_%s], rbx", e->name, e->const_name[k]);
		}
	}

	cg_emit(cg,"    mov rbx, [rbp - %d]", cg->rbx_save);   /* Restore the caller's rbx. */
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
}

/* Enum singleton storage slots (one permanent reference each) + the constant
   name strings used to stamp __name and to drive valueOf(). */
static void cg_emit_enum_data(Codegen *cg)
{
	for (int i=0; i<enum_total(); i++)
	{
		const EnumInfo *e=enum_at(i);
		for (int k=0; k<e->constant_count; k++)
		{
			if (e->const_payload && e->const_payload[k])
			{
				continue;   /* No singleton slot/name for a payload variant. */
			}

			cg_emit(cg,"__enum_%s_%s: dq 0", e->name, e->const_name[k]);
			fprintf(cg->out,"__enumname_%s_%s: db ", e->name, e->const_name[k]);
			for (const char *p=e->const_name[k]; *p; p++)
			{
				fprintf(cg->out,"%d,", (unsigned char)*p);
			}

			fprintf(cg->out,"0\n");
		}
	}
}

/* True if any class declares a static field (drives whether __static_init and
   its main-entry call exist). Inherited duplicate copies don't affect the y/n. */
static int cg_tt_has_statics(TypeTable *tt)
{
	for (int i=0; i<tt->class_count; i++)
	{
		for (int k=0; k<tt->classes[i]->field_count; k++)
		{
			if (tt->classes[i]->fields[k].is_static)
			{
				return 1;
			}
		}
	}

	return 0;
}

/* One global slot per declared static field (iterate the declaring ClassDecls so
   inherited copies don't emit duplicate slots). */
static void cg_emit_static_data(Codegen *cg, Unit **units, int n)
{
	for (int i=0; i<n; i++)
	{
		for (int ci=0; ci<units[i]->class_count; ci++)
		{
			ClassDecl *d=units[i]->klasses[ci];

			for (int k=0; k<d->field_count; k++)
			{
				if (d->fields[k].is_static || d->is_static)
				{
					cg_emit(cg,"__static_%s_%s: dq 0", d->name, d->fields[k].name);
				}
			}
		}
	}
}

/* Run static-field declaration initializers once, before any user statement. */
static void cg_emit_static_init(Codegen *cg, TypeTable *tt, Unit **units, int n)
{
	if (!cg_tt_has_statics(tt))
	{
		return;
	}

	int locals=16;
	cg->sp_save=locals+8;
	cg->val_save=locals+16;
	cg->argtmp_base=locals+24;
	cg->assign_save=locals+56;
	cg->fp_save=locals+64;
	cg->rbx_save=locals+72;
	int scratch=80;
	/* This hand-rolled frame runs each static field's initializer via cg_expr, which
	   uses frame temp slots and the scratch arena and emits now-bare runtime calls.
	   Like __enum_init, it must set up & arm those regions (cg_emit_func does this per
	   function) plus a bottom shadow region, or the arena/temp addresses land outside
	   the frame and the bare calls have no shadow space. Sized generously - this runs
	   once at startup; the overflow traps backstop an unusually complex initializer. */
	int temps=32*8;
	int arena=512;
	cg->temp_base=locals+scratch+8;
	cg->cur_temp_depth=0;
	cg->cur_temp_cap=32;
	cg->scratch_base=locals+scratch+temps;
	cg->cur_scratch=0;
	cg->cur_scratch_cap=arena;
	int frame=locals+scratch+temps+arena+32;   /* +32: bottom shadow for the bare calls. */
	if (frame%16!=0)
	{
		frame=(frame/16+1)*16;
	}

	cg_emit(cg,"global __static_init");
	cg_emit(cg,"__static_init:");
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, %d", frame);
	cg_emit(cg,"    mov [rbp - %d], rbx", cg->rbx_save);   /* Preserve the caller's callee-saved rbx. */
	for (int i=0; i<n; i++)
	{
		for (int ci=0; ci<units[i]->class_count; ci++)
		{
			ClassDecl *d=units[i]->klasses[ci];

			for (int k=0; k<d->field_count; k++)
			{
				if (!(d->fields[k].is_static || d->is_static) || !d->fields[k].init)
				{
					continue;
				}

				TypeKind tk=d->fields[k].type.kind;
				char mem[192];
				snprintf(mem,sizeof(mem),"[rel __static_%s_%s]", d->name, d->fields[k].name);
				if (ty_is_float(tk))
				{
					cg_expr(cg,tt,d->fields[k].init);         /* Value -> xmm0. */
					cg_store_fp(cg,tk,mem);
				}
				else if (ty_is_managed(tk))
				{
					cg_expr_owned(cg,tt,d->fields[k].init);   /* Owned (+1); slot starts 0, no release. */
					/* A static slot is reachable from every breeze: deep-share the
					   initial value before publishing (hand-rolled frame: park the
					   value in rbx, which this frame preserves, across the call). */
					cg_emit(cg,"    mov rbx, rax");
					cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
					cg_aligned_call(cg,"bzy_share_crosscore");
					cg_emit(cg,"    mov rax, rbx");
					cg_emit(cg,"    mov %s, rax", mem);
				}
				else
				{
					cg_expr(cg,tt,d->fields[k].init);         /* Scalar -> rax. */
					cg_emit(cg,"    mov %s, rax", mem);
				}
			}
		}
	}

	cg_emit(cg,"    mov rbx, [rbp - %d]", cg->rbx_save);   /* Restore the caller's rbx. */
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
}

void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count)
{
	cg_emit(cg,"bits 64");
	cg_emit(cg,"default rel");
	/* Multi-byte nop padding for align directives: loop headers fall through
	   their padding once per entry, and long nops retire far fewer uops than
	   the default single-byte fill. */
	cg_emit(cg,"%%use smartalign");
	cg_emit(cg,"alignmode p6");
	cg_emit(cg,"extern malloc");
	cg_emit(cg,"extern free");
	cg_emit(cg,"extern bzy_alloc");
	if (cg->target == TARGET_WINDOWS)
	{
		cg_emit(cg,"extern bzy_pool_slot");          /* Inline allocator: TEB TLS slot index. */
		cg_emit(cg,"extern bzy_pool_slot_ready");    /* Inline allocator: slot-initialized flag. */
	}
	else
	{
		cg_emit(cg,"extern bzy_tpool_off");          /* Inline allocator: thread-pointer -> t_pool offset. */
		cg_emit(cg,"extern bzy_tpool_off_ready");    /* Inline allocator: offset-computed flag. */
	}
	cg_emit(cg,"extern bzy_retain");
	cg_emit(cg,"extern bzy_share_crosscore");
	cg_emit(cg,"extern bzy_array_get_shared");
	cg_emit(cg,"extern bzy_array_set_shared");
	cg_emit(cg,"extern bzy_release");
	cg_emit(cg,"extern bzy_live_count");
	cg_emit(cg,"extern bzy_collect_cycles");
	cg_emit(cg,"extern bzy_print_i64");
	cg_emit(cg,"extern bzy_print_u64");
	cg_emit(cg,"extern bzy_print_bool");
	cg_emit(cg,"extern bzy_print_f64");
	cg_emit(cg,"extern bzy_str_new");
	cg_emit(cg,"extern bzy_str_concat");
	cg_emit(cg,"extern bzy_str_concat_n");
	cg_emit(cg,"extern bzy_str_from_i64");
	cg_emit(cg,"extern bzy_str_from_u64");
	cg_emit(cg,"extern bzy_str_from_bool");
	cg_emit(cg,"extern bzy_str_from_f64");
	cg_emit(cg,"extern bzy_str_len");
	cg_emit(cg,"extern bzy_str_from_cstring");
	cg_emit(cg,"extern bzy_str_from_cbytes");
	cg_emit(cg,"extern bzy_str_to_bytes");
	cg_emit(cg,"extern bzy_str_from_bytes");
	cg_emit(cg,"extern bzy_print_str");
	cg_emit(cg,"extern bzy_input_line");
	cg_emit(cg,"extern bzy_sb_new");
	cg_emit(cg,"extern bzy_sb_append");
	cg_emit(cg,"extern bzy_sb_to_string");
	cg_emit(cg,"extern bzy_array_new");
	cg_emit(cg,"extern bzy_array_new_sized");
	cg_emit(cg,"extern bzy_array_len");
	cg_emit(cg,"extern bzy_oob");
	cg_emit(cg,"extern bzy_oob_abort");
	cg_emit(cg,"extern bzy_map_new");
	cg_emit(cg,"extern bzy_map_put");
	cg_emit(cg,"extern bzy_map_get");
	cg_emit(cg,"extern bzy_map_put_if_absent");
	cg_emit(cg,"extern bzy_map_get_or_default");
	cg_emit(cg,"extern bzy_map_has");
	cg_emit(cg,"extern bzy_map_remove");
	cg_emit(cg,"extern bzy_map_contains_value");
	cg_emit(cg,"extern bzy_map_keys");
	cg_emit(cg,"extern bzy_map_values");
	cg_emit(cg,"extern bzy_map_entries");
	cg_emit(cg,"extern bzy_entry_key");
	cg_emit(cg,"extern bzy_entry_val");
	cg_emit(cg,"extern bzy_spawn");
	cg_emit(cg,"extern bzy_spawn_args_begin");
	cg_emit(cg,"extern bzy_spawn_args_commit");
	cg_emit(cg,"extern bzy_offload_run");   /* FFI: `extern blocking` dispatch. */
	cg_emit(cg,"extern bzy_yield");
	cg_emit(cg,"extern bzy_callback_enter");
	cg_emit(cg,"extern bzy_callback_leave");
	cg_emit(cg,"extern bzy_channel_new");
	cg_emit(cg,"extern bzy_channel_send");
	cg_emit(cg,"extern bzy_channel_recv");
	cg_emit(cg,"extern bzy_channel_try_send");
	cg_emit(cg,"extern bzy_channel_try_recv");
	cg_emit(cg,"extern bzy_timer_after");
	cg_emit(cg,"extern bzy_timer_every");
	cg_emit(cg,"extern bzy_timer_cancel");
	cg_emit(cg,"extern bzy_listener_new");
	cg_emit(cg,"extern bzy_listener_accept");
	cg_emit(cg,"extern bzy_listener_accept_timeout");
	cg_emit(cg,"extern bzy_listener_try_accept");
	cg_emit(cg,"extern bzy_listener_port");
	cg_emit(cg,"extern bzy_listener_close");
	cg_emit(cg,"extern bzy_socket_connect");
	cg_emit(cg,"extern bzy_raw_socket");
	cg_emit(cg,"extern bzy_tls_connect");
	cg_emit(cg,"extern bzy_tls_connect_ca");
	cg_emit(cg,"extern bzy_tls_listen");
	cg_emit(cg,"extern bzy_tls_accept");
	cg_emit(cg,"extern bzy_tls_listener_port");
	cg_emit(cg,"extern bzy_tls_read");
	cg_emit(cg,"extern bzy_tls_write");
	cg_emit(cg,"extern bzy_tls_close");
	cg_emit(cg,"extern bzy_tls_close_listener");
	cg_emit(cg,"extern bzy_surface_open");
	cg_emit(cg,"extern bzy_surface_present");
	cg_emit(cg,"extern bzy_surface_poll_event");
	cg_emit(cg,"extern bzy_surface_is_open");
	cg_emit(cg,"extern bzy_surface_close");
	cg_emit(cg,"extern bzy_glsurface_open");
	cg_emit(cg,"extern bzy_glsurface_poll");
	cg_emit(cg,"extern bzy_glsurface_swap");
	cg_emit(cg,"extern bzy_glsurface_isopen");
	cg_emit(cg,"extern bzy_glsurface_close");
	cg_emit(cg,"extern bzy_dynsym");
	cg_emit(cg,"extern bzy_ffi_bind");
	cg_emit(cg,"extern bzy_net_read_url");
	cg_emit(cg,"extern bzy_socket_read");
	cg_emit(cg,"extern bzy_socket_read_timeout");
	cg_emit(cg,"extern bzy_socket_try_read");
	cg_emit(cg,"extern bzy_socket_read_text");
	cg_emit(cg,"extern bzy_socket_read_text_timeout");
	cg_emit(cg,"extern bzy_socket_try_read_text");
	cg_emit(cg,"extern bzy_socket_write");
	cg_emit(cg,"extern bzy_socket_write_text");
	cg_emit(cg,"extern bzy_socket_close");
	cg_emit(cg,"extern bzy_udp_new");
	cg_emit(cg,"extern bzy_udp_port");
	cg_emit(cg,"extern bzy_udp_send_to");
	cg_emit(cg,"extern bzy_udp_send_text_to");
	cg_emit(cg,"extern bzy_udp_receive");
	cg_emit(cg,"extern bzy_udp_receive_timeout");
	cg_emit(cg,"extern bzy_udp_try_receive");
	cg_emit(cg,"extern bzy_udp_close");
	cg_emit(cg,"extern bzy_dgram_data");
	cg_emit(cg,"extern bzy_dgram_text");
	cg_emit(cg,"extern bzy_dgram_host");
	cg_emit(cg,"extern bzy_dgram_port");
	cg_emit(cg,"extern bzy_filechannel_open");
	cg_emit(cg,"extern bzy_filechannel_read_at");
	cg_emit(cg,"extern bzy_filechannel_read_into");
	cg_emit(cg,"extern bzy_filechannel_write_at");
	cg_emit(cg,"extern bzy_filechannel_size");
	cg_emit(cg,"extern bzy_filechannel_truncate");
	cg_emit(cg,"extern bzy_filechannel_sync");
	cg_emit(cg,"extern bzy_filechannel_close");
	cg_emit(cg,"extern bzy_filechannel_lock");
	cg_emit(cg,"extern bzy_filechannel_unlock");
	cg_emit(cg,"extern bzy_mmap_map");
	cg_emit(cg,"extern bzy_memory_map");
	cg_emit(cg,"extern bzy_mmap_size");
	cg_emit(cg,"extern bzy_mmap_get_byte");
	cg_emit(cg,"extern bzy_mmap_get_int");
	cg_emit(cg,"extern bzy_mmap_get_long");
	cg_emit(cg,"extern bzy_mmap_put_byte");
	cg_emit(cg,"extern bzy_mmap_put_int");
	cg_emit(cg,"extern bzy_mmap_put_long");
	cg_emit(cg,"extern bzy_mmap_copy_into");
	cg_emit(cg,"extern bzy_mmap_copy_from");
	cg_emit(cg,"extern bzy_mmap_flush");
	cg_emit(cg,"extern bzy_mmap_close");
	cg_emit(cg,"extern bzy_filewriter_open");
	cg_emit(cg,"extern bzy_filewriter_write");
	cg_emit(cg,"extern bzy_filewriter_write_line");
	cg_emit(cg,"extern bzy_filewriter_write_bytes");
	cg_emit(cg,"extern bzy_filewriter_flush");
	cg_emit(cg,"extern bzy_filewriter_close");
	cg_emit(cg,"extern bzy_logger_open");
	cg_emit(cg,"extern bzy_logger_log");
	cg_emit(cg,"extern bzy_logger_close");
	cg_emit(cg,"extern bzy_str_data");
	cg_emit(cg,"extern bzy_map_iter");
	cg_emit(cg,"extern bzy_map_iter_snapshot");
	cg_emit(cg,"extern bzy_map_key_at");
	cg_emit(cg,"extern bzy_map_val_at");
	cg_emit(cg,"extern bzy_str_eq");
	cg_emit(cg,"extern bzy_str_hashcode");
	cg_emit(cg,"extern bzy_ptr_hash");
	cg_emit(cg,"extern bzy_str_contains");
	cg_emit(cg,"extern bzy_str_starts_with");
	cg_emit(cg,"extern bzy_str_ends_with");
	cg_emit(cg,"extern bzy_str_index_of");
	cg_emit(cg,"extern bzy_str_substring");
	cg_emit(cg,"extern bzy_str_replace");
	cg_emit(cg,"extern bzy_str_trim");
	cg_emit(cg,"extern bzy_str_to_upper");
	cg_emit(cg,"extern bzy_str_to_lower");
	cg_emit(cg,"extern bzy_str_equals_ignore_case");
	cg_emit(cg,"extern bzy_str_is_empty");
	cg_emit(cg,"extern bzy_str_is_numeric");
	cg_emit(cg,"extern bzy_str_is_alphanumeric");
	cg_emit(cg,"extern bzy_str_char_at");
	cg_emit(cg,"extern bzy_str_last_index_of");
	cg_emit(cg,"extern bzy_str_repeat");
	cg_emit(cg,"extern bzy_str_split");
	cg_emit(cg,"extern bzy_str_to_int");
	cg_emit(cg,"extern bzy_str_to_long");
	cg_emit(cg,"extern bzy_str_to_byte");
	cg_emit(cg,"extern bzy_str_to_short");
	cg_emit(cg,"extern bzy_str_to_float");
	cg_emit(cg,"extern bzy_str_to_double");
	cg_emit(cg,"extern bzy_str_to_bool");
	cg_emit(cg,"extern bzy_number_check");
	cg_emit(cg,"extern bzy_vec_new");
	cg_emit(cg,"extern bzy_vec_reserve");
	cg_emit(cg,"extern bzy_vec_len");
	cg_emit(cg,"extern bzy_vec_push_back");
	cg_emit(cg,"extern bzy_vec_push_front");
	cg_emit(cg,"extern bzy_vec_pop_back");
	cg_emit(cg,"extern bzy_vec_pop_front");
	cg_emit(cg,"extern bzy_vec_get");
	cg_emit(cg,"extern bzy_vec_set");
	cg_emit(cg,"extern bzy_vec_peek_back");
	cg_emit(cg,"extern bzy_vec_peek_front");
	cg_emit(cg,"extern bzy_vec_remove_at");
	cg_emit(cg,"extern bzy_vec_index_of");
	cg_emit(cg,"extern bzy_vec_contains");
	cg_emit(cg,"extern bzy_order_cmp_for");
	cg_emit(cg,"extern bzy_obj_compare");
	cg_emit(cg,"extern bzy_pq_new");
	cg_emit(cg,"extern bzy_pq_add");
	cg_emit(cg,"extern bzy_pq_poll");
	cg_emit(cg,"extern bzy_pq_peek");
	cg_emit(cg,"extern bzy_pq_size");
	cg_emit(cg,"extern bzy_btree_new");
	cg_emit(cg,"extern bzy_btree_put");
	cg_emit(cg,"extern bzy_btree_get");
	cg_emit(cg,"extern bzy_btree_has");
	cg_emit(cg,"extern bzy_btree_remove");
	cg_emit(cg,"extern bzy_btree_size");
	cg_emit(cg,"extern bzy_btree_first");
	cg_emit(cg,"extern bzy_btree_last");
	cg_emit(cg,"extern bzy_btree_floor");
	cg_emit(cg,"extern bzy_btree_ceiling");
	cg_emit(cg,"extern bzy_btree_keys");
	cg_emit(cg,"extern bzy_btree_values");
	cg_emit(cg,"extern bzy_btree_entries");
	cg_emit(cg,"extern sin");
	cg_emit(cg,"extern cos");
	cg_emit(cg,"extern tan");
	cg_emit(cg,"extern exp");
	cg_emit(cg,"extern pow");
	cg_emit(cg,"extern bzy_clock_millis");
	cg_emit(cg,"extern bzy_clock_nanos");
	cg_emit(cg,"extern bzy_clock_date");
	cg_emit(cg,"extern bzy_clock_date_fmt");
	cg_emit(cg,"extern bzy_system_shell");
	cg_emit(cg,"extern bzy_sys_args");
	cg_emit(cg,"extern bzy_sys_getenv");
	cg_emit(cg,"extern bzy_await_shutdown");
	cg_emit(cg,"extern bzy_sys_sleep");
	cg_emit(cg,"extern bzy_sys_raw_mode");
	cg_emit(cg,"extern bzy_sys_poll_key");
	cg_emit(cg,"extern bzy_sys_mouse_mode");
	cg_emit(cg,"extern bzy_sys_poll_mouse");
	cg_emit(cg,"extern bzy_sys_cpu_count");
	cg_emit(cg,"extern bzy_sys_affinity");
	cg_emit(cg,"extern bzy_class_name");
	cg_emit(cg,"extern bzy_rnd_bool");
	cg_emit(cg,"extern bzy_rnd_int");
	cg_emit(cg,"extern bzy_rnd_long");
	cg_emit(cg,"extern bzy_rnd_float");
	cg_emit(cg,"extern bzy_rnd_double");
	cg_emit(cg,"extern bzy_rnd_gaussian");
	cg_emit(cg,"extern bzy_rnd_get_i");
	cg_emit(cg,"extern bzy_rnd_get_ii");
	cg_emit(cg,"extern bzy_rnd_get_l");
	cg_emit(cg,"extern bzy_rnd_get_ll");
	cg_emit(cg,"extern bzy_rnd_get_f");
	cg_emit(cg,"extern bzy_rnd_get_ff");
	cg_emit(cg,"extern bzy_rnd_get_d");
	cg_emit(cg,"extern bzy_rnd_get_dd");
	cg_emit(cg,"extern bzy_rnd_bytes");
	cg_emit(cg,"extern bzy_regex_matches");
	cg_emit(cg,"extern bzy_regex_test");
	cg_emit(cg,"extern bzy_regex_find");
	cg_emit(cg,"extern bzy_regex_replace");
	cg_emit(cg,"extern bzy_throw");
	cg_emit(cg,"extern bzy_enum_no_constant");
	cg_emit(cg,"extern bzy_io_check");
	cg_emit(cg,"extern bzy_xml_parse");
	cg_emit(cg,"extern bzy_xml_check");
	cg_emit(cg,"extern bzy_xml_attr");
	cg_emit(cg,"extern bzy_xml_has_attr");
	cg_emit(cg,"extern bzy_xml_attr_name_at");
	cg_emit(cg,"extern bzy_xml_descendants");
	cg_emit(cg,"extern bzy_json_parse");
	cg_emit(cg,"extern bzy_json_check");
	cg_emit(cg,"extern bzy_json_is_null");
	cg_emit(cg,"extern bzy_json_is_bool");
	cg_emit(cg,"extern bzy_json_is_number");
	cg_emit(cg,"extern bzy_json_is_string");
	cg_emit(cg,"extern bzy_json_is_array");
	cg_emit(cg,"extern bzy_json_is_object");
	cg_emit(cg,"extern bzy_json_type_name");
	cg_emit(cg,"extern bzy_json_as_long");
	cg_emit(cg,"extern bzy_json_as_double");
	cg_emit(cg,"extern bzy_json_as_string");
	cg_emit(cg,"extern bzy_json_as_bool");
	cg_emit(cg,"extern bzy_json_get");
	cg_emit(cg,"extern bzy_json_has");
	cg_emit(cg,"extern bzy_json_keys");
	cg_emit(cg,"extern bzy_json_items");
	cg_emit(cg,"extern bzy_json_at");
	cg_emit(cg,"extern bzy_json_size");
	cg_emit(cg,"extern bzy_json_of_long");
	cg_emit(cg,"extern bzy_json_of_double");
	cg_emit(cg,"extern bzy_json_of_string");
	cg_emit(cg,"extern bzy_json_of_bool");
	cg_emit(cg,"extern bzy_json_null");
	cg_emit(cg,"extern bzy_json_of_array");
	cg_emit(cg,"extern bzy_json_of_object");
	cg_emit(cg,"extern bzy_json_stringify");
	cg_emit(cg,"extern bzy_http_read_request");
	cg_emit(cg,"extern bzy_http_read_response");
	cg_emit(cg,"extern bzy_http_check");
	cg_emit(cg,"extern bzy_http_header");
	cg_emit(cg,"extern bzy_http_has_header");
	cg_emit(cg,"extern bzy_http_header_names");
	cg_emit(cg,"extern bzy_http_body_bytes");
	cg_emit(cg,"extern bzy_http_respond");
	cg_emit(cg,"extern bzy_http_response");
	cg_emit(cg,"extern bzy_http_request");
	cg_emit(cg,"extern bzy_http_set_header");
	cg_emit(cg,"extern bzy_http_set_body");
	cg_emit(cg,"extern bzy_http_send_response");
	cg_emit(cg,"extern bzy_http_send_request");
	cg_emit(cg,"extern bzy_file_exists");
	cg_emit(cg,"extern bzy_file_is_file");
	cg_emit(cg,"extern bzy_file_is_folder");
	cg_emit(cg,"extern bzy_file_create_file");
	cg_emit(cg,"extern bzy_file_create_folder");
	cg_emit(cg,"extern bzy_file_delete");
	cg_emit(cg,"extern bzy_file_delete_recursive");
	cg_emit(cg,"extern bzy_file_read_text");
	cg_emit(cg,"extern bzy_file_read_lines");
	cg_emit(cg,"extern bzy_file_write_text");
	cg_emit(cg,"extern bzy_file_append_text");
	cg_emit(cg,"extern bzy_file_read_bytes");
	cg_emit(cg,"extern bzy_file_write_bytes");
	cg_emit(cg,"extern bzy_file_list");
	cg_emit(cg,"extern bzy_file_search");
	cg_emit(cg,"extern bzy_file_search_recursive");
	cg_emit(cg,"extern bzy_file_set_attribute");
	cg_emit(cg,"extern bzy_file_has_attribute");
	cg_emit(cg,"global __bzy_exception_funcs");
	cg_emit(cg,"global __bzy_exception_func_count");
	cg_emit(cg,"global __bzy_vtable_parents");
	cg_emit(cg,"global __bzy_vtable_parent_count");
	cg_emit(cg,"global __vtable_IndexOutOfBounds");   /* Referenced by the runtime bzy_oob. */
	cg_emit(cg,"global __vtable_IOException");        /* Referenced by the runtime bzy_io_check. */
	cg_emit(cg,"global __vtable_NumberFormatException");   /* Referenced by the runtime bzy_number_check. */
	cg_emit(cg,"global __vtable_XmlException");            /* Referenced by the runtime bzy_xml_check. */
	cg_emit(cg,"global __vtable_JsonException");           /* Referenced by the runtime bzy_json_check. */
	cg_emit(cg,"global __vtable_HttpException");           /* Referenced by the runtime bzy_http_check. */
	for (int i=0; i<unit_count; i++)   /* FFI: declare each extern C symbol for the linker. */
	{
		for (int k=0; k<units[i]->func_count; k++)
		{
			if (units[i]->funcs[k]->is_extern)
			{
				cg_emit(cg,"extern %s", units[i]->funcs[k]->name);
			}
		}
	}

	cg_emit(cg,"section .text");

	for (int i=0; i<unit_count; i++)
	{
		Unit *u=units[i];
		for (int k=0; k<u->func_count; k++)
		{
			Func *f=u->funcs[k];
			if (f->is_extern)
			{
				continue;   /* No body; the symbol is declared extern + resolved by the linker. */
			}

			char buf[160];
			const char *label;
			if (strcmp(f->name,"main")==0)
			{
				label="bzy_user_main";   /* The runtime entry.o owns C main and runs this as breeze 0. */
			}
			else
			{
				FuncInfo *fi=NULL;
				for (int q=0; q<tt->func_count; q++)
				{
					if (tt->funcs[q]->ast==f)   /* Match this exact overload by AST identity. */
					{
						fi=tt->funcs[q];
						break;
					}
				}
				strcpy(buf,fi->asm_label);
				label=buf;
			}

			cg_emit_func(cg,tt,label,f,NULL);
		}
	}

	for (int i=0; i<unit_count; i++)
	{
		Unit *u=units[i];
		for (int ci=0; ci<u->class_count; ci++)
		{
			ClassDecl *d=u->klasses[ci];
			ClassInfo *c=types_find_class(tt,d->name);
			for (int k=0; k<d->method_count; k++)
			{
				Func *m=d->methods[k];
				MethodInfo *mi=NULL;
				for (int q=0; q<c->method_count; q++)
				{
					if (c->methods[q].ast==m)   /* Match this exact overload by AST identity. */
					{
						mi=&c->methods[q];
						break;
					}
				}
				cg_emit_func(cg,tt,mi->asm_label,m, mi->is_static ? NULL : c->name);   /* Static: no `this`. */
			}

			for (int k=0; k<d->ctor_count; k++)
			{
				cg_emit_func(cg,tt,c->ctors[k].asm_label,d->ctors[k],c->name);
			}

			if (c->is_record)
			{
				cg_emit_record_methods(cg,tt,c);   /* Synthesized hashCode/equals at slots 0/1. */
			}
		}
	}

	for (int i=0; i<cg->breeze_thunk_count; i++)   /* spawn-with-args thunks (in .text). */
	{
		cg_emit_breeze_thunk(cg, cg->breeze_thunks[i]);
	}

	for (int i=0; i<cg->blocking_thunk_count; i++)   /* extern-blocking offload thunks (in .text). */
	{
		cg_emit_blocking_thunk(cg, cg->blocking_thunks[i]);
	}

	for (int i=0; i<bzy_lambda_count(); i++)   /* synthesized lambda body functions (in .text). */
	{
		LambdaInfo *lam = bzy_lambda_at(i);
		cg_emit_func(cg, tt, lam->label, lam->sf, NULL);
	}

	cg_emit_enum_init(cg,tt);   /* __enum_init (constructs the singletons), still in .text. */
	cg_emit_static_init(cg,tt,units,unit_count);   /* __static_init (runs field initializers). */

	cg_emit(cg,"");
	cg_emit(cg,"section .data");

	/* AVX startup guard hook: a program using f64x4 plants a data pointer to the
	   AVX-check helper. The runtime entry weakly references this symbol and calls
	   through it before main, so the program aborts with a clear message on a non-
	   AVX CPU instead of faulting. The `dq` is a strong reference that pulls in the
	   cpu.c TU; a program with no 256-bit SIMD emits nothing here and links none of
	   it. Lives in .data so it never shifts the .text the prelude depends on. */
	if (g_program_uses_avx)
	{
		cg_emit(cg,"extern bzy_require_avx");
		cg_emit(cg,"global __bzy_avx_check");
		cg_emit(cg,"__bzy_avx_check:");
		cg_emit(cg,"    dq bzy_require_avx");
	}

	for (int i=0; i<tt->class_count; i++)
	{
		cg_emit_vtable(cg,tt->classes[i]);
	}

	/* Per-lambda closure descriptors: a type info {finalizer=0, managed-capture
	   count, managed-capture offsets...} whose pointer lands one word before the
	   vtable label, so the ARC/cycle machinery reaches captured references exactly
	   as it reaches a class's managed fields. */
	for (int i=0; i<bzy_lambda_count(); i++)
	{
		LambdaInfo *lam = bzy_lambda_at(i);
		int nman = 0;
		for (int k=0; k<lam->cap_count; k++)
		{
			if (lam->caps[k].is_managed)
			{
				nman++;
			}
		}

		cg_emit(cg,"__%s_ti:", lam->label);
		cg_emit(cg,"    dq 0");           /* Finalizer: none (ARC releases declared captures). */
		cg_emit(cg,"    dq %d", nman);    /* Managed-capture count. */
		for (int k=0; k<lam->cap_count; k++)
		{
			if (lam->caps[k].is_managed)
			{
				cg_emit(cg,"    dq %d", lam->caps[k].env_offset);
			}
		}

		cg_emit(cg,"    dq __%s_ti", lam->label);   /* Descriptor pointer at vtable-8. */
		cg_emit(cg,"__%s_vt:", lam->label);
		cg_emit(cg,"    dq 0");           /* No methods; the slot exists only to anchor the label. */
		if (lam->cap_count == 0)
		{
			cg_emit(cg,"__%s_single: dq 0", lam->label);   /* Cached stateless closure. */
		}
	}

	cg_emit(cg,"__bzy_vtable_parents:");           /* (child vtable, parent vtable) pairs for is-a. */
	for (int i=0; i<tt->class_count; i++)
	{
		ClassInfo *c = tt->classes[i];
		cg_emit(cg,"    dq __vtable_%s", c->name);
		if (c->parent)
		{
			cg_emit(cg,"    dq __vtable_%s", c->parent->name);
		}
		else
		{
			cg_emit(cg,"    dq 0");
		}
	}

	cg_emit(cg,"__bzy_vtable_parent_count: dq %d", tt->class_count);

	for (int i=0; i<cg->fpk_count; i++)
	{
		if (cg->fpk[i].is_float)
		{
			cg_emit(cg,"__fpk%d: dd 0x%08llx", i, cg->fpk[i].bits & 0xffffffffULL);
		}
		else
		{
			cg_emit(cg,"__fpk%d: dq 0x%016llx", i, cg->fpk[i].bits);
		}
	}

	for (int i=0; i<cg->strk_count; i++)
	{
		fprintf(cg->out, "__str%d: db ", i);
		for (int j=0; j<cg->strk[i].len; j++)
		{
			fprintf(cg->out, "%d,", (unsigned char)cg->strk[i].bytes[j]);
		}

		fprintf(cg->out, "0\n");   /* Trailing NUL (bzy_str_new also NUL-terminates). */
	}

	cg_emit(cg,"__deg2rad: dq 0x3f91df46a2529d39");   /* PI/180 = 0.017453292519943295 (Math.toRadians). */

	cg_emit(cg,"__bzy_exception_funcs:");
	for (int i=0; i<cg->exception_fn_count; i++)
	{
		cg_emit(cg,"    dq __exceptionfn%d", i);
	}

	cg_emit(cg,"__bzy_exception_func_count: dq %d", cg->exception_fn_count);

	/* Per-extern slot + symbol-name statics for `extern dynamic` (in .data: the slot
	   is written at the first call). Keyed on the bare name (matches cg_dynamic_extern_call;
	   fi->asm_label's NASM '$' escape would be an invalid label). Deduped by name. */
	for (int ui=0; ui<unit_count; ui++)
	{
		Unit *u=units[ui];
		for (int fi2=0; fi2<u->func_count; fi2++)
		{
			Func *f=u->funcs[fi2];
			if (!f->is_extern || !f->is_dynamic) { continue; }
			int dup=0;
			for (int uj=0; uj<=ui && !dup; uj++)
			{
				Unit *u2=units[uj];
				int lim=(uj==ui)?fi2:u2->func_count;
				for (int fj=0; fj<lim; fj++)
				{
					Func *g=u2->funcs[fj];
					if (g->is_extern && g->is_dynamic && strcmp(g->name,f->name)==0) { dup=1; break; }
				}
			}
			if (dup) { continue; }
			cg_emit(cg,"__dynslot_%s: dq 0", f->name);
			cg_emit(cg,"__dynname_%s: db \"%s\", 0", f->name, f->name);
		}
	}

	cg_emit_enum_data(cg);   /* Enum singleton slots + constant name strings. */
	cg_emit_static_data(cg,units,unit_count);   /* Static-field global slots. */
}
