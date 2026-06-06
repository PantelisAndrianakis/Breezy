#include "codegen.h"
#include "symtable.h"
#include "lexer.h"
#include "enums.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void cg_init(Codegen *cg, FILE *out)
{
	cg->out=out;
	cg->label_count=0;
	cg->fpk_count=0;
	cg->strk_count=0;
	cg->cur_break_label=-1;
	cg->cur_continue_label=-1;
	cg->exception_fn_count=0;
	cg->exception_try_count=0;
	cg->breeze_thunk_count=0;
	cg->blocking_thunk_count=0;
	cg->target=TARGET_WINDOWS;   /* Driver overrides via --target. */
}

void cg_emit(Codegen *cg, const char *fmt, ...)
{
	va_list ap;
	va_start(ap,fmt);
	vfprintf(cg->out,fmt,ap);
	va_end(ap);
	fputc('\n',cg->out);
}

int  cg_label(Codegen *cg)
{
	return cg->label_count++;
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
	int id = cg->strk_count++;
	int n = 0;
	while (e->str_val[n] && n < 255)
	{
		cg->strk[id].bytes[n] = e->str_val[n];
		n++;
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

/* Leave the address of element a[i] in rbx, bounds-checked. Evaluates the array
   (lhs) then the index (rhs); clobbers rax/rcx/rdx. An out-of-range index calls
   bzy_oob (no return). xmm0 is untouched on the in-range path, so a float/double
   value being stored survives address computation. */
static void cg_index_addr(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);                 /* Base -> rax. */
	cg_temp_push(cg);
	cg_expr(cg,tt,e->rhs);                 /* Index -> rax. */
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_temp_pop(cg);             /* base */
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
	cg_emit(cg,"    lea rbx, [rax + %s*8 + 32]", cg_iarg(cg, 0));   /* Index reg matches the OOB-check load above. */
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

/* Release the object pointer currently in rcx; rax is clobbered. */
static void cg_release_rcx(Codegen *cg)
{
	cg_aligned_call(cg,"bzy_release");
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

	/* A managed EX_BINARY is a string concat (bzy_str_concat returns +1); an
	   EX_STR literal is +1 from bzy_str_new. */
	return e->kind==EX_NEW || e->kind==EX_CALL || e->kind==EX_METHOD_CALL
		   || e->kind==EX_STR || e->kind==EX_BINARY || e->kind==EX_NEWARRAY
		   || e->kind==EX_NEWMAP || e->kind==EX_NEWGEN || e->kind==EX_NEWCHANNEL;
}

/* Retain the object pointer currently in rax; rax is preserved. */
static void cg_retain_rax(Codegen *cg)
{
	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
	cg_emit(cg,"    mov %s, rax", cg_iarg(cg, 0));
	cg_aligned_call(cg,"bzy_retain");
	cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
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

/* Floating-point binary op. Operands evaluate to xmm0; the left operand is
   spilled on the machine stack so nested FP expressions compose correctly. Both
   operands promote to the common type: double if either side is double,
   otherwise float (integers convert in with cvtsi2ss/sd). */
static void cg_binary_fp(Codegen *cg, TypeTable *tt, Expr *e)
{
	TypeKind ct = (e->lhs->type.kind==TY_DOUBLE || e->rhs->type.kind==TY_DOUBLE) ? TY_DOUBLE : TY_FLOAT;
	const char *sfx = (ct==TY_DOUBLE) ? "sd" : "ss";
	cg_expr(cg,tt,e->lhs);
	cg_fp_promote(cg, e->lhs->type.kind, ct);
	int b = cg_scratch_alloc(cg, 8);
	cg_emit(cg,"    movsd qword [rbp - %d], xmm0", b);   /* Spill lhs (float lives in the low 4 bytes). */
	cg_expr(cg,tt,e->rhs);
	cg_fp_promote(cg, e->rhs->type.kind, ct);
	cg_emit(cg,"    movsd xmm1, xmm0");          /* rhs -> xmm1. */
	cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b);   /* lhs -> xmm0. */
	cg_scratch_free(cg, 8);
	switch (e->op)
	{
	case TOKEN_PLUS:
		cg_emit(cg,"    add%s xmm0, xmm1", sfx);
		break;
	case TOKEN_MINUS:
		cg_emit(cg,"    sub%s xmm0, xmm1", sfx);
		break;
	case TOKEN_STAR:
		cg_emit(cg,"    mul%s xmm0, xmm1", sfx);
		break;
	case TOKEN_SLASH:
		cg_emit(cg,"    div%s xmm0, xmm1", sfx);
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

		cg_emit(cg,"    ucomi%s xmm0, xmm1", sfx);
		cg_emit(cg,"    %s al", set);
		cg_emit(cg,"    movzx rax, al");
		break;
	}
	}
}

static void cg_binary(Codegen *cg, TypeTable *tt, Expr *e)
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

	cg_expr(cg,tt,e->lhs);                  /* lhs -> rax. */

	/* The rhs operand for add/sub/cmp. Default is the rbx register; a small enough
	   integer literal is folded straight into the instruction (rhsop = the literal)
	   so no rbx load is emitted at all. imul/idiv have no usable immediate form here
	   and always read rbx, which the fuse condition below excludes. */
	const char *rhsop = "rbx";
	char immbuf[24];
	int fuse = (e->rhs->kind==EX_INT || e->rhs->kind==EX_BOOL)
			   && e->rhs->int_val >= -2147483648LL && e->rhs->int_val <= 2147483647LL
			   && (e->op==TOKEN_PLUS || e->op==TOKEN_MINUS || e->op==TOKEN_EQ
				   || e->op==TOKEN_NEQ || e->op==TOKEN_LT || e->op==TOKEN_GT
				   || e->op==TOKEN_LTE || e->op==TOKEN_GTE);
	if (fuse)
	{
		snprintf(immbuf,sizeof(immbuf),"%lld", e->rhs->int_val);
		rhsop = immbuf;
	}
	/* Otherwise load a side-effect-free leaf rhs (literal, null, or a local read)
	   straight into rbx, skipping the push/pop the general path needs to preserve
	   lhs across an rhs evaluation that would clobber rax. lhs is already in rax and
	   none of these loads disturb it, so evaluation order (lhs then rhs) holds. */
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
		char mem[32];
		sprintf(mem,"[rbp - %d]", e->rhs->anno_int);
		cg_load_scalar_into(cg,e->rhs->type.kind,mem,"rbx","ebx");
	}
	else
	{
		cg_temp_push(cg);            /* Preserve lhs in a frame slot across the rhs evaluation. */
		cg_expr(cg,tt,e->rhs);
		cg_emit(cg,"    mov rbx, rax");
		cg_temp_pop(cg);
	}

	/* Unsigned if either operand is unsigned: division then uses the unsigned
	   form (C-style "unsigned wins"), and comparisons — where the resolver still
	   requires matching signedness — see the operands' shared signedness. */
	int uns = ty_is_unsigned(e->lhs->type.kind) || ty_is_unsigned(e->rhs->type.kind);
	switch (e->op)
	{
	case TOKEN_PLUS:
		cg_emit(cg,"    add rax, %s", rhsop);
		cg_extend_reg(cg,e->type.kind);
		break;
	case TOKEN_MINUS:
		cg_emit(cg,"    sub rax, %s", rhsop);
		cg_extend_reg(cg,e->type.kind);
		break;
	case TOKEN_STAR:
		cg_emit(cg,"    imul rax, rbx");   /* Low bits agree with mul at any width. */
		cg_extend_reg(cg,e->type.kind);
		break;
	case TOKEN_SLASH:
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

		cg_extend_reg(cg,e->type.kind);
		break;
	case TOKEN_PERCENT:
		/* Same divide as '/', but the result is the remainder (rdx), not the
		   quotient (rax). div/idiv leave the remainder in rdx; move it back. */
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
		cg_extend_reg(cg,e->type.kind);
		break;
	case TOKEN_SHL:
		/* Shift count must be in cl. lhs in rax, rhs (count) in rbx. */
		cg_emit(cg,"    mov rcx, rbx");
		cg_emit(cg,"    shl rax, cl");
		cg_extend_reg(cg,e->type.kind);
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

		cg_extend_reg(cg,e->type.kind);
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

		cg_emit(cg,"    cmp rax, %s", rhsop);
		cg_emit(cg,"    %s al", set);
		cg_emit(cg,"    movzx rax, al");
		break;
	}
	default:
		fprintf(stderr,"Codegen: bad binary op\n");
		exit(1);
	}
}

