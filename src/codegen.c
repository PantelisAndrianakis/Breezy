#include "codegen.h"
#include "symtable.h"
#include "lexer.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void cg_init(Codegen *cg, FILE *out)
{
	cg->out=out;
	cg->label_count=0;
	cg->fpk_count=0;
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
static const char *ARG_REG[4] = { "rcx","rdx","r8","r9" };   /* Win64 */

/* Load a scalar from 'mem' into rax, sign- or zero-extending to 64 bits per its
   declared width. Objects and 64-bit integers load with a plain mov. */
static void cg_load_scalar(Codegen *cg, TypeKind k, const char *mem)
{
	if (k==TY_BOOL)
	{
		cg_emit(cg,"    movzx rax, byte %s", mem);
		return;
	}

	switch (ty_bits(k))
	{
	case 8:
		if (ty_is_signed(k))
		{
			cg_emit(cg,"    movsx rax, byte %s", mem);
		}
		else
		{
			cg_emit(cg,"    movzx rax, byte %s", mem);
		}

		break;
	case 16:
		if (ty_is_signed(k))
		{
			cg_emit(cg,"    movsx rax, word %s", mem);
		}
		else
		{
			cg_emit(cg,"    movzx rax, word %s", mem);
		}

		break;
	case 32:
		if (ty_is_signed(k))
		{
			cg_emit(cg,"    movsxd rax, dword %s", mem);
		}
		else
		{
			cg_emit(cg,"    mov eax, dword %s", mem);   /* Writing eax zero-extends rax. */
		}

		break;
	default:
		cg_emit(cg,"    mov rax, %s", mem);             /* 64-bit integer or object. */
		break;
	}
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

/* Save rsp at an rbp-relative slot so a runtime call is 16-byte aligned no
   matter the current rsp alignment or pending pushes; the argument is in rcx. */
static void cg_aligned_call(Codegen *cg, const char *fn)
{
	cg_emit(cg,"    mov [rbp - %d], rsp", cg->sp_save);
	cg_emit(cg,"    and rsp, -16");
	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call %s", fn);
	cg_emit(cg,"    mov rsp, [rbp - %d]", cg->sp_save);
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

		cg_emit(cg,"    mov rcx, [rbp - %d]", off);
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

	return e->kind==EX_NEW || e->kind==EX_CALL || e->kind==EX_METHOD_CALL;
}

/* Retain the object pointer currently in rax; rax is preserved. */
static void cg_retain_rax(Codegen *cg)
{
	cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
	cg_emit(cg,"    mov rcx, rax");
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

/* Floating-point binary op. Operands evaluate to xmm0; the left operand is
   spilled on the machine stack so nested FP expressions compose correctly. The
   one legal int operand (int + double) is promoted with cvtsi2sd. */
static void cg_binary_fp(Codegen *cg, TypeTable *tt, Expr *e)
{
	const char *sfx = (e->lhs->type.kind==TY_FLOAT && e->rhs->type.kind==TY_FLOAT) ? "ss" : "sd";
	cg_expr(cg,tt,e->lhs);
	if (ty_is_int(e->lhs->type.kind))
	{
		cg_emit(cg,"    cvtsi2sd xmm0, rax");
	}

	cg_emit(cg,"    sub rsp, 8");
	cg_emit(cg,"    movsd qword [rsp], xmm0");   /* Spill lhs (float lives in the low 4 bytes). */
	cg_expr(cg,tt,e->rhs);
	if (ty_is_int(e->rhs->type.kind))
	{
		cg_emit(cg,"    cvtsi2sd xmm0, rax");
	}

	cg_emit(cg,"    movsd xmm1, xmm0");          /* rhs -> xmm1. */
	cg_emit(cg,"    movsd xmm0, qword [rsp]");   /* lhs -> xmm0. */
	cg_emit(cg,"    add rsp, 8");
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
	default:   /* comparison -> boolean in rax (unordered/NaN compares false except !=). */
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
	if (ty_is_float(e->lhs->type.kind) || ty_is_float(e->rhs->type.kind))
	{
		cg_binary_fp(cg,tt,e);
		return;
	}

	cg_expr(cg,tt,e->lhs);
	cg_emit(cg,"    push rax");
	cg_expr(cg,tt,e->rhs);
	cg_emit(cg,"    mov rbx, rax");
	cg_emit(cg,"    pop rax");
	int uns = ty_is_unsigned(e->lhs->type.kind);   /* Operands share signedness. */
	switch (e->op)
	{
	case TOKEN_PLUS:
		cg_emit(cg,"    add rax, rbx");
		cg_extend_reg(cg,e->type.kind);
		break;
	case TOKEN_MINUS:
		cg_emit(cg,"    sub rax, rbx");
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

		cg_emit(cg,"    cmp rax, rbx");
		cg_emit(cg,"    %s al", set);
		cg_emit(cg,"    movzx rax, al");
		break;
	}
	default:
		fprintf(stderr,"codegen: bad binary op\n");
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
							  const TypeRef *params, int param_count)
{
	int total = (self?1:0) + argc;
	if (total > 4)
	{
		fprintf(stderr,"codegen: >4 args unsupported in core plan\n");
		exit(1);
	}

	if (indirect)
	{
		cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);   /* Callee address (freed by call time). */
	}

	int block = ((total*8 + 15)/16)*16;   /* 16-aligned scratch for spilled args. */
	if (block)
	{
		cg_emit(cg,"    sub rsp, %d", block);
	}

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

		cg_emit(cg,"    mov [rsp + %d], rax", slot*8);
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
			cg_emit(cg,"    movsd qword [rsp + %d], xmm0", slot*8);
		}
		else
		{
			if (expr_is_owned(args[i]))
			{
				owned_tmp[owned_n++] = slot;
			}

			cg_emit(cg,"    mov [rsp + %d], rax", slot*8);
		}

		slot++;
	}

	for (int s=0; s<total; s++)
	{
		if (slot_kind[s]==TY_FLOAT)
		{
			cg_emit(cg,"    movss xmm%d, dword [rsp + %d]", s, s*8);
		}
		else if (slot_kind[s]==TY_DOUBLE)
		{
			cg_emit(cg,"    movsd xmm%d, qword [rsp + %d]", s, s*8);
		}
		else
		{
			cg_emit(cg,"    mov %s, [rsp + %d]", ARG_REG[s], s*8);
		}
	}

	if (indirect)
	{
		cg_emit(cg,"    mov r11, [rbp - %d]", cg->val_save);
		cg_emit(cg,"    sub rsp, 32");
		cg_emit(cg,"    call r11");
		cg_emit(cg,"    add rsp, 32");
	}
	else
	{
		cg_emit(cg,"    sub rsp, 32");
		cg_emit(cg,"    call %s", target);
		cg_emit(cg,"    add rsp, 32");
	}

	if (owned_n > 0)
	{
		if (result_is_object)
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->val_save);
		}

		if (result_is_fp)
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm0", cg->fp_save);
		}

		for (int i=0; i<owned_n; i++)
		{
			cg_emit(cg,"    mov rcx, [rsp + %d]", owned_tmp[i]*8);
			cg_release_rcx(cg);
		}

		if (result_is_object)
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}

		if (result_is_fp)
		{
			cg_emit(cg,"    movsd xmm0, qword [rbp - %d]", cg->fp_save);
		}
	}

	if (block)
	{
		cg_emit(cg,"    add rsp, %d", block);
	}
}

