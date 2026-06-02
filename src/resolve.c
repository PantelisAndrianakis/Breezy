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
static int g_loop_depth;       /* >0 inside a while/for/foreach body; gates continue. */
static int g_break_depth;      /* >0 inside a loop OR switch body; gates break. */

static void die(int line, const char *msg, const char *arg)
{
	fprintf(stderr,"line %d: %s%s\n",line,msg,arg?arg:"");
	exit(1);
}

static ClassInfo *class_of(const TypeRef *t)
{
	return t->kind==TY_OBJECT ? types_find_class(g_types,t->class_name) : NULL;
}

static int class_is_exception(ClassInfo *c)
{
	for (; c; c=c->parent)
	{
		if (strcmp(c->name,"Exception")==0)
		{
			return 1;
		}
	}

	return 0;
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

	if (x->kind==TY_GENERIC)
	{
		return strcmp(x->class_name,y->class_name)==0 && typeref_equal(x->elem,y->elem);
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

	if (to->kind==TY_GENERIC && from->kind==TY_GENERIC)
	{
		return typeref_equal(to,from);   /* invariant on template + element. */
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

static void resolve_math(Expr *e)
{
	const char *m = e->name + 5;   /* After "Math.". */
	if (strcmp(m,"abs")==0)
	{
		if (e->arg_count!=1)
		{
			die(e->line,"Math.abs takes one argument",NULL);
		}

		TypeKind k=e->args[0]->type.kind;
		if (!ty_is_int(k) && !ty_is_float(k))
		{
			die(e->line,"Math.abs requires a number",NULL);
		}

		e->type.kind = ty_is_int(k) ? k : TY_DOUBLE;   /* float promotes to double */
		return;
	}

	if (strcmp(m,"sqrt")==0 || strcmp(m,"floor")==0 || strcmp(m,"ceil")==0
			|| strcmp(m,"round")==0 || strcmp(m,"toRadians")==0
			|| strcmp(m,"cos")==0 || strcmp(m,"tan")==0 || strcmp(m,"exp")==0)
	{
		if (e->arg_count!=1)
		{
			die(e->line,"this Math function takes one argument",NULL);
		}

		TypeKind k=e->args[0]->type.kind;
		if (!ty_is_int(k) && !ty_is_float(k))
		{
			die(e->line,"this Math function requires a number",NULL);
		}

		e->type.kind = TY_DOUBLE;
		return;
	}

	if (strcmp(m,"pow")==0)
	{
		if (e->arg_count!=2)
		{
			die(e->line,"Math.pow takes two arguments",NULL);
		}

		for (int i=0; i<2; i++)
		{
			TypeKind k=e->args[i]->type.kind;
			if (!ty_is_int(k) && !ty_is_float(k))
			{
				die(e->line,"Math.pow requires numbers",NULL);
			}
		}

		e->type.kind = TY_DOUBLE;
		return;
	}

	if (strcmp(m,"min")==0 || strcmp(m,"max")==0 || strcmp(m,"clamp")==0)
	{
		int n = strcmp(m,"clamp")==0 ? 3 : 2;
		if (e->arg_count!=n)
		{
			die(e->line,"Math.min/max/clamp argument count",NULL);
		}

		int allint=1;
		for (int i=0; i<n; i++)
		{
			TypeKind k=e->args[i]->type.kind;
			if (!ty_is_int(k) && !ty_is_float(k))
			{
				die(e->line,"Math.min/max/clamp require numbers",NULL);
			}

			if (!ty_is_int(k) || k!=e->args[0]->type.kind)
			{
				allint=0;
			}
		}

		e->type.kind = allint ? e->args[0]->type.kind : TY_DOUBLE;
		return;
	}

	die(e->line,"unknown Math method: ",m);
}

static void resolve_clock(Expr *e)
{
	const char *m = e->name + 6;   /* After "Clock.". */
	if (strcmp(m,"currentTimeMillis")!=0 && strcmp(m,"currentTimeNanos")!=0)
	{
		die(e->line,"unknown Clock method: ",m);
	}

	if (e->arg_count!=0)
	{
		die(e->line,"Clock methods take no arguments",NULL);
	}

	e->type.kind = TY_LONG;
}

static void resolve_regex(Expr *e)
{
	const char *m = e->name + 6;   /* After "Regex.". */
	int predicate = (strcmp(m,"matches")==0 || strcmp(m,"test")==0);
	int find = strcmp(m,"find")==0;
	int replace = strcmp(m,"replace")==0;
	if (!predicate && !find && !replace)
	{
		die(e->line,"unknown Regex method: ",m);
	}

	int want = replace ? 3 : 2;
	if (e->arg_count != want)
	{
		die(e->line,"wrong number of arguments for this Regex method",NULL);
	}

	for (int i=0; i<e->arg_count; i++)
	{
		if (e->args[i]->type.kind != TY_STRING)
		{
			die(e->line,"Regex arguments must be strings",NULL);
		}
	}

	e->type.kind = predicate ? TY_BOOL : TY_STRING;
}

static void resolve_random(Expr *e)
{
	const char *m = e->name + 7;   /* After "Random.". */
	if (strcmp(m,"nextBoolean")==0)
	{
		e->type.kind = TY_BOOL;
	}
	else if (strcmp(m,"nextInt")==0)
	{
		e->type.kind = TY_INT;
	}
	else if (strcmp(m,"nextLong")==0)
	{
		e->type.kind = TY_LONG;
	}
	else if (strcmp(m,"nextFloat")==0)
	{
		e->type.kind = TY_FLOAT;
	}
	else if (strcmp(m,"nextDouble")==0 || strcmp(m,"nextGaussian")==0)
	{
		e->type.kind = TY_DOUBLE;
	}
	else if (strcmp(m,"nextBytes")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_ARRAY || e->args[0]->type.elem->kind!=TY_BYTE)
		{
			die(e->line,"Random.nextBytes expects a byte[]",NULL);
		}

		e->type.kind = TY_VOID;
	}
	else if (strcmp(m,"get")==0)
	{
		if (e->arg_count!=1 && e->arg_count!=2)
		{
			die(e->line,"Random.get takes one or two arguments",NULL);
		}

		TypeKind k=e->args[0]->type.kind;
		if (k!=TY_INT && k!=TY_LONG && k!=TY_FLOAT && k!=TY_DOUBLE)
		{
			die(e->line,"Random.get bound must be int, long, float, or double",NULL);
		}

		if (e->arg_count==2 && e->args[1]->type.kind!=k)
		{
			die(e->line,"Random.get(origin, bound) arguments must be the same type",NULL);
		}

		e->type.kind = k;
	}
	else
	{
		die(e->line,"unknown Random method: ",m);
	}

	if (e->arg_count!=0 && strcmp(m,"get")!=0 && strcmp(m,"nextBytes")!=0)
	{
		die(e->line,"this Random method takes no arguments",NULL);
	}
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
	case EX_NEWGEN:
	{
		const char *tmpl=e->type.class_name;
		TypeKind ek=e->type.elem->kind;
		int scalar_or_obj = ty_is_int(ek) || ty_is_float(ek) || ek==TY_BOOL || ek==TY_STRING || ek==TY_OBJECT;
		if (!scalar_or_obj)
		{
			die(e->line,"collection element must be a scalar, string, or object",NULL);
		}

		if (strcmp(tmpl,"Set")==0 && ek!=TY_INT && ek!=TY_STRING)
		{
			die(e->line,"Set element must be int or string (hashable)",NULL);
		}

		if (ek==TY_OBJECT
				&& !is_stringbuilder(e->type.elem)
				&& !types_find_class(g_types,e->type.elem->class_name))
		{
			die(e->line,"unknown collection element type: ",e->type.elem->class_name);
		}

		break;   /* e->type already TY_GENERIC + elem, set by the parser. */
	}
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
	case EX_INCDEC:
		resolve_expr(st,e->lhs,tc);
		if (e->lhs->kind!=EX_IDENT || !ty_is_int(e->lhs->type.kind))
		{
			die(e->line,"'++'/'--' requires an integer variable",NULL);
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

		if (e->lhs->type.kind==TY_GENERIC)
		{
			if (strcmp(e->name,"size")!=0)
			{
				die(e->line,"collections have only '.size'",NULL);
			}

			e->type.kind=TY_INT;
			e->anno_int=24;             /* length (vector) / size (map) field offset. */
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
		if (e->lhs->type.kind==TY_GENERIC)
		{
			resolve_args(st,e,tc);
			const char *tmpl=e->lhs->type.class_name;
			TypeRef *T=e->lhs->type.elem;
			const char *nm=e->name;

			/* Shared by all: contains(T) -> bool. */
			if (strcmp(nm,"contains")==0)
			{
				if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
				{
					die(e->line,"contains(value) type mismatch",NULL);
				}

				e->type.kind=TY_BOOL;
				break;
			}

			if (strcmp(tmpl,"Box")==0)
			{
				if (strcmp(nm,"set")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"Box.set type mismatch",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else if (strcmp(nm,"get")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"Box.get takes no arguments",NULL);
					}

					e->type=*T;
				}
				else
				{
					die(e->line,"unknown Box method: ",nm);
				}

				break;
			}

			if (strcmp(tmpl,"Set")==0)
			{
				if (strcmp(nm,"add")==0 || strcmp(nm,"remove")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"Set op type mismatch",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else
				{
					die(e->line,"unknown Set method: ",nm);
				}

				break;
			}

			/* Vector-backed: List / Stack / Queue / Deque / ArrayDeque. */
			int add1   = strcmp(nm,"add")==0 || strcmp(nm,"push")==0 || strcmp(nm,"enqueue")==0
						 || strcmp(nm,"addFirst")==0 || strcmp(nm,"addLast")==0;
			int takeT  = strcmp(nm,"pop")==0 || strcmp(nm,"dequeue")==0 || strcmp(nm,"peek")==0
						 || strcmp(nm,"removeFirst")==0 || strcmp(nm,"removeLast")==0
						 || strcmp(nm,"peekFirst")==0 || strcmp(nm,"peekLast")==0;
			if (add1)
			{
				if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
				{
					die(e->line,"add(value) type mismatch",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (takeT)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"this method takes no arguments",NULL);
				}

				e->type=*T;
			}
			else if (strcmp(nm,"get")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"get(index) needs an integer",NULL);
				}

				e->type=*T;
			}
			else if (strcmp(nm,"set")==0)
			{
				if (e->arg_count!=2 || !ty_is_int(e->args[0]->type.kind) || !assignable(T,&e->args[1]->type))
				{
					die(e->line,"set(index,value) type mismatch",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(nm,"removeAt")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"removeAt(index) needs an integer",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(nm,"indexOf")==0)
			{
				if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
				{
					die(e->line,"indexOf(value) type mismatch",NULL);
				}

				e->type.kind=TY_INT;
			}
			else
			{
				die(e->line,"unknown collection method: ",nm);
			}

			break;
		}

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
		if (strncmp(e->name,"Math.",5)==0)
		{
			resolve_math(e);
			break;
		}

		if (strncmp(e->name,"Clock.",6)==0)
		{
			resolve_clock(e);
			break;
		}

		if (strncmp(e->name,"Random.",7)==0)
		{
			resolve_random(e);
			break;
		}

		if (strncmp(e->name,"Regex.",6)==0)
		{
			resolve_regex(e);
			break;
		}

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

		g_loop_depth++;
		g_break_depth++;
		resolve_block(st,s->then_blk,tc);
		g_loop_depth--;
		g_break_depth--;
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
	{
		resolve_expr(st,s->expr,tc);
		TypeKind ik=s->expr->type.kind;
		TypeRef elem;
		elem.kind=TY_VOID;
		elem.class_name[0]='\0';
		elem.elem=NULL;
		elem.elem2=NULL;
		if (ik==TY_ARRAY || ik==TY_MAP)
		{
			elem=*s->expr->type.elem;   /* Array element, or map key. */
		}
		else if (ik==TY_STRING)
		{
			elem.kind=TY_INT;           /* One byte per step, as an int. */
		}
		else if (ik==TY_GENERIC && strcmp(s->expr->type.class_name,"Box")!=0)
		{
			elem=*s->expr->type.elem;   /* List/Stack/Queue/Deque/Set yield T. */
		}
		else
		{
			die(s->line,"foreach requires an array, string, map, or collection",NULL);
		}

		if (!assignable(&s->decl_type,&elem))
		{
			die(s->line,"foreach loop variable type does not match the element type",NULL);
		}

		Symbol *lv=sym_add(st,s->decl_name,s->decl_type);
		s->decl_offset=lv->offset;
		s->fe_coll_offset=sym_add(st,"",s->expr->type)->offset;
		s->fe_index_offset=sym_add(st,"",s->decl_type)->offset;
		s->fe_len_offset=sym_add(st,"",s->decl_type)->offset;
		s->fe_aux_offset=sym_add(st,"",s->decl_type)->offset;
		g_loop_depth++;
		g_break_depth++;
		resolve_block(st,s->then_blk,tc);
		g_loop_depth--;
		g_break_depth--;
		break;
	}
	case ST_BREAK:
		if (g_break_depth==0)
		{
			die(s->line,"break outside a loop or switch",NULL);
		}
		break;
	case ST_CONTINUE:
		if (g_loop_depth==0)
		{
			die(s->line,"continue outside a loop",NULL);
		}
		break;
	case ST_FOR:
		resolve_stmt(st,s->for_init,tc);
		resolve_expr(st,s->cond,tc);
		if (s->cond->type.kind!=TY_BOOL)
		{
			die(s->line,"'for' condition must be boolean",NULL);
		}

		resolve_stmt(st,s->for_post,tc);
		g_loop_depth++;
		g_break_depth++;
		resolve_block(st,s->then_blk,tc);
		g_loop_depth--;
		g_break_depth--;
		break;
	case ST_SWITCH:
	{
		resolve_expr(st,s->cond,tc);
		if (!ty_is_int(s->cond->type.kind))
		{
			die(s->line,"switch operand must be an integer",NULL);
		}

		long long seen[256];
		int nseen=0, ndefault=0;
		Block *b=s->then_blk;
		g_break_depth++;
		for (int i=0; i<b->count; i++)
		{
			Stmt *c=b->stmts[i];
			if (c->kind==ST_CASE)
			{
				for (int j=0; j<nseen; j++)
				{
					if (seen[j]==c->value->int_val)
					{
						die(c->line,"duplicate case value in switch",NULL);
					}
				}

				if (nseen<256)
				{
					seen[nseen++]=c->value->int_val;
				}
			}
			else if (c->kind==ST_DEFAULT)
			{
				if (ndefault++ > 0)
				{
					die(c->line,"more than one default in switch",NULL);
				}
			}
			else
			{
				resolve_stmt(st,c,tc);
			}
		}

		g_break_depth--;
		break;
	}
	case ST_CASE:
	case ST_DEFAULT:
		break;   /* Resolved as part of the enclosing switch body. */
	case ST_THROW:
	{
		resolve_expr(st,s->expr,tc);
		ClassInfo *c = s->expr->type.kind==TY_OBJECT ? class_of(&s->expr->type) : NULL;
		if (!c || !class_is_exception(c))
		{
			die(s->line,"thrown value must be an Exception (or subclass)",NULL);
		}

		break;
	}
	case ST_TRY:
	{
		resolve_block(st,s->then_blk,tc);
		for (int i=0; i<s->else_blk->count; i++)
		{
			Stmt *c=s->else_blk->stmts[i];
			ClassInfo *cc = c->decl_type.kind==TY_OBJECT ? class_of(&c->decl_type) : NULL;
			if (!cc || !class_is_exception(cc))
			{
				die(c->line,"catch type must be an Exception (or subclass)",NULL);
			}

			Symbol *cv=sym_add(st,c->decl_name,c->decl_type);
			c->decl_offset=cv->offset;
			resolve_block(st,c->then_blk,tc);
		}

		break;
	}
	case ST_CATCH:
		break;   /* Resolved as part of the enclosing try. */
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
	g_loop_depth=0;
	g_break_depth=0;
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
