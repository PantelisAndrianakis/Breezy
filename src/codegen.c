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
                              Expr *self, Expr **args, int argc, int indirect)
{
	int total = (self?1:0) + argc;
	if (total > 4)
	{
		fprintf(stderr,"codegen: >4 args unsupported in core plan\n");
		exit(1);
	}
	if (indirect) cg_emit(cg,"    push rax");          /* callee addr */
	if (self)
	{
		cg_expr(cg,tt,self);
		cg_emit(cg,"    push rax");
	}
	for (int i=0; i<argc; i++)
	{
		cg_expr(cg,tt,args[i]);
		cg_emit(cg,"    push rax");
	}
	for (int i=total-1; i>=0; i--) cg_emit(cg,"    pop %s", ARG_REG[i]);
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
}

static void cg_method_call(Codegen *cg, TypeTable *tt, Expr *e)
{
	cg_expr(cg,tt,e->lhs);                       /* receiver ptr in rax */
	cg_emit(cg,"    mov rax, [rax]");             /* vtable ptr */
	cg_emit(cg,"    mov rax, [rax + %d]", e->anno_int * 8);
	cg_call_with_args(cg,tt,NULL,e->lhs,e->args,e->arg_count,1);
}

static void cg_new(Codegen *cg, TypeTable *tt, Expr *e)
{
	ClassInfo *c=types_find_class(tt,e->name);
	cg_emit(cg,"    mov rcx, %d", c->object_size);
	cg_emit(cg,"    sub rsp, 32");
	cg_emit(cg,"    call malloc");
	cg_emit(cg,"    add rsp, 32");
	cg_emit(cg,"    lea rbx, [rel __vtable_%s]", c->name);
	cg_emit(cg,"    mov [rax], rbx");
	for (int off=8; off<c->object_size; off+=8) cg_emit(cg,"    mov qword [rax + %d], 0", off);
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
		if (strcmp(e->name,"print")==0) cg_print(cg,tt,e);
		else
		{
			FuncInfo *fi=types_find_func(tt,e->name);
			cg_call_with_args(cg,tt,fi->asm_label,NULL,e->args,e->arg_count,0);
		}
		break;
	}
}

static void cg_block(Codegen *cg, TypeTable *tt, Block *b, int in_main);

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
static void cg_stmt(Codegen *cg, TypeTable *tt, Stmt *s, int in_main)
{
	switch (s->kind)
	{
	case ST_VARDECL:
		if (s->decl_init)
		{
			cg_expr(cg,tt,s->decl_init);
			cg_emit(cg,"    mov [rbp - %d], rax", s->decl_offset);
		}
		break;
	case ST_ASSIGN:
		cg_expr(cg,tt,s->value);
		cg_store(cg,tt,s->target);
		break;
	case ST_EXPR:
		cg_expr(cg,tt,s->expr);
		break;
	case ST_RETURN:
		if (s->ret_val) cg_expr(cg,tt,s->ret_val);
		if (in_main) cg_emit(cg,"    xor eax, eax");
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
		cg_block(cg,tt,s->then_blk,in_main);
		if (s->else_blk)
		{
			cg_emit(cg,"    jmp .L%d",end_l);
			cg_emit(cg,".L%d:",else_l);
			cg_block(cg,tt,s->else_blk,in_main);
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
		cg_block(cg,tt,s->then_blk,in_main);
		cg_emit(cg,"    jmp .L%d",top);
		cg_emit(cg,".L%d:",end);
		break;
	}
	}
}
static void cg_block(Codegen *cg, TypeTable *tt, Block *b, int in_main)
{
	for (int i=0; i<b->count; i++) cg_stmt(cg,tt,b->stmts[i],in_main);
}

void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count)
{
	(void)cg;    /* replaced in Task 12 */
	(void)tt;
	(void)units;
	(void)unit_count;
}