static void cg_method_call(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);                       /* Receiver pointer in rax. */
	cg_emit(cg,"    mov rax, [rax]");             /* Vtable pointer. */
	cg_emit(cg,"    mov rax, [rax + %d]", e->anno_int * 8);
	ClassInfo *c=types_find_class(tt,e->anno_str);
	MethodInfo *m=types_find_method(c,e->name);
	cg_call_with_args(cg,tt,NULL,e->lhs,e->args,e->arg_count,1, ty_is_managed(e->type.kind),
					  ty_is_float(e->type.kind), m->param_types, m->param_count);
}

static void cg_new(Codegen *cg, TypeTable *tt, Expr *e)
{
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

		return;
	}

	cg_emit(cg,"    mov rcx, %d", c->object_size);
	cg_emit(cg,"    mov [rbp - %d], rsp", cg->sp_save);
	cg_emit(cg,"    and rsp, -16");
	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call bzy_alloc");
	cg_emit(cg,"    mov rsp, [rbp - %d]", cg->sp_save);
	cg_emit(cg,"    lea rbx, [rel __vtable_%s]", c->name);
	cg_emit(cg,"    mov [rax], rbx");
	/* The refcount and fields are zeroed by bzy_alloc, so rax holds an owned reference. */
}

static void cg_print(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->args[0]);
	TypeKind k=e->args[0]->type.kind;
	if (ty_is_float(k))
	{
		if (k==TY_FLOAT)
		{
			cg_emit(cg,"    cvtss2sd xmm0, xmm0");   /* Promote to double for printing. */
		}

		cg_aligned_call(cg,"bzy_print_f64");         /* Double arg already in xmm0. */
		return;
	}

	cg_emit(cg,"    mov rcx, rax");
	const char *fn = k==TY_BOOL ? "bzy_print_bool"
					 : ty_is_unsigned(k) ? "bzy_print_u64"
					 : "bzy_print_i64";
	cg_aligned_call(cg,fn);
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
	case EX_STR:
		fprintf(stderr,"codegen: string lowering arrives in Part 4a Task 5\n");
		exit(1);
		break;
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
	case EX_NEW:
		cg_new(cg,tt,e);
		break;
	case EX_METHOD_CALL:
		cg_method_call(cg,tt,e);
		break;
	case EX_CALL:
		if (strcmp(e->name,"print")==0)
		{
			cg_print(cg,tt,e);
		}
		else if (strcmp(e->name,"liveCount")==0)
		{
			cg_emit(cg,"    mov [rbp - %d], rsp", cg->sp_save);
			cg_emit(cg,"    and rsp, -16");
			cg_emit(cg,"    sub rsp, 32");
			cg_emit(cg,"    call bzy_live_count");
			cg_emit(cg,"    mov rsp, [rbp - %d]", cg->sp_save);
		}
		else if (strcmp(e->name,"collectCycles")==0)
		{
			cg_emit(cg,"    mov [rbp - %d], rsp", cg->sp_save);
			cg_emit(cg,"    and rsp, -16");
			cg_emit(cg,"    sub rsp, 32");
			cg_emit(cg,"    call bzy_collect_cycles");
			cg_emit(cg,"    mov rsp, [rbp - %d]", cg->sp_save);
		}
		else
		{
			FuncInfo *fi=types_find_func(tt,e->name);
			cg_call_with_args(cg,tt,fi->asm_label,NULL,e->args,e->arg_count,0, ty_is_managed(e->type.kind),
							  ty_is_float(e->type.kind), fi->param_types, fi->param_count);
		}
		break;
	}
}