/* Emit a Win64 call. Each positional argument is materialized into rcx/rdx/r8/r9
   (integer/object) or xmm0..3 (float/double) by index — arg i uses register slot
   i regardless of class. Arguments are evaluated left-to-right into a 16-aligned
   stack block (so nested calls compose), then loaded into their registers. The
   integer-or-float routing follows the *parameter* type, so an int passed to a
   double parameter is promoted with cvtsi2sd. */
static void cg_call_with_args(Codegen *cg, TypeTable *tt, const char *target,
							  Expr *self, Expr **args, int argc, int indirect,
							  int result_is_object, int result_is_fp,
							  const TypeRef *params, int param_count, int marshal_cstr)
{
	int total = (self?1:0) + argc;
	if (total > 4)
	{
		fprintf(stderr,"Codegen: >4 args unsupported\n");
		exit(1);
	}

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
		if (total==1 && only && !expr_is_owned(only))
		{
			TypeKind pk = self ? TY_OBJECT : ((param_count>0) ? params[0].kind : args[0]->type.kind);
			int is_cstr = marshal_cstr && !self && args[0]->type.kind==TY_STRING;
			if (!ty_is_float(pk) && !is_cstr)
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

	TypeKind slot_kind[4];
	int owned_tmp[4];
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

	int int_idx = 0, fp_idx = 0;
	for (int s=0; s<total; s++)
	{
		/* Win64 numbers both classes by argument position (xi==ii==s); System V
		   numbers integer and FP registers independently. */
		if (slot_kind[s]==TY_FLOAT)
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : s;
			cg_emit(cg,"    movss xmm%d, dword [rbp - %d]", xi, b - s*8);
		}
		else if (slot_kind[s]==TY_DOUBLE)
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : s;
			cg_emit(cg,"    movsd xmm%d, qword [rbp - %d]", xi, b - s*8);
		}
		else
		{
			int ii = (cg->target==TARGET_LINUX) ? int_idx++ : s;
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, ii), b - s*8);
			if (marshal_cstr && slot_kind[s]==TY_STRING)
			{
				cg_emit(cg,"    add %s, 32", cg_iarg(cg, ii));   /* string object -> char* data (NUL-terminated). */
			}
		}
	}

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

