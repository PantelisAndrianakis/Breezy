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
		die(e->line,"Malformed integer literal suffix: ",e->int_suffix);
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
		die(e->line,"Integer literal out of int range; add an 'L' suffix.",NULL);
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
		return typeref_equal(to,from);   /* Invariant: int[]!=long[], Dog[]!=Animal[]. */
	}

	if (to->kind==TY_MAP && from->kind==TY_MAP)
	{
		return typeref_equal(to,from);   /* Invariant on both key and value. */
	}

	if (to->kind==TY_GENERIC && from->kind==TY_GENERIC)
	{
		return typeref_equal(to,from);   /* Invariant on template + element. */
	}

	if (to->kind == from->kind)
	{
		return 1;   /* Same scalar/bool/object-by-kind/string/void. */
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
			die(e->line,"Math.abs takes one argument.",NULL);
		}

		TypeKind k=e->args[0]->type.kind;
		if (!ty_is_int(k) && !ty_is_float(k))
		{
			die(e->line,"Math.abs requires a number.",NULL);
		}

		e->type.kind = ty_is_int(k) ? k : TY_DOUBLE;   /* Float promotes to double. */
		return;
	}

	if (strcmp(m,"sqrt")==0 || strcmp(m,"floor")==0 || strcmp(m,"ceil")==0
			|| strcmp(m,"round")==0 || strcmp(m,"toRadians")==0
			|| strcmp(m,"cos")==0 || strcmp(m,"tan")==0 || strcmp(m,"exp")==0)
	{
		if (e->arg_count!=1)
		{
			die(e->line,"This Math function takes one argument.",NULL);
		}

		TypeKind k=e->args[0]->type.kind;
		if (!ty_is_int(k) && !ty_is_float(k))
		{
			die(e->line,"This Math function requires a number.",NULL);
		}

		e->type.kind = TY_DOUBLE;
		return;
	}

	if (strcmp(m,"pow")==0)
	{
		if (e->arg_count!=2)
		{
			die(e->line,"Math.pow takes two arguments.",NULL);
		}

		for (int i=0; i<2; i++)
		{
			TypeKind k=e->args[i]->type.kind;
			if (!ty_is_int(k) && !ty_is_float(k))
			{
				die(e->line,"Math.pow requires numbers.",NULL);
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
			die(e->line,"Math.min/max/clamp argument count.",NULL);
		}

		int allint=1;
		for (int i=0; i<n; i++)
		{
			TypeKind k=e->args[i]->type.kind;
			if (!ty_is_int(k) && !ty_is_float(k))
			{
				die(e->line,"Math.min/max/clamp require numbers.",NULL);
			}

			if (!ty_is_int(k) || k!=e->args[0]->type.kind)
			{
				allint=0;
			}
		}

		e->type.kind = allint ? e->args[0]->type.kind : TY_DOUBLE;
		return;
	}

	die(e->line,"Unknown Math method: ",m);
}

static void resolve_clock(Expr *e)
{
	const char *m = e->name + 6;   /* After "Clock.". */
	if (strcmp(m,"getDateString")==0)
	{
		if (e->arg_count == 1)
		{
			if (!ty_is_int(e->args[0]->type.kind))
			{
				die(e->line,"Clock.getDateString(millis) expects an integer.",NULL);
			}
		}
		else if (e->arg_count == 2)
		{
			if (!ty_is_int(e->args[0]->type.kind) || e->args[1]->type.kind != TY_STRING)
			{
				die(e->line,"Clock.getDateString(millis, format) expects (integer, string).",NULL);
			}
		}
		else
		{
			die(e->line,"Clock.getDateString takes (millis) or (millis, format).",NULL);
		}

		e->type.kind = TY_STRING;
		return;
	}

	if (strcmp(m,"currentTimeMillis")!=0 && strcmp(m,"currentTimeNanos")!=0)
	{
		die(e->line,"Unknown Clock method: ",m);
	}

	if (e->arg_count!=0)
	{
		die(e->line,"Clock methods take no arguments.",NULL);
	}

	e->type.kind = TY_LONG;
}

