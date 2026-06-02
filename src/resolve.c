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

/* StringBuilder is a runtime-provided builtin object class (not user-declared),
   so its type, new, and methods are special-cased rather than table-resolved. */
static int is_stringbuilder(const TypeRef *t)
{
	return t->kind==TY_OBJECT && strcmp(t->class_name,"StringBuilder")==0;
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
/* Structural type equality (used for invariant array element comparison). */
static int typeref_equal(const TypeRef *x, const TypeRef *y)
{
	if (x->kind != y->kind)
	{
		return 0;
	}

	if (x->kind==TY_OBJECT)
	{
		return strcmp(x->class_name,y->class_name)==0;
	}

	if (x->kind==TY_ARRAY)
	{
		return typeref_equal(x->elem,y->elem);
	}

	if (x->kind==TY_MAP)
	{
		return typeref_equal(x->elem,y->elem) && typeref_equal(x->elem2,y->elem2);
	}

	return 1;
}

static int assignable(const TypeRef *to, const TypeRef *from)
{
	if (to->kind==TY_ARRAY && from->kind==TY_ARRAY)
	{
		return typeref_equal(to,from);   /* invariant: int[]!=long[], Dog[]!=Animal[] */
	}

	if (to->kind==TY_MAP && from->kind==TY_MAP)
	{
		return typeref_equal(to,from);   /* invariant on both key and value. */
	}

	if (to->kind == from->kind)
	{
		return 1;   /* same scalar/bool/object-by-kind/string/void */
	}

	if (ty_is_int(to->kind) && ty_is_int(from->kind))
	{
		return ty_is_signed(to->kind) == ty_is_signed(from->kind) && ty_rank(from->kind) <= ty_rank(to->kind);
	}

	if (to->kind==TY_DOUBLE && ty_is_int(from->kind))
	{
		return 1;   /* Implicit int->double widening. */
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
	case EX_FLOAT:
		e->type.kind = (e->int_suffix[0]=='f') ? TY_FLOAT : TY_DOUBLE;
		break;
	case EX_STR:
		e->type.kind=TY_STRING;
		break;
	case EX_NEWARRAY:
		resolve_expr(st,e->lhs,tc);                 /* the count */
		if (!ty_is_int(e->lhs->type.kind))
		{
			die(e->line,"array length must be an integer",NULL);
		}

		if (e->type.elem->kind==TY_OBJECT
				&& !is_stringbuilder(e->type.elem)
				&& !types_find_class(g_types,e->type.elem->class_name))
		{
			die(e->line,"unknown array element type: ",e->type.elem->class_name);
		}

		break;
	case EX_NEWMAP:
		if (e->type.elem->kind!=TY_INT && e->type.elem->kind!=TY_STRING)
		{
			die(e->line,"map key must be int or string",NULL);
		}

		if (e->type.elem2->kind==TY_OBJECT
				&& !is_stringbuilder(e->type.elem2)
				&& !types_find_class(g_types,e->type.elem2->class_name))
		{
			die(e->line,"unknown map value type: ",e->type.elem2->class_name);
		}

		break;   /* e->type is already TY_MAP + key/value, set by the parser. */
	case EX_INDEX:
		resolve_expr(st,e->lhs,tc);
		resolve_expr(st,e->rhs,tc);
		if (e->lhs->type.kind!=TY_ARRAY)
		{
			die(e->line,"indexing a non-array",NULL);
		}

		if (!ty_is_int(e->rhs->type.kind))
		{
			die(e->line,"array index must be an integer",NULL);
		}

		e->type = *e->lhs->type.elem;
		break;
	case EX_CAST:
	{
		resolve_expr(st,e->lhs,tc);
		TypeKind to=e->type.kind, from=e->lhs->type.kind;   /* target set by parser. */
		int to_num=ty_is_int(to) || ty_is_float(to);
		int from_num=ty_is_int(from) || ty_is_float(from);
		if (to==TY_BOOL || from==TY_BOOL)
		{
			if (to != from)
			{
				die(e->line,"cannot cast between boolean and a number",NULL);
			}
		}
		else if (!to_num || !from_num)
		{
			die(e->line,"cast operand and target must be scalar numbers",NULL);
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
		if (strcmp(e->name,"StringBuilder")!=0 && !types_find_class(g_types,e->name))
		{
			die(e->line,"unknown class: ",e->name);
		}

		e->type.kind=TY_OBJECT;
		strcpy(e->type.class_name,e->name);
		break;
	case EX_UNARY:
		resolve_expr(st,e->lhs,tc);
		if (!ty_is_int(e->lhs->type.kind) && !ty_is_float(e->lhs->type.kind))
		{
			die(e->line,"unary '-' requires a numeric operand",NULL);
		}

		e->type=e->lhs->type;
		break;
	case EX_BINARY:
	{
		resolve_expr(st,e->lhs,tc);
		resolve_expr(st,e->rhs,tc);
		TypeKind a=e->lhs->type.kind, b=e->rhs->type.kind;
		if (a==TY_STRING || b==TY_STRING)
		{
			if (e->op!=TOKEN_PLUS || a!=TY_STRING || b!=TY_STRING)
			{
				die(e->line,"strings support only '+' concatenation of two strings",NULL);
			}

			e->type.kind=TY_STRING;
			break;
		}

		if (ty_is_float(a) || ty_is_float(b))
		{
			TypeKind ft;
			if (ty_is_float(a) && ty_is_float(b))
			{
				if (a != b)
				{
					die(e->line,"mix of float and double; add a cast",NULL);
				}

				ft = a;
			}
			else
			{
				/* One operand is floating, the other must be an integer that
				   promotes; only int->double is implicit, so float+int errors. */
				TypeKind fk = ty_is_float(a) ? a : b;
				TypeKind ik = ty_is_float(a) ? b : a;
				if (!ty_is_int(ik))
				{
					die(e->line,"non-numeric operand",NULL);
				}

				if (fk==TY_FLOAT)
				{
					die(e->line,"mix of float and integer; add a cast",NULL);
				}

				ft = TY_DOUBLE;
			}

			int cmp = e->op==TOKEN_EQ || e->op==TOKEN_NEQ || e->op==TOKEN_LT
					  || e->op==TOKEN_GT || e->op==TOKEN_LTE || e->op==TOKEN_GTE;
			e->type.kind = cmp ? TY_BOOL : ft;
			break;
		}

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
		if (e->lhs->type.kind==TY_ARRAY)
		{
			if (strcmp(e->name,"length")!=0)
			{
				die(e->line,"arrays have only '.length'",NULL);
			}

			e->type.kind=TY_INT;
			e->anno_int=24;             /* The length field offset. */
			break;
		}

		if (e->lhs->type.kind==TY_MAP)
		{
			if (strcmp(e->name,"size")!=0)
			{
				die(e->line,"maps have only '.size'",NULL);
			}

			e->type.kind=TY_INT;
			e->anno_int=24;             /* The size field offset. */
			break;
		}

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
		if (e->lhs->type.kind==TY_MAP)
		{
			resolve_args(st,e,tc);
			TypeRef *K=e->lhs->type.elem, *V=e->lhs->type.elem2;
			if (strcmp(e->name,"put")==0)
			{
				if (e->arg_count!=2 || !assignable(K,&e->args[0]->type) || !assignable(V,&e->args[1]->type))
				{
					die(e->line,"map.put(key,value) type mismatch",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"get")==0)
			{
				if (e->arg_count!=1 || !assignable(K,&e->args[0]->type))
				{
					die(e->line,"map.get(key) type mismatch",NULL);
				}

				e->type = *V;
			}
			else if (strcmp(e->name,"has")==0)
			{
				if (e->arg_count!=1 || !assignable(K,&e->args[0]->type))
				{
					die(e->line,"map.has(key) type mismatch",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"remove")==0)
			{
				if (e->arg_count!=1 || !assignable(K,&e->args[0]->type))
				{
					die(e->line,"map.remove(key) type mismatch",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"unknown map method: ",e->name);
			}

			break;
		}

		if (is_stringbuilder(&e->lhs->type))
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"append")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"StringBuilder.append expects one string",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"toString")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"StringBuilder.toString takes no arguments",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else
			{
				die(e->line,"unknown StringBuilder method: ",e->name);
			}

			break;
		}

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
			if (!assignable(&m->param_types[i], &e->args[i]->type))
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
			if (e->arg_count<1)
			{
				die(e->line,"print expects a scalar argument",NULL);
			}

			TypeKind ak=e->args[0]->type.kind;
			if (!ty_is_int(ak) && ak!=TY_BOOL && !ty_is_float(ak) && ak!=TY_STRING)
			{
				die(e->line,"print expects a scalar or string argument",NULL);
			}

			e->type.kind=TY_VOID;
			break;
		}

		if (strcmp(e->name,"length")==0)
		{
			if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
			{
				die(e->line,"length expects one string argument",NULL);
			}

			e->type.kind=TY_INT;
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
				if (!assignable(&fi->param_types[i], &e->args[i]->type))
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
		if (s->decl_type.kind==TY_OBJECT && !is_stringbuilder(&s->decl_type)
				&& !types_find_class(g_types,s->decl_type.class_name))
		{
			die(s->line,"unknown type: ",s->decl_type.class_name);
		}

		if (s->decl_init)
		{
			resolve_expr(st,s->decl_init,tc);
			if (!assignable(&s->decl_type, &s->decl_init->type))
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
		if (!assignable(&s->target->type, &s->value->type))
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
			if (!assignable(g_ret, &s->ret_val->type))
			{
				die(s->line,"return type does not match; add a cast",NULL);
			}
		}
		break;
	case ST_EXPR:
		resolve_expr(st,s->expr,tc);
		break;
	case ST_FOREACH:
		die(s->line,"foreach resolve arrives in Part 4d Task 2",NULL);
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
