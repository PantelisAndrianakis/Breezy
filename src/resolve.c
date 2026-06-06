#include "resolve.h"
#include "symtable.h"
#include "ownership.h"
#include "escape.h"
#include "enums.h"
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
   checking is out of scope). Bool<->integer and narrowing need an explicit cast. */
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
	if (from->kind==TY_NULL && ty_is_managed(to->kind))
	{
		return 1;   /* null assigns to any managed reference. */
	}

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
	else if (strcmp(nm,"isEmpty")==0 || strcmp(nm,"isNumeric")==0 || strcmp(nm,"isAlphaNumeric")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"This string method takes no arguments.",NULL);
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
	else if (strcmp(nm,"toInt")==0 || strcmp(nm,"toLong")==0 || strcmp(nm,"toByte")==0
			 || strcmp(nm,"toShort")==0 || strcmp(nm,"toFloat")==0 || strcmp(nm,"toDouble")==0
			 || strcmp(nm,"toBool")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"This string parse method takes no arguments.",NULL);
		}

		e->type.kind = strcmp(nm,"toInt")==0    ? TY_INT :
					   strcmp(nm,"toLong")==0   ? TY_LONG :
					   strcmp(nm,"toByte")==0   ? TY_BYTE :
					   strcmp(nm,"toShort")==0  ? TY_SHORT :
					   strcmp(nm,"toFloat")==0  ? TY_FLOAT :
					   strcmp(nm,"toDouble")==0 ? TY_DOUBLE : TY_BOOL;
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
	if (strcmp(m,"args")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"System.args() takes no arguments.",NULL);
		}

		TypeRef elem;
		memset(&elem, 0, sizeof(elem));
		elem.kind = TY_STRING;
		e->type.kind = TY_ARRAY;
		e->type.elem = typeref_box(elem);
		return;
	}

	if (strcmp(m,"shell")==0)
	{
		if (e->arg_count<1 || e->arg_count>2 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"System.shell(command[, wait]) takes a string and an optional bool.",NULL);
		}

		if (e->arg_count==2 && e->args[1]->type.kind!=TY_BOOL)
		{
			die(e->line,"System.shell wait argument must be a bool.",NULL);
		}

		e->type.kind=TY_INT;   /* Async: pid. Wait: exit code. */
		return;
	}

	die(e->line,"Unknown System method: ",m);
}

static void resolve_network(Expr *e)
{
	const char *m = e->name + 8;   /* After "Network.". */
	if (strcmp(m,"listen")==0)
	{
		if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"Network.listen(port) takes one integer port.",NULL);
		}

		e->type.kind=TY_LISTENER;
		return;
	}

	if (strcmp(m,"readUrl")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"Network.readUrl(url) takes one string URL.",NULL);
		}

		e->type.kind=TY_STRING;
		return;
	}

	if (strcmp(m,"connect")==0)
	{
		if (e->arg_count!=2 || e->args[0]->type.kind!=TY_STRING || !ty_is_int(e->args[1]->type.kind))
		{
			die(e->line,"Network.connect(host, port) takes a string and an integer.",NULL);
		}

		e->type.kind=TY_SOCKET;
		return;
	}

	if (strcmp(m,"udp")==0)
	{
		if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"Network.udp(port) takes one integer port.",NULL);
		}

		e->type.kind=TY_UDPSOCKET;
		return;
	}

	die(e->line,"Unknown Network method: ",m);
}

static void resolve_log(Expr *e)
{
	const char *m = e->name + 4;   /* After "Log.". */
	if (strcmp(m,"open")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"Log.open(path) takes one path argument.",NULL);
		}

		e->type.kind=TY_LOGGER;
		return;
	}

	die(e->line,"Unknown Log method: ",m);
}

static void resolve_file(Expr *e)
{
	const char *m = e->name + 5;   /* After "File.". */

	if (strcmp(m,"openChannel")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"File.openChannel(path) takes one path argument.",NULL);
		}

		e->type.kind=TY_FILECHANNEL;
		return;
	}

	if (strcmp(m,"openWrite")==0 || strcmp(m,"openAppend")==0)
	{
		if (e->arg_count<1 || e->arg_count>2 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"File.openWrite/openAppend(path[, bufferBytes]) takes a path and an optional integer.",NULL);
		}

		if (e->arg_count==2 && !ty_is_int(e->args[1]->type.kind))
		{
			die(e->line,"File.openWrite/openAppend buffer size must be an integer.",NULL);
		}

		e->type.kind=TY_FILEWRITER;
		return;
	}

	/* Path -> bool predicates. */
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
			die(e->line,"File.setAttribute(path, attr, on): attr is int, on is bool.",NULL);
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
	if (strcmp(m,"nextBool")==0)
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

/* Append cloned default-value arguments for omitted trailing parameters of a call
   whose target's AST is `ast` (NULL for builtins — a no-op). Stops at the first
   missing parameter that has no default, leaving the caller's existing arg-count
   check to report the error. Each injected default is resolved in place. */