static void resolve_string_method(Expr *e)
{
	const char *nm = e->name;
	if (strcmp(nm,"length")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"String.length() takes no arguments.",NULL);
		}

		e->type.kind = TY_INT;
	}
	else if (strcmp(nm,"contains")==0 || strcmp(nm,"startsWith")==0 || strcmp(nm,"endsWith")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_STRING)
		{
			die(e->line,"This string method expects one string argument.",NULL);
		}

		e->type.kind = TY_BOOL;
	}
	else if (strcmp(nm,"indexOf")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_STRING)
		{
			die(e->line,"String.indexOf expects one string argument.",NULL);
		}

		e->type.kind = TY_INT;
	}
	else if (strcmp(nm,"substring")==0)
	{
		if (e->arg_count != 2 || !ty_is_int(e->args[0]->type.kind) || !ty_is_int(e->args[1]->type.kind))
		{
			die(e->line,"String.substring expects two integer arguments.",NULL);
		}

		e->type.kind = TY_STRING;
	}
	else if (strcmp(nm,"replace")==0)
	{
		if (e->arg_count != 2 || e->args[0]->type.kind != TY_STRING || e->args[1]->type.kind != TY_STRING)
		{
			die(e->line,"String.replace expects two string arguments.",NULL);
		}

		e->type.kind = TY_STRING;
	}
	else if (strcmp(nm,"trim")==0 || strcmp(nm,"toUpper")==0 || strcmp(nm,"toLower")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"This string method takes no arguments.",NULL);
		}

		e->type.kind = TY_STRING;
	}
	else if (strcmp(nm,"isEmpty")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"String.isEmpty() takes no arguments.",NULL);
		}

		e->type.kind = TY_BOOL;
	}
	else if (strcmp(nm,"equals")==0 || strcmp(nm,"equalsIgnoreCase")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_STRING)
		{
			die(e->line,"This string method expects one string argument.",NULL);
		}

		e->type.kind = TY_BOOL;
	}
	else if (strcmp(nm,"lastIndexOf")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_STRING)
		{
			die(e->line,"String.lastIndexOf expects one string argument.",NULL);
		}

		e->type.kind = TY_INT;
	}
	else if (strcmp(nm,"charAt")==0)
	{
		if (e->arg_count != 1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"String.charAt expects one integer argument.",NULL);
		}

		e->type.kind = TY_INT;
	}
	else if (strcmp(nm,"repeat")==0)
	{
		if (e->arg_count != 1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"String.repeat expects one integer argument.",NULL);
		}

		e->type.kind = TY_STRING;
	}
	else if (strcmp(nm,"split")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_STRING)
		{
			die(e->line,"String.split expects one string argument.",NULL);
		}

		TypeRef elem;
		memset(&elem, 0, sizeof(elem));
		elem.kind = TY_STRING;
		e->type.kind = TY_ARRAY;
		e->type.elem = typeref_box(elem);
	}
	else
	{
		die(e->line,"Unknown string method: ",nm);
	}
}

static void resolve_regex(Expr *e)
{
	const char *m = e->name + 6;   /* After "Regex.". */
	int predicate = (strcmp(m,"matches")==0 || strcmp(m,"test")==0);
	int find = strcmp(m,"find")==0;
	int replace = strcmp(m,"replace")==0;
	if (!predicate && !find && !replace)
	{
		die(e->line,"Unknown Regex method: ",m);
	}

	int want = replace ? 3 : 2;
	if (e->arg_count != want)
	{
		die(e->line,"Wrong number of arguments for this Regex method.",NULL);
	}

	for (int i=0; i<e->arg_count; i++)
	{
		if (e->args[i]->type.kind != TY_STRING)
		{
			die(e->line,"Regex arguments must be strings.",NULL);
		}
	}

	e->type.kind = predicate ? TY_BOOL : TY_STRING;
}

/* Require argument i to be a string. */
static void file_arg_string(Expr *e, int i)
{
	if (i >= e->arg_count || e->args[i]->type.kind != TY_STRING)
	{
		die(e->line,"File method: argument must be a string.",NULL);
	}
}

static int file_is_byte_array(TypeRef *t)
{
	return t->kind == TY_ARRAY && t->elem && t->elem->kind == TY_BYTE;
}

static void resolve_system(Expr *e)
{
	const char *m = e->name + 7;   /* After "System.". */
	if (strcmp(m,"shell")==0)
	{
		if (e->arg_count<1 || e->arg_count>2 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"System.shell(command[, wait]) takes a string and an optional boolean.",NULL);
		}

		if (e->arg_count==2 && e->args[1]->type.kind!=TY_BOOL)
		{
			die(e->line,"System.shell wait argument must be a boolean.",NULL);
		}

		e->type.kind=TY_INT;   /* Async: pid. Wait: exit code. */
		return;
	}

	die(e->line,"Unknown System method: ",m);
}

