#include "iremit.h"
#include "regalloc.h"
#include "lexer.h"   /* Comparison TokenTypes. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Emit a function body from IR, consulting the linear-scan allocation: each value
   (temporary or frame local) lives in a physical register for its live range, or
   in a frame spill slot when the 11-register pool is exhausted. Operands are
   staged through rax/rcx/rdx scratch - cheap when the source is a register (the
   whole point: no stack traffic in the hot loop). The function is a call-free
   leaf; only callee-saved registers it actually uses are saved/restored.

   Frame layout (downward from rbp):
     [rbp - 8 .. rbp - lbase]      function locals/params at their resolver offsets
                                   (used only for locals the allocator spilled)
     [rbp - (lbase+8) ..]          spill slots for spilled temporaries
     [rbp - cs_base ..]            saved callee-saved registers */

typedef struct
{
	Codegen *cg;
	IRAlloc *a;
	int      spill_base;          /* rbp offset of spilled temp slot 0. */
	int     *blabel;              /* Per-block unique label id. */
	int      cs_base;             /* rbp offset of saved-callee-reg 0. */
	int      cs_regs[RA_NREGS];   /* Used callee-saved register indices. */
	int      ncs;
	char      *cis;               /* [vreg_count] 1 if the vreg is a known constant. */
	long long *cval;              /* [vreg_count] that constant's value. */
	char      *cdead;            /* [vreg_count] 1 if the const folds into every use (skip emit). */
	char      *nn;               /* [vreg_count] 1 if the vreg's value is provably >= 0. */
	char      *mclean;          /* [vreg_count] 1 if some use observes the value's high 32
	                               bits, so a narrow-int result must be re-extended (clean). */
	/* Cold bounds-failure stubs, buffered so the hot path is a single not-taken
	   jae: each records the label and the registers holding base/index at the
	   check (still live at the stub - the jump leaves them untouched). */
	struct
	{
		int  lbl;
		char base[8];
		char idx[8];
	} oob[256];
	int noob;
} Emit;

/* If v is a positive power of two, set *k = log2(v) and return 1. */
static int pow2_log(long long v, int *k)
{
	if (v <= 0 || (v & (v - 1)) != 0)
	{
		return 0;
	}

	int e = 0;
	while ((1LL << e) != v)
	{
		e++;
	}

	*k = e;
	return 1;
}

/* Is this instruction's result provably >= 0, given current vreg/local facts?
   Conservative: any op not listed is assumed possibly-negative. */
static int val_nonneg(IRFunc *f, IRAlloc *a, const IRInstr *in, const char *nn, const char *nnl)
{
	IRReg av = in->a;
	IRReg bv = in->b;
	int na = (av >= 0 && av < f->vreg_count) ? nn[av] : 0;
	int nb = (bv >= 0 && bv < f->vreg_count) ? nn[bv] : 0;
	switch (in->op)
	{
	case IR_CONST:
		return in->imm >= 0;
	case IR_MOVE:
		return na;
	case IR_CAST:
		return na && ty_bits(in->to_kind) >= 64;   /* Widening keeps the sign; narrowing might not. */
	case IR_LOAD:
		if (in->is_frame)
		{
			for (int k = 0; k < a->nlocal; k++)
			{
				if (a->local_disp[k] == in->disp)
				{
					return nnl[k];
				}
			}
		}

		return 0;
	case IR_ADD:
	case IR_MUL:
	case IR_OR:
	case IR_XOR:
		return na && nb;
	case IR_AND:
		return na || nb;
	case IR_SHR:
		return na || ty_is_unsigned(in->type);
	case IR_DIV:
	case IR_MOD:
		return na && nb;
	case IR_CMP:
		return 1;   /* 0 or 1. */
	default:
		return 0;   /* SUB, NEG, SHL: may be negative. */
	}
}

/* Fill nn[0..vreg_count) with "provably >= 0". A local is non-negative only if
   every store to it stores a non-negative value (greatest fixpoint on locals,
   least fixpoint on vregs). */
static void compute_nonneg(IRFunc *f, IRAlloc *a, char *nn)
{
	int nl = a->nlocal;
	char *nnl = malloc((size_t)(nl > 0 ? nl : 1));
	for (int k = 0; k < nl; k++)
	{
		nnl[k] = 1;   /* Optimistic; refuted below. */
	}

	int outer = 1;
	while (outer)
	{
		outer = 0;
		for (int v = 0; v < f->vreg_count; v++)
		{
			nn[v] = 0;
		}

		int inner = 1;
		while (inner)
		{
			inner = 0;
			for (int b = 0; b < f->block_count; b++)
			{
				IRBlock *blk = &f->blocks[b];
				for (int i = 0; i < blk->count; i++)
				{
					IRInstr *in = &blk->instrs[i];
					if (in->dst >= 0 && in->dst < f->vreg_count && !nn[in->dst]
						&& val_nonneg(f, a, in, nn, nnl))
					{
						nn[in->dst] = 1;
						inner = 1;
					}
				}
			}
		}

		/* Refute any local that receives a not-proven-non-negative store. */
		for (int b = 0; b < f->block_count; b++)
		{
			IRBlock *blk = &f->blocks[b];
			for (int i = 0; i < blk->count; i++)
			{
				IRInstr *in = &blk->instrs[i];
				if (in->op == IR_STORE && in->is_frame)
				{
					int notnn = !(in->c >= 0 && in->c < f->vreg_count && nn[in->c]);
					if (notnn)
					{
						for (int k = 0; k < nl; k++)
						{
							if (a->local_disp[k] == in->disp && nnl[k])
							{
								nnl[k] = 0;
								outer = 1;
							}
						}
					}
				}
			}
		}
	}

	free(nnl);
}

