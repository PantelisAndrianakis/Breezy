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

	cg_emit(e->cg, "    mov %s, [rbp - %d]", scratch, e->spill_base + ra_vreg_slot(e->a, v) * 8);
	return scratch;
}

/* The register an op should write its result into: dst's physical register, or
   rax when dst is spilled (finish_dst then stores it). */
static const char *dst_reg(Emit *e, IRReg v)
{
	int r = ra_vreg_reg(e->a, v);
	return (r >= 0) ? ra_reg_name(r) : "rax";
}

/* After computing into dst_reg(v): store rax to the spill slot if v is spilled. */
static void finish_dst(Emit *e, IRReg v)
{
	if (ra_vreg_reg(e->a, v) < 0)
	{
		cg_emit(e->cg, "    mov [rbp - %d], rax", e->spill_base + ra_vreg_slot(e->a, v) * 8);
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

/* Move the value in `src` into frame local `disp` (its register, or home slot). */
static void store_local_from(Emit *e, long long disp, const char *src)
{
	int r = ra_local_reg(e->a, disp);
	if (r >= 0)
	{
		if (strcmp(ra_reg_name(r), src))   /* Coalesced: source already in the local's register. */
		{
			cg_emit(e->cg, "    mov %s, %s", ra_reg_name(r), src);
		}
	}
	else
	{
		cg_emit(e->cg, "    mov [rbp - %lld], %s", disp, src);
	}
}

/* dst = a / 2^k or a % 2^k via shifts/masks, signed-correct (matches idiv's
   truncate-toward-zero), avoiding the ~20-40 cycle idiv. Stages through rax/rdx. */
static void emit_divmod_pow2(Emit *e, const IRInstr *in, int k)
{
	Codegen *cg = e->cg;
	const char *Ra = vreg_in(e, in->a, "rax");
	if (strcmp(Ra, "rax"))
	{
		cg_emit(cg, "    mov rax, %s", Ra);
	}

	int uns = ty_is_unsigned(in->type);
	int nonneg = (in->a >= 0 && in->a < e->a->vreg_count && e->nn[in->a]);
	long long mask = (1LL << k) - 1;
	if (in->op == IR_DIV)
	{
		if (uns)
		{
			cg_emit(cg, "    shr rax, %d", k);
		}
		else if (nonneg)
		{
			cg_emit(cg, "    sar rax, %d", k);   /* Dividend proven >= 0: no sign bias. */
		}
		else
		{
			cg_emit(cg, "    mov rdx, rax");
			cg_emit(cg, "    sar rdx, 63");
			cg_emit(cg, "    shr rdx, %d", 64 - k);
			cg_emit(cg, "    add rax, rdx");
			cg_emit(cg, "    sar rax, %d", k);
		}
	}
	else
	{
		if (uns || nonneg)
		{
			cg_emit(cg, "    and rax, %lld", mask);   /* Non-negative: remainder is the low bits. */
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
			cg_emit(cg, "    imul %s, %s, %lld", Rd, Ra, imm);
		}
		else
		{
			if (strcmp(Rd, Ra))
			{
				cg_emit(cg, "    mov %s, %s", Rd, Ra);
			}

			cg_emit(cg, "    %s %s, %lld", opc, Rd, imm);
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

		cg_emit(cg, "    mov %s, %lld", dst_reg(e, in->dst), in->imm);
		finish_dst(e, in->dst);
		break;
	case IR_MOVE:
	{
		const char *Ra = vreg_in(e, in->a, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_LOAD:
	{
		const char *Rl = local_in(e, in->disp, buf);
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Rl))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Rl);
		}

		finish_dst(e, in->dst);
		break;
	}
	case IR_STORE:
	{
		const char *Rc = vreg_in(e, in->c, "rax");
		store_local_from(e, in->disp, Rc);
		break;
	}
	case IR_ADD: emit_bin(e, in, "add", 1);  break;
	case IR_SUB: emit_bin(e, in, "sub", 0);  break;
	case IR_MUL: emit_bin(e, in, "imul", 1); break;
	case IR_AND: emit_bin(e, in, "and", 1);  break;
	case IR_OR:  emit_bin(e, in, "or", 1);   break;
	case IR_XOR: emit_bin(e, in, "xor", 1);  break;
	case IR_DIV:
	case IR_MOD:
	{
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

		const char *Rb = vreg_in(e, in->b, "rcx");   /* Divisor (never rax/rdx). */
		if (ty_is_unsigned(in->type))
		{
			cg_emit(cg, "    xor edx, edx");
			cg_emit(cg, "    div %s", Rb);
		}
		else
		{
			cg_emit(cg, "    cqo");
			cg_emit(cg, "    idiv %s", Rb);
		}

		const char *Rd = dst_reg(e, in->dst);
		if (in->op == IR_MOD)
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
		const char *Ra = vreg_in(e, in->a, "rax");
		const char *Rd = dst_reg(e, in->dst);
		if (strcmp(Rd, Ra))
		{
			cg_emit(cg, "    mov %s, %s", Rd, Ra);
		}

		const char *Rb = vreg_in(e, in->b, "rcx");
		if (strcmp(Rb, "rcx"))
		{
			cg_emit(cg, "    mov rcx, %s", Rb);   /* Shift count in cl. */
		}

		if (in->op == IR_SHL)
		{
			cg_emit(cg, "    shl %s, cl", Rd);
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
			if (strcmp(Ra, "rax"))
			{
				cg_emit(cg, "    mov rax, %s", Ra);
			}

			norm_rax(cg, in->to_kind);
			if (strcmp(Rd, "rax"))
			{
				cg_emit(cg, "    mov %s, rax", Rd);
			}
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
			const char *Ra = vreg_in(e, in->a, "rax");
			if (strcmp(Ra, "rax"))
			{
				cg_emit(cg, "    mov rax, %s", Ra);
			}
		}

		emit_epilogue(e);
		break;
	default:
		break;
	}
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

	e.blabel = malloc((size_t)(f->block_count > 0 ? f->block_count : 1) * sizeof(int));
	for (int i = 0; i < f->block_count; i++)
	{
		e.blabel[i] = cg_label(cg);
	}

	/* Constant table: each vreg is defined once; a vreg defined by IR_CONST holds
	   that constant (used to strength-reduce div/mod by a power of two). */
	int nv = f->vreg_count > 0 ? f->vreg_count : 1;
	e.cis = calloc((size_t)nv, 1);
	e.cval = calloc((size_t)nv, sizeof(long long));
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if (in->op == IR_CONST && in->dst >= 0 && in->dst < f->vreg_count)
			{
				e.cis[in->dst] = 1;
				e.cval[in->dst] = in->imm;
			}
		}
	}

	/* Dead constants: a const vreg whose every use folds into an immediate never
	   needs a register, so its IR_CONST is not emitted. A use folds when it is the
	   immediate operand of an arithmetic/compare op, or a power-of-two divisor. */
	e.cdead = calloc((size_t)nv, 1);
	for (int v = 0; v < f->vreg_count; v++)
	{
		e.cdead[v] = e.cis[v];
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
			int a_imm = (in->a != IR_NO_REG && in->a < f->vreg_count) && const_imm32(&e, in->a, &t);
			int b_imm = (in->b != IR_NO_REG && in->b < f->vreg_count) && const_imm32(&e, in->b, &t);

			if (in->a != IR_NO_REG && in->a < f->vreg_count && e.cis[in->a])
			{
				if (!(comm && a_imm && !b_imm))   /* Folds only when swapped to the immediate. */
				{
					e.cdead[in->a] = 0;
				}
			}

			if (in->b != IR_NO_REG && in->b < f->vreg_count && e.cis[in->b])
			{
				int k;
				int folds = ((comm || op == IR_SUB) && b_imm)
							|| ((op == IR_DIV || op == IR_MOD) && pow2_log(e.cval[in->b], &k))
							|| (op == IR_CMP && b_imm);
				if (!folds)
				{
					e.cdead[in->b] = 0;
				}
			}

			if (in->c != IR_NO_REG && in->c < f->vreg_count && e.cis[in->c])
			{
				e.cdead[in->c] = 0;   /* A stored value is read, never folded. */
			}
		}
	}

	e.nn = calloc((size_t)nv, 1);
	compute_nonneg(f, a, e.nn);

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

	/* Spill integer args into their local's register (or home slot), sign/zero-
	   extending narrow ints so the high bits are well-defined. */
	for (int i = 0; i < src->param_count; i++)
	{
		int slot = 8 + i * 8;
		const char *ar = iremit_iarg(cg, i);
		TypeKind pk = src->params[i].type.kind;
		cg_emit(cg, "    mov rax, %s", ar);
		if (pk == TY_BOOL)
		{
			cg_emit(cg, "    movzx rax, al");
		}
		else if (ty_is_int(pk) && ty_bits(pk) <= 32)
		{
			norm_rax(cg, pk);
		}

		store_local_from(&e, slot, "rax");
	}

	for (int b = 0; b < f->block_count; b++)
	{
		cg_emit(cg, ".L%d:", e.blabel[b]);
		IRBlock *blk = &f->blocks[b];
		int next = (b + 1 < f->block_count) ? b + 1 : -1;
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
				&& e.cis[in->b] && pow2_log(e.cval[in->b], &k) && k <= 31
				&& j + 1 < blk->count
				&& blk->instrs[j].op == IR_CMP && blk->instrs[j].a == in->dst
				&& (blk->instrs[j].cmp_op == TOKEN_EQ || blk->instrs[j].cmp_op == TOKEN_NEQ)
				&& const_imm32(&e, blk->instrs[j].b, &zero) && zero == 0
				&& blk->instrs[j + 1].op == IR_BRCOND && blk->instrs[j + 1].a == blk->instrs[j].dst)
			{
				emit_divisibility_branch(&e, in, &blk->instrs[j], &blk->instrs[j + 1], k, next);
				i = j + 2;
			}
			/* Fuse a compare feeding the very next branch into cmp + jcc. */
			else if (in->op == IR_CMP && i + 1 < blk->count
					 && blk->instrs[i + 1].op == IR_BRCOND
					 && blk->instrs[i + 1].a == in->dst)
			{
				emit_fused_branch(&e, in, &blk->instrs[i + 1], next);
				i += 2;
			}
			else
			{
				emit_instr(&e, in, next);
				i++;
			}
		}
	}

	free(e.blabel);
	free(e.cis);
	free(e.cval);
	free(e.cdead);
	free(e.nn);
	ra_free(a);
}
