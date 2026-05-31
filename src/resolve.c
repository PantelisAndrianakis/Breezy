#include "resolve.h"
#include "symtable.h"
#include "ownership.h"
#include "escape.h"
#include "lexer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static TypeTable *g_types;
static const TypeRef *g_ret;   /* Return type of the function being resolved. */

static void die(int line, const char *msg, const char *arg)
{
	fprintf(stderr,"line %d: %s%s\n",line,msg,arg?arg:"");
	exit(1);
}

static ClassInfo *class_of(const TypeRef *t)
{
	return t->kind==TY_OBJECT ? types_find_class(g_types,t->class_name) : NULL;
}

/* Type of an integer literal from its suffix; range-checks the unsuffixed form. */
static TypeKind literal_type(Expr *e)
{
	int has_u=0, has_l=0, n=0;
	for (const char *p=e->int_suffix; *p; p++)
	{
		n++;
		if (*p=='u' || *p=='U')
		{
			has_u++;
		}
		else
		{
			has_l++;
		}
	}

	if (n>2 || has_u>1 || has_l>1)
	{
		die(e->line,"malformed integer literal suffix: ",e->int_suffix);
	}

	if (has_u && has_l)
	{
		return TY_ULONG;
	}

	if (has_u)
	{
		return TY_UINT;
	}

	if (has_l)
	{
		return TY_LONG;
	}

	if (e->int_val > 2147483647LL)
	{
		die(e->line,"integer literal out of int range; add an 'L' suffix",NULL);
	}

	return TY_INT;
}

/* Can a value of kind 'from' be stored into a slot of kind 'to' without a cast?
   Scalars widen only within the same signedness; objects stay permissive (subtype
   checking is out of scope). Boolean<->integer and narrowing need an explicit cast. */
static int assignable(TypeKind to, TypeKind from)
{
	if (to == from)
	{
		return 1;
	}

	if (ty_is_int(to) && ty_is_int(from))
	{
		return ty_is_signed(to) == ty_is_signed(from) && ty_rank(from) <= ty_rank(to);
	}

	return 0;
}

static void resolve_expr(SymTable *st, Expr *e, const char *this_class);

static void resolve_args(SymTable *st, Expr *e, const char *tc)
{
	for (int i=0; i<e->arg_count; i++)
	{
		resolve_expr(st,e->args[i],tc);
	}
}