static const char *iremit_iarg(Codegen *cg, int i)
{
	static const char *win[4]  = { "rcx", "rdx", "r8", "r9" };
	static const char *sysv[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
	return (cg->target == TARGET_LINUX) ? sysv[i] : win[i];
}

/* The xmm register an FP argument arrives in. Valid for pure-double signatures on
   both ABIs: Win64 passes the i-th arg (any type) by position in xmm(i) for i<4;
   System V uses a separate FP sequence, which coincides with the position when every
   parameter is FP. Mixed int/FP signatures are gated out until Stage 3. */
static const char *iremit_farg(int i)
{
	static const char *xr[8] = { "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7" };
	return xr[i];
}

static void norm_rax(Codegen *cg, TypeKind to)
{
	switch (ty_bits(to))
	{
	case 8:
		cg_emit(cg, ty_is_signed(to) ? "    movsx rax, al" : "    movzx rax, al");
		break;
	case 16:
		cg_emit(cg, ty_is_signed(to) ? "    movsx rax, ax" : "    movzx rax, ax");
		break;
	case 32:
		cg_emit(cg, ty_is_signed(to) ? "    movsxd rax, eax" : "    mov eax, eax");
		break;
	default:
		break;
	}
}

static const char *setcc_op(int cmp_op, int uns)
{
	switch (cmp_op)
	{
	case TOKEN_EQ:  return "sete";
	case TOKEN_NEQ: return "setne";
	case TOKEN_LT:  return uns ? "setb"  : "setl";
	case TOKEN_GT:  return uns ? "seta"  : "setg";
	case TOKEN_LTE: return uns ? "setbe" : "setle";
	case TOKEN_GTE: return uns ? "setae" : "setge";
	default:        return "sete";
	}
}

static const char *jcc_op(int cmp_op, int uns)
{
	switch (cmp_op)
	{
	case TOKEN_EQ:  return "je";
	case TOKEN_NEQ: return "jne";
	case TOKEN_LT:  return uns ? "jb"  : "jl";
	case TOKEN_GT:  return uns ? "ja"  : "jg";
	case TOKEN_LTE: return uns ? "jbe" : "jle";
	case TOKEN_GTE: return uns ? "jae" : "jge";
	default:        return "je";
	}
}

/* The opposite comparison, so a branch can fall through to its true side. */
static int cmp_invert(int cmp_op)
{
	switch (cmp_op)
	{
	case TOKEN_EQ:  return TOKEN_NEQ;
	case TOKEN_NEQ: return TOKEN_EQ;
	case TOKEN_LT:  return TOKEN_GTE;
	case TOKEN_GTE: return TOKEN_LT;
	case TOKEN_GT:  return TOKEN_LTE;
	case TOKEN_LTE: return TOKEN_GT;
	default:        return cmp_op;
	}
}

/* Emit a two-way branch (true -> tblk, else -> fblk), using fallthrough when one
   target is the next block in layout: drop the unconditional jmp, inverting the
   condition when it is the true side that falls through. `next` is the next block
   index, or -1. */
static void emit_two_way(Emit *e, const char *jcc, int cmp_op_for_inv, int uns,
                         int tblk, int fblk, int next)
{
	Codegen *cg = e->cg;
	if (fblk == next)
	{
		cg_emit(cg, "    %s .L%d", jcc, e->blabel[tblk]);
	}
	else if (tblk == next)
	{
		cg_emit(cg, "    %s .L%d", jcc_op(cmp_invert(cmp_op_for_inv), uns), e->blabel[fblk]);
	}
	else
	{
		cg_emit(cg, "    %s .L%d", jcc, e->blabel[tblk]);
		cg_emit(cg, "    jmp .L%d", e->blabel[fblk]);
	}
}

/* True if a value (vreg id v) lives in the xmm register class. */
static int vreg_is_fp(Emit *e, IRReg v)
{
	return ra_vreg_class(e->a, v) == RC_XMM;
}

/* Scratch register for a spilled value of v's class: xmm0 for FP, rax for GP. */
static const char *scratch_for(Emit *e, IRReg v)
{
	return vreg_is_fp(e, v) ? "xmm0" : "rax";
}

/* The move mnemonic for a value of v's class: movsd for FP, mov for GP. */
static const char *mov_for(Emit *e, IRReg v)
{
	return vreg_is_fp(e, v) ? "movsd" : "mov";
}

/* The operand string for vreg v: its physical register name, or, if spilled, the
   value loaded into `scratch` (returned). Register-resident values are used in
   place, so no value bounces through rax unless it was spilled. */
static const char *vreg_in(Emit *e, IRReg v, const char *scratch)
{
	int r = ra_vreg_reg(e->a, v);
	if (r >= 0)
	{
		return ra_reg_name(r);
	}

	/* Rematerialize: the value is a read-only local's load, so read the local
	   where it lives (its register if it got one, else its home slot) instead
	   of a spill slot the def would have had to fill. */
	long long rd = ra_vreg_remat(e->a, v);
	if (rd >= 0)
	{
		int lr = ra_local_reg(e->a, rd);
		if (lr >= 0)
		{
			return ra_reg_name(lr);
		}

		cg_emit(e->cg, "    %s %s, [rbp - %lld]", mov_for(e, v), scratch, rd);
		return scratch;
	}

	cg_emit(e->cg, "    %s %s, [rbp - %d]", mov_for(e, v), scratch, e->spill_base + ra_vreg_slot(e->a, v) * 8);
	return scratch;
}

/* The register an op should write its result into: dst's physical register, or
   rax when dst is spilled (finish_dst then stores it). */
static const char *dst_reg(Emit *e, IRReg v)
{
	int r = ra_vreg_reg(e->a, v);
	return (r >= 0) ? ra_reg_name(r) : scratch_for(e, v);
}

/* After computing into dst_reg(v): store the scratch to the spill slot if v is
   spilled (xmm0/movsd for an FP value, rax/mov for a GP value). */
static void finish_dst(Emit *e, IRReg v)
{
	if (ra_vreg_reg(e->a, v) < 0)
	{
		cg_emit(e->cg, "    %s [rbp - %d], %s", mov_for(e, v),
			e->spill_base + ra_vreg_slot(e->a, v) * 8, scratch_for(e, v));
	}
}

/* The operand string for frame local `disp`: its register, or its home slot. */
static const char *local_in(Emit *e, long long disp, char *buf)
{
	int r = ra_local_reg(e->a, disp);
	if (r >= 0)
	{
		return ra_reg_name(r);
	}

	snprintf(buf, 32, "[rbp - %lld]", disp);
	return buf;
}

/* Move the value in `src` into frame local `disp` (its register, or home slot).
   `fp` selects movsd (double) over mov (integer/pointer). */
static void store_local_from(Emit *e, long long disp, const char *src, int fp)
{
	const char *mv = fp ? "movsd" : "mov";
	int r = ra_local_reg(e->a, disp);
	if (r >= 0)
	{
		if (strcmp(ra_reg_name(r), src))   /* Coalesced: source already in the local's register. */
		{
			cg_emit(e->cg, "    %s %s, %s", mv, ra_reg_name(r), src);
		}
	}
	else
	{
		cg_emit(e->cg, "    %s [rbp - %lld], %s", mv, disp, src);
	}
}

/* The `bytes`-wide sub-register of a 64-bit register name (for narrow array
   element loads/stores). Falls back to the 64-bit name for an unknown register. */
static const char *reg_low(const char *r, int bytes)
{
	static const struct { const char *q, *d, *w, *b; } m[] =
	{
		{ "rax", "eax", "ax", "al" },   { "rbx", "ebx", "bx", "bl" },
		{ "rcx", "ecx", "cx", "cl" },   { "rdx", "edx", "dx", "dl" },
		{ "rsi", "esi", "si", "sil" },  { "rdi", "edi", "di", "dil" },
		{ "r8", "r8d", "r8w", "r8b" },  { "r9", "r9d", "r9w", "r9b" },
		{ "r10", "r10d", "r10w", "r10b" }, { "r11", "r11d", "r11w", "r11b" },
		{ "r12", "r12d", "r12w", "r12b" }, { "r13", "r13d", "r13w", "r13b" },
		{ "r14", "r14d", "r14w", "r14b" }, { "r15", "r15d", "r15w", "r15b" },
	};
	for (int i = 0; i < (int)(sizeof m / sizeof m[0]); i++)
	{
		if (!strcmp(r, m[i].q))
		{
			return (bytes == 1) ? m[i].b : (bytes == 2) ? m[i].w : (bytes == 4) ? m[i].d : m[i].q;
		}
	}

	return r;
}

/* Sign/zero-extend `Ra` to width `to` directly into `Rd` (one instruction), instead
   of staging through rax. For a 32-bit unsigned target a plain 32-bit mov suffices
   (it zero-extends to 64). Rd == Ra is fine (re-extends in place). */
static void norm_reg(Codegen *cg, const char *Rd, const char *Ra, TypeKind to)
{
	switch (ty_bits(to))
	{
	case 8:
		cg_emit(cg, ty_is_signed(to) ? "    movsx %s, %s" : "    movzx %s, %s", Rd, reg_low(Ra, 1));
		break;
	case 16:
		cg_emit(cg, ty_is_signed(to) ? "    movsx %s, %s" : "    movzx %s, %s", Rd, reg_low(Ra, 2));
		break;
	case 32:
		if (ty_is_signed(to))
		{
			cg_emit(cg, "    movsxd %s, %s", Rd, reg_low(Ra, 4));
		}
		else
		{
			cg_emit(cg, "    mov %s, %s", reg_low(Rd, 4), reg_low(Ra, 4));
		}

		break;
	default:
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		break;
	}
}

/* Emit a runtime bounds check for index `Ridx` against array `Rbase`'s length at
   [base + 24]: in range -> fall through; out of range -> call bzy_oob (no return).
   The check is the only hot-path cost (cmp + jb); the oob argument setup and call
   are reached only on the failing path, so clobbering arg/scratch registers there
   is harmless. The order loads length into rax before overwriting any arg register
   so an array base or index that happens to be allocated to an arg register is not
   destroyed before it is read. The pc passed to bzy_oob is a label inside this
   function's recorded PC range, so the unwinder finds its exception record. */
static void emit_bounds_check(Emit *e, const char *Rbase, const char *Ridx)
{
	Codegen *cg = e->cg;
	if (e->noob < (int)(sizeof e->oob / sizeof e->oob[0]))
	{
		/* Hot path: one not-taken branch to a cold stub emitted after the
		   body (emit_oob_stubs). The registers named here still hold base and
		   index at the stub: the jump leaves them untouched. */
		int cold = cg_label(cg);
		cg_emit(cg, "    cmp %s, [%s + 24]", Ridx, Rbase);   /* Unsigned: catches negative and >= length. */
		cg_emit(cg, "    jae .L%d", cold);
		e->oob[e->noob].lbl = cold;
		snprintf(e->oob[e->noob].base, sizeof e->oob[e->noob].base, "%s", Rbase);
		snprintf(e->oob[e->noob].idx, sizeof e->oob[e->noob].idx, "%s", Ridx);
		e->noob++;
		return;
	}

	/* Stub table full: fall back to the inline form. */
	int ok = cg_label(cg);
	int pc = cg_label(cg);
	cg_emit(cg, "    cmp %s, [%s + 24]", Ridx, Rbase);   /* Unsigned: catches negative and >= length. */
	cg_emit(cg, "    jb .L%d", ok);
	cg_emit(cg, "    mov rax, [%s + 24]", Rbase);         /* Length into rax (rax is never an arg register). */
	cg_emit(cg, "    mov %s, %s", iremit_iarg(cg, 0), Ridx);   /* index. */
	cg_emit(cg, "    mov %s, rax", iremit_iarg(cg, 1));        /* length. */
	cg_emit(cg, "    lea %s, [rel .L%d]", iremit_iarg(cg, 2), pc);
	cg_emit(cg, ".L%d:", pc);
	cg_emit(cg, "    mov %s, rbp", iremit_iarg(cg, 3));
	cg_emit(cg, "    call bzy_oob");
	cg_emit(cg, ".L%d:", ok);
}

/* Emit the buffered cold bounds-failure stubs. The caller guarantees control
   cannot fall into them (after a ret, or behind a skip jump for regions).
   bzy_oob never returns; the pc label keeps the unwinder inside this
   function's recorded range. */
static void emit_oob_stubs(Emit *e)
{
	Codegen *cg = e->cg;
	for (int i = 0; i < e->noob; i++)
	{
		int pc = cg_label(cg);
		cg_emit(cg, ".L%d:", e->oob[i].lbl);
		cg_emit(cg, "    mov rax, [%s + 24]", e->oob[i].base);     /* Length (rax is never an arg register). */
		cg_emit(cg, "    mov %s, %s", iremit_iarg(cg, 0), e->oob[i].idx);
		cg_emit(cg, "    mov %s, rax", iremit_iarg(cg, 1));
		cg_emit(cg, "    lea %s, [rel .L%d]", iremit_iarg(cg, 2), pc);
		cg_emit(cg, ".L%d:", pc);
		cg_emit(cg, "    mov %s, rbp", iremit_iarg(cg, 3));
		cg_emit(cg, "    call bzy_oob");
	}

	e->noob = 0;
}

/* Format the element address [base + index*scale + disp] (or [base + disp] with no
   index) into buf, from already-fetched base/index register names. */
static void elem_addr(const IRInstr *in, const char *Rbase, const char *Ridx, char *buf, size_t n)
{
	if (Ridx)
	{
		snprintf(buf, n, "[%s + %s*%d + %lld]", Rbase, Ridx, in->scale, in->disp);
	}
	else
	{
		snprintf(buf, n, "[%s + %lld]", Rbase, in->disp);
	}
}

/* dst = a / 2^k or a % 2^k via shifts/masks, signed-correct (matches idiv's
   truncate-toward-zero), avoiding the ~20-40 cycle idiv. The bias-correcting
   signed cases stage through rax/rdx; the single-op unsigned/non-negative cases
   work directly on Rd so an in-place x = x / 2^k stays one instruction. */
static void emit_divmod_pow2(Emit *e, const IRInstr *in, int k)
{
	Codegen *cg = e->cg;
	int uns = ty_is_unsigned(in->type);
	int nonneg = (in->a >= 0 && in->a < e->a->vreg_count && e->nn[in->a]);
	long long mask = (1LL << k) - 1;

	/* No sign bias to correct: a single shift (DIV) or mask (MOD). Emit it on Rd
	   directly - when the allocator coalesced Rd with the source (x = x / 2^k),
	   this is one in-place instruction instead of a mov/op/mov round-trip. */
	if (uns || nonneg)
	{
		const char *Ra = vreg_in(e, in->a, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		if (in->op == IR_DIV)
		{
			cg_emit(cg, uns ? "    shr %s, %d" : "    sar %s, %d", Rd, k);
		}
		else
		{
			cg_emit(cg, "    and %s, %lld", Rd, mask);
		}

		finish_dst(e, in->dst);
		return;
	}

	const char *Ra = vreg_in(e, in->a, "rax");
	if (strcmp(Ra, "rax"))
	{
		cg_emit(cg, "    mov rax, %s", Ra);
	}

	if (in->op == IR_DIV)
	{
		cg_emit(cg, "    mov rdx, rax");
		cg_emit(cg, "    sar rdx, 63");
		cg_emit(cg, "    shr rdx, %d", 64 - k);
		cg_emit(cg, "    add rax, rdx");
		cg_emit(cg, "    sar rax, %d", k);
	}
	else
	{
		cg_emit(cg, "    mov rdx, rax");
		cg_emit(cg, "    sar rdx, 63");
		cg_emit(cg, "    shr rdx, %d", 64 - k);
		cg_emit(cg, "    add rax, rdx");
		cg_emit(cg, "    and rax, %lld", mask);
		cg_emit(cg, "    sub rax, rdx");
	}

	const char *Rd = dst_reg(e, in->dst);
	if (strcmp(Rd, "rax"))
	{
		cg_emit(cg, "    mov %s, rax", Rd);
	}

	finish_dst(e, in->dst);
}

/* 1 if vreg v is a known constant fitting a 32-bit immediate (-> *out). */
static int const_imm32(Emit *e, IRReg v, long long *out)
{
	if (v >= 0 && v < e->a->vreg_count && e->cis[v]
		&& e->cval[v] >= -2147483648LL && e->cval[v] <= 2147483647LL)
	{
		*out = e->cval[v];
		return 1;
	}

	return 0;
}

/* 32-bit-native integer model: an `int` value is correct only in its low 32 bits;
   the high 32 are don't-care unless a consumer observes them (see compute_must_clean
   and `e->mclean`). Re-extend a narrow-int arithmetic/bitwise result with `movsxd`
   ONLY when some use observes its high bits. This drops the per-op re-extension on
   loop-carried chains (e.g. the packet FNV `imul`/`xor` hash) while keeping it at the
   indexing/shift/compare/widen sites that need a clean 64-bit register.

   add/sub/mul/shl/neg always carry into the high bits, so they re-extend whenever the
   result is observed. and/or/xor leave the high bits a combination of their operands'
   high bits (possibly dirty under this model), so they too re-extend when observed -
   sound, at worst a redundant movsxd on a clean-input bitwise result that is indexed. */
static int needs_reext(Emit *e, const IRInstr *in)
{
	if (!ty_is_int(in->type) || ty_bits(in->type) >= 64)
	{
		return 0;
	}

	if (in->dst < 0 || in->dst >= e->a->vreg_count || !e->mclean[in->dst])
	{
		return 0;
	}

	switch (in->op)
	{
	case IR_ADD:
	case IR_SUB:
	case IR_MUL:
	case IR_SHL:
	case IR_NEG:
	case IR_AND:
	case IR_OR:
	case IR_XOR:
		return 1;
	default:
		return 0;
	}
}

/* Scalar double: dst = a <op> b (opc = addsd/subsd/mulsd/divsd). Values are
   xmm-class; spilled operands stage through xmm0/xmm1 (the reserved FP scratch,
   never allocated). Guards the dst==rhs aliasing hazard: writing a into dst would
   clobber a still-needed rhs, so preserve rhs in xmm1 first. */
static void emit_bin_fp(Emit *e, const IRInstr *in, const char *opc)
{
	const char *Ra = vreg_in(e, in->a, "xmm0");
	const char *Rb = vreg_in(e, in->b, "xmm1");
	const char *Rd = dst_reg(e, in->dst);
	if (!strcmp(Rd, Rb) && strcmp(Rd, Ra))
	{
		if (strcmp(Rb, "xmm1"))
		{
			cg_emit(e->cg, "    movaps xmm1, %s", Rb);
		}

		Rb = "xmm1";
	}

	if (strcmp(Rd, Ra))
	{
		cg_emit(e->cg, "    movaps %s, %s", Rd, Ra);
	}

	cg_emit(e->cg, "    %s %s, %s", opc, Rd, Rb);
	finish_dst(e, in->dst);
}

/* dst = a <op> b, register-direct, honoring x86's two-operand form and aliasing. */
static void emit_bin(Emit *e, const IRInstr *in, const char *opc, int commutative)
{
	Codegen *cg = e->cg;
	IRReg av = in->a;
	IRReg bv = in->b;
	long long imm;

	/* For a commutative op, prefer the constant as the folded immediate (so
	   `3 * m` becomes `imul Rd, Rm, 3` rather than materializing 3). */
	if (commutative && !const_imm32(e, bv, &imm) && const_imm32(e, av, &imm))
	{
		IRReg t = av;
		av = bv;
		bv = t;
	}

	/* Fold a fits-imm32 constant rhs into the instruction instead of a register. */
	if (const_imm32(e, bv, &imm))
	{
		const char *Ra = vreg_in(e, av, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (!strcmp(opc, "imul"))
		{
			/* Strength-reduce a small constant multiply to a single LEA: the
			   address unit computes Ra*{2,3,5,9} as base+index*scale in one
			   1-cycle op, off the 3-cycle imul latency that would otherwise sit
			   on a loop-carried chain (e.g. collatz's odd step 3*n+1). LEA's
			   low-64 result equals imul's (both wrap mod 2^64), so the
			   32-bit-native re-extension below stays valid. */
			int sc = (imm == 3) ? 2 : (imm == 5) ? 4 : (imm == 9) ? 8 : 0;
			if (sc)
			{
				cg_emit(cg, "    lea %s, [%s + %s*%d]", Rd, Ra, Ra, sc);
			}
			else if (imm == 2)
			{
				cg_emit(cg, "    lea %s, [%s + %s]", Rd, Ra, Ra);
			}
			else
			{
				cg_emit(cg, "    imul %s, %s, %lld", Rd, Ra, imm);
			}
		}
		else
		{
			if (strcmp(Rd, Ra))
			{
				cg_emit(cg, "    mov %s, %s", Rd, Ra);
			}

			cg_emit(cg, "    %s %s, %lld", opc, Rd, imm);
		}

		if (needs_reext(e, in))
		{
			norm_reg(cg, Rd, Rd, in->type);
		}

		finish_dst(e, in->dst);
		return;
	}

	const char *Ra = vreg_in(e, av, "rax");
	const char *Rb = vreg_in(e, bv, "rcx");
	const char *Rd = dst_reg(e, in->dst);

	if (!strcmp(Rd, Rb) && strcmp(Rd, Ra))
	{
		/* dst aliases the second operand. */
		if (commutative)
		{
			cg_emit(cg, "    %s %s, %s", opc, Rd, Ra);
		}
		else
		{
			cg_emit(cg, "    mov rax, %s", Ra);
			cg_emit(cg, "    %s rax, %s", opc, Rb);
			cg_emit(cg, "    mov %s, rax", Rd);
		}
	}
	else
	{
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		cg_emit(cg, "    %s %s, %s", opc, Rd, Rb);
	}

	if (needs_reext(e, in))
	{
		norm_reg(cg, Rd, Rd, in->type);
	}

	finish_dst(e, in->dst);
}

static void emit_epilogue(Emit *e)
{
	Codegen *cg = e->cg;
	for (int j = e->ncs - 1; j >= 0; j--)
	{
		cg_emit(cg, "    mov %s, [rbp - %d]", ra_reg_name(e->cs_regs[j]), e->cs_base + j * 8);
	}

	cg_emit(cg, "    mov rsp, rbp");
	cg_emit(cg, "    pop rbp");
	cg_emit(cg, "    ret");
}

/* A compare immediately followed by a branch on its result: emit cmp + a single
   conditional jump, skipping the setcc/movzx/test the two would otherwise need. */
static void emit_fused_branch(Emit *e, const IRInstr *cmp, const IRInstr *br, int next)
{
	Codegen *cg = e->cg;
	const char *Ra = vreg_in(e, cmp->a, "rax");
	long long imm;
	if (const_imm32(e, cmp->b, &imm))
	{
		cg_emit(cg, "    cmp %s, %lld", Ra, imm);
	}
	else
	{
		const char *Rb = vreg_in(e, cmp->b, "rcx");
		cg_emit(cg, "    cmp %s, %s", Ra, Rb);
	}

	int uns = ty_is_unsigned(cmp->type);
	emit_two_way(e, jcc_op(cmp->cmp_op, uns), cmp->cmp_op, uns, br->blk_true, br->blk_false, next);
}

/* (x % 2^k) == 0 (or != 0) feeding a branch: divisibility is just the low k bits,
   so emit `test x, mask` + jz/jnz - no signed remainder, no compare. Sign-correct
   because evenness/divisibility by a power of two is independent of sign. */
static void emit_divisibility_branch(Emit *e, const IRInstr *mod, const IRInstr *cmp, const IRInstr *br, int k, int next)
{
	Codegen *cg = e->cg;
	const char *Rx = vreg_in(e, mod->a, "rax");
	long long mask = (1LL << k) - 1;
	cg_emit(cg, "    test %s, %lld", Rx, mask);
	/* je/jne are encoding-identical to jz/jnz, so the generic two-way helper (and
	   its condition inversion for fallthrough) applies to the divisibility test. */
	const char *j = (cmp->cmp_op == TOKEN_EQ) ? "je" : "jne";
	emit_two_way(e, j, cmp->cmp_op, 0, br->blk_true, br->blk_false, next);
}

static void emit_instr(Emit *e, const IRInstr *in, int next)
{
	Codegen *cg = e->cg;
	char buf[40];
	switch (in->op)
	{
	case IR_CONST:
		if (in->dst >= 0 && in->dst < e->a->vreg_count && e->cdead[in->dst])
		{
			break;   /* Folds into every use; no register needed. */
		}

		if (vreg_is_fp(e, in->dst))
		{
			/* Materialize the double bit pattern via a GP scratch, then move it
			   into the xmm dst (SSE2 movq GP->xmm; no data section needed). */
			const char *Rd = dst_reg(e, in->dst);
			cg_emit(cg, "    mov rax, %lld", in->imm);
			cg_emit(cg, "    movq %s, rax", Rd);
			finish_dst(e, in->dst);
			break;
		}

		cg_emit(cg, "    mov %s, %lld", dst_reg(e, in->dst), in->imm);
		finish_dst(e, in->dst);
		break;
	case IR_MOVE:
	{
		const char *Ra = vreg_in(e, in->a, scratch_for(e, in->a));
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    %s %s, %s", mov_for(e, in->dst), Rd, Ra);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_LOAD:
	{
		if (in->is_frame)
		{
			/* A rematerializing dst needs no def at all: every use reads the
			   (never-stored) local directly. */
			if (in->dst >= 0 && ra_vreg_remat(e->a, in->dst) >= 0)
			{
				break;
			}

			const char *Rl = local_in(e, in->disp, buf);
			const char *Rd = dst_reg(e, in->dst);
			if (strcmp(Rd, Rl))
			{
				cg_emit(cg, "    %s %s, %s", mov_for(e, in->dst), Rd, Rl);
			}

			finish_dst(e, in->dst);
			break;
		}

		/* Array element / header load: [base + index*scale + disp]. */
		const char *Rbase = vreg_in(e, in->a, "rax");
		const char *Ridx = (in->b != IR_NO_REG) ? vreg_in(e, in->b, "rcx") : NULL;
		if (in->checked && Ridx)
		{
			emit_bounds_check(e, Rbase, Ridx);
		}

		char addr[64];
		elem_addr(in, Rbase, Ridx, addr, sizeof addr);
		const char *Rd = dst_reg(e, in->dst);
		int bytes = in->scale ? in->scale : 8;   /* scale 0 = a .length (8-byte) load. */
		int sgn = ty_is_signed(in->type);
		if (bytes == 8)
		{
			cg_emit(cg, "    mov %s, %s", Rd, addr);
		}
		else if (bytes == 4)
		{
			cg_emit(cg, sgn ? "    movsxd %s, dword %s" : "    mov %s, dword %s",
					sgn ? Rd : reg_low(Rd, 4), addr);
		}
		else if (bytes == 2)
		{
			cg_emit(cg, sgn ? "    movsx %s, word %s" : "    movzx %s, word %s", Rd, addr);
		}
		else
		{
			cg_emit(cg, sgn ? "    movsx %s, byte %s" : "    movzx %s, byte %s", Rd, addr);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_STORE:
	{
		if (in->is_frame)
		{
			const char *Rc = vreg_in(e, in->c, scratch_for(e, in->c));
			store_local_from(e, in->disp, Rc, vreg_is_fp(e, in->c));
			break;
		}

		/* Array element store: [base + index*scale + disp] = value (natural width).
		   Base/index/value take distinct scratch (rax/rcx/rdx) when spilled. */
		const char *Rbase = vreg_in(e, in->a, "rax");
		const char *Ridx = (in->b != IR_NO_REG) ? vreg_in(e, in->b, "rcx") : NULL;
		if (in->checked && Ridx)
		{
			emit_bounds_check(e, Rbase, Ridx);
		}

		char addr[64];
		elem_addr(in, Rbase, Ridx, addr, sizeof addr);
		const char *Rc = vreg_in(e, in->c, "rdx");
		int bytes = in->scale ? in->scale : 8;
		if (bytes == 1)
		{
			cg_emit(cg, "    mov byte %s, %s", addr, reg_low(Rc, 1));
		}
		else if (bytes == 2)
		{
			cg_emit(cg, "    mov word %s, %s", addr, reg_low(Rc, 2));
		}
		else if (bytes == 4)
		{
			cg_emit(cg, "    mov dword %s, %s", addr, reg_low(Rc, 4));
		}
		else
		{
			cg_emit(cg, "    mov %s, %s", addr, Rc);
		}

		break;
	}
	case IR_ADD:
		if (ty_is_float(in->type)) { emit_bin_fp(e, in, "addsd"); break; }
		emit_bin(e, in, "add", 1);
		break;
	case IR_SUB:
		if (ty_is_float(in->type)) { emit_bin_fp(e, in, "subsd"); break; }
		emit_bin(e, in, "sub", 0);
		break;
	case IR_MUL:
		if (ty_is_float(in->type)) { emit_bin_fp(e, in, "mulsd"); break; }
		emit_bin(e, in, "imul", 1);
		break;
	case IR_AND: emit_bin(e, in, "and", 1);  break;
	case IR_OR:  emit_bin(e, in, "or", 1);   break;
	case IR_XOR: emit_bin(e, in, "xor", 1);  break;
	case IR_DIV:
	case IR_MOD:
	{
		if (ty_is_float(in->type)) { emit_bin_fp(e, in, "divsd"); break; }   /* IR_MOD never float. */
		int k;
		if (in->b != IR_NO_REG && in->b < e->a->vreg_count
			&& e->cis[in->b] && pow2_log(e->cval[in->b], &k))
		{
			emit_divmod_pow2(e, in, k);
			break;
		}

		const char *Ra = vreg_in(e, in->a, "rax");
		if (strcmp(Ra, "rax"))
		{
			cg_emit(cg, "    mov rax, %s", Ra);   /* Dividend must be in rax. */
		}

		/* A 32-bit-result divide uses the 32-bit form (cdq/idiv r32): roughly half
		   the latency of a 64-bit idiv, and a clean operand's low 32 bits are the
		   int value - mirrors the emitter's int divide exactly. */
		const char *Rb = vreg_in(e, in->b, "rcx");   /* Divisor (never rax/rdx). */
		int narrow = ty_is_int(in->type) && ty_bits(in->type) <= 32;
		if (ty_is_unsigned(in->type))
		{
			cg_emit(cg, "    xor edx, edx");
			cg_emit(cg, "    div %s", narrow ? reg_low(Rb, 4) : Rb);
		}
		else if (narrow)
		{
			cg_emit(cg, "    cdq");
			cg_emit(cg, "    idiv %s", reg_low(Rb, 4));
		}
		else
		{
			cg_emit(cg, "    cqo");
			cg_emit(cg, "    idiv %s", Rb);
		}

		const char *Rd = dst_reg(e, in->dst);
		const char *res = (in->op == IR_MOD) ? "rdx" : "rax";
		if (narrow)
		{
			norm_reg(cg, Rd, res, in->type);   /* Re-extend the 32-bit result: clean invariant. */
		}
		else if (in->op == IR_MOD)
		{
			cg_emit(cg, "    mov %s, rdx", Rd);
		}
		else if (strcmp(Rd, "rax"))
		{
			cg_emit(cg, "    mov %s, rax", Rd);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_SHL:
	case IR_SHR:
	{
		/* Count into cl FIRST: the count's register may be the same as dst (the
		   count dies at this instruction, so the allocator may reuse its register
		   for the result) and writing Rd before reading Rb would clobber it. Rd
		   and Ra can never be rcx themselves: rcx is not allocatable while any
		   shift exists (regalloc's claim guard). */
		const char *Rb = vreg_in(e, in->b, "rcx");
		if (strcmp(Rb, "rcx"))
		{
			cg_emit(cg, "    mov rcx, %s", Rb);
		}

		const char *Ra = vreg_in(e, in->a, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		if (in->op == IR_SHL)
		{
			cg_emit(cg, "    shl %s, cl", Rd);
			if (needs_reext(e, in))
			{
				norm_reg(cg, Rd, Rd, in->type);
			}
		}
		else
		{
			cg_emit(cg, ty_is_unsigned(in->type) ? "    shr %s, cl" : "    sar %s, cl", Rd);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_NEG:
	{
		const char *Ra = vreg_in(e, in->a, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		cg_emit(cg, "    neg %s", Rd);
		if (needs_reext(e, in))
		{
			norm_reg(cg, Rd, Rd, in->type);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_CAST:
	{
		const char *Ra = vreg_in(e, in->a, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (ty_bits(in->to_kind) >= 64 || ty_bits(in->to_kind) == 0)
		{
			if (strcmp(Rd, Ra))   /* Widening to 64 bits is a plain move. */
			{
				cg_emit(cg, "    mov %s, %s", Rd, Ra);
			}
		}
		else
		{
			norm_reg(cg, Rd, Ra, in->to_kind);   /* Extend straight into Rd, no rax round-trip. */
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_CMP:
	{
		const char *Ra = vreg_in(e, in->a, "rax");
		long long imm;
		if (const_imm32(e, in->b, &imm))
		{
			cg_emit(cg, "    cmp %s, %lld", Ra, imm);
		}
		else
		{
			const char *Rb = vreg_in(e, in->b, "rcx");
			cg_emit(cg, "    cmp %s, %s", Ra, Rb);
		}

		cg_emit(cg, "    %s al", setcc_op(in->cmp_op, ty_is_unsigned(in->type)));
		cg_emit(cg, "    movzx rax, al");
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, "rax"))
		{
			cg_emit(cg, "    mov %s, rax", Rd);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_SEL:
	{
		/* dst = (a <cmp_op> b) ? c : d, branchless. The false side moves into
		   dst first; mov preserves flags, so a spilled true side may reload
		   between the cmp and the cmov. Scratches dodge dst's register (which
		   can be the claimable rcx/rdx, or rax when dst spilled). */
		const char *Rd = dst_reg(e, in->dst);
		const char *s1 = strcmp(Rd, "rax") ? "rax" : "rcx";
		const char *s2 = strcmp(Rd, "rdx") ? "rdx" : "rcx";
		const char *Dd = vreg_in(e, in->d, Rd);
		if (strcmp(Rd, Dd))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Dd);
		}

		const char *Ra = vreg_in(e, in->a, s1);
		long long imm;
		if (const_imm32(e, in->b, &imm))
		{
			cg_emit(cg, "    cmp %s, %lld", Ra, imm);
		}
		else
		{
			const char *Rb = vreg_in(e, in->b, s2);
			cg_emit(cg, "    cmp %s, %s", Ra, Rb);
		}

		const char *Rc = vreg_in(e, in->c, s2);   /* After the cmp: mov keeps flags. */
		cg_emit(cg, "    cmov%s %s, %s",
				jcc_op(in->cmp_op, ty_is_unsigned(in->type)) + 1, Rd, Rc);
		finish_dst(e, in->dst);
		break;
	}
	case IR_BR:
		if (in->blk_true != next)   /* Fall through when the target is the next block. */
		{
			cg_emit(cg, "    jmp .L%d", e->blabel[in->blk_true]);
		}

		break;
	case IR_BRCOND:
	{
		const char *Ra = vreg_in(e, in->a, "rax");
		cg_emit(cg, "    cmp %s, 0", Ra);
		/* a != 0 -> true; TOKEN_NEQ gives jne, its inverse je. */
		emit_two_way(e, "jne", TOKEN_NEQ, 0, in->blk_true, in->blk_false, next);
		break;
	}
	case IR_RET:
		if (in->a != IR_NO_REG)
		{
			if (vreg_is_fp(e, in->a))
			{
				const char *Ra = vreg_in(e, in->a, "xmm0");
				if (strcmp(Ra, "xmm0"))
				{
					cg_emit(cg, "    movaps xmm0, %s", Ra);
				}
			}
			else
			{
				const char *Ra = vreg_in(e, in->a, "rax");
				if (strcmp(Ra, "rax"))
				{
					cg_emit(cg, "    mov rax, %s", Ra);
				}
			}
		}

		emit_epilogue(e);
		break;
	default:
		break;
	}
}

/* Mark vreg v as observed at its high 32 bits (must be a clean sign-extended 64). */
static void mc_mark(char *mc, int nv, IRReg v)
{
	if (v >= 0 && v < nv)
	{
		mc[v] = 1;
	}
}

/* The local index (into a->local_disp) for frame offset `disp`, or -1. */
static int local_of_disp(IRAlloc *a, long long disp)
{
	for (int k = 0; k < a->nlocal; k++)
	{
		if (a->local_disp[k] == disp)
		{
			return k;
		}
	}

	return -1;
}

/* Compute `must_clean[v]`: a narrow-int result is re-extended only when some use
   observes its high 32 bits. Three passes (32-bit-native model):

   A. Mark the operands of every instruction that reads the full 64-bit register
      (array index, right shift, div/mod, compare/select/brcond, widen-to-long cast,
      8-byte store value, move/ret). NOT marked: add/sub/mul/and/or/xor/neg operands
      and shift counts - those are correct from the low 32 bits alone.
   B. A frame local needs clean stores iff it has a high-32-observed LOAD inside this
      IR body, or (in a region) it is memory-resident: the region exit store-back
      re-extends register-allocated narrow-int locals, but a spilled local has no such
      fixup, so its stores must already be clean for the emitter that reads it.
   C. Mark a frame-store's value operand clean iff its target local needs clean stores.
      A local whose loads are all low-32-safe (e.g. the FNV hash, read only by xor/imul
      and a final masking AND) carries dirty intermediate values - no re-extension. */
static void compute_must_clean(IRFunc *f, IRAlloc *a, char *mc, int nv)
{
	/* Pass A: direct high-32 observers. */
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			switch (in->op)
			{
			case IR_LOAD:
				if (!in->is_frame)
				{
					mc_mark(mc, nv, in->a);                 /* Array base pointer. */
					if (in->scale)
					{
						mc_mark(mc, nv, in->b);             /* Index (full 64-bit reg). */
					}
				}

				break;
			case IR_STORE:
				if (!in->is_frame)
				{
					mc_mark(mc, nv, in->a);
					if (in->scale)
					{
						mc_mark(mc, nv, in->b);             /* Index. */
					}

					if ((in->scale ? in->scale : 8) == 8)
					{
						mc_mark(mc, nv, in->c);             /* 8-byte store writes the full reg. */
					}
				}

				break;                                      /* Frame-store value handled in Pass C. */
			case IR_SHR:
				mc_mark(mc, nv, in->a);                     /* Right shift pulls high bits down. */
				break;
			case IR_DIV:
			case IR_MOD:
				mc_mark(mc, nv, in->a);                     /* Pow2 path uses the sign bit. */
				mc_mark(mc, nv, in->b);
				break;
			case IR_CMP:
				mc_mark(mc, nv, in->a);                     /* 64-bit cmp. */
				mc_mark(mc, nv, in->b);
				break;
			case IR_SEL:
				mc_mark(mc, nv, in->a);
				mc_mark(mc, nv, in->b);
				mc_mark(mc, nv, in->c);
				mc_mark(mc, nv, in->d);
				break;
			case IR_BRCOND:
				mc_mark(mc, nv, in->a);                     /* cmp a, 0 (full reg). */
				break;
			case IR_CAST:
				if (ty_bits(in->to_kind) >= 64 || ty_bits(in->to_kind) == 0)
				{
					mc_mark(mc, nv, in->a);                 /* Widen to long is a full-reg mov. */
				}

				break;
			case IR_MOVE:
			case IR_RET:
				mc_mark(mc, nv, in->a);                     /* Copies/returns the full register. */
				break;
			default:
				break;                                      /* add/sub/mul/and/or/xor/shl/neg/const: low-32 safe. */
			}
		}
	}

	/* Pass B: per-local "stores must be clean". */
	char *lnc = calloc((size_t)(a->nlocal > 0 ? a->nlocal : 1), 1);
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if (in->op == IR_LOAD && in->is_frame && in->dst >= 0 && in->dst < nv && mc[in->dst])
			{
				int k = local_of_disp(a, in->disp);
				if (k >= 0)
				{
					lnc[k] = 1;                             /* High-32-observed load of this local. */
				}
			}
		}
	}

	if (f->is_region)
	{
		for (int k = 0; k < a->nlocal; k++)
		{
			if (ra_local_reg(a, a->local_disp[k]) < 0)
			{
				lnc[k] = 1;                                 /* Memory-resident: no exit re-extension. */
			}
		}
	}

	/* Pass C: a frame-store to a needs-clean local forces its value clean. */
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if (in->op == IR_STORE && in->is_frame)
			{
				int k = local_of_disp(a, in->disp);
				if (k >= 0 && lnc[k])
				{
					mc_mark(mc, nv, in->c);
				}
			}
		}
	}

	free(lnc);
}

/* The narrow-int TypeKind a frame local is accessed as (≤32-bit int), or TY_VOID if it
   is not a narrow int. A register-resident narrow-int local may be dirty at a region
   exit (the 32-bit-native model leaves its intra-region results un-extended), so it
   must be re-normalized to its declared width before the emitter reads its home slot.
   Returns the access kind so unsigned narrow locals zero-extend and signed sign-extend,
   matching the emitter's own load convention. */
static TypeKind local_narrow_int_kind(IRFunc *f, long long disp)
{
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if ((in->op == IR_LOAD || in->op == IR_STORE) && in->is_frame && in->disp == disp)
			{
				if (ty_is_int(in->type) && ty_bits(in->type) <= 32)
				{
					return in->type;
				}

				return TY_VOID;
			}
		}
	}

	return TY_VOID;
}

/* Allocate and fill the per-emission tables shared by the whole-function and
   region paths: block labels, the constant/dead-constant tables, and the
   non-negativity facts. e->cg and e->a must be set. */
static void emit_tables_init(Emit *e, IRFunc *f)
{
	Codegen *cg = e->cg;
	IRAlloc *a = e->a;

	e->noob = 0;
	e->blabel = malloc((size_t)(f->block_count > 0 ? f->block_count : 1) * sizeof(int));
	for (int i = 0; i < f->block_count; i++)
	{
		e->blabel[i] = cg_label(cg);
	}

	/* Constant table: each vreg is defined once; a vreg defined by IR_CONST holds
	   that constant (used to strength-reduce div/mod by a power of two). */
	int nv = f->vreg_count > 0 ? f->vreg_count : 1;
	e->cis = calloc((size_t)nv, 1);
	e->cval = calloc((size_t)nv, sizeof(long long));
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if (in->op == IR_CONST && in->dst >= 0 && in->dst < f->vreg_count)
			{
				e->cis[in->dst] = 1;
				e->cval[in->dst] = in->imm;
			}
		}
	}

	/* Dead constants: a const vreg whose every use folds into an immediate never
	   needs a register, so its IR_CONST is not emitted. A use folds when it is the
	   immediate operand of an arithmetic/compare op, or a power-of-two divisor. */
	e->cdead = calloc((size_t)nv, 1);
	for (int v = 0; v < f->vreg_count; v++)
	{
		e->cdead[v] = e->cis[v];
	}

	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			int op = in->op;
			int comm = (op == IR_ADD || op == IR_MUL || op == IR_AND || op == IR_OR || op == IR_XOR);
			long long t;
			int a_imm = (in->a != IR_NO_REG && in->a < f->vreg_count) && const_imm32(e, in->a, &t);
			int b_imm = (in->b != IR_NO_REG && in->b < f->vreg_count) && const_imm32(e, in->b, &t);

			if (in->a != IR_NO_REG && in->a < f->vreg_count && e->cis[in->a])
			{
				if (!(comm && a_imm && !b_imm))   /* Folds only when swapped to the immediate. */
				{
					e->cdead[in->a] = 0;
				}
			}

			if (in->b != IR_NO_REG && in->b < f->vreg_count && e->cis[in->b])
			{
				int k;
				int folds = ((comm || op == IR_SUB) && b_imm)
							|| ((op == IR_DIV || op == IR_MOD) && pow2_log(e->cval[in->b], &k))
							|| ((op == IR_CMP || op == IR_SEL) && b_imm);
				if (!folds)
				{
					e->cdead[in->b] = 0;
				}
			}

			if (in->c != IR_NO_REG && in->c < f->vreg_count && e->cis[in->c])
			{
				e->cdead[in->c] = 0;   /* A stored value is read, never folded. */
			}

			if (op == IR_SEL && in->d != IR_NO_REG && in->d < f->vreg_count && e->cis[in->d])
			{
				e->cdead[in->d] = 0;   /* The false side is read, never folded. */
			}
		}
	}

	e->nn = calloc((size_t)nv, 1);
	compute_nonneg(f, a, e->nn);

	e->mclean = calloc((size_t)nv, 1);
	compute_must_clean(f, a, e->mclean, nv);
}

static void emit_tables_free(Emit *e)
{
	free(e->blabel);
	free(e->cis);
	free(e->cval);
	free(e->cdead);
	free(e->nn);
	free(e->mclean);
}

/* A block is terminated when its last instruction transfers control explicitly
   (return or branch). A non-terminated block — including an empty one — falls
   through to the next block in CREATION order; the region epilogue relies on
   the final empty block doing exactly this, and empty merge blocks do too. */
static int blk_is_terminated(const IRFunc *f, int b)
{
	const IRBlock *blk = &f->blocks[b];
	if (blk->count == 0)
	{
		return 0;
	}

	IROp op = blk->instrs[blk->count - 1].op;
	return op == IR_RET || op == IR_BR || op == IR_BRCOND;
}

/* The per-block emission loop with the mod/cmp/branch fusion windows. Shared by
   the whole-function and region paths. */
static void emit_blocks(Emit *e, IRFunc *f)
{
	Codegen *cg = e->cg;
	/* A block targeted by a branch from itself or a later block is a loop
	   header. Align those to 32 bytes: a hot loop straddling a 32-byte
	   boundary pays a fetch penalty that dwarfs the one-time nop padding
	   (measured ~20% on the collatz kernel), and placement luck otherwise
	   differs per platform. NASM's align pads code sections with nops, so
	   falling through into the header stays safe. */
	char *lhead = calloc((size_t)(f->block_count > 0 ? f->block_count : 1), 1);
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if (in->op == IR_BR || in->op == IR_BRCOND)
			{
				if (in->blk_true >= 0 && in->blk_true <= b)
				{
					lhead[in->blk_true] = 1;
				}
				if (in->op == IR_BRCOND && in->blk_false >= 0 && in->blk_false <= b)
				{
					lhead[in->blk_false] = 1;
				}
			}
		}
	}
	/* Greedy chain placement over "glue units" to turn taken branches into
	   fall-throughs. A non-terminated block must stay immediately before its
	   creation-order successor (its implicit fall-through), so maximal runs of
	   such blocks form atomic units placed in index order. Units are chained by
	   following each terminating branch's TRUE target: on a loop header that
	   keeps the body adjacent (the false/exit arm stays out of line), and on an
	   in-body if/else it makes the taken arm fall through (e.g. collatz's even
	   step). A region's final block is empty and falls off into the epilogue, so
	   when that last block is non-terminated it is pinned last. */
	int N = f->block_count;
	int *eord = malloc((size_t)(N > 0 ? N : 1) * sizeof(int));
	int nord = 0;
	if (N > 0)
	{
		int *unit_head = malloc((size_t)N * sizeof(int));
		for (int b = 0; b < N; b++)
		{
			unit_head[b] = (b == 0 || blk_is_terminated(f, b - 1)) ? b : unit_head[b - 1];
		}

		char *placed = calloc((size_t)N, 1);
		/* Pin the final fall-off unit last only when the last block has no
		   terminator (region epilogue fall-through); whole-function blocks all
		   end in a terminator, so their last block is placed by the greedy. */
		int pin_last = !blk_is_terminated(f, N - 1);
		int last_head = pin_last ? unit_head[N - 1] : -1;
		for (int seed = 0; seed < N; seed++)
		{
			int h = unit_head[seed];
			if (placed[h] || h == last_head)
			{
				continue;
			}
			int u = h;
			while (u >= 0 && !placed[u] && u != last_head)
			{
				int bb = u, nxt_head = -1;
				for (;;)
				{
					placed[bb] = 1;
					eord[nord++] = bb;
					if (blk_is_terminated(f, bb))
					{
						const IRBlock *tb = &f->blocks[bb];
						const IRInstr *ti = &tb->instrs[tb->count - 1];
						if (ti->op == IR_BRCOND || ti->op == IR_BR)
						{
							nxt_head = unit_head[ti->blk_true];
						}

						break;
					}

					bb++;   /* Non-terminated: glue to the next block (same unit). */
				}
				u = (nxt_head >= 0 && !placed[nxt_head] && nxt_head != last_head) ? nxt_head : -1;
			}
		}

		for (int bb = last_head; bb >= 0 && bb < N && !placed[bb]; bb++)
		{
			placed[bb] = 1;
			eord[nord++] = bb;
			if (blk_is_terminated(f, bb))
			{
				break;
			}
		}

		free(placed);
		free(unit_head);
	}

	for (int oi = 0; oi < nord; oi++)
	{
		int b = eord[oi];
		if (lhead[b])
		{
			cg_emit(cg, "    align 32");
		}
		cg_emit(cg, ".L%d:", e->blabel[b]);
		IRBlock *blk = &f->blocks[b];
		int next = (oi + 1 < nord) ? eord[oi + 1] : -1;
		for (int i = 0; i < blk->count; )
		{
			IRInstr *in = &blk->instrs[i];
			int k;
			long long zero;
			/* Fuse (x % 2^k) == 0 / != 0 feeding a branch into test + jz/jnz. The
			   compare's constant rhs is emitted between the mod and the compare, so
			   skip any constant defs in between (they fold to the immediate 0). */
			int j = i + 1;
			while (j < blk->count && blk->instrs[j].op == IR_CONST)
			{
				j++;
			}

			if (in->op == IR_MOD && in->b != IR_NO_REG && in->b < f->vreg_count
				&& e->cis[in->b] && pow2_log(e->cval[in->b], &k) && k <= 31
				&& j + 1 < blk->count
				&& blk->instrs[j].op == IR_CMP && blk->instrs[j].a == in->dst
				&& (blk->instrs[j].cmp_op == TOKEN_EQ || blk->instrs[j].cmp_op == TOKEN_NEQ)
				&& const_imm32(e, blk->instrs[j].b, &zero) && zero == 0
				&& blk->instrs[j + 1].op == IR_BRCOND && blk->instrs[j + 1].a == blk->instrs[j].dst)
			{
				emit_divisibility_branch(e, in, &blk->instrs[j], &blk->instrs[j + 1], k, next);
				i = j + 2;
			}
			/* Fuse a compare feeding the very next branch into cmp + jcc. */
			else if (in->op == IR_CMP && i + 1 < blk->count
					 && blk->instrs[i + 1].op == IR_BRCOND
					 && blk->instrs[i + 1].a == in->dst)
			{
				emit_fused_branch(e, in, &blk->instrs[i + 1], next);
				i += 2;
			}
			else
			{
				emit_instr(e, in, next);
				i++;
			}
		}
	}
	free(eord);
	free(lhead);
}

void ir_emit_func(Codegen *cg, IRFunc *f, const char *label)
{
	const Func *src = f->src;
	IRAlloc *a = ra_run(f);
	int linux_target = (cg->target == TARGET_LINUX);

	int lbase = src->frame_size;
	if (lbase < 16)
	{
		lbase = 16;
	}

	lbase = (lbase + 7) & ~7;

	Emit e;
	e.cg = cg;
	e.a = a;
	e.spill_base = lbase + 8;
	int cs_base = e.spill_base + a->spill_bytes;

	/* Which callee-saved registers the allocation actually used. */
	e.ncs = 0;
	for (int i = 0; i < RA_NREGS; i++)
	{
		if (a->used_reg[i] && ra_is_callee_saved(i, linux_target))
		{
			e.cs_regs[e.ncs++] = i;
		}
	}

	e.cs_base = cs_base;
	int frame = cs_base + e.ncs * 8;
	frame = (frame + 15) & ~15;

	emit_tables_init(&e, f);

	cg_emit(cg, "section .text");   /* A preceding function's exception record left .data active. */
	cg_emit(cg, "global %s", label);
	cg_emit(cg, "%s:", label);
	cg_emit(cg, "    push rbp");
	cg_emit(cg, "    mov rbp, rsp");
	cg_emit(cg, "    sub rsp, %d", frame);

	/* Save the caller's callee-saved registers we will use. */
	for (int j = 0; j < e.ncs; j++)
	{
		cg_emit(cg, "    mov [rbp - %d], %s", cs_base + j * 8, ra_reg_name(e.cs_regs[j]));
	}

	/* Load parameters in two phases. The allocator may assign a parameter's local
	   to a register that is also a LATER parameter's incoming argument register
	   (e.g. param 0 -> r8, which on Win64 carries param 2); distributing args one at
	   a time would overwrite an argument before it is read. So first dump every
	   incoming argument register to its home stack slot, then load each from its
	   slot into its allocated register (or back to the slot), sign/zero-extending
	   narrow ints so the high bits are well-defined. */
	for (int i = 0; i < src->param_count; i++)
	{
		if (ty_is_float(src->params[i].type.kind))
		{
			cg_emit(cg, "    movsd [rbp - %d], %s", 8 + i * 8, iremit_farg(i));
		}
		else
		{
			cg_emit(cg, "    mov [rbp - %d], %s", 8 + i * 8, iremit_iarg(cg, i));
		}
	}

	for (int i = 0; i < src->param_count; i++)
	{
		int slot = 8 + i * 8;
		TypeKind pk = src->params[i].type.kind;
		if (ty_is_float(pk))
		{
			cg_emit(cg, "    movsd xmm0, [rbp - %d]", slot);
			store_local_from(&e, slot, "xmm0", 1);
			continue;
		}

		cg_emit(cg, "    mov rax, [rbp - %d]", slot);
		if (pk == TY_BOOL)
		{
			cg_emit(cg, "    movzx rax, al");
		}
		else if (ty_is_int(pk) && ty_bits(pk) <= 32)
		{
			norm_rax(cg, pk);
		}

		store_local_from(&e, slot, "rax", ty_is_float(pk));
	}

	emit_blocks(&e, f);
	emit_oob_stubs(&e);   /* Cold bounds-failure stubs; every block ends in ret/jmp, so nothing falls in. */

	/* Emit this function's exception record so a bounds error thrown from it is
	   unwindable: the throw site's PC resolves to this record, which has no try
	   region (eligibility rejects try) and no object locals (guarded in
	   ir_eligible), so the unwinder walks straight through to the caller's handler.
	   cur_try_count is zeroed because the early IR dispatch skips the emitter's
	   per-function reset, and the helper leaves .text active for the next function. */
	cg->cur_try_count = 0;
	cg_emit_exception_record(cg, label, frame, (Func *)src);

	emit_tables_free(&e);
	ra_free(a);
}

/* ---- Element-wise int[] auto-vectorization ----
   A strict analysis recognizes a counted, unit-stride int[] element-wise loop
   whose stored value is an op tree over +,-,* of same-index loads, and records
   that tree; the packed-emission path lowers it to a 4-lane body (movdqu +
   paddd/psubd/pmulld) plus a scalar remainder. Every other loop is left to the
   scalar block emitter unchanged, so non-qualifying code is byte-identical. */

typedef enum { VN_LOAD, VN_BINOP } VecNodeKind;

typedef struct VecNode VecNode;
struct VecNode
{
	VecNodeKind kind;
	int          slot;    /* VN_LOAD: input array frame slot. */
	int          op;      /* VN_BINOP: TokenType (TOKEN_PLUS/MINUS/STAR). */
	VecNode     *l, *r;   /* VN_BINOP operands. */
};

#define VEC_MAX_NODES 32

typedef struct
{
	int         ok;
	int         slot_i;     /* Induction local frame offset. */
	long long   bound;      /* The loop runs i over [0, bound). */
	int         out_slot;   /* Output array frame offset. */
	VecNode     pool[VEC_MAX_NODES];
	int         npool;
	VecNode    *root;       /* The stored value's op tree (into pool). */
	const char *reason;     /* Verdict text, for BZY_SIMD_REPORT triage. */
} VecLoop;

/* arr[i]: an EX_INDEX over an int[] with a BCE-proved induction index. Returns
   the array's frame slot, or -1 if the shape does not match. */
static int vec_idx_slot(const Expr *e, int slot_i)
{
	if (!e || e->kind != EX_INDEX || !e->lhs || !e->rhs)
	{
		return -1;
	}

	if (e->lhs->kind != EX_IDENT || e->rhs->kind != EX_IDENT)
	{
		return -1;
	}

	if (e->rhs->anno_int != slot_i || !e->anno_index_safe || e->type.kind != TY_INT)
	{
		return -1;
	}

	return e->lhs->anno_int;
}

static VecNode *vec_node(VecLoop *v, VecNodeKind k)
{
	if (v->npool >= VEC_MAX_NODES)
	{
		return NULL;
	}

	VecNode *n = &v->pool[v->npool++];
	n->kind = k;
	n->slot = -1;
	n->op = 0;
	n->l = NULL;
	n->r = NULL;
	return n;
}

/* Build the stored value's op tree: an int leaf is a same-index load arr[i]; an
   internal node is +,-,* over int subtrees. Anything else (a scalar leaf, a
   call, a non-int op, a foreign or non-induction index) fails the gate. */
static VecNode *vec_build_tree(VecLoop *v, const Expr *e, int slot_i)
{
	if (!e)
	{
		return NULL;
	}

	int slot = vec_idx_slot(e, slot_i);
	if (slot >= 0)
	{
		VecNode *n = vec_node(v, VN_LOAD);
		if (n)
		{
			n->slot = slot;
		}

		return n;
	}

	if (e->kind == EX_BINARY && e->type.kind == TY_INT
		&& (e->op == TOKEN_PLUS || e->op == TOKEN_MINUS || e->op == TOKEN_STAR))
	{
		VecNode *l = vec_build_tree(v, e->lhs, slot_i);
		VecNode *r = vec_build_tree(v, e->rhs, slot_i);
		if (!l || !r)
		{
			return NULL;
		}

		VecNode *n = vec_node(v, VN_BINOP);
		if (n)
		{
			n->op = e->op;
			n->l = l;
			n->r = r;
		}

		return n;
	}

	return NULL;
}

/* The strict vectorizability gate. Fills `v` and returns v->ok. Pure over the
   AST. The eight conditions:
     1. counted loop: single induction i, init 0, step +1, bound i < C;
     2. unit-stride, induction-only indexing (vec_idx_slot: index vreg == i);
     3. exactly one store, out[i] (the single body statement);
     4. no loop-carried dependence (a unit-stride i never revisits an index, and
        every load is at the current i, so even out[i] = f(out[i], ...) is safe;
        a prior-iteration index like out[i-1] is rejected by (2));
     5. the stored value is an op tree over +,-,* of same-index int loads;
     6. int (4-byte) element kind only (vec_idx_slot / the EX_BINARY type check);
     7. no control flow, call, or side effect in the body (single assignment);
     8. indices already BCE-proved in range (anno_index_safe in vec_idx_slot). */
static int vec_analyze(const Stmt *s, VecLoop *v)
{
	v->ok = 0;
	v->npool = 0;
	v->root = NULL;
	v->reason = "shape";

	if (!s || s->kind != ST_FOR || !s->for_init || !s->cond || !s->for_post)
	{
		return 0;
	}

	/* (1) init: int i = 0  (or  i = 0). */
	int slot_i;
	const Stmt *in = s->for_init;
	if (in->kind == ST_VARDECL && in->decl_init
		&& in->decl_init->kind == EX_INT && in->decl_init->int_val == 0)
	{
		slot_i = in->decl_offset;
	}
	else if (in->kind == ST_ASSIGN && in->target->kind == EX_IDENT
			 && in->value->kind == EX_INT && in->value->int_val == 0)
	{
		slot_i = in->target->anno_int;
	}
	else
	{
		v->reason = "init";
		return 0;
	}

	/* (1) bound: i < C, a constant upper bound. */
	const Expr *c = s->cond;
	if (c->kind != EX_BINARY || c->op != TOKEN_LT
		|| !c->lhs || c->lhs->kind != EX_IDENT || c->lhs->anno_int != slot_i
		|| !c->rhs || c->rhs->kind != EX_INT)
	{
		v->reason = "bound";
		return 0;
	}

	long long bound = c->rhs->int_val;

	/* (1) step: i = i + 1. */
	const Stmt *po = s->for_post;
	if (po->kind != ST_ASSIGN || po->target->kind != EX_IDENT || po->target->anno_int != slot_i
		|| po->value->kind != EX_BINARY || po->value->op != TOKEN_PLUS
		|| po->value->lhs->kind != EX_IDENT || po->value->lhs->anno_int != slot_i
		|| po->value->rhs->kind != EX_INT || po->value->rhs->int_val != 1)
	{
		v->reason = "step";
		return 0;
	}

	/* (3,7) body: exactly one assignment statement, no control flow or calls. */
	if (!s->then_blk || s->then_blk->count != 1)
	{
		v->reason = "body";
		return 0;
	}

	const Stmt *as = s->then_blk->stmts[0];
	if (as->kind != ST_ASSIGN)
	{
		v->reason = "body-stmt";
		return 0;
	}

	/* (2,3,6,8) store target out[i]. */
	int out_slot = vec_idx_slot(as->target, slot_i);
	if (out_slot < 0)
	{
		v->reason = "store";
		return 0;
	}

	/* (5) the stored value's op tree. */
	VecNode *root = vec_build_tree(v, as->value, slot_i);
	if (!root)
	{
		v->reason = "rhs";
		return 0;
	}

	/* Need a few full vector iterations to be worth replacing a loop the emitter
	   already unrolls well at tiny trip counts. */
	if (bound < 8)
	{
		v->reason = "tiny";
		return 0;
	}

	v->slot_i = slot_i;
	v->bound = bound;
	v->out_slot = out_slot;
	v->root = root;
	v->ok = 1;
	v->reason = "ok";
	return 1;
}

/* Packed (4-lane) op for a recorded TokenType. */
static const char *vec_packed_op(int tok)
{
	switch (tok)
	{
	case TOKEN_PLUS:  return "paddd";
	case TOKEN_MINUS: return "psubd";
	case TOKEN_STAR:  return "pmulld";
	default:          return NULL;
	}
}

/* Scalar (one-lane) op for the remainder; multiply is the two-operand imul. */
static const char *vec_scalar_op(int tok)
{
	switch (tok)
	{
	case TOKEN_PLUS:  return "add";
	case TOKEN_MINUS: return "sub";
	case TOKEN_STAR:  return "imul";
	default:          return NULL;
	}
}

/* Flatten a left-leaning op tree whose every right operand is a leaf load into an
   ordered chain: base = slots[0], then result = result <ops[k]> slots[k+1] for k
   in [0, *nops). Returns 1 on success. Such a chain evaluates in two xmm
   registers (an accumulator plus one freshly-loaded operand), which keeps the
   packed body on xmm0/xmm1 - the only registers both caller-saved on the two
   ABIs and outside the xmm2-5 range float promotion uses. A node whose right
   child is not a leaf would need a third register, so it fails here and the loop
   stays scalar (still correct). */
static int vec_chain(const VecNode *n, int *slots, int *ops, int *nops)
{
	if (n->kind == VN_LOAD)
	{
		slots[0] = n->slot;
		*nops = 0;
		return 1;
	}

	if (n->kind == VN_BINOP && n->r->kind == VN_LOAD)
	{
		if (!vec_chain(n->l, slots, ops, nops))
		{
			return 0;
		}

		ops[*nops] = n->op;
		slots[*nops + 1] = n->r->slot;
		(*nops)++;
		return 1;
	}

	return 0;
}

/* If the region is the recognized loop, its op tree flattens to a two-register
   chain, and every base/index local is register-resident, emit the packed body
   (movdqu + paddd/psubd/pmulld per the chain) plus a matching scalar remainder
   and return 1. Otherwise return 0 and let the scalar block emitter run. The
   packed temporaries use xmm0/xmm1, outside the xmm2-5 range float promotion
   claims, so nothing live across the region is clobbered. */
static int ir_try_vectorize_region(Emit *e, const Stmt *region)
{
	VecLoop v;
	int ok = vec_analyze(region, &v);
	if (region && getenv("BZY_SIMD_REPORT"))
	{
		fprintf(stderr, "simd: line %d: %s\n", region->line, ok ? "VECTORIZED" : v.reason);
	}

	if (!ok)
	{
		return 0;
	}

	/* Flatten the op tree into a two-register left-leaning chain. */
	int slots[VEC_MAX_NODES];
	int ops[VEC_MAX_NODES];
	int nops = 0;
	if (!vec_chain(v.root, slots, ops, &nops))
	{
		return 0;
	}

	/* The index, the output base, and every input base must be register-resident:
	   the packed body addresses them directly. */
	int ri = ra_local_reg(e->a, v.slot_i);
	int rc = ra_local_reg(e->a, v.out_slot);
	if (ri < 0 || rc < 0)
	{
		return 0;
	}

	const char *base[VEC_MAX_NODES];
	for (int k = 0; k <= nops; k++)
	{
		int rk = ra_local_reg(e->a, slots[k]);
		if (rk < 0)
		{
			return 0;
		}

		base[k] = ra_reg_name(rk);
	}

	Codegen *cg = e->cg;
	const char *Ri = ra_reg_name(ri);
	const char *Rc = ra_reg_name(rc);
	long long vbound = v.bound & ~3LL;
	int Lvec = cg_label(cg);
	int Lrem = cg_label(cg);
	int Ldone = cg_label(cg);

	cg_emit(cg, "    ; ir-region vectorized: int4 element-wise op chain");
	cg_emit(cg, "    xor %s, %s", Ri, Ri);                          /* i = 0. */
	cg_emit(cg, ".L%d:", Lvec);
	cg_emit(cg, "    cmp %s, %lld", Ri, vbound);
	cg_emit(cg, "    jge .L%d", Lrem);
	cg_emit(cg, "    movdqu xmm0, [%s + %s*4 + 32]", base[0], Ri);
	for (int k = 0; k < nops; k++)
	{
		cg_emit(cg, "    movdqu xmm1, [%s + %s*4 + 32]", base[k + 1], Ri);
		cg_emit(cg, "    %s xmm0, xmm1", vec_packed_op(ops[k]));
	}

	cg_emit(cg, "    movdqu [%s + %s*4 + 32], xmm0", Rc, Ri);
	cg_emit(cg, "    add %s, 4", Ri);
	cg_emit(cg, "    jmp .L%d", Lvec);
	cg_emit(cg, ".L%d:", Lrem);                                     /* Scalar tail [vbound, bound). */
	cg_emit(cg, "    cmp %s, %lld", Ri, v.bound);
	cg_emit(cg, "    jge .L%d", Ldone);
	cg_emit(cg, "    mov eax, dword [%s + %s*4 + 32]", base[0], Ri);
	for (int k = 0; k < nops; k++)
	{
		cg_emit(cg, "    %s eax, dword [%s + %s*4 + 32]", vec_scalar_op(ops[k]), base[k + 1], Ri);
	}

	cg_emit(cg, "    mov dword [%s + %s*4 + 32], eax", Rc, Ri);
	cg_emit(cg, "    add %s, 1", Ri);
	cg_emit(cg, "    jmp .L%d", Lrem);
	cg_emit(cg, ".L%d:", Ldone);
	return 1;
}

void ir_emit_region(Codegen *cg, IRFunc *f, IRAlloc *a, int spill_base, const Stmt *region)
{
	int linux_target = (cg->target == TARGET_LINUX);
	Emit e;
	e.cg = cg;
	e.a = a;
	e.spill_base = spill_base;
	e.cs_base = spill_base + a->spill_bytes;

	/* Which callee-saved registers the allocation actually used. rcx/rdx (the
	   claimable indices past RA_NREGS) are caller-saved on both ABIs, so the
	   base pool covers every save candidate. */
	e.ncs = 0;
	for (int i = 0; i < RA_NREGS; i++)
	{
		if (a->used_reg[i] && ra_is_callee_saved(i, linux_target))
		{
			e.cs_regs[e.ncs++] = i;
		}
	}

	emit_tables_init(&e, f);
	cg_emit(cg, "    ; ir-region begin");

	/* Entry: save the callee-saved registers the allocation uses, then load each
	   register-allocated frame local from its home slot (a local first written
	   inside the region loads garbage here and is then overwritten - harmless). */
	for (int j = 0; j < e.ncs; j++)
	{
		cg_emit(cg, "    mov [rbp - %d], %s", e.cs_base + j * 8, ra_reg_name(e.cs_regs[j]));
	}

	for (int k = 0; k < a->nlocal; k++)
	{
		int r = ra_local_reg(a, a->local_disp[k]);
		if (r >= 0)
		{
			cg_emit(cg, "    mov %s, [rbp - %lld]", ra_reg_name(r), a->local_disp[k]);
		}
	}

	if (!ir_try_vectorize_region(&e, region))
	{
		emit_blocks(&e, f);
	}

	/* Exit (the final lowered block is empty and falls through here): store the
	   register-allocated locals back to their home slots, restore callee-saved. A
	   register-resident narrow-int local may hold a dirty value under the 32-bit-native
	   model (its intra-region stores were not forced clean); re-extend it once here so
	   the emitter that reads its home slot sees a sign-extended int. */
	for (int k = 0; k < a->nlocal; k++)
	{
		int r = ra_local_reg(a, a->local_disp[k]);
		if (r >= 0)
		{
			TypeKind lk = local_narrow_int_kind(f, a->local_disp[k]);
			if (lk != TY_VOID)
			{
				norm_reg(cg, ra_reg_name(r), ra_reg_name(r), lk);
			}

			cg_emit(cg, "    mov [rbp - %lld], %s", a->local_disp[k], ra_reg_name(r));
		}
	}

	for (int j = e.ncs - 1; j >= 0; j--)
	{
		cg_emit(cg, "    mov %s, [rbp - %d]", ra_reg_name(e.cs_regs[j]), e.cs_base + j * 8);
	}

	/* Cold bounds-failure stubs live behind a skip jump: the fall-through path
	   above continues into the enclosing function, so they need a fence. The
	   jump costs one taken branch per region ENTRY, not per iteration. */
	if (e.noob > 0)
	{
		int skip = cg_label(cg);
		cg_emit(cg, "    jmp .L%d", skip);
		emit_oob_stubs(&e);
		cg_emit(cg, ".L%d:", skip);
	}

	cg_emit(cg, "    ; ir-region end");
	emit_tables_free(&e);
}