static void fill_default_args(SymTable *st, Expr *e, Func *ast, const char *tc)
{
	if (!ast)
	{
		return;
	}

	while (e->arg_count < ast->param_count && e->arg_count < 8)
	{
		Expr *d = ast->params[e->arg_count].def;
		if (!d)
		{
			break;
		}

		Expr *copy = expr_clone(d);
		resolve_expr(st, copy, tc);
		e->args[e->arg_count++] = copy;
	}
}

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
	case EX_NULL:
		e->type.kind=TY_NULL;
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
		if (!ty_is_int(e->type.elem->kind) && e->type.elem->kind!=TY_STRING
				&& e->type.elem->kind!=TY_OBJECT)
		{
			die(e->line,"Map key must be an integer type, string, enum, or object.",NULL);
		}

		if (e->type.elem->kind==TY_OBJECT
				&& !is_stringbuilder(e->type.elem)
				&& !enum_is(e->type.elem->class_name)
				&& !types_find_class(g_types,e->type.elem->class_name))
		{
			die(e->line,"Unknown map key type: ",e->type.elem->class_name);
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

		if (strcmp(tmpl,"Set")==0 && !ty_is_int(ek) && ek!=TY_STRING && ek!=TY_OBJECT)
		{
			die(e->line,"Set element must be an integer type, string, enum, or object (hashable).",NULL);
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
				die(e->line,"Cannot cast between bool and a number.",NULL);
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
			/* Not a local or parameter. Inside a method, a bare name may refer
			   to a field of the enclosing class: desugar `x` to `this.x` (or, for
			   a static field, to the `Class.field` form). This rewrites the node
			   into the exact EX_FIELD shape the parser builds for explicit field
			   access, so resolve/codegen handle it through their existing paths. */
			if (tc)
			{
				ClassInfo *cc=types_find_class(g_types,tc);
				FieldInfo *fld=cc ? types_find_field(cc,e->name) : NULL;
				if (fld)
				{
					if (fld->is_static)
					{
						/* Mirror the parser's `Class.field` shape: EX_FIELD with an
						   EX_IDENT class-name lhs. Downstream passes assume every
						   EX_FIELD has a non-NULL lhs, so the lhs must be present. */
						Expr *cls=expr_new(EX_IDENT,e->line);
						strcpy(cls->name,tc);
						e->kind=EX_FIELD;
						e->lhs=cls;
						e->type=fld->type;
						e->anno_int=-1;             /* Static-field sentinel (see codegen). */
						strcpy(e->anno_str,tc);
						break;
					}

					Expr *self=expr_new(EX_THIS,e->line);
					self->type.kind=TY_OBJECT;
					strcpy(self->type.class_name,tc);
					e->kind=EX_FIELD;
					e->lhs=self;
					e->type=fld->type;
					e->anno_int=fld->offset;
					break;
				}
			}

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
		if (nc && nc->is_static)
		{
			die(e->line,"Cannot instantiate a static class: ",e->name);
		}

		for (int i=0; i<e->arg_count; i++)
		{
			resolve_expr(st,e->args[i],tc);
		}

		if (nc && nc->has_ctor)
		{
			fill_default_args(st, e, nc->ctor_ast, tc);
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
		if (e->op==TOKEN_TILDE)
		{
			if (!ty_is_int(e->lhs->type.kind))
			{
				die(e->line,"Bitwise '~' requires an integer operand.",NULL);
			}

			e->type=e->lhs->type;
			break;
		}

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
			/* Concatenation with '+': when either operand is a string, the other
			   may be a string or any scalar (integer, floating, or bool) — the
			   scalar is converted to its text form. Objects/arrays are rejected. */
			int a_ok = a==TY_STRING || ty_is_int(a) || ty_is_float(a) || a==TY_BOOL;
			int b_ok = b==TY_STRING || ty_is_int(b) || ty_is_float(b) || b==TY_BOOL;
			if (e->op!=TOKEN_PLUS || !a_ok || !b_ok)
			{
				die(e->line,"Strings support '+' concatenation with strings or scalar values only.",NULL);
			}

			e->type.kind=TY_STRING;
			break;
		}

		if (ty_is_float(a) || ty_is_float(b))
		{
			/* Numeric promotion: when either operand is floating, the other must
			   also be numeric (integer or floating). The result widens to the
			   most general type present: double if either side is double,
			   otherwise float. Integers promote to that float type implicitly. */
			if (!(ty_is_int(a) || ty_is_float(a)) || !(ty_is_int(b) || ty_is_float(b)))
			{
				die(e->line,"Non-numeric operand.",NULL);
			}

			if (e->op==TOKEN_PERCENT)
			{
				die(e->line,"Modulo '%' requires integer operands.",NULL);
			}

			if (e->op==TOKEN_SHL || e->op==TOKEN_SHR)
			{
				die(e->line,"Shift operators '<<'/'>>' require integer operands.",NULL);
			}

			if (e->op==TOKEN_AMP || e->op==TOKEN_PIPE || e->op==TOKEN_CARET)
			{
				die(e->line,"Bitwise '&'/'|'/'^' require integer operands.",NULL);
			}

			TypeKind ft = (a==TY_DOUBLE || b==TY_DOUBLE) ? TY_DOUBLE : TY_FLOAT;
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

			int a_null=(a==TY_NULL), b_null=(b==TY_NULL);
			int managed_vs_null=(a_null && ty_is_managed(b)) || (b_null && ty_is_managed(a)) || (a_null && b_null);
			if (!both_int && !(a==TY_BOOL && b==TY_BOOL) && !(a==TY_OBJECT && b==TY_OBJECT) && !managed_vs_null)
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

		if (e->op==TOKEN_SHL || e->op==TOKEN_SHR)
		{
			if (!ty_is_int(a) || !ty_is_int(b))
			{
				die(e->line,"Shift operators '<<'/'>>' require integer operands.",NULL);
			}

			/* C-style: the result takes the left operand's type; the shift count's
			   type and signedness do not affect the result. A signed left operand
			   shifts arithmetically ('>>' = sar), an unsigned one logically (shr). */
			e->type.kind = a;
			break;
		}

		if (!ty_is_int(a) || !ty_is_int(b))
		{
			die(e->line,"Arithmetic operands must be integers.",NULL);
		}

		/* Integer promotion: the wider rank wins. For mixed signedness, unsigned
		   wins (C-style) — the result is the unsigned type at the wider rank. */
		TypeKind wider = ty_rank(a)>=ty_rank(b) ? a : b;
		e->type.kind = (ty_is_signed(a)!=ty_is_signed(b)) ? ty_to_unsigned(wider) : wider;
		break;
	}
	case EX_FIELD:
	{
		/* Enum constant access (Color.RED): the lhs is the enum type name, not a
		   variable. The node stays EX_FIELD; codegen re-detects it via enum_is and
		   loads the singleton slot, so lhs is left unresolved. */
		if (e->lhs->kind==EX_IDENT && enum_is(e->lhs->name))
		{
			if (enum_ordinal(e->lhs->name,e->name)<0)
			{
				die(e->line,"No such enum constant: ",e->name);
			}

			e->type.kind=TY_OBJECT;
			strcpy(e->type.class_name,e->lhs->name);
			break;
		}

		/* Static field access (C.total): the lhs is a class name, not a variable.
		   Annotate with the sentinel anno_int=-1 + class so codegen loads the slot;
		   lhs is left unresolved. */
		if (e->lhs->kind==EX_IDENT && !sym_find(st,e->lhs->name))
		{
			ClassInfo *sc=types_find_class(g_types,e->lhs->name);
			if (sc)
			{
				FieldInfo *f=types_find_field(sc,e->name);
				if (!f)
				{
					die(e->line,"No such static field: ",e->name);
				}

				if (!f->is_static)
				{
					die(e->line,"Access an instance field through an object, not the class name: ",e->name);
				}

				e->type=f->type;
				e->anno_int=-1;
				strcpy(e->anno_str,e->lhs->name);
				break;
			}
		}

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
		/* Static enum calls: Enum.values() / Enum.valueOf(string). The lhs is the
		   enum type name, not a variable; codegen re-detects via enum_is. */
		if (e->lhs->kind==EX_IDENT && enum_is(e->lhs->name))
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"values")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Enum.values() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_OBJECT;
				strcpy(el.class_name,e->lhs->name);
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"valueOf")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"Enum.valueOf(string) takes one string.",NULL);
				}

				e->type.kind=TY_OBJECT;
				strcpy(e->type.class_name,e->lhs->name);
			}
			else
			{
				die(e->line,"Unknown static enum method: ",e->name);
			}

			break;
		}

		/* Static method call (C.peek()): the lhs is a class name, not a variable. */
		if (e->lhs->kind==EX_IDENT && !sym_find(st,e->lhs->name))
		{
			ClassInfo *sc=types_find_class(g_types,e->lhs->name);
			if (sc)
			{
				MethodInfo *m=types_find_method(sc,e->name);
				if (!m || !m->is_static)
				{
					die(e->line,"No such static method: ",e->name);
				}

				resolve_args(st,e,tc);
				fill_default_args(st, e, m->ast, tc);
				if (e->arg_count != m->param_count)
				{
					die(e->line,"Static method argument count mismatch.",NULL);
				}

				for (int i=0; i<e->arg_count; i++)
				{
					if (!assignable(&m->param_types[i], &e->args[i]->type))
					{
						die(e->line,"Argument type mismatch; add a cast.",NULL);
					}
				}

				e->type=m->ret_type;
				e->anno_int=-1;
				strcpy(e->anno_str,e->lhs->name);
				break;
			}
		}

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

		if (e->lhs->type.kind==TY_LISTENER)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"accept")==0)
			{
				if (e->arg_count==1 && !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"Listener.accept(timeoutMs) takes one integer.",NULL);
				}

				if (e->arg_count>1)
				{
					die(e->line,"Listener.accept() / accept(timeoutMs).",NULL);
				}

				e->type.kind=TY_SOCKET;   /* null on timeout when a timeout is given. */
			}
			else if (strcmp(e->name,"tryAccept")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Listener.tryAccept() takes no arguments.",NULL);
				}

				e->type.kind=TY_SOCKET;   /* null if none pending. */
			}
			else if (strcmp(e->name,"port")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Listener.port() takes no arguments.",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Listener.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown Listener method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_SOCKET)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"read")==0 || strcmp(e->name,"tryRead")==0)
			{
				int is_try = (e->name[0]=='t');
				if (e->arg_count<1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"Socket.read(maxBytes[, timeoutMs]) takes integer(s).",NULL);
				}

				if (e->arg_count>1 && (is_try || !ty_is_int(e->args[1]->type.kind)))
				{
					die(e->line,"Socket.read second arg is an integer timeout (not on tryRead).",NULL);
				}

				if (e->arg_count>2)
				{
					die(e->line,"Socket.read takes at most maxBytes, timeoutMs.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_BYTE;
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);   /* byte[] (null on timeout/none). */
			}
			else if (strcmp(e->name,"readText")==0 || strcmp(e->name,"tryReadText")==0)
			{
				int is_try = (e->name[0]=='t');
				if (e->arg_count<1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"Socket.readText(maxBytes[, timeoutMs]) takes integer(s).",NULL);
				}

				if (e->arg_count>1 && (is_try || !ty_is_int(e->args[1]->type.kind)))
				{
					die(e->line,"Socket.readText second arg is an integer timeout (not on tryReadText).",NULL);
				}

				if (e->arg_count>2)
				{
					die(e->line,"Socket.readText takes at most maxBytes, timeoutMs.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"write")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_ARRAY)
				{
					die(e->line,"Socket.write(byte[]) takes one byte[].",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"writeText")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"Socket.writeText(string) takes one string.",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Socket.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown Socket method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_UDPSOCKET)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"sendTo")==0)
			{
				if (e->arg_count!=3 || e->args[0]->type.kind!=TY_STRING
						|| !ty_is_int(e->args[1]->type.kind) || e->args[2]->type.kind!=TY_ARRAY)
				{
					die(e->line,"UdpSocket.sendTo(host, port, byte[]).",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"sendTextTo")==0)
			{
				if (e->arg_count!=3 || e->args[0]->type.kind!=TY_STRING
						|| !ty_is_int(e->args[1]->type.kind) || e->args[2]->type.kind!=TY_STRING)
				{
					die(e->line,"UdpSocket.sendTextTo(host, port, string).",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"receive")==0)
			{
				if (e->arg_count==1 && !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"UdpSocket.receive(timeoutMs) takes one integer.",NULL);
				}

				if (e->arg_count>1)
				{
					die(e->line,"UdpSocket.receive() / receive(timeoutMs).",NULL);
				}

				e->type.kind=TY_DATAGRAM;   /* null on timeout when a timeout is given. */
			}
			else if (strcmp(e->name,"tryReceive")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"UdpSocket.tryReceive() takes no arguments.",NULL);
				}

				e->type.kind=TY_DATAGRAM;   /* null if none ready. */
			}
			else if (strcmp(e->name,"port")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"UdpSocket.port() takes no arguments.",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"UdpSocket.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown UdpSocket method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_DATAGRAM)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"data")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Datagram.data() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_BYTE;
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"text")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Datagram.text() takes no arguments.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"host")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Datagram.host() takes no arguments.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"port")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Datagram.port() takes no arguments.",NULL);
				}

				e->type.kind=TY_INT;
			}
			else
			{
				die(e->line,"Unknown Datagram method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_FILECHANNEL)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"readAt")==0)
			{
				if (e->arg_count!=2 || !ty_is_int(e->args[0]->type.kind) || !ty_is_int(e->args[1]->type.kind))
				{
					die(e->line,"FileChannel.readAt(offset, maxBytes) takes two integers.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_BYTE;
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);   /* byte[]. */
			}
			else if (strcmp(e->name,"writeAt")==0)
			{
				if (e->arg_count!=2 || !ty_is_int(e->args[0]->type.kind) || e->args[1]->type.kind!=TY_ARRAY)
				{
					die(e->line,"FileChannel.writeAt(offset, byte[]) takes an integer and a byte[].",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"size")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"FileChannel.size() takes no arguments.",NULL);
				}

				e->type.kind=TY_LONG;
			}
			else if (strcmp(e->name,"truncate")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"FileChannel.truncate(size) takes one integer.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"sync")==0 || strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"FileChannel.sync()/close() take no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown FileChannel method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_FILEWRITER)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"write")==0 || strcmp(e->name,"writeLine")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"FileWriter.write/writeLine(string) takes one string.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"writeBytes")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_ARRAY)
				{
					die(e->line,"FileWriter.writeBytes(byte[]) takes one byte[].",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"flush")==0 || strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"FileWriter.flush()/close() take no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown FileWriter method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_LOGGER)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"log")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"Logger.log(string) takes one string.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Logger.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown Logger method: ",e->name);
			}

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
		if (!c && e->lhs->type.kind==TY_OBJECT)
		{
			/* Interface-typed receiver: resolve against the interface's method set;
			   dispatch uses the method's reserved global vtable slot. */
			InterfaceInfo *itf=types_find_interface(g_types,e->lhs->type.class_name);
			if (itf)
			{
				int k=-1;
				for (int i=0; i<itf->method_count; i++)
				{
					if (strcmp(itf->methods[i],e->name)==0)
					{
						k=i;
						break;
					}
				}

				if (k<0)
				{
					die(e->line,"Unknown interface method: ",e->name);
				}

				resolve_args(st,e,tc);
				if (e->arg_count != itf->param_counts[k])
				{
					die(e->line,"Interface method argument count mismatch.",NULL);
				}

				for (int i=0; i<e->arg_count; i++)
				{
					if (!assignable(&itf->param_types[k][i], &e->args[i]->type))
					{
						die(e->line,"Argument type mismatch; add a cast.",NULL);
					}
				}

				e->type=itf->ret_types[k];
				e->anno_int=itf->vslot[k];   /* The global interface slot. */
				strcpy(e->anno_str,itf->name);
				break;
			}
		}

		if (!c)
		{
			die(e->line,"Method call on non-object.",NULL);
		}

		/* Enum instance built-ins: name() -> string, ordinal() -> int (read the
		   hidden fields; lowered directly in codegen). */
		if (enum_is(c->name) && (strcmp(e->name,"name")==0 || strcmp(e->name,"ordinal")==0))
		{
			if (e->arg_count!=0)
			{
				die(e->line,"Enum name()/ordinal() take no arguments.",NULL);
			}

			e->type.kind = (strcmp(e->name,"name")==0) ? TY_STRING : TY_INT;
			break;
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
		fill_default_args(st, e, m->ast, tc);
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

		if (strncmp(e->name,"Network.",8)==0)
		{
			resolve_network(e);
			break;
		}

		if (strncmp(e->name,"Log.",4)==0)
		{
			resolve_log(e);
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

			fill_default_args(st, e, fi->ast, tc);
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
				&& !types_find_class(g_types,s->decl_type.class_name)
				&& !types_is_interface(g_types,s->decl_type.class_name))
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
		if (s->target->kind==EX_FIELD && s->target->lhs->kind==EX_IDENT && enum_is(s->target->lhs->name))
		{
			die(s->line,"Enum constants are immutable.",NULL);
		}

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
			die(s->line,"'if' condition must be bool.",NULL);
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
			die(s->line,"'while' condition must be bool.",NULL);
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
			die(s->line,"'for' condition must be bool.",NULL);
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
		TypeKind ck=s->cond->type.kind;
		int is_enum_switch = (ck==TY_OBJECT && enum_is(s->cond->type.class_name));
		int is_string_switch = (ck==TY_STRING);
		/* Permitted operands: integers, bool, enum, string. Floats are rejected
		   (exact-equality matching is unsafe for floating point, as in Java). */
		if (!ty_is_int(ck) && ck!=TY_BOOL && !is_enum_switch && !is_string_switch)
		{
			if (ty_is_float(ck))
			{
				die(s->line,"Switch operand cannot be float/double (exact equality is unreliable).",NULL);
			}

			die(s->line,"Switch operand must be an integer, bool, enum, or string.",NULL);
		}

		long long seen[256];
		const char *seen_str[256];
		int nseen=0, ndefault=0;
		Block *b=s->then_blk;
		g_break_depth++;
		for (int i=0; i<b->count; i++)
		{
			Stmt *c=b->stmts[i];
			if (c->kind==ST_CASE)
			{
				if (is_enum_switch)
				{
					/* case CONST: -> the operand enum's ordinal. */
					if (c->value->kind!=EX_IDENT)
					{
						die(c->line,"Enum switch case must be a constant name.",NULL);
					}

					int ord=enum_ordinal(s->cond->type.class_name,c->value->name);
					if (ord<0)
					{
						die(c->line,"No such enum constant: ",c->value->name);
					}

					c->value->kind=EX_INT;
					c->value->int_val=ord;
					c->value->type.kind=TY_INT;
				}
				else if (is_string_switch)
				{
					if (c->value->kind!=EX_STR)
					{
						die(c->line,"String switch case must be a string literal.",NULL);
					}

					c->value->type.kind=TY_STRING;
					for (int j=0; j<nseen; j++)
					{
						if (strcmp(seen_str[j],c->value->str_val)==0)
						{
							die(c->line,"Duplicate case value in switch.",NULL);
						}
					}

					if (nseen<256)
					{
						seen_str[nseen++]=c->value->str_val;
					}

					continue;   /* String dedupe handled above; skip the integer path. */
				}
				else if (ck==TY_BOOL)
				{
					if (c->value->kind!=EX_BOOL)
					{
						die(c->line,"Bool switch case must be true or false.",NULL);
					}
				}
				else if (c->value->kind!=EX_INT)
				{
					die(c->line,"Switch case must be an integer literal.",NULL);
				}

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

/* Frame analysis (Task 1 of the fixed-rsp plan, docs/superpowers/plans/
   2026-06-05-fixed-rsp-frame.md). max_temp_depth approximates the deepest count
   of values codegen must preserve across a sub-evaluation (today via push rax);
   max_outgoing_args is the widest call. These size the future static-rsp frame.
   Over-estimating only wastes a few frame bytes; the precise calibration cross-
   check is Task 5's "no push remains" assertion. Nothing reads these yet. */
static int frame_expr_depth(Expr *e, int *depth_out, int *args_out)
{
	if (!e)
	{
		return 0;
	}

	/* Conservative upper bound: the expression-tree HEIGHT. Codegen preserves at
	   most one value per nesting level descended (a binary lhs, an index base, or a
	   result held across a release), so the count of simultaneously-live temp slots
	   never exceeds the height. Leaves preserve nothing (height 0). This over-counts
	   slightly (a few wasted 8-byte slots) but can never under-count, so converting
	   every push site to temp slots is safe and the overflow trap stays silent. */
	int is_call = e->kind==EX_CALL || e->kind==EX_METHOD_CALL || e->kind==EX_NEW;
	if (is_call)
	{
		int n = e->arg_count + (e->kind==EX_METHOD_CALL ? 1 : 0);
		if (n > *args_out)
		{
			*args_out = n;
		}
	}

	int leaf = e->kind==EX_INT || e->kind==EX_BOOL || e->kind==EX_FLOAT
			   || e->kind==EX_STR || e->kind==EX_IDENT || e->kind==EX_THIS
			   || e->kind==EX_NULL;

	int ch = 0;
	int c = frame_expr_depth(e->lhs, depth_out, args_out);
	if (c > ch) { ch = c; }
	c = frame_expr_depth(e->rhs, depth_out, args_out);
	if (c > ch) { ch = c; }
	if (is_call)
	{
		for (int i = 0; i < e->arg_count; i++)
		{
			c = frame_expr_depth(e->args[i], depth_out, args_out);
			if (c > ch) { ch = c; }
		}
	}

	int d = leaf ? 0 : (1 + ch);
	if (d > *depth_out)
	{
		*depth_out = d;
	}

	return d;
}

/* Upper bound on the scratch block codegen reserves AT THIS NODE (the bytes its
   `sub rsp,N` claims; 0 if it emits none). The static-rsp arena (Task 4 of the
   fixed-rsp plan) replaces those `sub rsp,N` blocks with a software byte-stack, so
   the frame must hold the peak sum of simultaneously-live blocks. Each value here
   over-bounds the corresponding codegen site; the arena overflow trap is the
   runtime backstop if a site is ever missed. */
static int frame_node_block(Expr *e)
{
	switch (e->kind)
	{
	case EX_BINARY:
		if (e->type.kind==TY_STRING)
		{
			return ((64 + 1) * 8 + 15) & ~15;   /* n-ary concat, n <= CONCAT_MAX (64): 528. */
		}

		if (e->type.kind==TY_FLOAT || e->type.kind==TY_DOUBLE)
		{
			return 16;                          /* cg_binary_fp spills the lhs (8, rounded to 16). */
		}

		return 0;                               /* Integer binary uses temp slots now, no sub rsp. */
	case EX_CALL:
	case EX_METHOD_CALL:
	case EX_NEW:
		return 48;                              /* Widest call-shaped block (blocking ctx); arg/builtin blocks <= 32. */
	case EX_INDEX:
		return 32;                              /* Map/array index marshaling block. */
	default:
		return 0;
	}
}

/* Peak nested scratch-block bytes needed to evaluate e. A node's own block is live
   while its children are evaluated (codegen reserves the block, then evaluates the
   operands into it), so the requirement is this node's block plus the deepest child
   requirement. Conservatively treats every child as evaluated under the block. */
static int frame_scratch_bytes(Expr *e, int *max_out)
{
	if (!e)
	{
		return 0;
	}

	int child = 0;
	int c = frame_scratch_bytes(e->lhs, max_out);
	if (c > child) { child = c; }
	c = frame_scratch_bytes(e->rhs, max_out);
	if (c > child) { child = c; }
	if (e->kind==EX_CALL || e->kind==EX_METHOD_CALL || e->kind==EX_NEW)
	{
		for (int i = 0; i < e->arg_count; i++)
		{
			c = frame_scratch_bytes(e->args[i], max_out);
			if (c > child) { child = c; }
		}
	}

	int total = frame_node_block(e) + child;
	if (total > *max_out)
	{
		*max_out = total;
	}

	return total;
}

static void frame_stmt(Stmt *s, int *d, int *a, int *sc);

static void frame_block(Block *b, int *d, int *a, int *sc)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		frame_stmt(b->stmts[i], d, a, sc);
	}
}

static void frame_stmt(Stmt *s, int *d, int *a, int *sc)
{
	if (!s)
	{
		return;
	}

	frame_expr_depth(s->decl_init, d, a);
	frame_expr_depth(s->target, d, a);
	frame_expr_depth(s->value, d, a);
	frame_expr_depth(s->cond, d, a);
	frame_expr_depth(s->ret_val, d, a);
	frame_expr_depth(s->expr, d, a);
	frame_scratch_bytes(s->decl_init, sc);
	frame_scratch_bytes(s->target, sc);
	frame_scratch_bytes(s->value, sc);
	frame_scratch_bytes(s->cond, sc);
	frame_scratch_bytes(s->ret_val, sc);
	frame_scratch_bytes(s->expr, sc);
	frame_stmt(s->for_init, d, a, sc);
	frame_stmt(s->for_post, d, a, sc);
	frame_block(s->then_blk, d, a, sc);
	frame_block(s->else_blk, d, a, sc);
}

static void frame_annotate(Func *f)
{
	int d = 0, a = 0, sc = 0;
	frame_block(f->body, &d, &a, &sc);
	/* d is the expression-tree height. A single construct can hold up to two
	   simultaneous preserves at one level (e.g. an array/field store keeps both the
	   value and the receiver/base across a sub-evaluation), so the live-temp count
	   can reach ~2x the height; 2*d+2 is a safe over-estimate. The cg_temp_push
	   overflow trap remains the backstop if any path still exceeds it. */
	f->max_temp_depth = d > 0 ? (2*d + 2) : 0;
	f->max_outgoing_args = a;
	/* sc is the expression-tree scratch peak. Statement-level codegen (spawn-arg
	   marshaling, foreach iterators) reserves its own scratch outside the expression
	   tree; a fixed margin over-covers it. The arena overflow trap stays the backstop. */
	f->max_scratch_bytes = sc + 128;
}

/* ---- P5: string self-accumulation loop recognizer ----------------------------
   Recognize the O(n^2) shape `s = s + ...` (and `s += ...`) repeated in a loop,
   where `s` is a string local, the chain appends on the end (leftmost leaf is s),
   and `s` is used nowhere else in the loop. When recognized, annotate the loop so
   codegen (Task 2) can lower it to an O(n) StringBuilder. Annotation only here;
   behaviour is unchanged until codegen consumes accum_sb_offset.

   The escape check the design lists (condition 5) is unnecessary: escape.c only
   tracks `new`-allocated object locals, never string locals, so esc_has(s) is
   always false for a string. Condition 4 below (`s` appears nowhere else in the
   body / cond / post) is strictly stronger -- if `s` is never read, passed, or
   aliased in the loop and the loop cannot exit early (condition 6), materializing
   `s` once at loop end is byte-identical to per-iteration concatenation. Being too
   strict only forgoes the optimization; it can never change behaviour. */

static int p5_expr_count_offset(Expr *e, int off)
{
	if (!e)
	{
		return 0;
	}

	int n = (e->kind==EX_IDENT && e->anno_int==off) ? 1 : 0;
	n += p5_expr_count_offset(e->lhs, off);
	n += p5_expr_count_offset(e->rhs, off);
	for (int i=0; i<e->arg_count; i++)
	{
		n += p5_expr_count_offset(e->args[i], off);
	}

	return n;
}

static int p5_stmt_mentions(Stmt *s, int off);

static int p5_block_mentions(Block *b, int off)
{
	if (!b)
	{
		return 0;
	}

	for (int i=0; i<b->count; i++)
	{
		if (p5_stmt_mentions(b->stmts[i], off))
		{
			return 1;
		}
	}

	return 0;
}

static int p5_stmt_mentions(Stmt *s, int off)
{
	if (!s)
	{
		return 0;
	}

	if (p5_expr_count_offset(s->decl_init, off)
			|| p5_expr_count_offset(s->target, off)
			|| p5_expr_count_offset(s->value, off)
			|| p5_expr_count_offset(s->cond, off)
			|| p5_expr_count_offset(s->ret_val, off)
			|| p5_expr_count_offset(s->expr, off))
	{
		return 1;
	}

	return p5_stmt_mentions(s->for_init, off)
		   || p5_stmt_mentions(s->for_post, off)
		   || p5_block_mentions(s->then_blk, off)
		   || p5_block_mentions(s->else_blk, off);
}

/* True if the statement (recursively) can transfer control out of the enclosing
   loop's single fall-off-the-end exit: return/throw leave the function; break
   leaves a loop; try installs a handler the unwinder uses. Conservatively bails
   on a break in a nested loop or switch too (safe, just a missed optimization). */
static int p5_stmt_has_exit(Stmt *s);

static int p5_block_has_exit(Block *b)
{
	if (!b)
	{
		return 0;
	}

	for (int i=0; i<b->count; i++)
	{
		if (p5_stmt_has_exit(b->stmts[i]))
		{
			return 1;
		}
	}

	return 0;
}

static int p5_stmt_has_exit(Stmt *s)
{
	if (!s)
	{
		return 0;
	}

	switch (s->kind)
	{
		case ST_RETURN:
		case ST_THROW:
		case ST_BREAK:
		case ST_TRY:
			return 1;
		default:
			break;
	}

	return p5_stmt_has_exit(s->for_init)
		   || p5_stmt_has_exit(s->for_post)
		   || p5_block_has_exit(s->then_blk)
		   || p5_block_has_exit(s->else_blk);
}

/* Count the leaf operands of a string-concat chain (a '+' whose result is string
   is an interior node). Mirrors codegen's cg_collect_concat so the recognizer can
   refuse a chain the flattener would overflow (CONCAT_MAX). */
static int p5_count_leaves(Expr *e)
{
	if (e->kind == EX_BINARY && e->type.kind == TY_STRING)
	{
		return p5_count_leaves(e->lhs) + p5_count_leaves(e->rhs);
	}

	return 1;
}

static void p5_try_lower_accum(Stmt *loop, Func *f)
{
	Block *B = loop->then_blk;
	if (!B)
	{
		return;
	}

	/* Find the single direct-child `s = <string + chain>` whose leftmost leaf is
	   the assigned string local itself (append-on-end). Two such assignments -> bail. */
	Stmt *A = NULL;
	for (int i=0; i<B->count; i++)
	{
		Stmt *st = B->stmts[i];
		if (st->kind != ST_ASSIGN || st->target->kind != EX_IDENT
				|| st->target->type.kind != TY_STRING)
		{
			continue;
		}

		if (!st->value || st->value->kind != EX_BINARY || st->value->type.kind != TY_STRING)
		{
			continue;
		}

		Expr *leaf = st->value;
		while (leaf->kind == EX_BINARY)
		{
			leaf = leaf->lhs;
		}

		if (leaf->kind != EX_IDENT || leaf->anno_int != st->target->anno_int)
		{
			continue;   /* Prepend (`s = x + s`) or some other shape. */
		}

		if (A)
		{
			return;     /* More than one accumulator into s -> keep v1 simple. */
		}

		A = st;
	}

	if (!A)
	{
		return;
	}

	int soff = A->target->anno_int;

	/* The chain must fit codegen's concat flattener (CONCAT_MAX == 64 leaves);
	   longer chains are vanishingly rare and are simply left unlowered. */
	if (p5_count_leaves(A->value) > 64)
	{
		return;
	}

	/* Condition 4: s occurs exactly once in the chain (the leftmost leaf) and
	   nowhere else in the body, cond, or for clauses. */
	if (p5_expr_count_offset(A->value, soff) != 1)
	{
		return;
	}

	for (int i=0; i<B->count; i++)
	{
		if (B->stmts[i] != A && p5_stmt_mentions(B->stmts[i], soff))
		{
			return;
		}
	}

	if (p5_expr_count_offset(loop->cond, soff) != 0)
	{
		return;
	}

	if (loop->kind == ST_FOR
			&& (p5_stmt_mentions(loop->for_init, soff) || p5_stmt_mentions(loop->for_post, soff)))
	{
		return;
	}

	/* Condition 6: the only loop exit must be falling off the end. */
	if (p5_block_has_exit(B))
	{
		return;
	}

	/* Recognized: reserve a frame slot for the builder and annotate. The slot is
	   the next 8 bytes below the existing locals; codegen lays temps/scratch below
	   frame_size, so [rbp - frame_size] does not collide with any local. */
	f->frame_size += 8;
	loop->accum_sb_offset = f->frame_size;
	loop->accum_stmt = A;
}

static void p5_scan_block(Block *b, Func *f);

static void p5_scan_stmt(Stmt *s, Func *f)
{
	if (!s)
	{
		return;
	}

	if (s->kind == ST_WHILE || s->kind == ST_FOR)
	{
		p5_try_lower_accum(s, f);
	}

	p5_scan_stmt(s->for_init, f);
	p5_scan_stmt(s->for_post, f);
	p5_scan_block(s->then_blk, f);
	p5_scan_block(s->else_blk, f);
}

static void p5_scan_block(Block *b, Func *f)
{
	if (!b)
	{
		return;
	}

	for (int i=0; i<b->count; i++)
	{
		p5_scan_stmt(b->stmts[i], f);
	}
}

void resolve_func(TypeTable *tt, Func *f, const char *this_class)
{
	g_types=tt;
	if (f->is_extern)
	{
		/* Validate the FFI signature, then skip body resolution (there is none). */
		if (f->ret_type.kind==TY_STRING || f->ret_type.kind==TY_OBJECT
				|| f->ret_type.kind==TY_ARRAY || f->ret_type.kind==TY_MAP)
		{
			die(0,"extern return type must be a scalar or long (string/object returns are not supported yet).",NULL);
		}

		return;
	}

	/* Default parameter values may only fill trailing parameters: once a
	   parameter has a default, every parameter after it must have one too. */
	int seen_default=0;
	for (int i=0; i<f->param_count; i++)
	{
		if (f->params[i].def)
		{
			seen_default=1;
		}
		else if (seen_default)
		{
			die(0,"A parameter without a default cannot follow one with a default.",NULL);
		}
	}

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
	p5_scan_block(f->body,f);   /* P5: recognize string self-accumulation loops (annotation only). */
	frame_annotate(f);
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
			ClassDecl *d=u->klass;
			for (int k=0; k<d->method_count; k++)
			{
				/* A static method has no `this`: resolve it like a free function. */
				int mstatic = d->methods[k]->is_static || d->is_static;
				resolve_func(tt,d->methods[k], mstatic ? NULL : d->name);
			}

			if (d->ctor)
			{
				resolve_func(tt,d->ctor,d->name);
			}

			/* Static field initializers live outside any function body; resolve
			   each in an empty scope and type-check against the field. */
			for (int k=0; k<d->field_count; k++)
			{
				int fstatic = d->fields[k].is_static || d->is_static;
				if (d->fields[k].init)
				{
					if (!fstatic)
					{
						die(d->fields[k].init->line,"Field initializers are only allowed on static fields.",NULL);
					}

					SymTable es;
					sym_init(&es);
					resolve_expr(&es,d->fields[k].init,NULL);
					if (!assignable(&d->fields[k].type,&d->fields[k].init->type))
					{
						die(d->fields[k].init->line,"Static field initializer type mismatch; add a cast.",NULL);
					}
				}
			}
		}
	}
}