static void cg_block(Codegen *cg, TypeTable *tt, Func *f, Block *b, int in_main);

static void cg_store(Codegen *cg, TypeTable *tt, Expr *target)
{
	int fp = ty_is_float(target->type.kind);
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
		cg_emit(cg,"    push rax");
		cg_expr(cg,tt,target->lhs);
		cg_emit(cg,"    mov rbx, rax");
		cg_emit(cg,"    pop rax");
		cg_emit(cg,"    mov [rbx + %d], rax", target->anno_int);
	}
}

/* Store a +1 object into an object-typed target, releasing the previous occupant
   and any owned receiver temporary. */
static void cg_assign_object(Codegen *cg, TypeTable *tt, Expr *target, Expr *value)
{
	if (target->kind==EX_IDENT)
	{
		cg_expr_owned(cg,tt,value);
		cg_emit(cg,"    mov rbx, [rbp - %d]", target->anno_int);
		cg_emit(cg,"    mov [rbp - %d], rax", target->anno_int);
		cg_emit(cg,"    mov rcx, rbx");
		cg_release_rcx(cg);
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
		cg_emit(cg,"    mov rcx, rdx");
		cg_release_rcx(cg);
		if (expr_is_owned(target->lhs))
		{
			cg_emit(cg,"    mov rcx, rbx");
			cg_release_rcx(cg);
		}
	}
}

static void cg_stmt(Codegen *cg, TypeTable *tt, Func *f, Stmt *s, int in_main)
{
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
			cg_emit(cg,"    mov rcx, rax");
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
			}

			cg_release_object_locals(cg, f, -1);
		}

		if (in_main)
		{
			cg_emit(cg,"    xor eax, eax");
		}

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
		cg_emit(cg,".L%d:",top);
		cg_expr(cg,tt,s->cond);
		cg_emit(cg,"    cmp rax, 0");
		cg_emit(cg,"    je .L%d",end);
		cg_block(cg,tt,f,s->then_blk,in_main);
		cg_emit(cg,"    jmp .L%d",top);
		cg_emit(cg,".L%d:",end);
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