/* Lower a call to an `extern blocking` function: build a ctx blob on the stack,
   marshal the args into it, hand it to the offload pool (the breeze parks while a
   worker runs the C call), then read the result back. Args are stored raw (the
   string->char* marshalling happens worker-side in the thunk), so the owned-temp
   release frees the original object. Up to 4 args. */
static void cg_blocking_call(Codegen *cg, TypeTable *tt, Expr *e, FuncInfo *fi)
{
	if (e->arg_count > 4)
	{
		fprintf(stderr,"Codegen: >4 args unsupported\n");
		exit(1);
	}

	int b = cg_scratch_alloc(cg, 48);              /* ctx blob: 4 arg slots + result, 16-aligned. */
	int owned_tmp[4];
	int owned_n = 0;
	for (int i=0; i<e->arg_count; i++)
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
	cg_aligned_call(cg,"bzy_offload_run");         /* Parks the breeze; worker fills ctx[32]. */

	for (int i=0; i<owned_n; i++)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - owned_tmp[i]*8);
		cg_release_rcx(cg);
	}

	if (fi->ret_type.kind==TY_DOUBLE)
	{
		cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", b - 32);
	}
	else if (fi->ret_type.kind==TY_FLOAT)
	{
		cg_emit(cg,"    movss xmm0, dword [rbp - %d]", b - 32);
	}
	else if (fi->ret_type.kind!=TY_VOID)
	{
		cg_emit(cg,"    mov rax, [rbp - %d]", b - 32);
	}

	cg_scratch_free(cg, 48);
}