static void resolve_file(Expr *e)
{
	const char *m = e->name + 5;   /* After "File.". */

	/* Path -> boolean predicates. */
	if (strcmp(m,"exists")==0 || strcmp(m,"isFile")==0 || strcmp(m,"isFolder")==0)
	{
		if (e->arg_count != 1)
		{
			die(e->line,"File predicate takes one path argument.",NULL);
		}
		file_arg_string(e,0);
		e->type.kind = TY_BOOL;
		return;
	}

	/* Path -> void mutations. */
	if (strcmp(m,"createFile")==0 || strcmp(m,"createFolder")==0
			|| strcmp(m,"delete")==0 || strcmp(m,"deleteRecursive")==0)
	{
		if (e->arg_count != 1)
		{
			die(e->line,"File mutation takes one path argument.",NULL);
		}
		file_arg_string(e,0);
		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"readText")==0)
	{
		if (e->arg_count != 1)
		{
			die(e->line,"File.readText takes one path argument.",NULL);
		}
		file_arg_string(e,0);
		e->type.kind = TY_STRING;
		return;
	}

	if (strcmp(m,"readLines")==0)
	{
		if (e->arg_count != 1)
		{
			die(e->line,"File.readLines takes one path argument.",NULL);
		}
		file_arg_string(e,0);
		TypeRef el;
		memset(&el,0,sizeof(el));
		el.kind = TY_STRING;
		e->type.kind = TY_ARRAY;
		e->type.elem = typeref_box(el);
		return;
	}

	if (strcmp(m,"readBytes")==0)
	{
		if (e->arg_count != 1)
		{
			die(e->line,"File.readBytes takes one path argument.",NULL);
		}
		file_arg_string(e,0);
		TypeRef el;
		memset(&el,0,sizeof(el));
		el.kind = TY_BYTE;
		e->type.kind = TY_ARRAY;
		e->type.elem = typeref_box(el);
		return;
	}

	if (strcmp(m,"writeText")==0 || strcmp(m,"appendText")==0)
	{
		if (e->arg_count != 2)
		{
			die(e->line,"File.writeText/appendText take (path, content).",NULL);
		}
		file_arg_string(e,0);
		file_arg_string(e,1);
		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"writeBytes")==0)
	{
		if (e->arg_count != 2)
		{
			die(e->line,"File.writeBytes takes (path, byte[]).",NULL);
		}
		file_arg_string(e,0);
		if (!file_is_byte_array(&e->args[1]->type))
		{
			die(e->line,"File.writeBytes: second argument must be byte[].",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	/* Search: folder [+ pattern] -> string[] of full paths. */
	if (strcmp(m,"list")==0 || strcmp(m,"search")==0 || strcmp(m,"searchRecursive")==0)
	{
		int want = (strcmp(m,"list")==0) ? 1 : 2;
		if (e->arg_count != want)
		{
			die(e->line,"File.list/search argument count.",NULL);
		}

		for (int i=0; i<want; i++)
		{
			file_arg_string(e,i);
		}

		TypeRef el;
		memset(&el,0,sizeof(el));
		el.kind = TY_STRING;
		e->type.kind = TY_ARRAY;
		e->type.elem = typeref_box(el);
		return;
	}

	if (strcmp(m,"setAttribute")==0)
	{
		if (e->arg_count != 3)
		{
			die(e->line,"File.setAttribute takes (path, attr, on).",NULL);
		}

		file_arg_string(e,0);
		if (!ty_is_int(e->args[1]->type.kind) || e->args[2]->type.kind != TY_BOOL)
		{
			die(e->line,"File.setAttribute(path, attr, on): attr is int, on is boolean.",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"hasAttribute")==0)
	{
		if (e->arg_count != 2)
		{
			die(e->line,"File.hasAttribute takes (path, attr).",NULL);
		}

		file_arg_string(e,0);
		if (!ty_is_int(e->args[1]->type.kind))
		{
			die(e->line,"File.hasAttribute(path, attr): attr is int.",NULL);
		}

		e->type.kind = TY_BOOL;
		return;
	}

	die(e->line,"Unknown File method: ",m);
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
			die(e->line,"Random.nextBytes expects a byte[].",NULL);
		}

		e->type.kind = TY_VOID;
	}
	else if (strcmp(m,"get")==0)
	{
		if (e->arg_count!=1 && e->arg_count!=2)
		{
			die(e->line,"Random.get takes one or two arguments.",NULL);
		}

		TypeKind k=e->args[0]->type.kind;
		if (k!=TY_INT && k!=TY_LONG && k!=TY_FLOAT && k!=TY_DOUBLE)
		{
			die(e->line,"Random.get bound must be int, long, float, or double.",NULL);
		}

		if (e->arg_count==2 && e->args[1]->type.kind!=k)
		{
			die(e->line,"Random.get(origin, bound) arguments must be the same type.",NULL);
		}

		e->type.kind = k;
	}
	else
	{
		die(e->line,"Unknown Random method: ",m);
	}

	if (e->arg_count!=0 && strcmp(m,"get")!=0 && strcmp(m,"nextBytes")!=0)
	{
		die(e->line,"This Random method takes no arguments.",NULL);
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

/* scheduleAfter(f, delayMs) / scheduleEvery(f, delayMs, periodMs): f is a bare
   named zero-arg void function (not a value), the rest are integer delays. */
static void resolve_schedule(SymTable *st, Expr *e, const char *tc)
{
	int periodic = (strcmp(e->name,"scheduleEvery")==0);
	int want = periodic ? 3 : 2;
	if (e->arg_count != want)
	{
		die(e->line, periodic ? "scheduleEvery expects (function, delayMs, periodMs)."
			: "scheduleAfter expects (function, delayMs).", NULL);
	}

	if (e->args[0]->kind != EX_IDENT)
	{
		die(e->line,"Schedule target must be a named function.",NULL);
	}

	FuncInfo *fi = types_find_func(g_types, e->args[0]->name);
	if (!fi)
	{
		die(e->line,"Schedule target is not a function: ",e->args[0]->name);
	}

	if (fi->ret_type.kind != TY_VOID)
	{
		die(e->line,"Schedule target must return void.",NULL);
	}

	if (fi->param_count != 0)
	{
		die(e->line,"Schedule target must take no arguments.",NULL);
	}

	/* The delay (and period) are ordinary integer expressions. */
	for (int i = 1; i < e->arg_count; i++)
	{
		resolve_expr(st, e->args[i], tc);
		if (!ty_is_int(e->args[i]->type.kind))
		{
			die(e->line,"Schedule delay/period must be an integer.",NULL);
		}
	}

	e->type.kind = TY_TIMER;
	e->type.class_name[0] = '\0';
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
		resolve_expr(st,e->lhs,tc);                 /* The count. */
		if (!ty_is_int(e->lhs->type.kind))
		{
			die(e->line,"Array length must be an integer.",NULL);
		}

		if (e->type.elem->kind==TY_OBJECT
				&& !is_stringbuilder(e->type.elem)
				&& !types_find_class(g_types,e->type.elem->class_name))
		{
			die(e->line,"Unknown array element type: ",e->type.elem->class_name);
		}

		break;
	case EX_NEWMAP:
		if (e->type.elem->kind!=TY_INT && e->type.elem->kind!=TY_STRING)
		{
			die(e->line,"Map key must be int or string.",NULL);
		}

		if (e->type.elem2->kind==TY_OBJECT
				&& !is_stringbuilder(e->type.elem2)
				&& !types_find_class(g_types,e->type.elem2->class_name))
		{
			die(e->line,"Unknown map value type: ",e->type.elem2->class_name);
		}

		break;   /* e->type is already TY_MAP + key/value, set by the parser. */
	case EX_NEWCHANNEL:
		if (e->arg_count!=1)
		{
			die(e->line,"New channel<T>(capacity) takes one argument.",NULL);
		}

		resolve_expr(st,e->args[0],tc);
		if (!ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"Channel capacity must be an integer.",NULL);
		}

		if (e->type.elem->kind==TY_OBJECT
				&& !is_stringbuilder(e->type.elem)
				&& !types_find_class(g_types,e->type.elem->class_name))
		{
			die(e->line,"Unknown channel element type: ",e->type.elem->class_name);
		}

		break;   /* e->type is already TY_CHANNEL + elem, set by the parser. */
	case EX_NEWGEN:
	{
		const char *tmpl=e->type.class_name;
		TypeKind ek=e->type.elem->kind;
		int scalar_or_obj = ty_is_int(ek) || ty_is_float(ek) || ek==TY_BOOL || ek==TY_STRING || ek==TY_OBJECT;
		if (!scalar_or_obj)
		{
			die(e->line,"Collection element must be a scalar, string, or object.",NULL);
		}

		if (strcmp(tmpl,"Set")==0 && ek!=TY_INT && ek!=TY_STRING)
		{
			die(e->line,"Set element must be int or string (hashable).",NULL);
		}

		if (ek==TY_OBJECT
				&& !is_stringbuilder(e->type.elem)
				&& !types_find_class(g_types,e->type.elem->class_name))
		{
			die(e->line,"Unknown collection element type: ",e->type.elem->class_name);
		}

		break;   /* e->type already TY_GENERIC + elem, set by the parser. */
	}
	case EX_INDEX:
		resolve_expr(st,e->lhs,tc);
		resolve_expr(st,e->rhs,tc);
		if (e->lhs->type.kind!=TY_ARRAY)
		{
			die(e->line,"Indexing a non-array.",NULL);
		}

		if (!ty_is_int(e->rhs->type.kind))
		{
			die(e->line,"Array index must be an integer.",NULL);
		}

		e->type = *e->lhs->type.elem;
		break;
	case EX_CAST:
	{
		resolve_expr(st,e->lhs,tc);
		TypeKind to=e->type.kind, from=e->lhs->type.kind;   /* Target set by parser. */
		int to_num=ty_is_int(to) || ty_is_float(to);
		int from_num=ty_is_int(from) || ty_is_float(from);
		if (to==TY_BOOL || from==TY_BOOL)
		{
			if (to != from)
			{
				die(e->line,"Cannot cast between boolean and a number.",NULL);
			}
		}
		else if (!to_num || !from_num)
		{
			die(e->line,"Cast operand and target must be scalar numbers.",NULL);
		}

		break;
	}
	case EX_THIS:
		if (!tc)
		{
			die(e->line,"'this' outside a method.",NULL);
		}

		e->type.kind=TY_OBJECT;
		strcpy(e->type.class_name,tc);
		break;
	case EX_IDENT:
	{
		Symbol *s=sym_find(st,e->name);
		if (!s)
		{
			die(e->line,"Unknown variable: ",e->name);
		}

		e->type=s->type;
		e->anno_int=s->offset;
		break;
	}
	case EX_NEW:
	{
		if (strcmp(e->name,"StringBuilder")!=0 && !types_find_class(g_types,e->name))
		{
			die(e->line,"Unknown class: ",e->name);
		}

		ClassInfo *nc=types_find_class(g_types,e->name);
		for (int i=0; i<e->arg_count; i++)
		{
			resolve_expr(st,e->args[i],tc);
		}

		if (nc && nc->has_ctor)
		{
			if (e->arg_count != nc->ctor_param_count)
			{
				die(e->line,"Constructor argument count mismatch.",NULL);
			}

			for (int i=0; i<e->arg_count; i++)
			{
				if (!assignable(&nc->ctor_param_types[i], &e->args[i]->type))
				{
					die(e->line,"Constructor argument type mismatch; add a cast.",NULL);
				}
			}
		}
		else if (e->arg_count != 0)
		{
			die(e->line,"This class has no constructor; use new Class().",NULL);
		}

		e->type.kind=TY_OBJECT;
		strcpy(e->type.class_name,e->name);
		break;
	}
	case EX_UNARY:
		resolve_expr(st,e->lhs,tc);
		if (!ty_is_int(e->lhs->type.kind) && !ty_is_float(e->lhs->type.kind))
		{
			die(e->line,"Unary '-' requires a numeric operand.",NULL);
		}

		e->type=e->lhs->type;
		break;
	case EX_INCDEC:
		resolve_expr(st,e->lhs,tc);
		if (e->lhs->kind!=EX_IDENT || !ty_is_int(e->lhs->type.kind))
		{
			die(e->line,"'++'/'--' requires an integer variable.",NULL);
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
				die(e->line,"Strings support only '+' concatenation of two strings.",NULL);
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
					die(e->line,"Mix of float and double; add a cast.",NULL);
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
					die(e->line,"Non-numeric operand.",NULL);
				}

				if (fk==TY_FLOAT)
				{
					die(e->line,"Mix of float and integer; add a cast.",NULL);
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
				die(e->line,"Mixed signedness in comparison; add a cast.",NULL);
			}

			if (!both_int && !(a==TY_BOOL && b==TY_BOOL) && !(a==TY_OBJECT && b==TY_OBJECT))
			{
				die(e->line,"'==' operands are not comparable.",NULL);
			}

			e->type.kind=TY_BOOL;
			break;
		}

		if (e->op==TOKEN_LT || e->op==TOKEN_GT || e->op==TOKEN_LTE || e->op==TOKEN_GTE)
		{
			if (!ty_is_int(a) || !ty_is_int(b))
			{
				die(e->line,"Relational operands must be integers.",NULL);
			}

			if (ty_is_signed(a)!=ty_is_signed(b))
			{
				die(e->line,"Mixed signedness in comparison; add a cast.",NULL);
			}

			e->type.kind=TY_BOOL;
			break;
		}

		if (!ty_is_int(a) || !ty_is_int(b))
		{
			die(e->line,"Arithmetic operands must be integers.",NULL);
		}

		if (ty_is_signed(a)!=ty_is_signed(b))
		{
			die(e->line,"Mixed signedness in arithmetic; add a cast.",NULL);
		}

		e->type.kind = ty_rank(a)>=ty_rank(b) ? a : b;   /* Wider operand wins. */
		break;
	}
	case EX_FIELD:
	{
		/* Namespace constant access (File.READONLY etc.): the lhs is the namespace
		   identifier, not a variable. Rewrite the node to an integer literal. */
		if (e->lhs->kind==EX_IDENT && strcmp(e->lhs->name,"File")==0)
		{
			long long v = -1;
			if (strcmp(e->name,"READONLY")==0)
			{
				v = 1;
			}
			else if (strcmp(e->name,"HIDDEN")==0)
			{
				v = 2;
			}
			else if (strcmp(e->name,"SYSTEM")==0)
			{
				v = 4;
			}
			else if (strcmp(e->name,"ARCHIVE")==0)
			{
				v = 32;
			}
			else
			{
				die(e->line,"Unknown File constant: ",e->name);
			}

			e->kind = EX_INT;
			e->int_val = v;
			e->int_suffix[0] = '\0';
			e->type.kind = TY_INT;
			break;
		}

		resolve_expr(st,e->lhs,tc);
		if (e->lhs->type.kind==TY_ARRAY)
		{
			if (strcmp(e->name,"length")!=0)
			{
				die(e->line,"Arrays have only '.length'.",NULL);
			}

			e->type.kind=TY_INT;
			e->anno_int=24;             /* The length field offset. */
			break;
		}

		if (e->lhs->type.kind==TY_MAP)
		{
			if (strcmp(e->name,"size")!=0)
			{
				die(e->line,"Maps have only '.size'.",NULL);
			}

			e->type.kind=TY_INT;
			e->anno_int=24;             /* The size field offset. */
			break;
		}

		if (e->lhs->type.kind==TY_GENERIC)
		{
			if (strcmp(e->name,"size")!=0)
			{
				die(e->line,"Collections have only '.size'.",NULL);
			}

			e->type.kind=TY_INT;
			e->anno_int=24;             /* Length (vector) / size (map) field offset. */
			break;
		}

		ClassInfo *c=class_of(&e->lhs->type);
		if (!c)
		{
			die(e->line,"Field access on non-object.",NULL);
		}

		FieldInfo *f=types_find_field(c,e->name);
		if (!f)
		{
			die(e->line,"Unknown field: ",e->name);
		}

		e->type=f->type;
		e->anno_int=f->offset;
		break;
	}
	case EX_METHOD_CALL:
	{
		resolve_expr(st,e->lhs,tc);
		if (e->lhs->type.kind==TY_STRING)
		{
			resolve_args(st,e,tc);
			resolve_string_method(e);
			break;
		}

		if (e->lhs->type.kind==TY_ENTRY)
		{
			if (e->arg_count!=0)
			{
				die(e->line,"Entry methods take no arguments.",NULL);
			}

			if (strcmp(e->name,"getKey")==0)
			{
				e->type = *e->lhs->type.elem;    /* K. */
			}
			else if (strcmp(e->name,"getValue")==0)
			{
				e->type = *e->lhs->type.elem2;   /* V. */
			}
			else
			{
				die(e->line,"Unknown Entry method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_CHANNEL)
		{
			resolve_args(st,e,tc);
			TypeRef *T=e->lhs->type.elem;
			if (strcmp(e->name,"send")==0)
			{
				if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
				{
					die(e->line,"Channel.send(value) type mismatch.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"recv")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Channel.recv() takes no arguments.",NULL);
				}

				e->type = *T;
			}
			else
			{
				die(e->line,"Unknown channel method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_TIMER)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"cancel")!=0)
			{
				die(e->line,"Unknown Timer method: ",e->name);
			}

			if (e->arg_count!=0)
			{
				die(e->line,"Timer.cancel() takes no arguments.",NULL);
			}

			e->type.kind=TY_VOID;
			break;
		}

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
					die(e->line,"Contains(value) type mismatch.",NULL);
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
						die(e->line,"Box.set type mismatch.",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else if (strcmp(nm,"get")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"Box.get takes no arguments.",NULL);
					}

					e->type=*T;
				}
				else
				{
					die(e->line,"Unknown Box method: ",nm);
				}

				break;
			}

			if (strcmp(tmpl,"Set")==0)
			{
				if (strcmp(nm,"add")==0 || strcmp(nm,"remove")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"Set op type mismatch.",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else
				{
					die(e->line,"Unknown Set method: ",nm);
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
					die(e->line,"Add(value) type mismatch.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (takeT)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"This method takes no arguments.",NULL);
				}

				e->type=*T;
			}
			else if (strcmp(nm,"get")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"Get(index) needs an integer.",NULL);
				}

				e->type=*T;
			}
			else if (strcmp(nm,"set")==0)
			{
				if (e->arg_count!=2 || !ty_is_int(e->args[0]->type.kind) || !assignable(T,&e->args[1]->type))
				{
					die(e->line,"Set(index,value) type mismatch.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(nm,"removeAt")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"RemoveAt(index) needs an integer.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(nm,"indexOf")==0)
			{
				if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
				{
					die(e->line,"IndexOf(value) type mismatch.",NULL);
				}

				e->type.kind=TY_INT;
			}
			else
			{
				die(e->line,"Unknown collection method: ",nm);
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
					die(e->line,"Map.put(key,value) type mismatch.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"get")==0)
			{
				if (e->arg_count!=1 || !assignable(K,&e->args[0]->type))
				{
					die(e->line,"Map.get(key) type mismatch.",NULL);
				}

				e->type = *V;
			}
			else if (strcmp(e->name,"containsKey")==0)
			{
				if (e->arg_count!=1 || !assignable(K,&e->args[0]->type))
				{
					die(e->line,"Map.containsKey(key) type mismatch.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"containsValue")==0)
			{
				if (e->arg_count!=1 || !assignable(V,&e->args[0]->type))
				{
					die(e->line,"Map.containsValue(value) type mismatch.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"remove")==0)
			{
				if (e->arg_count!=1 || !assignable(K,&e->args[0]->type))
				{
					die(e->line,"Map.remove(key) type mismatch.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"getKeys")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Map.getKeys() takes no arguments.",NULL);
				}

				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(*K);
			}
			else if (strcmp(e->name,"getValues")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Map.getValues() takes no arguments.",NULL);
				}

				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(*V);
			}
			else if (strcmp(e->name,"getEntries")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Map.getEntries() takes no arguments.",NULL);
				}

				TypeRef ent;
				memset(&ent,0,sizeof(ent));
				ent.kind=TY_ENTRY;
				ent.elem=typeref_box(*K);
				ent.elem2=typeref_box(*V);
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(ent);
			}
			else
			{
				die(e->line,"Unknown map method: ",e->name);
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
					die(e->line,"StringBuilder.append expects one string.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"toString")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"StringBuilder.toString takes no arguments.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else
			{
				die(e->line,"Unknown StringBuilder method: ",e->name);
			}

			break;
		}

		ClassInfo *c=class_of(&e->lhs->type);
		if (!c)
		{
			die(e->line,"Method call on non-object.",NULL);
		}

		MethodInfo *m=types_find_method(c,e->name);
		if (!m)
		{
			if (strcmp(e->name,"getClassName")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"getClassName() takes no arguments.",NULL);
				}

				e->type.kind=TY_STRING;   /* Builtin: dynamic class name, lowered directly in codegen. */
				break;
			}

			die(e->line,"Unknown method: ",e->name);
		}

		resolve_args(st,e,tc);
		for (int i=0; i<e->arg_count && i<m->param_count; i++)
		{
			if (!assignable(&m->param_types[i], &e->args[i]->type))
			{
				die(e->line,"Argument type mismatch; add a cast.",NULL);
			}
		}

		e->type=m->ret_type;
		e->anno_int=m->vtable_slot;
		strcpy(e->anno_str,c->name);
		break;
	}
	case EX_CALL:
		if (strcmp(e->name,"scheduleAfter")==0 || strcmp(e->name,"scheduleEvery")==0)
		{
			resolve_schedule(st,e,tc);
			break;
		}

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

		if (strncmp(e->name,"File.",5)==0)
		{
			resolve_file(e);
			break;
		}

		if (strncmp(e->name,"System.",7)==0)
		{
			resolve_system(e);
			break;
		}

		if (strcmp(e->name,"print")==0)
		{
			if (e->arg_count<1)
			{
				die(e->line,"Print expects a scalar argument.",NULL);
			}

			TypeKind ak=e->args[0]->type.kind;
			if (!ty_is_int(ak) && ak!=TY_BOOL && !ty_is_float(ak) && ak!=TY_STRING)
			{
				die(e->line,"Print expects a scalar or string argument.",NULL);
			}

			e->type.kind=TY_VOID;
			break;
		}

		if (strcmp(e->name,"length")==0)
		{
			if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
			{
				die(e->line,"Length expects one string argument.",NULL);
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

		if (strcmp(e->name,"yield")==0)
		{
			if (e->arg_count!=0)
			{
				die(e->line,"Yield() takes no arguments.",NULL);
			}

			e->type.kind=TY_VOID;
			break;
		}

		{
			FuncInfo *fi=types_find_func(g_types,e->name);
			if (!fi)
			{
				die(e->line,"Unknown function: ",e->name);
			}

			for (int i=0; i<e->arg_count && i<fi->param_count; i++)
			{
				if (!assignable(&fi->param_types[i], &e->args[i]->type))
				{
					die(e->line,"Argument type mismatch; add a cast.",NULL);
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
			die(s->line,"Unknown type: ",s->decl_type.class_name);
		}

		if (s->decl_init)
		{
			resolve_expr(st,s->decl_init,tc);
			if (!assignable(&s->decl_type, &s->decl_init->type))
			{
				die(s->line,"Initializer type does not match; add a cast.",NULL);
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
			die(s->line,"Assigned value type does not match; add a cast.",NULL);
		}
		break;
	case ST_IF:
		resolve_expr(st,s->cond,tc);
		if (s->cond->type.kind!=TY_BOOL)
		{
			die(s->line,"'if' condition must be boolean.",NULL);
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
			die(s->line,"'while' condition must be boolean.",NULL);
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
				die(s->line,"Return type does not match; add a cast.",NULL);
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
			die(s->line,"Foreach requires an array, string, map, or collection.",NULL);
		}

		if (!assignable(&s->decl_type,&elem))
		{
			die(s->line,"Foreach loop variable type does not match the element type.",NULL);
		}

		if (elem.kind==TY_ENTRY)
		{
			s->decl_type = elem;   /* Adopt the iterable's Entry<K,V> so getKey/getValue type. */
		}

		Symbol *lv=sym_add(st,s->decl_name,s->decl_type);
		s->decl_offset=lv->offset;
		s->fe_coll_offset=sym_add(st,"",s->expr->type)->offset;
		s->fe_index_offset=sym_add(st,"",s->decl_type)->offset;
		s->fe_len_offset=sym_add(st,"",s->decl_type)->offset;
		s->fe_aux_offset=sym_add(st,"",s->decl_type)->offset;
		if (s->fe_val_type.kind != TY_VOID)
		{
			if (ik != TY_MAP)
			{
				die(s->line,"The key, value foreach form requires a map.",NULL);
			}

			if (!assignable(&s->decl_type, s->expr->type.elem)
					|| !assignable(&s->fe_val_type, s->expr->type.elem2))
			{
				die(s->line,"Foreach key/value types do not match the map.",NULL);
			}

			s->fe_val_offset=sym_add(st,s->fe_val_name,s->fe_val_type)->offset;
		}

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
			die(s->line,"Break outside a loop or switch.",NULL);
		}
		break;
	case ST_CONTINUE:
		if (g_loop_depth==0)
		{
			die(s->line,"Continue outside a loop.",NULL);
		}
		break;
	case ST_FOR:
		resolve_stmt(st,s->for_init,tc);
		resolve_expr(st,s->cond,tc);
		if (s->cond->type.kind!=TY_BOOL)
		{
			die(s->line,"'for' condition must be boolean.",NULL);
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
			die(s->line,"Switch operand must be an integer.",NULL);
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
						die(c->line,"Duplicate case value in switch.",NULL);
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
					die(c->line,"More than one default in switch.",NULL);
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
			die(s->line,"Thrown value must be an Exception (or subclass).",NULL);
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
				die(c->line,"Catch type must be an Exception (or subclass).",NULL);
			}

			Symbol *cv=sym_add(st,c->decl_name,c->decl_type);
			c->decl_offset=cv->offset;
			resolve_block(st,c->then_blk,tc);
		}

		break;
	}
	case ST_CATCH:
		break;   /* Resolved as part of the enclosing try. */
	case ST_SPAWN:
	{
		resolve_expr(st,s->expr,tc);
		FuncInfo *fi = types_find_func(g_types, s->expr->name);
		if (!fi || s->expr->kind != EX_CALL)
		{
			die(s->line,"Spawn expects a call to a named function.",NULL);
		}

		if (fi->ret_type.kind != TY_VOID)
		{
			die(s->line,"Spawn target must return void.",NULL);
		}

		if (s->expr->arg_count > 4)
		{
			die(s->line,"Spawn target takes at most 4 arguments.",NULL);
		}

		if (s->expr->arg_count != fi->param_count)
		{
			die(s->line,"Spawn argument count does not match the target.",NULL);
		}

		for (int i=0; i<s->expr->arg_count; i++)
		{
			if (!assignable(&fi->param_types[i], &s->expr->args[i]->type))
			{
				die(s->line,"Spawn argument type mismatch; add a cast.",NULL);
			}
		}

		break;
	}
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
	types_compute_shared_set(tt);   /* Decide which classes get atomic refcounts before resolving bodies. */

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

			if (u->klass->ctor)
			{
				resolve_func(tt,u->klass->ctor,u->klass->name);
			}
		}
	}
}
