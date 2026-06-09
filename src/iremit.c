#include "iremit.h"
#include "lexer.h"   /* Comparison TokenTypes. */
#include <stdlib.h>

/* The naive backend: every virtual register is spilled to its own 8-byte frame
   slot. Each instruction loads its operands into rax/rcx, computes, and stores
   the result back to the destination slot. No allocation, no folding - this
   exists only to prove the IR lowers and runs identically to the emitter. The
   linear-scan allocator that makes it fast is a later plan.

   Frame layout (downward from rbp):
     [rbp - 8 ..]        function locals and params, at their resolver offsets
     [rbp - vbase ..]    one slot per virtual register
   The IR function is call-free, so only caller-saved rax/rcx/rdx are used and no
   callee-saved register, scratch arena, or exception record is needed. */

typedef struct
{
	Codegen *cg;
	int      vbase;        /* rbp offset of vreg 0; vreg v is at [rbp - (vbase + v*8)]. */
	int     *blabel;       /* Per-block unique label id (from cg_label). */
} Emit;

/* The i-th integer argument register for the current ABI. v1 caps params at 4,
   and accepts only integer params, so position == integer index on both ABIs. */
static const char *iremit_iarg(Codegen *cg, int i)
{
	static const char *win[4]  = { "rcx", "rdx", "r8", "r9" };
	static const char *sysv[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
	return (cg->target == TARGET_LINUX) ? sysv[i] : win[i];
}

static int vslot(Emit *e, IRReg v)
{
	return e->vbase + v * 8;
}

static void load_op(Emit *e, IRReg v, const char *reg)
{
	cg_emit(e->cg, "    mov %s, [rbp - %d]", reg, vslot(e, v));
}

static void store_dst(Emit *e, IRReg v)
{
	cg_emit(e->cg, "    mov [rbp - %d], rax", vslot(e, v));
}

/* Normalize the value in rax to width `to`, as a fresh load would (sign/zero
   extend). 64-bit is already full width. */
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

static void emit_instr(Emit *e, const IRInstr *in)
{
	Codegen *cg = e->cg;
	switch (in->op)
	{
	case IR_CONST:
		cg_emit(cg, "    mov rax, %lld", in->imm);
		store_dst(e, in->dst);
		break;
	case IR_MOVE:
		load_op(e, in->a, "rax");
		store_dst(e, in->dst);
		break;
	case IR_LOAD:
		/* v1 only sees frame-local reads (arrays/base+index are a later plan). */
		cg_emit(cg, "    mov rax, [rbp - %lld]", in->disp);
		store_dst(e, in->dst);
		break;
	case IR_STORE:
		load_op(e, in->c, "rax");
		cg_emit(cg, "    mov [rbp - %lld], rax", in->disp);
		break;
	case IR_ADD:
	case IR_SUB:
	case IR_MUL:
	case IR_AND:
	case IR_OR:
	case IR_XOR:
		load_op(e, in->a, "rax");
		load_op(e, in->b, "rcx");
		switch (in->op)
		{
		case IR_ADD: cg_emit(cg, "    add rax, rcx");  break;
		case IR_SUB: cg_emit(cg, "    sub rax, rcx");  break;
		case IR_MUL: cg_emit(cg, "    imul rax, rcx"); break;
		case IR_AND: cg_emit(cg, "    and rax, rcx");  break;
		case IR_OR:  cg_emit(cg, "    or rax, rcx");   break;
		case IR_XOR: cg_emit(cg, "    xor rax, rcx");  break;
		default: break;
		}

		store_dst(e, in->dst);
		break;
	case IR_DIV:
	case IR_MOD:
		load_op(e, in->a, "rax");
		load_op(e, in->b, "rcx");
		if (ty_is_unsigned(in->type))
		{
			cg_emit(cg, "    xor edx, edx");
			cg_emit(cg, "    div rcx");
		}
		else
		{
			cg_emit(cg, "    cqo");
			cg_emit(cg, "    idiv rcx");
		}

		if (in->op == IR_MOD)
		{
			cg_emit(cg, "    mov rax, rdx");
		}

		store_dst(e, in->dst);
		break;
	case IR_SHL:
	case IR_SHR:
		load_op(e, in->a, "rax");
		load_op(e, in->b, "rcx");
		if (in->op == IR_SHL)
		{
			cg_emit(cg, "    shl rax, cl");
		}
		else
		{
			cg_emit(cg, ty_is_unsigned(in->type) ? "    shr rax, cl" : "    sar rax, cl");
		}

		store_dst(e, in->dst);
		break;
	case IR_NEG:
		load_op(e, in->a, "rax");
		cg_emit(cg, "    neg rax");
		store_dst(e, in->dst);
		break;
	case IR_CAST:
		load_op(e, in->a, "rax");
		norm_rax(cg, in->to_kind);
		store_dst(e, in->dst);
		break;
	case IR_CMP:
		load_op(e, in->a, "rax");
		load_op(e, in->b, "rcx");
		cg_emit(cg, "    cmp rax, rcx");
		cg_emit(cg, "    %s al", setcc_op(in->cmp_op, ty_is_unsigned(in->type)));
		cg_emit(cg, "    movzx rax, al");
		store_dst(e, in->dst);
		break;
	case IR_BR:
		cg_emit(cg, "    jmp .L%d", e->blabel[in->blk_true]);
		break;
	case IR_BRCOND:
		load_op(e, in->a, "rax");
		cg_emit(cg, "    cmp rax, 0");
		cg_emit(cg, "    jne .L%d", e->blabel[in->blk_true]);
		cg_emit(cg, "    jmp .L%d", e->blabel[in->blk_false]);
		break;
	case IR_RET:
		if (in->a != IR_NO_REG)
		{
			load_op(e, in->a, "rax");
		}

		cg_emit(cg, "    mov rsp, rbp");
		cg_emit(cg, "    pop rbp");
		cg_emit(cg, "    ret");
		break;
	default:
		break;
	}
}

void ir_emit_func(Codegen *cg, IRFunc *f, const char *label)
{
	const Func *src = f->src;

	/* Locals/params occupy [rbp - 8 .. rbp - frame_size] at their resolver
	   offsets. Virtual-register slots sit just below, 8 bytes each. */
	int lbase = src->frame_size;
	if (lbase < 16)
	{
		lbase = 16;
	}

	lbase = (lbase + 7) & ~7;
	int vbase = lbase + 8;
	int frame = vbase + f->vreg_count * 8;
	frame = (frame + 15) & ~15;   /* Keep rsp 16-aligned after the prologue. */

	Emit e;
	e.cg = cg;
	e.vbase = vbase;
	e.blabel = malloc((size_t)(f->block_count > 0 ? f->block_count : 1) * sizeof(int));
	for (int i = 0; i < f->block_count; i++)
	{
		e.blabel[i] = cg_label(cg);
	}

	cg_emit(cg, "global %s", label);
	cg_emit(cg, "%s:", label);
	cg_emit(cg, "    push rbp");
	cg_emit(cg, "    mov rbp, rsp");
	cg_emit(cg, "    sub rsp, %d", frame);

	/* Spill integer args to their slots (slot 8 + i*8 for a free function),
	   sign/zero-extending narrow ints so the high bits are well-defined. */
	for (int i = 0; i < src->param_count; i++)
	{
		int slot = 8 + i * 8;
		const char *ar = iremit_iarg(cg, i);
		TypeKind pk = src->params[i].type.kind;
		if (pk == TY_BOOL)
		{
			cg_emit(cg, "    mov rax, %s", ar);
			cg_emit(cg, "    movzx rax, al");
			cg_emit(cg, "    mov [rbp - %d], rax", slot);
		}
		else if (ty_is_int(pk) && ty_bits(pk) <= 32)
		{
			cg_emit(cg, "    mov rax, %s", ar);
			norm_rax(cg, pk);
			cg_emit(cg, "    mov [rbp - %d], rax", slot);
		}
		else
		{
			cg_emit(cg, "    mov [rbp - %d], %s", slot, ar);
		}
	}

	for (int b = 0; b < f->block_count; b++)
	{
		cg_emit(cg, ".L%d:", e.blabel[b]);
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			emit_instr(&e, &blk->instrs[i]);
		}
	}

	free(e.blabel);
}