static void cg_emit_func(Codegen *cg, TypeTable *tt, const char *label, Func *f, const char *this_class)
{
	int is_main = (this_class==NULL && strcmp(f->name,"main")==0);
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
	int scratch = 72;        /* sp_save, val_save, four arg temps, assign_save, fp_save. */
	int stack_objs = f->stack_alloc_bytes;
	if (stack_objs % 16 != 0)
	{
		stack_objs = (stack_objs/16 + 1)*16;   /* Keep the frame 16-byte aligned. */
	}

	int frame = locals + scratch + stack_objs;

	cg_emit(cg,"global %s", label);
	cg_emit(cg,"%s:", label);
	cg_emit(cg,"    push rbp");
	cg_emit(cg,"    mov rbp, rsp");
	cg_emit(cg,"    sub rsp, %d", frame);

	/* Spill incoming args: this at [rbp-8], then params at [rbp-16], [rbp-24], and so on. */
	int reg = 0;
	if (this_class)
	{
		cg_emit(cg,"    mov [rbp - 8], %s", ARG_REG[reg]);
		reg++;
	}

	for (int i=0; i<f->param_count; i++)
	{
		int slot = this_class ? (16 + i*8) : (8 + i*8);
		TypeKind pk = f->params[i].type.kind;
		if (pk==TY_FLOAT)
		{
			cg_emit(cg,"    movss dword [rbp - %d], xmm%d", slot, reg);
		}
		else if (pk==TY_DOUBLE)
		{
			cg_emit(cg,"    movsd qword [rbp - %d], xmm%d", slot, reg);
		}
		else
		{
			cg_emit(cg,"    mov [rbp - %d], %s", slot, ARG_REG[reg]);
		}

		reg++;
	}

	/* Object locals must be NULL before any release; bzy_alloc only zeroes heap
	   objects, not stack slots. */
	for (int i=0; i<f->obj_local_count; i++)
	{
		cg_emit(cg,"    mov qword [rbp - %d], 0", f->obj_local_offsets[i]);
	}

	cg_block(cg, tt, f, f->body, is_main);

	cg_release_object_locals(cg, f, -1);
	if (is_main)
	{
		cg_emit(cg,"    xor eax, eax");
	}

	cg_emit(cg,"    mov rsp, rbp");
	cg_emit(cg,"    pop rbp");
	cg_emit(cg,"    ret");
}

static void cg_emit_vtable(Codegen *cg, ClassInfo *c)
{
	cg_emit(cg,"__typeinfo_%s:", c->name);
	cg_emit(cg,"    dq 0");   /* Finalizer slot (vtable-16): none for user classes. */
	int nobj=0;
	for (int i=0; i<c->field_count; i++)
	{
		if (ty_is_managed(c->fields[i].type.kind))
		{
			nobj++;
		}
	}

	cg_emit(cg,"    dq %d", nobj);
	for (int i=0; i<c->field_count; i++)
	{
		if (ty_is_managed(c->fields[i].type.kind))
		{
			cg_emit(cg,"    dq %d", c->fields[i].offset);
		}
	}

	cg_emit(cg,"    dq __typeinfo_%s", c->name);   /* This word lands at the vtable label minus eight. */
	cg_emit(cg,"__vtable_%s:", c->name);
	for (int slot=0; slot<c->vtable_size; slot++)
	{
		for (int i=0; i<c->method_count; i++)
		{
			if (c->methods[i].vtable_slot==slot)
			{
				cg_emit(cg,"    dq %s", c->methods[i].asm_label);
				break;
			}
		}
	}
}

void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count)
{
	cg_emit(cg,"bits 64");
	cg_emit(cg,"default rel");
	cg_emit(cg,"extern malloc");
	cg_emit(cg,"extern bzy_alloc");
	cg_emit(cg,"extern bzy_retain");
	cg_emit(cg,"extern bzy_release");
	cg_emit(cg,"extern bzy_live_count");
	cg_emit(cg,"extern bzy_collect_cycles");
	cg_emit(cg,"extern bzy_print_i64");
	cg_emit(cg,"extern bzy_print_u64");
	cg_emit(cg,"extern bzy_print_bool");
	cg_emit(cg,"extern bzy_print_f64");
	cg_emit(cg,"section .text");

	for (int i=0; i<unit_count; i++)
	{
		Unit *u=units[i];
		for (int k=0; k<u->func_count; k++)
		{
			Func *f=u->funcs[k];
			char buf[160];
			const char *label;
			if (strcmp(f->name,"main")==0)
			{
				label="main";
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
			cg_emit_func(cg,tt,mi->asm_label,m,c->name);
		}
	}

	cg_emit(cg,"");
	cg_emit(cg,"section .data");
	for (int i=0; i<tt->class_count; i++)
	{
		cg_emit_vtable(cg,&tt->classes[i]);
	}

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
}