static void cg_method_call(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);                       /* Receiver pointer in rax. */
	cg_emit(cg,"    mov rax, [rax]");             /* Vtable pointer. */
	cg_emit(cg,"    mov rax, [rax + %d]", e->anno_int * 8);   /* Method at its (virtual or interface) slot. */
	const TypeRef *params;
	int pcount;
	ClassInfo *c=types_find_class(tt,e->anno_str);
	if (c)
	{
		MethodInfo *m=types_find_method(c,e->name);
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
		const char *fn = strcmp(e->name,"getKeys")==0 ? "bzy_map_keys"
						 : strcmp(e->name,"getValues")==0 ? "bzy_map_values"
						 : "bzy_map_entries";
		cg_aligned_call(cg,fn);
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
			cg_emit(cg,"    mov [rbx + 32], rax");    /* Width-extended 8-byte slot. */
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
		cg_emit(cg,"    mov rax, [rax + 32]"); /* Slot value. */
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
			   (its 8-byte slots at +32); phys = (head+i) & (cap-1). */
			int ok = cg_label(cg);
			cg_emit(cg,"    mov rcx, rax");               /* index */
			cg_emit(cg,"    mov rdx, [rbp - %d]", b);     /* receiver */
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
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b - 8);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 2), b - 16);
		cg_aligned_call(cg,"bzy_vec_set");
		if (!fp && ty_is_managed(tk) && expr_is_owned(e->args[1]))
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 16);
			cg_release_rcx(cg);
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
	cg_aligned_call(cg,fn);
	if (!fp && ty_is_managed(tk) && expr_is_owned(e->args[0]))
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - 8);
		cg_release_rcx(cg);
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
	if (total > 4)
	{
		fprintf(stderr,"Codegen: constructor with more than 3 arguments unsupported\n");
		exit(1);
	}

	int block = ((total*8 + 15)/16)*16;
	int b = cg_scratch_alloc(cg, block);
	cg_emit(cg,"    mov [rbp - %d], rax", b);     /* this (borrowed). */

	TypeKind slot_kind[4];
	int owned_tmp[4];
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

	int int_idx = 0, fp_idx = 0;
	for (int s=0; s<total; s++)
	{
		if (slot_kind[s]==TY_FLOAT)
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : s;
			cg_emit(cg,"    movss xmm%d, dword [rbp - %d]", xi, b - s*8);
		}
		else if (slot_kind[s]==TY_DOUBLE)
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : s;
			cg_emit(cg,"    movsd xmm%d, qword [rbp - %d]", xi, b - s*8);
		}
		else
		{
			int ii = (cg->target==TARGET_LINUX) ? int_idx++ : s;
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, ii), b - s*8);
		}
	}

	cg_emit(cg,"    call %s", label);

	for (int i=0; i<owned_n; i++)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), b - owned_tmp[i]*8);
		cg_release_rcx(cg);
	}

	cg_emit(cg,"    mov rax, [rbp - %d]", b);     /* The object is the result of `new`. */
	cg_scratch_free(cg, block);
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
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), c->object_size);
		cg_emit(cg,"    call bzy_alloc");
		cg_emit(cg,"    lea rbx, [rel __vtable_%s]", c->name);
		cg_emit(cg,"    mov [rax], rbx");
		/* The refcount and fields are zeroed by bzy_alloc, so rax holds an owned reference. */
		if (c->is_shared)
		{
			/* Channel-reachable class: mark gcinfo so retain/release go atomic.
			   8 = BZY_GCINFO_SHARED (bit 3) in runtime/breezy.h. */
			cg_emit(cg,"    or qword [rax + 16], 8");
		}
	}

	if (c->has_ctor)
	{
		cg_ctor_call(cg,tt,c->ctor_asm_label,e->args,e->arg_count,c->ctor_param_types,c->ctor_param_count);
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
			cg_emit(cg,"    movsd xmm1, xmm0");
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

	if (strcmp(m,"cos")==0 || strcmp(m,"tan")==0 || strcmp(m,"exp")==0)
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
		cg_emit(cg,"    movsd xmm1, xmm0");
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
	else
	{
		fn = "bzy_filechannel_close";
		fallible = 0;
	}

	TypeRef ps[2];
	for (int i=0; i<e->arg_count; i++)
	{
		ps[i]=e->args[i]->type;
	}

	int obj = ty_is_managed(e->type.kind);   /* readAt -> byte[] (owned); others scalar/void. */
	cg_call_with_args(cg,tt,fn,e->lhs,e->args,e->arg_count,0, obj, 0, ps, e->arg_count, 0);

	if (fallible)
	{
		if (obj)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Preserve the byte[] across the check. */
		}

		int k = cg_label(cg);
		cg_emit(cg,"    lea %s, [rel .L%d]", cg_iarg(cg, 0), k);
		cg_emit(cg,".L%d:", k);
		cg_emit(cg,"    mov %s, rbp", cg_iarg(cg, 1));
		cg_aligned_call(cg,"bzy_io_check");
		if (obj)
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
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
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), ty_is_managed(e->type.elem->kind) ? 1 : 0);
		cg_aligned_call(cg,"bzy_array_new");   /* Owned (+1) array in rax. */
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
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 1), ty_is_managed(e->type.elem->kind) ? 1 : 0);
			cg_aligned_call(cg,"bzy_array_new");   /* Box = length-1 array. */
		}
		else if (strcmp(e->type.class_name,"Set")==0)
		{
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_map_key_kind(tt, e->type.elem));        /* key_kind. */
			cg_emit(cg,"    mov %s, 0", cg_iarg(cg, 1));                                            /* Values unmanaged. */
			cg_aligned_call(cg,"bzy_map_new");
		}
		else   /* List / Stack / Queue / Deque / ArrayDeque -> vector. */
		{
			cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), cg_elem_kind(e->type.elem->kind));
			cg_aligned_call(cg,"bzy_vec_new");
		}

		break;
	case EX_INDEX:
		cg_index_addr(cg,tt,e);
		if (ty_is_float(e->type.kind))
		{
			cg_load_fp(cg,e->type.kind,"[rbx]");
		}
		else
		{
			cg_emit(cg,"    mov rax, [rbx]");
		}

		break;
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
		if (ty_is_float(e->type.kind))
		{
			const char *sfx = e->type.kind==TY_FLOAT ? "ss" : "sd";
			cg_emit(cg,"    xorps xmm1, xmm1");
			cg_emit(cg,"    sub%s xmm1, xmm0", sfx);   /* 0 - x = -x. */
			cg_emit(cg,"    movaps xmm0, xmm1");
		}
		else
		{
			cg_emit(cg,"    neg rax");
			cg_extend_reg(cg,e->type.kind);
		}

		break;
	case EX_BINARY:
		cg_binary(cg,tt,e);
		break;
	case EX_INCDEC:
		cg_expr(cg,tt,e->lhs);                 /* Current value -> rax. */
		cg_emit(cg, e->op==TOKEN_PLUSPLUS ? "    add rax, 1" : "    sub rax, 1");
		cg_extend_reg(cg,e->type.kind);        /* Re-extend to the declared width. */
		cg_emit(cg,"    mov [rbp - %d], rax", e->lhs->anno_int);
		break;
	case EX_NEW:
		cg_new(cg,tt,e);
		break;
	case EX_METHOD_CALL:
		if (e->anno_int==-1)   /* Static method call (C.method(args)): no `this`. */
		{
			MethodInfo *sm=types_find_method(types_find_class(tt,e->anno_str),e->name);
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
		else if (e->lhs->type.kind==TY_FILECHANNEL)
		{
			cg_filechannel_method(cg,tt,e);
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
		if (strcmp(e->name,"print")==0)
		{
			cg_print(cg,tt,e);
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
		else if (strncmp(e->name,"Log.",4)==0)
		{
			cg_log(cg,tt,e);
		}
		else
		{
			FuncInfo *fi=types_find_func(tt,e->name);
			if (fi->is_blocking)
			{
				cg_request_blocking_thunk(cg, fi);
				cg_blocking_call(cg,tt,e,fi);
			}
			else
			{
				cg_call_with_args(cg,tt,fi->asm_label,NULL,e->args,e->arg_count,0, ty_is_managed(e->type.kind),
								  ty_is_float(e->type.kind), fi->param_types, fi->param_count, fi->is_extern);
			}
		}
		break;
	}
}

static void cg_block(Codegen *cg, TypeTable *tt, Func *f, Block *b, int in_main);
static void cg_stmt(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main);
static int cg_tt_has_statics(TypeTable *tt);

static void cg_store(Codegen *cg, TypeTable *tt, Expr *target)
{
	int fp = ty_is_float(target->type.kind);
	if (target->kind==EX_INDEX)
	{
		if (fp)
		{
			cg_index_addr(cg,tt,target);   /* rbx = element address; xmm0 preserved. */
			cg_store_fp(cg,target->type.kind,"[rbx]");
		}
		else
		{
			cg_temp_push(cg);      /* The integer value. */
			cg_index_addr(cg,tt,target);
			cg_temp_pop(cg);
			cg_emit(cg,"    mov [rbx], rax");
		}

		return;
	}
	if (target->kind==EX_IDENT)
	{
		char mem[32];
		sprintf(mem,"[rbp - %d]", target->anno_int);
		if (fp)
		{
			cg_store_fp(cg,target->type.kind,mem);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], rax", target->anno_int);
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

/* Store a +1 object into an object-typed target, releasing the previous occupant
   and any owned receiver temporary. */
static void cg_assign_object(Codegen *cg, TypeTable *tt, Expr *target, Expr *value)
{
	if (target->kind==EX_INDEX)
	{
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
		cg_emit(cg,"    mov rbx, %s", mem);    /* Old occupant. */
		cg_emit(cg,"    mov %s, rax", mem);    /* Store new (transfers the +1). */
		cg_emit(cg,"    mov %s, rbx", cg_iarg(cg, 0));
		cg_release_rcx(cg);                    /* Release old. */
	}
	else
	{
		cg_expr_owned(cg,tt,value);
		cg_emit(cg,"    mov [rbp - %d], rax", cg->assign_save);
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
static void cg_for(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
	int top=cg_label(cg), end=cg_label(cg), cont=cg_label(cg);
	int sb=cg->cur_break_label, sc=cg->cur_continue_label;
	cg_stmt(cg,tt,f,s->for_init,in_main);
	cg_emit(cg,".L%d:", top);
	cg_expr(cg,tt,s->cond);
	cg_emit(cg,"    cmp rax, 0");
	cg_emit(cg,"    je .L%d", end);
	cg->cur_break_label=end;
	cg->cur_continue_label=cont;
	cg_block(cg,tt,f,s->then_blk,in_main);
	cg->cur_break_label=sb;
	cg->cur_continue_label=sc;
	cg_emit(cg,".L%d:", cont);            /* Continue lands here -> post runs. */
	cg_stmt(cg,tt,f,s->for_post,in_main);
	cg_emit(cg,"    jmp .L%d", top);
	cg_emit(cg,".L%d:", end);
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

	cg_expr(cg,tt,s->expr);                     /* Container pointer -> rax. */
	cg_emit(cg,"    mov [rbp - %d], rax", s->fe_coll_offset);
	cg_emit(cg,"    mov qword [rbp - %d], 0", s->fe_index_offset);

	if (ik==TY_STRING)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_aligned_call(cg,"bzy_str_len");
		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_len_offset);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_aligned_call(cg,"bzy_str_data");
		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_aux_offset);
	}

	cg_emit(cg,".L%d:", top);
	if (ik==TY_MAP || gen_set)
	{
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), s->fe_index_offset);
		cg_aligned_call(cg,"bzy_map_iter");      /* Next full slot or -1 in rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_index_offset);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    jl .L%d", end);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), s->fe_index_offset);
		cg_aligned_call(cg,"bzy_map_key_at");    /* Key (borrowed) in rax. */
		cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
		if (s->fe_val_type.kind != TY_VOID)
		{
			TypeKind vt = s->fe_val_type.kind;
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), s->fe_index_offset);
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
				cg_emit(cg,"    mov [rbp - %d], rax", s->fe_val_offset);
			}
		}
	}
	else if (ik==TY_STRING)
	{
		cg_emit(cg,"    mov rcx, [rbp - %d]", s->fe_index_offset);
		cg_emit(cg,"    cmp rcx, [rbp - %d]", s->fe_len_offset);
		cg_emit(cg,"    jge .L%d", end);
		cg_emit(cg,"    mov rax, [rbp - %d]", s->fe_aux_offset);
		cg_emit(cg,"    movzx eax, byte [rax + rcx]");   /* byte -> int (zero-extended). */
		cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
	}
	else if (gen_vec)
	{
		TypeKind et = s->expr->type.elem->kind;
		int managed = ty_is_managed(et);
		cg_emit(cg,"    mov rcx, [rbp - %d]", s->fe_index_offset);
		cg_emit(cg,"    mov rdx, [rbp - %d]", s->fe_coll_offset);
		cg_emit(cg,"    cmp rcx, [rdx + 24]");           /* index vs length@24 */
		cg_emit(cg,"    jge .L%d", end);
		if (managed)
		{
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 0), s->fe_coll_offset);
			cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), s->fe_index_offset);
			cg_aligned_call(cg,"bzy_vec_get");           /* Element (owned -> retained) in rax. */
		}
		else
		{
			/* Value element: inline the ring load, skipping the bzy_vec_get
			   call, its redundant bounds check, and the value-case no-op
			   retain. rcx=index, rdx=coll survive from the bounds check above.
			   Vector layout: cap@32, head@40, data array@48; the data array
			   holds its 8-byte slots at +32. phys = (head+i) & (cap-1) (cap is
			   a power of two). No call here, so r8/rax/rcx scratch is safe. */
			cg_emit(cg,"    mov rax, [rdx + 48]");        /* data array ptr */
			cg_emit(cg,"    mov r8, [rdx + 40]");         /* head */
			cg_emit(cg,"    add rcx, r8");                /* head + index */
			cg_emit(cg,"    mov r8, [rdx + 32]");         /* cap */
			cg_emit(cg,"    dec r8");                     /* cap - 1 */
			cg_emit(cg,"    and rcx, r8");                /* phys = (head+i) & (cap-1) */
			cg_emit(cg,"    mov rax, [rax + rcx*8 + 32]");/* slot value -> rax */
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
			cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
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
		cg_emit(cg,"    mov rcx, [rbp - %d]", s->fe_index_offset);
		cg_emit(cg,"    cmp rcx, [rax + 24]");           /* index vs length */
		cg_emit(cg,"    jge .L%d", end);
		cg_emit(cg,"    lea rbx, [rax + rcx*8 + 32]");    /* element address */
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
			cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
		}
	}

	cg->cur_break_label = end;            /* break -> .Lend; continue -> .Lcont (the increment). */
	cg->cur_continue_label = cont;
	cg_block(cg,tt,f,s->then_blk,in_main);
	cg->cur_break_label = sb;
	cg->cur_continue_label = sc;

	cg_emit(cg,".L%d:", cont);            /* continue lands here, then the cursor advances. */
	if (ik==TY_MAP || gen_set)
	{
		cg_emit(cg,"    mov rax, [rbp - %d]", s->fe_index_offset);
		cg_emit(cg,"    inc rax");
		cg_emit(cg,"    mov [rbp - %d], rax", s->fe_index_offset);
	}
	else
	{
		cg_emit(cg,"    inc qword [rbp - %d]", s->fe_index_offset);
	}

	cg_emit(cg,"    jmp .L%d", top);
	cg_emit(cg,".L%d:", end);

	if (owned)   /* An owned iterable temporary (e.g. `new int[3]`) is released here. */
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
		cg_emit(cg,"    sub rcx, %lld", lo);
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
		if (cg->cur_try_count < 64)
		{
			cg->cur_try_k[cg->cur_try_count] = k;
			cg->cur_try_c[cg->cur_try_count] = c;
			strcpy(cg->cur_try_vt[cg->cur_try_count], cl->decl_type.class_name);
			cg->cur_try_count++;
		}
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

	if (cg->breeze_thunk_count < 64)
	{
		cg->breeze_thunks[cg->breeze_thunk_count++] = fi;
	}
}