static void resolve_expr(SymTable *st, Expr *e, const char *tc)
{
	switch (e->kind)
	{
	case EX_INT:
		e->type.kind=literal_type(e);
		break;
	case EX_BOOL:
		e->type.kind=TY_BOOL;
		break;
	case EX_CAST:
	{
		resolve_expr(st,e->lhs,tc);
		TypeKind to=e->type.kind, from=e->lhs->type.kind;   /* target set by parser. */
		if (!ty_is_int(to) && to!=TY_BOOL)
		{
			die(e->line,"cast target must be a scalar type",NULL);
		}

		if (!ty_is_int(from) && from!=TY_BOOL)
		{
			die(e->line,"cannot cast a non-scalar value",NULL);
		}

		if ((to==TY_BOOL) != (from==TY_BOOL))
		{
			die(e->line,"cannot cast between boolean and integer",NULL);
		}

		break;
	}
	case EX_THIS:
		if (!tc)
		{
			die(e->line,"'this' outside a method",NULL);
		}

		e->type.kind=TY_OBJECT;
		strcpy(e->type.class_name,tc);
		break;
	case EX_IDENT:
	{
		Symbol *s=sym_find(st,e->name);
		if (!s)
		{
			die(e->line,"unknown variable: ",e->name);
		}

		e->type=s->type;
		e->anno_int=s->offset;
		break;
	}
	case EX_NEW:
		if (!types_find_class(g_types,e->name))
		{
			die(e->line,"unknown class: ",e->name);
		}

		e->type.kind=TY_OBJECT;
		strcpy(e->type.class_name,e->name);
		break;
	case EX_UNARY:
		resolve_expr(st,e->lhs,tc);
		if (!ty_is_int(e->lhs->type.kind))
		{
			die(e->line,"unary '-' requires an integer operand",NULL);
		}

		e->type=e->lhs->type;
		break;
	case EX_BINARY:
	{
		resolve_expr(st,e->lhs,tc);
		resolve_expr(st,e->rhs,tc);
		TypeKind a=e->lhs->type.kind, b=e->rhs->type.kind;
		if (e->op==TOKEN_EQ || e->op==TOKEN_NEQ)
		{
			int both_int=ty_is_int(a) && ty_is_int(b);
			if (both_int && ty_is_signed(a)!=ty_is_signed(b))
			{
				die(e->line,"mixed signedness in comparison; add a cast",NULL);
			}

			if (!both_int && !(a==TY_BOOL && b==TY_BOOL) && !(a==TY_OBJECT && b==TY_OBJECT))
			{
				die(e->line,"'==' operands are not comparable",NULL);
			}

			e->type.kind=TY_BOOL;
			break;
		}

		if (e->op==TOKEN_LT || e->op==TOKEN_GT || e->op==TOKEN_LTE || e->op==TOKEN_GTE)
		{
			if (!ty_is_int(a) || !ty_is_int(b))
			{
				die(e->line,"relational operands must be integers",NULL);
			}

			if (ty_is_signed(a)!=ty_is_signed(b))
			{
				die(e->line,"mixed signedness in comparison; add a cast",NULL);
			}

			e->type.kind=TY_BOOL;
			break;
		}

		if (!ty_is_int(a) || !ty_is_int(b))
		{
			die(e->line,"arithmetic operands must be integers",NULL);
		}

		if (ty_is_signed(a)!=ty_is_signed(b))
		{
			die(e->line,"mixed signedness in arithmetic; add a cast",NULL);
		}

		e->type.kind = ty_rank(a)>=ty_rank(b) ? a : b;   /* Wider operand wins. */
		break;
	}
	case EX_FIELD:
	{
		resolve_expr(st,e->lhs,tc);
		ClassInfo *c=class_of(&e->lhs->type);
		if (!c)
		{
			die(e->line,"field access on non-object",NULL);
		}

		FieldInfo *f=types_find_field(c,e->name);
		if (!f)
		{
			die(e->line,"unknown field: ",e->name);
		}

		e->type=f->type;
		e->anno_int=f->offset;
		break;
	}
	case EX_METHOD_CALL:
	{
		resolve_expr(st,e->lhs,tc);
		ClassInfo *c=class_of(&e->lhs->type);
		if (!c)
		{
			die(e->line,"method call on non-object",NULL);
		}

		MethodInfo *m=types_find_method(c,e->name);
		if (!m)
		{
			die(e->line,"unknown method: ",e->name);
		}

		resolve_args(st,e,tc);
		for (int i=0; i<e->arg_count && i<m->param_count; i++)
		{
			if (!assignable(m->param_types[i].kind, e->args[i]->type.kind))
			{
				die(e->line,"argument type mismatch; add a cast",NULL);
			}
		}

		e->type=m->ret_type;
		e->anno_int=m->vtable_slot;
		strcpy(e->anno_str,c->name);
		break;
	}
	case EX_CALL:
		resolve_args(st,e,tc);
		if (strcmp(e->name,"print")==0)
		{
			if (e->arg_count<1
					|| (!ty_is_int(e->args[0]->type.kind) && e->args[0]->type.kind!=TY_BOOL))
			{
				die(e->line,"print expects a scalar argument",NULL);
			}

			e->type.kind=TY_VOID;
			break;
		}

		if (strcmp(e->name,"liveCount")==0)
		{
			e->type.kind=TY_INT;
			break;
		}

		if (strcmp(e->name,"collectCycles")==0)
		{
			e->type.kind=TY_VOID;
			break;
		}

		{
			FuncInfo *fi=types_find_func(g_types,e->name);
			if (!fi)
			{
				die(e->line,"unknown function: ",e->name);
			}

			for (int i=0; i<e->arg_count && i<fi->param_count; i++)
			{
				if (!assignable(fi->param_types[i].kind, e->args[i]->type.kind))
				{
					die(e->line,"argument type mismatch; add a cast",NULL);
				}
			}

			e->type=fi->ret_type;
		}
		break;
	}
}

