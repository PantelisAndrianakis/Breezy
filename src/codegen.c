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
	if (e->type.kind != TY_OBJECT)
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

static void cg_binary(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);
	cg_emit(cg,"    push rax");
	cg_expr(cg,tt,e->rhs);
	cg_emit(cg,"    mov rbx, rax");
	cg_emit(cg,"    pop rax");
	switch (e->op)
	{
	case TOKEN_PLUS:
		cg_emit(cg,"    add rax, rbx");
		break;
	case TOKEN_MINUS:
		cg_emit(cg,"    sub rax, rbx");
		break;
	case TOKEN_STAR:
		cg_emit(cg,"    imul rax, rbx");
		break;
	case TOKEN_SLASH:
		cg_emit(cg,"    cqo");
		cg_emit(cg,"    idiv rbx");
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
			set="setl";
			break;
		case TOKEN_GT:
			set="setg";
			break;
		case TOKEN_LTE:
			set="setle";
			break;
		default:
			set="setge";
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

static void cg_call_with_args(Codegen *cg, TypeTable *tt, const char *target,
                              Expr *self, Expr **args, int argc, int indirect,
                              int result_is_object)
{
	int total = (self?1:0) + argc;
	if (total > 4)
	{
		fprintf(stderr,"codegen: >4 args unsupported in core plan\n");
		exit(1);
	}

	int owned_tmp[4];
	int owned_n = 0;

	if (indirect)
	{
		cg_emit(cg,"    push rax");          /* Callee address. */
	}

	int slot_index = 0;
	if (self)
	{
		cg_expr(cg,tt,self);
		if (expr_is_owned(self))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->argtmp_base + slot_index*8);
			owned_tmp[owned_n++] = slot_index;
		}

		cg_emit(cg,"    push rax");
		slot_index++;
	}

	for (int i=0; i<argc; i++)
	{
		cg_expr(cg,tt,args[i]);
		if (expr_is_owned(args[i]))
		{
			cg_emit(cg,"    mov [rbp - %d], rax", cg->argtmp_base + slot_index*8);
			owned_tmp[owned_n++] = slot_index;
		}

		cg_emit(cg,"    push rax");
		slot_index++;
	}

	for (int i=total-1; i>=0; i--)
	{
		cg_emit(cg,"    pop %s", ARG_REG[i]);
	}

	if (indirect)
	{
		cg_emit(cg,"    pop rax");
		cg_emit(cg,"    sub rsp, 32");
		cg_emit(cg,"    call rax");
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

		for (int i=0; i<owned_n; i++)
		{
			cg_emit(cg,"    mov rcx, [rbp - %d]", cg->argtmp_base + owned_tmp[i]*8);
			cg_release_rcx(cg);
		}

		if (result_is_object)
		{
			cg_emit(cg,"    mov rax, [rbp - %d]", cg->val_save);
		}
	}
}

static void cg_method_call(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);                       /* Receiver pointer in rax. */
	cg_emit(cg,"    mov rax, [rax]");             /* Vtable pointer. */
	cg_emit(cg,"    mov rax, [rax + %d]", e->anno_int * 8);
	cg_call_with_args(cg,tt,NULL,e->lhs,e->args,e->arg_count,1, e->type.kind==TY_OBJECT);
}

static void cg_new(Codegen *cg, TypeTable *tt, Expr *e)
{
	ClassInfo *c=types_find_class(tt,e->name);
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
	cg_emit(cg,"    mov rdx, rax");
	cg_emit(cg,"    lea rcx, [rel __fmt_int]");
	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call printf");
	cg_emit(cg,"    add rsp, 32");
}

static void cg_expr(Codegen *cg, TypeTable *tt, Expr *e)
{
	switch (e->kind)
	{
	case EX_INT:
		cg_emit(cg,"    mov rax, %ld", e->int_val);
		break;
	case EX_THIS:
		cg_emit(cg,"    mov rax, [rbp - 8]");
		break;
	case EX_IDENT:
		cg_emit(cg,"    mov rax, [rbp - %d]", e->anno_int);
		break;
	case EX_FIELD:
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    mov rax, [rax + %d]", e->anno_int);
		break;
	case EX_UNARY:
		cg_expr(cg,tt,e->lhs);
		cg_emit(cg,"    neg rax");
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
		else
		{
			FuncInfo *fi=types_find_func(tt,e->name);
			cg_call_with_args(cg,tt,fi->asm_label,NULL,e->args,e->arg_count,0, e->type.kind==TY_OBJECT);
		}
		break;
	}
}

static void cg_block(Codegen *cg, TypeTable *tt, Func *f, Block *b, int in_main);

static void cg_store(Codegen *cg, TypeTable *tt, Expr *target)
{
	if (target->kind==EX_IDENT)
	{
		cg_emit(cg,"    mov [rbp - %d], rax", target->anno_int);
	}
	else     /* EX_FIELD */
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
			if (s->decl_type.kind==TY_OBJECT)
			{
				cg_expr_owned(cg,tt,s->decl_init);
			}
			else
			{
				cg_expr(cg,tt,s->decl_init);
			}

			cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
		}
		break;
	case ST_ASSIGN:
		if (s->target->type.kind==TY_OBJECT)
		{
			cg_assign_object(cg,tt,s->target,s->value);
		}
		else
		{
			cg_expr(cg,tt,s->value);
			cg_store(cg,tt,s->target);
		}
		break;
	case ST_EXPR:
		cg_expr(cg,tt,s->expr);
		if (s->expr->type.kind==TY_OBJECT && expr_is_owned(s->expr))
		{
			cg_emit(cg,"    mov rcx, rax");
			cg_release_rcx(cg);
		}
		break;
	case ST_RETURN:
		if (s->ret_val && s->ret_val->type.kind==TY_OBJECT)
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
	int frame = locals + 64;        /* Reserve scratch above locals: sp_save, val_save, four arg temps, and assign_save. */

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
		cg_emit(cg,"    mov [rbp - %d], %s", slot, ARG_REG[reg]);
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
	int nobj=0;
	for (int i=0; i<c->field_count; i++)
	{
		if (c->fields[i].type.kind==TY_OBJECT)
		{
			nobj++;
		}
	}

	cg_emit(cg,"    dq %d", nobj);
	for (int i=0; i<c->field_count; i++)
	{
		if (c->fields[i].type.kind==TY_OBJECT)
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
	cg_emit(cg,"extern printf");
	cg_emit(cg,"extern bzy_alloc");
	cg_emit(cg,"extern bzy_retain");
	cg_emit(cg,"extern bzy_release");
	cg_emit(cg,"extern bzy_live_count");
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

	cg_emit(cg,"__fmt_int: db \"%%lld\", 10, 0");
}