/* The per-target spawn thunk: rcx = arg block. Loads each arg into its Win64
   parameter register by class (int -> rcx/rdx/r8/r9, fp -> xmm0..3 by position),
   calls the target, releases managed args (the target borrowed them), frees the
   block, and returns to breeze_run. */
static void cg_emit_breeze_thunk(Codegen *cg, FuncInfo *fi)
{
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

	cg_emit(cg,"    mov %s, [rbp - 8]", a0);
	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call free");
	cg_emit(cg,"    add rsp, 32");
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

	if (cg->blocking_thunk_count < 64)
	{
		cg->blocking_thunks[cg->blocking_thunk_count++] = fi;
	}
}

/* The per-target blocking thunk, run on an offload worker: rcx = ctx blob.
   ctx[i*8] holds arg i (raw object pointer for a string); ctx[32] receives the
   return value. Loads each arg into its Win64 register by position, marshals a
   string to its char* data (+32), calls the raw C symbol, stores the result. */
static void cg_emit_blocking_thunk(Codegen *cg, FuncInfo *fi)
{
	const char *a0 = cg_iarg(cg, 0);   /* The thunk's incoming ctx pointer (arg0). */
	cg_emit(cg,"__blocking_%s:", fi->asm_label);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, 48");                 /* 16-aligned: ctx save at [rbp-8] + shadow. */
	cg_emit(cg,"    mov [rbp - 8], %s", a0);       /* Save the ctx pointer. */
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
			const char *r = cg_iarg(cg, ii);
			cg_emit(cg,"    mov %s, [rax + %d]", r, i*8);
			if (k==TY_STRING)
			{
				cg_emit(cg,"    add %s, 32", r);   /* string object -> char* data. */
			}
		}
	}

	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call %s", fi->asm_label);      /* The raw ($-escaped) C symbol. */
	cg_emit(cg,"    add rsp, 32");
	cg_emit(cg,"    mov %s, [rbp - 8]", a0);
	if (fi->ret_type.kind==TY_DOUBLE)
	{
		cg_emit(cg,"    movq [%s + 32], xmm0", a0);
	}
	else if (fi->ret_type.kind==TY_FLOAT)
	{
		cg_emit(cg,"    movd [%s + 32], xmm0", a0);
	}
	else if (fi->ret_type.kind!=TY_VOID)
	{
		cg_emit(cg,"    mov [%s + 32], rax", a0);
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
			if (ty_is_managed(s->decl_type.kind))
			{
				cg_expr_owned(cg,tt,s->decl_init);
				cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
			}
			else
			{
				cg_expr(cg,tt,s->decl_init);
				cg_coerce(cg,s->decl_type.kind,s->decl_init->type.kind);
				char mem[32];
				sprintf(mem,"[rbp - %d]", s->decl_offset);
				if (ty_is_float(s->decl_type.kind))
				{
					cg_store_fp(cg,s->decl_type.kind,mem);
				}
				else
				{
					cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
				}
			}
		}
		break;
	case ST_ASSIGN:
		if (ty_is_managed(s->target->type.kind))
		{
			cg_assign_object(cg,tt,s->target,s->value);
		}
		else
		{
			cg_expr(cg,tt,s->value);
			cg_coerce(cg,s->target->type.kind,s->value->type.kind);
			cg_store(cg,tt,s->target);
		}
		break;
	case ST_EXPR:
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

		cg_emit(cg,"    mov rbx, [rbp - %d]", cg->rbx_save);   /* Restore the caller's rbx. */
		cg_emit(cg,"    mov rsp, rbp");
		cg_emit(cg,"    pop rbp");
		cg_emit(cg,"    ret");
		break;
	case ST_IF:
	{
		int else_l=cg_label(cg), end_l=cg_label(cg);
		cg_expr(cg,tt,s->cond);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d", s->else_blk?else_l:end_l);
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
		int top=cg_label(cg), end=cg_label(cg);
		int sb=cg->cur_break_label, sc=cg->cur_continue_label;
		cg->cur_break_label=end;
		cg->cur_continue_label=top;
		cg_emit(cg,".L%d:",top);
		cg_expr(cg,tt,s->cond);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d",end);
		cg_block(cg,tt,f,s->then_blk,in_main);
		cg_emit(cg,"    jmp .L%d",top);
		cg_emit(cg,".L%d:",end);
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
		cg_for(cg,tt,f,s,in_main);
		break;
	case ST_SWITCH:
		cg_switch(cg,tt,f,s,in_main);
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

		/* Arg'd spawn: malloc a block of n 8-byte slots, fill it with the args
		   (managed args owned by the breeze), then bzy_spawn_args(thunk, block).
		   The block pointer lives on the native stack so it survives the arg
		   evaluations (which clobber rax and use val_save). */
		int n = s->expr->arg_count;
		int b = cg_scratch_alloc(cg, 16);
		cg_emit(cg,"    mov %s, %d", cg_iarg(cg, 0), n * 8);
		cg_aligned_call(cg,"malloc");
		cg_emit(cg,"    mov [rbp - %d], rax", b);           /* Save the block pointer. */
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

		cg_emit(cg,"    lea %s, [rel __breeze_%s]", cg_iarg(cg, 0), fi->asm_label);
		cg_emit(cg,"    mov %s, [rbp - %d]", cg_iarg(cg, 1), b);
		cg_aligned_call(cg,"bzy_spawn_args");
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

/* Emits one per-function exception record into .data (PC range, frame size, name,
   object-local offsets, and the try-region table). The end label is placed in
   .text just past the function; the record is appended in .data. */
static void cg_emit_exception_record(Codegen *cg, const char *label, int frame, Func *f)
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

static void cg_emit_func(Codegen *cg, TypeTable *tt, const char *label, Func *f, const char *this_class)
{
	int is_main = (this_class==NULL && strcmp(f->name,"main")==0);
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
	int scratch = 80;        /* sp_save, val_save, four arg temps, assign_save, fp_save, rbx_save. */
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

	int frame = locals + scratch + stack_objs + temps + arena + outargs;
	if (frame % 16 != 0)
	{
		frame = (frame/16 + 1)*16;   /* Keep rsp 16-aligned after the prologue so calls are aligned. */
	}

	cg->outarg_base = frame;   /* rsp = rbp - frame; shadow [rsp,rsp+32), outgoing args at [rsp+32+i*8]. */

	cg_emit(cg,"global %s", label);
	cg_emit(cg,"%s:", label);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, %d", frame);
	cg_emit(cg,"    mov [rbp - %d], rbx", cg->rbx_save);   /* Preserve the caller's callee-saved rbx. */

	/* Spill incoming args: this at [rbp-8], then params at [rbp-16], [rbp-24], and so on.
	   Win64 numbers both register classes by argument position; System V numbers
	   integer and FP registers independently (int_idx / fp_idx). */
	int int_idx = 0, fp_idx = 0;
	if (this_class)
	{
		cg_emit(cg,"    mov [rbp - 8], %s", cg_iarg(cg, int_idx));
		int_idx++;
	}

	for (int i=0; i<f->param_count; i++)
	{
		int slot = this_class ? (16 + i*8) : (8 + i*8);
		int pos = (this_class ? 1 : 0) + i;   /* Win64 by-position index. */
		TypeKind pk = f->params[i].type.kind;
		if (pk==TY_FLOAT)
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : pos;
			cg_emit(cg,"    movss dword [rbp - %d], xmm%d", slot, xi);
		}
		else if (pk==TY_DOUBLE)
		{
			int xi = (cg->target==TARGET_LINUX) ? fp_idx++ : pos;
			cg_emit(cg,"    movsd qword [rbp - %d], xmm%d", slot, xi);
		}
		else
		{
			int ii = (cg->target==TARGET_LINUX) ? int_idx++ : pos;
			cg_emit(cg,"    mov [rbp - %d], %s", slot, cg_iarg(cg, ii));
		}
	}

	/* Object locals must be NULL before any release; bzy_alloc only zeroes heap
	   objects, not stack slots. */
	for (int i=0; i<f->obj_local_count; i++)
	{
		cg_emit(cg,"    mov qword [rbp - %d], 0", f->obj_local_offsets[i]);
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

	cg_emit(cg,"    mov rbx, [rbp - %d]", cg->rbx_save);   /* Restore the caller's rbx. */
	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
	cg_emit_exception_record(cg, label, frame, f);
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
			Expr tmp;
			memset(&tmp,0,sizeof(tmp));
			tmp.kind=EX_NEW;
			strcpy(tmp.name,e->const_class[k]);
			tmp.type.kind=TY_OBJECT;
			strcpy(tmp.type.class_name,e->const_class[k]);
			tmp.arg_count=e->const_argc[k];
			for (int a=0; a<e->const_argc[k]; a++)
			{
				tmp.args[a]=e->const_args[k][a];
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
		for (int k=0; k<tt->classes[i].field_count; k++)
		{
			if (tt->classes[i].fields[k].is_static)
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
		ClassDecl *d=units[i]->klass;
		if (!d)
		{
			continue;
		}

		for (int k=0; k<d->field_count; k++)
		{
			if (d->fields[k].is_static || d->is_static)
			{
				cg_emit(cg,"__static_%s_%s: dq 0", d->name, d->fields[k].name);
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
		ClassDecl *d=units[i]->klass;
		if (!d)
		{
			continue;
		}

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
				cg_emit(cg,"    mov %s, rax", mem);
			}
			else
			{
				cg_expr(cg,tt,d->fields[k].init);         /* Scalar -> rax. */
				cg_emit(cg,"    mov %s, rax", mem);
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
	cg_emit(cg,"extern malloc");
	cg_emit(cg,"extern free");
	cg_emit(cg,"extern bzy_alloc");
	cg_emit(cg,"extern bzy_retain");
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
	cg_emit(cg,"extern bzy_print_str");
	cg_emit(cg,"extern bzy_sb_new");
	cg_emit(cg,"extern bzy_sb_append");
	cg_emit(cg,"extern bzy_sb_to_string");
	cg_emit(cg,"extern bzy_array_new");
	cg_emit(cg,"extern bzy_array_len");
	cg_emit(cg,"extern bzy_oob");
	cg_emit(cg,"extern bzy_oob_abort");
	cg_emit(cg,"extern bzy_map_new");
	cg_emit(cg,"extern bzy_map_put");
	cg_emit(cg,"extern bzy_map_get");
	cg_emit(cg,"extern bzy_map_has");
	cg_emit(cg,"extern bzy_map_remove");
	cg_emit(cg,"extern bzy_map_contains_value");
	cg_emit(cg,"extern bzy_map_keys");
	cg_emit(cg,"extern bzy_map_values");
	cg_emit(cg,"extern bzy_map_entries");
	cg_emit(cg,"extern bzy_entry_key");
	cg_emit(cg,"extern bzy_entry_val");
	cg_emit(cg,"extern bzy_spawn");
	cg_emit(cg,"extern bzy_spawn_args");
	cg_emit(cg,"extern bzy_offload_run");   /* FFI: `extern blocking` dispatch. */
	cg_emit(cg,"extern bzy_yield");
	cg_emit(cg,"extern bzy_channel_new");
	cg_emit(cg,"extern bzy_channel_send");
	cg_emit(cg,"extern bzy_channel_recv");
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
	cg_emit(cg,"extern bzy_filechannel_write_at");
	cg_emit(cg,"extern bzy_filechannel_size");
	cg_emit(cg,"extern bzy_filechannel_truncate");
	cg_emit(cg,"extern bzy_filechannel_sync");
	cg_emit(cg,"extern bzy_filechannel_close");
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
				FuncInfo *fi=types_find_func(tt,f->name);
				strcpy(buf,fi->asm_label);
				label=buf;
			}

			cg_emit_func(cg,tt,label,f,NULL);
		}
	}

	for (int i=0; i<unit_count; i++)
	{
		Unit *u=units[i];
		if (!u->klass)
		{
			continue;
		}

		ClassInfo *c=types_find_class(tt,u->klass->name);
		for (int k=0; k<u->klass->method_count; k++)
		{
			Func *m=u->klass->methods[k];
			MethodInfo *mi=types_find_method(c,m->name);
			cg_emit_func(cg,tt,mi->asm_label,m, mi->is_static ? NULL : c->name);   /* Static: no `this`. */
		}

		if (u->klass->ctor)
		{
			cg_emit_func(cg,tt,c->ctor_asm_label,u->klass->ctor,c->name);
		}

		if (c->is_record)
		{
			cg_emit_record_methods(cg,tt,c);   /* Synthesized hashCode/equals at slots 0/1. */
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

	cg_emit_enum_init(cg,tt);   /* __enum_init (constructs the singletons), still in .text. */
	cg_emit_static_init(cg,tt,units,unit_count);   /* __static_init (runs field initializers). */

	cg_emit(cg,"");
	cg_emit(cg,"section .data");
	for (int i=0; i<tt->class_count; i++)
	{
		cg_emit_vtable(cg,&tt->classes[i]);
	}

	cg_emit(cg,"__bzy_vtable_parents:");           /* (child vtable, parent vtable) pairs for is-a. */
	for (int i=0; i<tt->class_count; i++)
	{
		ClassInfo *c = &tt->classes[i];
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

	cg_emit_enum_data(cg);   /* Enum singleton slots + constant name strings. */
	cg_emit_static_data(cg,units,unit_count);   /* Static-field global slots. */
}