static void resolve_block(SymTable *st, Block *b, const char *tc);

static void resolve_stmt(SymTable *st, Stmt *s, const char *tc)
{
	switch (s->kind)
	{
	case ST_VARDECL:
	{
		if (s->decl_type.kind==TY_OBJECT && !types_find_class(g_types,s->decl_type.class_name))
		{
			die(s->line,"unknown type: ",s->decl_type.class_name);
		}

		if (s->decl_init)
		{
			resolve_expr(st,s->decl_init,tc);
			if (!assignable(s->decl_type.kind, s->decl_init->type.kind))
			{
				die(s->line,"initializer type does not match; add a cast",NULL);
			}
		}

		Symbol *sym=sym_add(st,s->decl_name,s->decl_type);
		s->decl_offset=sym->offset;
		break;
	}
	case ST_ASSIGN:
		resolve_expr(st,s->target,tc);
		resolve_expr(st,s->value,tc);
		if (!assignable(s->target->type.kind, s->value->type.kind))
		{
			die(s->line,"assigned value type does not match; add a cast",NULL);
		}
		break;
	case ST_IF:
		resolve_expr(st,s->cond,tc);
		if (s->cond->type.kind!=TY_BOOL)
		{
			die(s->line,"'if' condition must be boolean",NULL);
		}

		resolve_block(st,s->then_blk,tc);
		if (s->else_blk)
		{
			resolve_block(st,s->else_blk,tc);
		}
		break;
	case ST_WHILE:
		resolve_expr(st,s->cond,tc);
		if (s->cond->type.kind!=TY_BOOL)
		{
			die(s->line,"'while' condition must be boolean",NULL);
		}

		resolve_block(st,s->then_blk,tc);
		break;
	case ST_RETURN:
		if (s->ret_val)
		{
			resolve_expr(st,s->ret_val,tc);
			if (!assignable(g_ret->kind, s->ret_val->type.kind))
			{
				die(s->line,"return type does not match; add a cast",NULL);
			}
		}
		break;
	case ST_EXPR:
		resolve_expr(st,s->expr,tc);
		break;
	}
}

static void resolve_block(SymTable *st, Block *b, const char *tc)
{
	for (int i=0; i<b->count; i++)
	{
		resolve_stmt(st,b->stmts[i],tc);
	}
}

void resolve_func(TypeTable *tt, Func *f, const char *this_class)
{
	g_types=tt;
	g_ret=&f->ret_type;
	SymTable st;
	sym_init(&st);
	if (this_class)
	{
		TypeRef tr;
		tr.kind=TY_OBJECT;
		strcpy(tr.class_name,this_class);
		sym_add(&st,"this",tr);
	}

	for (int i=0; i<f->param_count; i++)
	{
		sym_add(&st,f->params[i].name,f->params[i].type);
	}

	resolve_block(&st,f->body,this_class);
	f->frame_size=sym_frame_size(&st);
	ownership_annotate(f);
	escape_annotate(g_types,f);
}

void resolve_program(TypeTable *tt, Unit **units, int unit_count)
{
	for (int i=0; i<unit_count; i++)
	{
		Unit *u=units[i];
		for (int k=0; k<u->func_count; k++)
		{
			resolve_func(tt,u->funcs[k],NULL);
		}

		if (u->klass)
		{
			for (int k=0; k<u->klass->method_count; k++)
			{
				resolve_func(tt,u->klass->methods[k],u->klass->name);
			}
		}
	}
}
