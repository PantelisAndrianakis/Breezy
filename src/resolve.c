#include "resolve.h"
#include "symtable.h"
#include "ownership.h"
#include "escape.h"
#include "constprop.h"
#include "promote.h"
#include "nonneg.h"
#include "bce.h"
#include "enums.h"
#include "lexer.h"
#include "overload.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static TypeTable *g_types;
static const TypeRef *g_ret;   /* Return type of the function being resolved. */
static int g_loop_depth;       /* >0 inside a while/for/foreach body; gates continue. */
static int g_break_depth;      /* >0 inside a loop OR switch body; gates break. */
static SymTable *g_lam_enc;    /* Enclosing scope while resolving a lambda body (NULL outside one). */
static LambdaInfo *g_cur_lam;  /* Lambda whose body is currently being resolved (for capture recording). */
static int g_lambda_seq;       /* Monotonic id for synthetic lambda body labels. */

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

/* A class is Comparable if it declares `int compareTo(SelfClass other)`. The
   ordered containers (PriorityQueue / TreeMap / TreeSet) call it to order object
   keys -- the ordering analogue of the reserved protocol the hash containers use
   to hash object keys. We do not add F-bounded generics: the self-typed parameter
   is accepted as any object/generic type, checked structurally here. */
static int class_is_comparable(const char *class_name)
{
	ClassInfo *ci = types_find_class(g_types, class_name);
	if (!ci)
	{
		return 0;
	}

	MethodInfo *m = types_find_method(ci, "compareTo");
	if (!m || m->ret_type.kind != TY_INT || m->param_count != 1)
	{
		return 0;
	}

	TypeKind pk = m->param_types[0].kind;
	return pk == TY_OBJECT || pk == TY_GENERIC;
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
			|| strcmp(m,"sin")==0 || strcmp(m,"cos")==0 || strcmp(m,"tan")==0 || strcmp(m,"exp")==0)
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
	else if (strcmp(nm,"toBytes")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"String.toBytes() takes no arguments.",NULL);
		}

		TypeRef elem;
		memset(&elem, 0, sizeof(elem));
		elem.kind = TY_BYTE;
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

	if (strcmp(m,"getenv")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_STRING)
		{
			die(e->line,"System.getenv(name) takes one string argument.",NULL);
		}

		e->type.kind = TY_STRING;
		return;
	}

	if (strcmp(m,"awaitShutdown")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"System.awaitShutdown() takes no arguments.",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"sleep")==0)
	{
		if (e->arg_count != 1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"System.sleep(ms) takes one integer argument.",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"rawMode")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_BOOL)
		{
			die(e->line,"System.rawMode(on) takes one bool argument.",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"pollKey")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"System.pollKey() takes no arguments.",NULL);
		}

		e->type.kind = TY_INT;   /* Next input byte (0..255), or -1 when none. */
		return;
	}

	if (strcmp(m,"mouseMode")==0)
	{
		if (e->arg_count != 1 || e->args[0]->type.kind != TY_BOOL)
		{
			die(e->line,"System.mouseMode(on) takes one bool argument.",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"pollMouse")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"System.pollMouse() takes no arguments.",NULL);
		}

		e->type.kind = TY_LONG;   /* Packed event (x/y/flags), or -1 when none. */
		return;
	}

	if (strcmp(m,"cpuCount")==0)
	{
		if (e->arg_count != 0)
		{
			die(e->line,"System.cpuCount() takes no arguments.",NULL);
		}

		e->type.kind = TY_INT;
		return;
	}

	if (strcmp(m,"affinity")==0)
	{
		if (e->arg_count != 1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"System.affinity(mask) takes one integer bitmask argument.",NULL);
		}

		e->type.kind = TY_BOOL;   /* true on success, false on failure. */
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

	if (strcmp(m,"rawSocket")==0)
	{
		if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"Network.rawSocket(protocol) takes one integer protocol.",NULL);
		}

		e->type.kind=TY_SOCKET;   /* Reuse the socket handle: read/write/close work as-is. */
		return;
	}

	if (strcmp(m,"tlsConnect")==0)
	{
		if ((e->arg_count!=2 && e->arg_count!=3)
			|| e->args[0]->type.kind!=TY_STRING || !ty_is_int(e->args[1]->type.kind)
			|| (e->arg_count==3 && e->args[2]->type.kind!=TY_STRING))
		{
			die(e->line,"Network.tlsConnect(host, port[, caBundlePath]) takes a string, an integer, and an optional string.",NULL);
		}

		e->type.kind=TY_TLSSOCKET;
		return;
	}

	if (strcmp(m,"tlsListen")==0)
	{
		if (e->arg_count!=3 || !ty_is_int(e->args[0]->type.kind)
			|| e->args[1]->type.kind!=TY_STRING || e->args[2]->type.kind!=TY_STRING)
		{
			die(e->line,"Network.tlsListen(port, certPath, keyPath) takes an integer and two strings.",NULL);
		}

		e->type.kind=TY_TLSLISTENER;
		return;
	}

	die(e->line,"Unknown Network method: ",m);
}

static void resolve_graphics(Expr *e)
{
	const char *m = e->name + 9;   /* After "Graphics.". */
	if (strcmp(m,"open")==0)
	{
		if (e->arg_count!=3 || !ty_is_int(e->args[0]->type.kind)
			|| !ty_is_int(e->args[1]->type.kind) || e->args[2]->type.kind!=TY_STRING)
		{
			die(e->line,"Graphics.open(width, height, title) takes two integers and a string.",NULL);
		}

		e->type.kind=TY_SURFACE;
		return;
	}

	if (strcmp(m,"openGL")==0)
	{
		if (e->arg_count!=3 || !ty_is_int(e->args[0]->type.kind)
			|| !ty_is_int(e->args[1]->type.kind) || e->args[2]->type.kind!=TY_STRING)
		{
			die(e->line,"Graphics.openGL(width, height, title) takes two integers and a string.",NULL);
		}

		e->type.kind=TY_GLSURFACE;
		return;
	}

	die(e->line,"Unknown Graphics method: ",m);
}

static void resolve_ffi(Expr *e)
{
	const char *m = e->name + 4;   /* After "Ffi.". */
	if (strcmp(m,"bind")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"Ffi.bind(path) takes one string library path.",NULL);
		}

		e->type.kind=TY_BOOL;
		return;
	}

	die(e->line,"Unknown Ffi method: ",m);
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

static void resolve_xml(Expr *e)
{
	const char *m = e->name + 4;   /* After "Xml.". */
	if (strcmp(m,"parse")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"Xml.parse(text) takes one string.",NULL);
		}

		e->type.kind = TY_XMLNODE;
		return;
	}

	die(e->line,"Unknown Xml method: ",m);
}

static void resolve_json(Expr *e)
{
	const char *m = e->name + 5;   /* After "Json.". */
	if (strcmp(m,"parse")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
		{
			die(e->line,"Json.parse(text) takes one string.",NULL);
		}

		e->type.kind = TY_JSONVALUE;
		return;
	}

	if (strcmp(m,"ofNull")==0)
	{
		if (e->arg_count!=0)
		{
			die(e->line,"Json.ofNull() takes no arguments.",NULL);
		}

		e->type.kind = TY_JSONVALUE;
		return;
	}

	if (strcmp(m,"stringify")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_JSONVALUE)
		{
			die(e->line,"Json.stringify(value) takes one JsonValue.",NULL);
		}

		e->type.kind = TY_STRING;
		return;
	}

	if (strcmp(m,"of")==0)
	{
		if (e->arg_count!=1)
		{
			die(e->line,"Json.of(value) takes one argument.",NULL);
		}

		TypeKind ak = e->args[0]->type.kind;
		int ok = ty_is_int(ak) || ak==TY_DOUBLE || ak==TY_FLOAT || ak==TY_STRING || ak==TY_BOOL
				 || (ak==TY_GENERIC && strcmp(e->args[0]->type.class_name,"List")==0
					 && e->args[0]->type.elem && e->args[0]->type.elem->kind==TY_JSONVALUE)
				 || ak==TY_MAP;
		if (!ok)
		{
			die(e->line,"Json.of accepts a number, string, bool, List<JsonValue>, or map<string,JsonValue>.",NULL);
		}

		e->type.kind = TY_JSONVALUE;
		return;
	}

	die(e->line,"Unknown Json method: ",m);
}

static void resolve_http(Expr *e)
{
	const char *m = e->name + 5;   /* After "Http.". */
	if (strcmp(m,"readRequest")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_SOCKET)
		{
			die(e->line,"Http.readRequest(socket) takes one Socket.",NULL);
		}

		e->type.kind = TY_HTTPREQUEST;
		return;
	}

	if (strcmp(m,"respond")==0)
	{
		if (e->arg_count!=3 || e->args[0]->type.kind!=TY_SOCKET
			|| !ty_is_int(e->args[1]->type.kind) || e->args[2]->type.kind!=TY_STRING)
		{
			die(e->line,"Http.respond(socket, status, body) takes a Socket, an int, and a string.",NULL);
		}

		e->type.kind = TY_VOID;
		return;
	}

	if (strcmp(m,"response")==0)
	{
		if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
		{
			die(e->line,"Http.response(status) takes one int.",NULL);
		}

		e->type.kind = TY_HTTPRESPONSE;
		return;
	}

	if (strcmp(m,"request")==0)
	{
		if (e->arg_count!=2 || e->args[0]->type.kind!=TY_STRING || e->args[1]->type.kind!=TY_STRING)
		{
			die(e->line,"Http.request(method, path) takes two strings.",NULL);
		}

		e->type.kind = TY_HTTPREQUEST;
		return;
	}

	if (strcmp(m,"readResponse")==0)
	{
		if (e->arg_count!=1 || e->args[0]->type.kind!=TY_SOCKET)
		{
			die(e->line,"Http.readResponse(socket) takes one Socket.",NULL);
		}

		e->type.kind = TY_HTTPRESPONSE;
		return;
	}

	die(e->line,"Unknown Http method: ",m);
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
static void resolve_block(SymTable *st, Block *b, const char *tc);
static void resolve_lambda(SymTable *st, Expr *e, const TypeRef *expected, const char *tc);
static void resolve_combinator(SymTable *st, Expr *e, TypeRef *T, const char *tc);

/* Resolve an expression that may be a lambda against an expected function type:
   a lambda gets its parameter types inferred from `expected` and its captures
   collected; anything else resolves normally (the expected type is unused). */
static void resolve_lambda(SymTable *st, Expr *e, const TypeRef *expected, const char *tc);

static void resolve_value(SymTable *st, Expr *e, const TypeRef *expected, const char *tc)
{
	if (e->kind == EX_LAMBDA)
	{
		resolve_lambda(st, e, expected, tc);
		return;
	}

	/* A bare named function in a (P)->R context becomes a function value: rewrite
	   the identifier in place into an adapter lambda (a0, ...) => name(a0, ...) and
	   resolve that, reusing the closure machinery (it captures nothing -> singleton). */
	if (e->kind == EX_IDENT && expected && expected->kind == TY_FUNC
			&& types_find_func(g_types, e->name))
	{
		char fname[64];
		snprintf(fname,sizeof(fname),"%s",e->name);
		LambdaInfo *li = lambda_new();
		Expr *call = expr_new(EX_CALL, e->line);
		snprintf(call->name,sizeof(call->name),"%s",fname);
		for (int i = 0; i < expected->targ_count; i++)
		{
			char an[24];
			snprintf(an,sizeof(an),"__a%d",i);
			lambda_add_param(li, an, 1, *expected->targs[i]);
			Expr *a = expr_new(EX_IDENT, e->line);
			snprintf(a->name,sizeof(a->name),"%s",an);
			expr_add_arg(call, a);
		}

		li->is_block = 0;
		li->body_expr = call;
		e->kind = EX_LAMBDA;
		e->lam = li;
		resolve_lambda(st, e, expected, tc);
		return;
	}

	resolve_expr(st, e, tc);
}

static LambdaCap *lam_find_cap(LambdaInfo *l, const char *name)
{
	for (int i = 0; i < l->cap_count; i++)
	{
		if (strcmp(l->caps[i].name, name) == 0)
		{
			return &l->caps[i];
		}
	}

	return NULL;
}

static LambdaCap *lam_add_cap(LambdaInfo *l, const char *name, TypeRef type, int src_off)
{
	if (l->cap_count >= (int)(sizeof(l->caps)/sizeof(l->caps[0])))
	{
		die(0,"Too many captured variables in a lambda.",NULL);
	}

	LambdaCap *c = &l->caps[l->cap_count++];
	snprintf(c->name,sizeof(c->name),"%s",name);
	c->type = type;
	c->src_offset = src_off;
	c->is_managed = ty_is_managed(type.kind);
	c->env_offset = 32 + (l->cap_count - 1) * 8;   /* Env header is 32 bytes; captures follow. */
	return c;
}

/* A malloc'd array of the call's resolved argument types, for overload_select.
   Leaked deliberately (the compiler is short-lived); avoids a fixed arg cap. */
static TypeRef *arg_types_of(Expr *e)
{
	TypeRef *a = malloc(sizeof(TypeRef) * (e->arg_count > 0 ? e->arg_count : 1));
	for (int i = 0; i < e->arg_count; i++)
	{
		a[i] = e->args[i]->type;
	}
	return a;
}

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

	while (e->arg_count < ast->param_count)
	{
		Expr *d = ast->params[e->arg_count].def;
		if (!d)
		{
			break;
		}

		Expr *copy = expr_clone(d);
		resolve_expr(st, copy, tc);
		expr_add_arg(e, copy);
	}
}

static void resolve_args(SymTable *st, Expr *e, const char *tc)
{
	for (int i=0; i<e->arg_count; i++)
	{
		if (e->args[i]->is_func_addr)
		{
			continue;   /* FFI function-pointer arg: already typed as its address (TY_LONG). */
		}

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
		int scalar_or_obj = ty_is_int(ek) || ty_is_float(ek) || ek==TY_BOOL || ek==TY_STRING || ek==TY_OBJECT || ek==TY_FUNC || ek==TY_XMLNODE || ek==TY_JSONVALUE;
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
		if (e->lhs->type.kind==TY_GENERIC && strcmp(e->lhs->type.class_name,"List")==0)
		{
			/* List subscript read: rewrite `list[i]` into `list.get(i)` so it reuses
			   the existing (ring-aware, bounds-checked, managed-safe) get lowering.
			   A `list[i] = v` target is rewritten to set() one level up, in ST_ASSIGN,
			   before this rvalue path is ever reached. */
			if (!ty_is_int(e->rhs->type.kind))
			{
				die(e->line,"List index must be an integer.",NULL);
			}

			Expr *idx = e->rhs;
			e->kind = EX_METHOD_CALL;
			snprintf(e->name, sizeof e->name, "get");
			e->rhs = NULL;
			e->args = NULL;
			e->arg_count = 0;
			expr_add_arg(e, idx);
			resolve_expr(st, e, tc);   /* Resolve as the method call (sets e->type = elem). */
			break;
		}

		if (e->lhs->type.kind!=TY_ARRAY)
		{
			die(e->line,"Indexing a non-array.",NULL);
		}

		if (!ty_is_int(e->rhs->type.kind))
		{
			die(e->line,"Array index must be an integer.",NULL);
		}

		e->type = *e->lhs->type.elem;
		/* A managed element of an array whose static type may cross cores gets
		   SHARED-bit gated access in codegen (the locked slot helpers when the
		   array actually crossed); the gated read is OWNED on both paths. Value
		   arrays never gate - aligned native element access is already atomic. */
		e->anno_shared_gate = ty_is_managed(e->type.kind)
							  && types_typeref_maybe_shared(g_types,&e->lhs->type);
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
		if (!s && g_cur_lam)
		{
			/* Inside a lambda body, a name not bound by a parameter/local that
			   names an enclosing local is captured BY VALUE into the closure env. */
			LambdaCap *c = lam_find_cap(g_cur_lam, e->name);
			if (!c)
			{
				Symbol *encs = g_lam_enc ? sym_find(g_lam_enc, e->name) : NULL;
				if (encs)
				{
					c = lam_add_cap(g_cur_lam, e->name, encs->type, encs->offset);
					/* Give the capture a real slot in the body's frame; a prologue
					   seeds it from the env object so every read path (including the
					   binary-op leaf fusion) just reads a normal local. `st` here is
					   the body's own symbol table. */
					Symbol *ls = sym_add(st, e->name, encs->type);
					c->local_slot = ls->offset;
				}
			}

			if (c)
			{
				e->type = c->type;
				e->anno_int = c->local_slot;
				break;
			}
		}

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
	case EX_LAMBDA:
		resolve_lambda(st, e, NULL, tc);   /* No target type here: params must be annotated. */
		break;
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
			OverloadCand cands[8] = {0};   /* Zero is_variadic for all (only externs are variadic). */
			for (int ci=0; ci<nc->ctor_count; ci++)
			{
				cands[ci].param_types=nc->ctors[ci].param_types;
				cands[ci].param_count=nc->ctors[ci].param_count;
				cands[ci].min_args=overload_min_args(nc->ctors[ci].ast);
			}

			TypeRef *argtypes=arg_types_of(e);

			int sel=overload_select(cands,nc->ctor_count,argtypes,e->arg_count);
			if (sel==OVL_NONE)
			{
				die(e->line,"No constructor matches these arguments: ",e->name);
			}

			if (sel==OVL_AMBIG)
			{
				die(e->line,"Ambiguous constructor call: ",e->name);
			}

			e->anno_overload=sel;
			fill_default_args(st, e, nc->ctors[sel].ast, tc);
			for (int i=0; i<e->arg_count; i++)
			{
				if (!assignable(&nc->ctors[sel].param_types[i], &e->args[i]->type))
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
		if (e->op==TOKEN_NOT)
		{
			if (e->lhs->type.kind!=TY_BOOL)
			{
				die(e->line,"Logical 'not' requires a boolean operand.",NULL);
			}

			e->type.kind=TY_BOOL;
			break;
		}

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
		if (e->op==TOKEN_AND || e->op==TOKEN_OR || e->op==TOKEN_XOR)
		{
			if (a!=TY_BOOL || b!=TY_BOOL)
			{
				die(e->line,"Logical 'and'/'or'/'xor' require boolean operands.",NULL);
			}

			e->type.kind=TY_BOOL;
			break;
		}

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

			if (enum_const_is_payload(e->lhs->name,e->name))
			{
				die(e->line,"Payload variant needs arguments, e.g. Enum.Variant(...): ",e->name);
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

		if (e->lhs->type.kind==TY_XMLNODE)
		{
			if (strcmp(e->name,"name")==0)
			{
				e->type.kind=TY_STRING;
				e->anno_int=24;         /* name@24. */
			}
			else if (strcmp(e->name,"text")==0)
			{
				e->type.kind=TY_STRING;
				e->anno_int=32;         /* text@32. */
			}
			else if (strcmp(e->name,"attrCount")==0)
			{
				e->type.kind=TY_INT;
				e->anno_int=56;         /* attr_count@56. */
			}
			else if (strcmp(e->name,"children")==0)
			{
				/* A List<XmlNode> stored on the node; a borrowed field read at 64,
				   traversed by the existing collection combinators. */
				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_XMLNODE;
				e->type.kind=TY_GENERIC;
				snprintf(e->type.class_name,sizeof(e->type.class_name),"List");
				e->type.elem=typeref_box(el);
				e->anno_int=64;         /* children@64. */
			}
			else
			{
				die(e->line,"Unknown XmlNode field: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_JSONVALUE)
		{
			if (strcmp(e->name,"size")==0)
			{
				/* Array length / object key count -- kind-dependent, so codegen
				   lowers it to a bzy_json_size call (not a raw offset load). */
				e->type.kind=TY_INT;
				e->anno_int=0;
			}
			else
			{
				die(e->line,"Unknown JsonValue field: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_HTTPREQUEST)
		{
			if (strcmp(e->name,"method")==0)       { e->type.kind=TY_STRING; e->anno_int=24; }
			else if (strcmp(e->name,"path")==0)    { e->type.kind=TY_STRING; e->anno_int=32; }
			else if (strcmp(e->name,"body")==0)    { e->type.kind=TY_STRING; e->anno_int=56; }
			else if (strcmp(e->name,"version")==0) { e->type.kind=TY_STRING; e->anno_int=64; }
			else { die(e->line,"Unknown HttpRequest field: ",e->name); }
			break;
		}

		if (e->lhs->type.kind==TY_HTTPRESPONSE)
		{
			if (strcmp(e->name,"status")==0)      { e->type.kind=TY_INT;    e->anno_int=24; }
			else if (strcmp(e->name,"reason")==0) { e->type.kind=TY_STRING; e->anno_int=32; }
			else if (strcmp(e->name,"body")==0)   { e->type.kind=TY_STRING; e->anno_int=56; }
			else { die(e->line,"Unknown HttpResponse field: ",e->name); }
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
			/* Payload variant construction (Shape.Circle(2.0)): rewrite to
			   `new Shape$Circle(2.0)`, which builds a fresh instance typed as the
			   variant subclass (is-a the base enum, so assignable/matchable). */
			if (enum_const_is_payload(e->lhs->name,e->name))
			{
				if (enum_is_generic(e->lhs->name))
				{
					die(e->line,"Generic enum construction is not supported yet (type inference pending): ",e->name);
				}

				const char *vc=enum_variant_class(e->lhs->name,e->name);
				Expr *nw=expr_new(EX_NEW,e->line);
				strcpy(nw->name,vc);
				for (int a=0; a<e->arg_count; a++)
				{
					expr_add_arg(nw,e->args[a]);
				}

				resolve_expr(st,nw,tc);
				*e=*nw;
				break;
			}

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
				int ocount=types_method_overload_count(sc,e->name);
				if (ocount==0)
				{
					die(e->line,"No such static method: ",e->name);
				}

				resolve_args(st,e,tc);

				OverloadCand cands[16] = {0};   /* Zero is_variadic for all (only externs are variadic). */
				for (int oi=0; oi<ocount && oi<16; oi++)
				{
					MethodInfo *mi=types_find_method_idx(sc,e->name,oi);
					cands[oi].param_types=mi->param_types;
					cands[oi].param_count=mi->param_count;
					cands[oi].min_args=mi->ast ? overload_min_args(mi->ast) : mi->param_count;
				}

				TypeRef *argtypes=arg_types_of(e);

				int sel=overload_select(cands,ocount<16?ocount:16,argtypes,e->arg_count);
				if (sel==OVL_NONE)
				{
					die(e->line,"No static method overload matches these arguments: ",e->name);
				}

				if (sel==OVL_AMBIG)
				{
					die(e->line,"Ambiguous static method call: ",e->name);
				}

				MethodInfo *m=types_find_method_idx(sc,e->name,sel);
				if (!m->is_static)
				{
					die(e->line,"No such static method: ",e->name);
				}

				e->anno_overload=sel;
				fill_default_args(st, e, m->ast, tc);
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

		if (e->lhs->type.kind==TY_TLSLISTENER)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"accept")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"TlsListener.accept() takes no arguments.",NULL);
				}

				e->type.kind=TY_TLSSOCKET;
			}
			else if (strcmp(e->name,"port")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"TlsListener.port() takes no arguments.",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"TlsListener.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown TlsListener method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_TLSSOCKET)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"read")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"TlsSocket.read(maxBytes) takes one integer.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_BYTE;
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);   /* byte[]. */
			}
			else if (strcmp(e->name,"write")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_ARRAY)
				{
					die(e->line,"TlsSocket.write(byte[]) takes one byte[].",NULL);
				}

				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"TlsSocket.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown TlsSocket method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_SURFACE)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"present")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_ARRAY)
				{
					die(e->line,"Surface.present(int[]) takes one int[] framebuffer.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"pollEvent")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Surface.pollEvent() takes no arguments.",NULL);
				}

				e->type.kind=TY_LONG;
			}
			else if (strcmp(e->name,"isOpen")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Surface.isOpen() takes no arguments.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"Surface.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown Surface method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_GLSURFACE)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"pollEvent")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"GlSurface.pollEvent() takes no arguments.",NULL);
				}

				e->type.kind=TY_LONG;
			}
			else if (strcmp(e->name,"swapBuffers")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"GlSurface.swapBuffers() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"isOpen")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"GlSurface.isOpen() takes no arguments.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"GlSurface.close() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown GlSurface method: ",e->name);
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

		if (e->lhs->type.kind==TY_XMLNODE)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"attr")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"XmlNode.attr(name) takes one string.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"hasAttr")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"XmlNode.hasAttr(name) takes one string.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"attrNameAt")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"XmlNode.attrNameAt(index) takes one integer.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"descendants")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"XmlNode.descendants() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_XMLNODE;
				e->type.kind=TY_GENERIC;
				snprintf(e->type.class_name,sizeof(e->type.class_name),"List");
				e->type.elem=typeref_box(el);
			}
			else
			{
				die(e->line,"Unknown XmlNode method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_JSONVALUE)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"isNull")==0 || strcmp(e->name,"isBool")==0
				|| strcmp(e->name,"isNumber")==0 || strcmp(e->name,"isString")==0
				|| strcmp(e->name,"isArray")==0 || strcmp(e->name,"isObject")==0
				|| strcmp(e->name,"asBool")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"JsonValue.",e->name);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"type")==0 || strcmp(e->name,"asString")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"JsonValue.",e->name);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"asLong")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"JsonValue.asLong() takes no arguments.",NULL);
				}

				e->type.kind=TY_LONG;
			}
			else if (strcmp(e->name,"asDouble")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"JsonValue.asDouble() takes no arguments.",NULL);
				}

				e->type.kind=TY_DOUBLE;
			}
			else if (strcmp(e->name,"get")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"JsonValue.get(key) takes one string.",NULL);
				}

				e->type.kind=TY_JSONVALUE;
			}
			else if (strcmp(e->name,"has")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"JsonValue.has(key) takes one string.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"keys")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"JsonValue.keys() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_STRING;
				e->type.kind=TY_GENERIC;
				snprintf(e->type.class_name,sizeof(e->type.class_name),"List");
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"items")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"JsonValue.items() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_JSONVALUE;
				e->type.kind=TY_GENERIC;
				snprintf(e->type.class_name,sizeof(e->type.class_name),"List");
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"at")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"JsonValue.at(index) takes one integer.",NULL);
				}

				e->type.kind=TY_JSONVALUE;
			}
			else
			{
				die(e->line,"Unknown JsonValue method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_HTTPREQUEST)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"header")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpRequest.header(name) takes one string.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"hasHeader")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpRequest.hasHeader(name) takes one string.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"headerNames")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"HttpRequest.headerNames() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_STRING;
				e->type.kind=TY_GENERIC;
				snprintf(e->type.class_name,sizeof(e->type.class_name),"List");
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"bodyBytes")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"HttpRequest.bodyBytes() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_BYTE;
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"setHeader")==0)
			{
				if (e->arg_count!=2 || e->args[0]->type.kind!=TY_STRING || e->args[1]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpRequest.setHeader(name, value) takes two strings.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"setBody")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpRequest.setBody(body) takes one string.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"send")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_SOCKET)
				{
					die(e->line,"HttpRequest.send(socket) takes one Socket.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown HttpRequest method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_HTTPRESPONSE)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"header")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpResponse.header(name) takes one string.",NULL);
				}

				e->type.kind=TY_STRING;
			}
			else if (strcmp(e->name,"hasHeader")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpResponse.hasHeader(name) takes one string.",NULL);
				}

				e->type.kind=TY_BOOL;
			}
			else if (strcmp(e->name,"headerNames")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"HttpResponse.headerNames() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_STRING;
				e->type.kind=TY_GENERIC;
				snprintf(e->type.class_name,sizeof(e->type.class_name),"List");
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"bodyBytes")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"HttpResponse.bodyBytes() takes no arguments.",NULL);
				}

				TypeRef el;
				memset(&el,0,sizeof(el));
				el.kind=TY_BYTE;
				e->type.kind=TY_ARRAY;
				e->type.elem=typeref_box(el);
			}
			else if (strcmp(e->name,"setHeader")==0)
			{
				if (e->arg_count!=2 || e->args[0]->type.kind!=TY_STRING || e->args[1]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpResponse.setHeader(name, value) takes two strings.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"setBody")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_STRING)
				{
					die(e->line,"HttpResponse.setBody(body) takes one string.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"send")==0)
			{
				if (e->arg_count!=1 || e->args[0]->type.kind!=TY_SOCKET)
				{
					die(e->line,"HttpResponse.send(socket) takes one Socket.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown HttpResponse method: ",e->name);
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
			else if (strcmp(e->name,"readInto")==0)
			{
				if (e->arg_count!=3 || e->args[0]->type.kind!=TY_ARRAY || !ty_is_int(e->args[1]->type.kind) || !ty_is_int(e->args[2]->type.kind))
				{
					die(e->line,"FileChannel.readInto(buf, offset, maxLen) takes a byte[] and two integers.",NULL);
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
			else if (strcmp(e->name,"lock")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"FileChannel.lock() takes no arguments.",NULL);
				}

				e->type.kind=TY_BOOL;   /* true once the exclusive lock is held. */
			}
			else if (strcmp(e->name,"unlock")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"FileChannel.unlock() takes no arguments.",NULL);
				}

				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"mmap")==0)
			{
				if (e->arg_count!=0)
				{
					die(e->line,"FileChannel.mmap() takes no arguments.",NULL);
				}

				e->type.kind=TY_MAPPEDFILE;
			}
			else
			{
				die(e->line,"Unknown FileChannel method: ",e->name);
			}

			break;
		}

		if (e->lhs->type.kind==TY_MAPPEDFILE)
		{
			resolve_args(st,e,tc);
			if (strcmp(e->name,"size")==0)
			{
				if (e->arg_count!=0) { die(e->line,"MappedFile.size() takes no arguments.",NULL); }
				e->type.kind=TY_LONG;
			}
			else if (strcmp(e->name,"getByte")==0 || strcmp(e->name,"getInt")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"MappedFile.getByte/getInt(offset) takes one integer.",NULL);
				}
				e->type.kind=TY_INT;
			}
			else if (strcmp(e->name,"getLong")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"MappedFile.getLong(offset) takes one integer.",NULL);
				}
				e->type.kind=TY_LONG;
			}
			else if (strcmp(e->name,"putByte")==0 || strcmp(e->name,"putInt")==0 || strcmp(e->name,"putLong")==0)
			{
				if (e->arg_count!=2 || !ty_is_int(e->args[0]->type.kind) || !ty_is_int(e->args[1]->type.kind))
				{
					die(e->line,"MappedFile.putByte/putInt/putLong(offset, value) takes two integers.",NULL);
				}
				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"copyInto")==0)
			{
				if (e->arg_count!=3 || e->args[0]->type.kind!=TY_ARRAY
					|| !ty_is_int(e->args[1]->type.kind) || !ty_is_int(e->args[2]->type.kind))
				{
					die(e->line,"MappedFile.copyInto(byte[] dst, srcOffset, len) takes a byte[] and two integers.",NULL);
				}
				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"flush")==0)
			{
				if (e->arg_count!=0) { die(e->line,"MappedFile.flush() takes no arguments.",NULL); }
				e->type.kind=TY_VOID;
			}
			else if (strcmp(e->name,"close")==0)
			{
				if (e->arg_count!=0) { die(e->line,"MappedFile.close() takes no arguments.",NULL); }
				e->type.kind=TY_VOID;
			}
			else
			{
				die(e->line,"Unknown MappedFile method: ",e->name);
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
			const char *tmpl=e->lhs->type.class_name;
			TypeRef *T=e->lhs->type.elem;
			const char *nm=e->name;

			/* Vector-backed combinators take a lambda whose parameter types come from
			   the element type: resolve the lambda argument against its constructed
			   function type (which drives param inference) BEFORE the generic
			   argument pass, which would otherwise resolve it with no expected type. */
			int vecbacked = strcmp(tmpl,"Box")!=0 && strcmp(tmpl,"Set")!=0
							&& strcmp(tmpl,"PriorityQueue")!=0 && strcmp(tmpl,"TreeSet")!=0
							&& strcmp(tmpl,"TreeMap")!=0;
			if (vecbacked && (strcmp(nm,"map")==0 || strcmp(nm,"filter")==0
							  || strcmp(nm,"forEach")==0 || strcmp(nm,"reduce")==0))
			{
				resolve_combinator(st,e,T,tc);
				break;
			}

			resolve_args(st,e,tc);

			/* Ordered containers require an ordering for object keys/elements:
			   the class must implement Comparable (int compareTo). Primitives and
			   strings order natively. */
			int is_ordered = strcmp(tmpl,"PriorityQueue")==0 || strcmp(tmpl,"TreeSet")==0 || strcmp(tmpl,"TreeMap")==0;
			if (is_ordered && T->kind==TY_OBJECT && !class_is_comparable(T->class_name))
			{
				die(e->line,"An object key/element of an ordered collection must implement Comparable (int compareTo).",NULL);
			}

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

			if (strcmp(tmpl,"PriorityQueue")==0)
			{
				if (strcmp(nm,"add")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"PriorityQueue.add type mismatch.",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else if (strcmp(nm,"poll")==0 || strcmp(nm,"peek")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"This method takes no arguments.",NULL);
					}

					e->type=*T;
				}
				else if (strcmp(nm,"size")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"size() takes no arguments.",NULL);
					}

					e->type.kind=TY_INT;
				}
				else if (strcmp(nm,"isEmpty")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"isEmpty() takes no arguments.",NULL);
					}

					e->type.kind=TY_BOOL;
				}
				else
				{
					die(e->line,"Unknown PriorityQueue method: ",nm);
				}

				break;
			}

			if (strcmp(tmpl,"TreeSet")==0)
			{
				if (strcmp(nm,"add")==0 || strcmp(nm,"remove")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"TreeSet op type mismatch.",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else if (strcmp(nm,"first")==0 || strcmp(nm,"last")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"This method takes no arguments.",NULL);
					}

					e->type=*T;
				}
				else if (strcmp(nm,"floor")==0 || strcmp(nm,"ceiling")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"floor/ceiling type mismatch.",NULL);
					}

					e->type=*T;
				}
				else if (strcmp(nm,"size")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"size() takes no arguments.",NULL);
					}

					e->type.kind=TY_INT;
				}
				else
				{
					die(e->line,"Unknown TreeSet method: ",nm);
				}

				break;
			}

			if (strcmp(tmpl,"TreeMap")==0)
			{
				TypeRef *V=e->lhs->type.elem2;
				if (strcmp(nm,"put")==0)
				{
					if (e->arg_count!=2 || !assignable(T,&e->args[0]->type) || !assignable(V,&e->args[1]->type))
					{
						die(e->line,"TreeMap.put type mismatch.",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else if (strcmp(nm,"get")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"TreeMap.get key type mismatch.",NULL);
					}

					e->type=*V;
				}
				else if (strcmp(nm,"remove")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"TreeMap.remove key type mismatch.",NULL);
					}

					e->type.kind=TY_VOID;
				}
				else if (strcmp(nm,"containsKey")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"TreeMap.containsKey type mismatch.",NULL);
					}

					e->type.kind=TY_BOOL;
				}
				else if (strcmp(nm,"firstKey")==0 || strcmp(nm,"lastKey")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"This method takes no arguments.",NULL);
					}

					e->type=*T;
				}
				else if (strcmp(nm,"floorKey")==0 || strcmp(nm,"ceilingKey")==0)
				{
					if (e->arg_count!=1 || !assignable(T,&e->args[0]->type))
					{
						die(e->line,"floorKey/ceilingKey type mismatch.",NULL);
					}

					e->type=*T;
				}
				else if (strcmp(nm,"size")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"size() takes no arguments.",NULL);
					}

					e->type.kind=TY_INT;
				}
				else if (strcmp(nm,"getKeys")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"getKeys() takes no arguments.",NULL);
					}

					e->type.kind=TY_ARRAY;
					e->type.elem=typeref_box(*T);
				}
				else if (strcmp(nm,"getValues")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"getValues() takes no arguments.",NULL);
					}

					e->type.kind=TY_ARRAY;
					e->type.elem=typeref_box(*V);
				}
				else if (strcmp(nm,"getEntries")==0)
				{
					if (e->arg_count!=0)
					{
						die(e->line,"getEntries() takes no arguments.",NULL);
					}

					TypeRef ent;
					memset(&ent,0,sizeof(ent));
					ent.kind=TY_ENTRY;
					ent.elem=typeref_box(*T);
					ent.elem2=typeref_box(*V);
					e->type.kind=TY_ARRAY;
					e->type.elem=typeref_box(ent);
				}
				else
				{
					die(e->line,"Unknown TreeMap method: ",nm);
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
			else if (strcmp(nm,"reserve")==0)
			{
				if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
				{
					die(e->line,"Reserve(capacity) needs an integer.",NULL);
				}

				e->type.kind=TY_VOID;
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
			else if (strcmp(e->name,"putIfAbsent")==0)
			{
				/* Atomic compound op: the result is the value now associated with
				   the key (the existing value when present, else the inserted one). */
				if (e->arg_count!=2 || !assignable(K,&e->args[0]->type) || !assignable(V,&e->args[1]->type))
				{
					die(e->line,"Map.putIfAbsent(key,value) type mismatch.",NULL);
				}

				e->type = *V;
			}
			else if (strcmp(e->name,"getOrDefault")==0)
			{
				/* Atomic compound op: the value when present, else the default. */
				if (e->arg_count!=2 || !assignable(K,&e->args[0]->type) || !assignable(V,&e->args[1]->type))
				{
					die(e->line,"Map.getOrDefault(key,default) type mismatch.",NULL);
				}

				e->type = *V;
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

		int ocount=types_method_overload_count(c,e->name);
		if (ocount==0)
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

		OverloadCand cands[16] = {0};   /* Zero is_variadic for all (only externs are variadic). */
		for (int oi=0; oi<ocount && oi<16; oi++)
		{
			MethodInfo *mi=types_find_method_idx(c,e->name,oi);
			cands[oi].param_types=mi->param_types;
			cands[oi].param_count=mi->param_count;
			cands[oi].min_args=mi->ast ? overload_min_args(mi->ast) : mi->param_count;
		}

		TypeRef *argtypes=arg_types_of(e);

		int sel=overload_select(cands,ocount<16?ocount:16,argtypes,e->arg_count);
		if (sel==OVL_NONE)
		{
			die(e->line,"No method overload matches these arguments: ",e->name);
		}

		if (sel==OVL_AMBIG)
		{
			die(e->line,"Ambiguous method call: ",e->name);
		}

		MethodInfo *m=types_find_method_idx(c,e->name,sel);
		e->anno_overload=sel;
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
	{
		/* Calling a function value: a local/parameter/captured name of type (P)->R,
		   rather than a named function. Resolve as an indirect call through the
		   closure object (its code pointer at +24, the closure itself as arg0). */
		Symbol *fv = sym_find(st, e->name);
		if (fv && fv->type.kind == TY_FUNC)
		{
			if (e->arg_count != fv->type.targ_count)
			{
				die(e->line,"Function value called with the wrong number of arguments: ",e->name);
			}

			for (int ai = 0; ai < e->arg_count; ai++)
			{
				resolve_expr(st, e->args[ai], tc);
				if (!assignable(fv->type.targs[ai], &e->args[ai]->type))
				{
					die(e->line,"Function-value argument type does not match: ",e->name);
				}
			}

			e->type = *fv->type.elem;
			e->anno_indirect = 1;
			e->anno_int = fv->offset;   /* Stack slot of the closure value. */
			break;
		}
	}

		if (strcmp(e->name,"scheduleAfter")==0 || strcmp(e->name,"scheduleEvery")==0)
		{
			resolve_schedule(st,e,tc);
			break;
		}

		/* FFI: a bare function name passed where the callee expects a `long` (a C
		   function pointer) marshals to the function's address. Mark such args before
		   resolve_args so it skips them; codegen emits `lea [rel label]`. */
		{
			FuncInfo *callee = types_find_func(g_types, e->name);
			if (callee)
			{
				for (int ai = 0; ai < e->arg_count && ai < callee->param_count; ai++)
				{
					TypeRef *pt = &callee->param_types[ai];
					if (e->args[ai]->kind == EX_IDENT
						&& (pt->kind == TY_LONG || pt->kind == TY_FUNC)
						&& types_find_func(g_types, e->args[ai]->name))
					{
						/* A TY_FUNC param checks the named function's signature exactly:
						   return type and every parameter type must match (scalar kinds).
						   A loose `long` param accepts any function address unchecked. */
						if (pt->kind == TY_FUNC)
						{
							FuncInfo *cb = types_find_func(g_types, e->args[ai]->name);
							int ok = (cb->ret_type.kind == (pt->elem ? pt->elem->kind : TY_VOID))
									 && cb->param_count == pt->targ_count;
							for (int k = 0; ok && k < pt->targ_count; k++)
							{
								if (cb->param_types[k].kind != pt->targs[k]->kind)
								{
									ok = 0;
								}
							}

							if (!ok)
							{
								die(e->line,"callback argument does not match the declared function signature: ",e->args[ai]->name);
							}
						}

						e->args[ai]->is_func_addr = 1;
						e->args[ai]->type = *pt;   /* Match the param (TY_LONG or TY_FUNC) for overload selection; codegen emits the address via is_func_addr. */
					}
				}
			}
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

		if (strncmp(e->name,"Graphics.",9)==0)
		{
			resolve_graphics(e);
			break;
		}

		if (strncmp(e->name,"Ffi.",4)==0)
		{
			resolve_ffi(e);
			break;
		}

		if (strncmp(e->name,"Log.",4)==0)
		{
			resolve_log(e);
			break;
		}

		if (strncmp(e->name,"Xml.",4)==0)
		{
			resolve_xml(e);
			break;
		}

		if (strncmp(e->name,"Json.",5)==0)
		{
			resolve_json(e);
			break;
		}

		if (strncmp(e->name,"Http.",5)==0)
		{
			resolve_http(e);
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

		if (strcmp(e->name,"input")==0)
		{
			if (e->arg_count!=0)
			{
				die(e->line,"Input takes no arguments.",NULL);
			}

			e->type.kind=TY_STRING;
			break;
		}

		if (strcmp(e->name,"fromCString")==0)
		{
			if (e->arg_count!=1 || !ty_is_int(e->args[0]->type.kind))
			{
				die(e->line,"fromCString expects one integer (char*) pointer argument.",NULL);
			}

			e->type.kind=TY_STRING;
			break;
		}

		if (strcmp(e->name,"fromCBytes")==0)
		{
			if (e->arg_count!=2 || !ty_is_int(e->args[0]->type.kind) || !ty_is_int(e->args[1]->type.kind))
			{
				die(e->line,"fromCBytes expects a pointer and a length, both integers.",NULL);
			}

			e->type.kind=TY_STRING;
			break;
		}

		if (strcmp(e->name,"fromBytes")==0)
		{
			if (e->arg_count!=1 || e->args[0]->type.kind!=TY_ARRAY
				|| (e->args[0]->type.elem->kind!=TY_BYTE && e->args[0]->type.elem->kind!=TY_UBYTE))
			{
				die(e->line,"fromBytes expects one byte[] or ubyte[] argument.",NULL);
			}

			e->type.kind=TY_STRING;
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
			int ocount=types_func_overload_count(g_types,e->name);
			if (ocount==0)
			{
				die(e->line,"Unknown function: ",e->name);
			}

			OverloadCand cands[16] = {0};   /* Zero is_variadic for all (only externs are variadic). */
			for (int oi=0; oi<ocount && oi<16; oi++)
			{
				FuncInfo *fo=types_find_func_idx(g_types,e->name,oi);
				cands[oi].param_types=fo->param_types;
				cands[oi].param_count=fo->param_count;
				cands[oi].min_args=fo->ast ? overload_min_args(fo->ast) : fo->param_count;
				cands[oi].is_variadic=fo->is_variadic;
			}

			TypeRef *argtypes=arg_types_of(e);

			int sel=overload_select(cands,ocount<16?ocount:16,argtypes,e->arg_count);
			if (sel==OVL_NONE)
			{
				die(e->line,"No function overload matches these arguments: ",e->name);
			}

			if (sel==OVL_AMBIG)
			{
				die(e->line,"Ambiguous function call: ",e->name);
			}

			FuncInfo *fi=types_find_func_idx(g_types,e->name,sel);
			e->anno_overload=sel;
			fill_default_args(st, e, fi->ast, tc);
			for (int i=0; i<e->arg_count && i<fi->param_count; i++)
			{
				if (!assignable(&fi->param_types[i], &e->args[i]->type))
				{
					die(e->line,"Argument type mismatch; add a cast.",NULL);
				}
			}

			/* Variadic (`...`) arguments are C-ABI-native: a scalar/long/double (placed
			   in GP/xmm) or a string (marshalled to char*). Arrays, objects, and maps
			   cannot be passed through `...`. */
			if (fi->is_variadic)
			{
				for (int i=fi->param_count; i<e->arg_count; i++)
				{
					TypeKind ak = e->args[i]->type.kind;
					if (!(ty_is_int(ak) || ty_is_float(ak) || ak==TY_BOOL || ak==TY_STRING))
					{
						die(e->line,"a variadic argument must be a scalar or string.",NULL);
					}
				}
			}

			e->type=fi->ret_type;
		}
		break;
	}
}

static void resolve_block(SymTable *st, Block *b, const char *tc);

/* Registry of every lambda's synthesized body, for the codegen driver to emit. */
#define LAMBDA_MAX 4096
static LambdaInfo *g_lams[LAMBDA_MAX];
static int g_lam_count;

int bzy_lambda_count(void)
{
	return g_lam_count;
}

LambdaInfo *bzy_lambda_at(int i)
{
	return g_lams[i];
}

/* Resolve a lambda literal against the expected function type from its context.
   Infers omitted parameter types from the target, then synthesizes a body
   function (env pointer as arg0, then the lambda parameters) and resolves it
   through the normal function pipeline so it gets a frame and the full analysis
   passes. Captures of enclosing locals are recorded by the EX_IDENT hook while
   that body resolves (g_cur_lam set, g_lam_enc = the enclosing scope). */
static void resolve_lambda(SymTable *st, Expr *e, const TypeRef *expected, const char *tc)
{
	(void)tc;
	LambdaInfo *lam = e->lam;
	if (!expected || expected->kind != TY_FUNC)
	{
		die(e->line,"A lambda needs a known function type from its context (assign it to a (P)->R variable, pass it where one is expected, or return it).",NULL);
	}

	if (expected->targ_count != lam->param_count)
	{
		die(e->line,"Lambda parameter count does not match the expected function type.",NULL);
	}

	for (int i = 0; i < lam->param_count; i++)
	{
		if (!lam->params[i].has_type)
		{
			lam->params[i].type = *expected->targs[i];
			lam->params[i].has_type = 1;
		}
	}

	/* The satisfied signature + a unique body label. */
	TypeRef sig;
	memset(&sig,0,sizeof(sig));
	sig.kind = TY_FUNC;
	for (int i = 0; i < lam->param_count; i++)
	{
		typeref_add_targ(&sig, lam->params[i].type);
	}

	sig.elem = expected->elem;
	lam->sig = sig;
	e->type = sig;
	snprintf(lam->label,sizeof(lam->label),"__lambda_%d",g_lambda_seq++);

	/* Synthesize the body function: env arg0, then the parameters. */
	Func *sf = func_new();
	snprintf(sf->name,sizeof(sf->name),"%s",lam->label);
	sf->is_lambda = 1;
	sf->ret_type = *expected->elem;
	Param *envp = func_add_param(sf);
	snprintf(envp->name,sizeof(envp->name),"__env");
	memset(&envp->type,0,sizeof(envp->type));
	envp->type.kind = TY_LONG;   /* Raw closure pointer; never ARC-managed as a parameter. */
	for (int i = 0; i < lam->param_count; i++)
	{
		Param *pp = func_add_param(sf);
		snprintf(pp->name,sizeof(pp->name),"%s",lam->params[i].name);
		pp->type = lam->params[i].type;
	}

	if (lam->is_block)
	{
		sf->body = lam->body_block;
	}
	else
	{
		Block *b = block_new();
		Stmt *s;
		if (expected->elem->kind == TY_VOID)
		{
			s = stmt_new(ST_EXPR, e->line);
			s->expr = lam->body_expr;
		}
		else
		{
			s = stmt_new(ST_RETURN, e->line);
			s->ret_val = lam->body_expr;
		}

		block_push(b, s);
		sf->body = b;
	}

	/* Resolve + analyze the body. Save the resolver state resolve_func clobbers
	   (this runs nested inside the enclosing function's resolution). */
	SymTable *save_enc = g_lam_enc;
	LambdaInfo *save_lam = g_cur_lam;
	const TypeRef *save_ret = g_ret;
	int save_loop = g_loop_depth, save_brk = g_break_depth;
	g_lam_enc = st;
	g_cur_lam = lam;
	resolve_func(g_types, sf, NULL);
	g_lam_enc = save_enc;
	g_cur_lam = save_lam;
	g_ret = save_ret;
	g_loop_depth = save_loop;
	g_break_depth = save_brk;

	/* Hand the capture seed list to codegen's body prologue. */
	sf->cap_count = lam->cap_count;
	for (int i = 0; i < lam->cap_count; i++)
	{
		sf->cap_env_off[i] = lam->caps[i].env_offset;
		sf->cap_local_off[i] = lam->caps[i].local_slot;
	}

	/* A captured local is a borrow -- the closure environment owns the reference
	   and releases it when the closure is freed. Drop capture slots from the body's
	   release set so the body does not over-release them at its exit. */
	int w = 0;
	for (int r = 0; r < sf->obj_local_count; r++)
	{
		int is_cap = 0;
		for (int i = 0; i < sf->cap_count; i++)
		{
			if (sf->obj_local_offsets[r] == sf->cap_local_off[i])
			{
				is_cap = 1;
				break;
			}
		}

		if (!is_cap)
		{
			sf->obj_local_offsets[w++] = sf->obj_local_offsets[r];
		}
	}

	sf->obj_local_count = w;

	lam->sf = sf;
	if (g_lam_count >= LAMBDA_MAX)
	{
		die(e->line,"Too many lambdas in one program.",NULL);
	}

	g_lams[g_lam_count++] = lam;
}

/* Build a List<elem> generic type (the result element kind of map/filter). */
static TypeRef list_of(TypeRef elem)
{
	TypeRef r;
	memset(&r,0,sizeof(r));
	r.kind = TY_GENERIC;
	snprintf(r.class_name,sizeof(r.class_name),"List");
	r.elem = typeref_box(elem);
	return r;
}

/* Build a function type (params...)->ret as the expected type for a combinator's
   lambda argument. params are copied from the given element/accumulator types. */
static TypeRef func_type(const TypeRef *params, int nparams, TypeRef ret)
{
	TypeRef f;
	memset(&f,0,sizeof(f));
	f.kind = TY_FUNC;
	for (int i = 0; i < nparams; i++)
	{
		typeref_add_targ(&f, params[i]);
	}

	f.elem = typeref_box(ret);
	return f;
}

/* Resolve a vector-backed combinator: map/filter/forEach/reduce. The lambda
   argument is resolved against a function type built from the element type T,
   which supplies the inferred parameter types. v1 map preserves the element
   type (U == T). */
static void resolve_combinator(SymTable *st, Expr *e, TypeRef *T, const char *tc)
{
	const char *nm = e->name;
	if (strcmp(nm,"reduce")==0)
	{
		if (e->arg_count != 2)
		{
			die(e->line,"reduce(seed, (acc,x)->acc) takes a seed and a lambda.",NULL);
		}

		resolve_value(st, e->args[0], NULL, tc);       /* Seed -> accumulator type U. */
		TypeRef U = e->args[0]->type;
		TypeRef params[2] = { U, *T };
		TypeRef ft = func_type(params, 2, U);
		resolve_value(st, e->args[1], &ft, tc);
		if (e->args[1]->type.kind != TY_FUNC)
		{
			die(e->line,"reduce expects a (acc, element) lambda.",NULL);
		}

		e->type = U;
		return;
	}

	if (e->arg_count != 1)
	{
		die(e->line,"This combinator takes a single lambda argument.",NULL);
	}

	TypeRef ret;
	memset(&ret,0,sizeof(ret));
	if (strcmp(nm,"map")==0)
	{
		ret = *T;                                      /* v1: element-type preserving. */
	}
	else if (strcmp(nm,"filter")==0)
	{
		ret.kind = TY_BOOL;
	}
	else   /* forEach */
	{
		ret.kind = TY_VOID;
	}

	TypeRef ft = func_type(T, 1, ret);
	resolve_value(st, e->args[0], &ft, tc);
	if (e->args[0]->type.kind != TY_FUNC)
	{
		die(e->line,"This combinator expects a lambda argument.",NULL);
	}

	if (strcmp(nm,"map")==0)
	{
		e->type = list_of(*T);
	}
	else if (strcmp(nm,"filter")==0)
	{
		e->type = list_of(*T);
	}
	else   /* forEach */
	{
		e->type.kind = TY_VOID;
	}
}

/* Lower payload binds in a `match`: for each arm `Variant(a, b) => ...`, splice
   in, right after the case marker, a tag-narrowing local and one field-read local
   per bind, so the arm body sees ordinary typed locals. Reuses the existing
   vardecl + field-read codegen (including ARC) - no special arm machinery.
       Variant(a, b) =>           __mvN = subject;   (is_narrow: base -> subclass)
                                  TA a = __mvN.f0;
                                  TB b = __mvN.f1;
   Runs once, before the arm bodies resolve, while case labels are still IDENTs. */
static int g_match_bind_seq = 0;

static void inject_match_binds(Stmt *sw, const char *enum_name)
{
	Block *b = sw->then_blk;
	int any = 0;
	for (int i = 0; i < b->count; i++)
	{
		if (b->stmts[i]->kind == ST_CASE && b->stmts[i]->case_bind_count > 0)
		{
			any = 1;
			break;
		}
	}

	if (!any)
	{
		return;
	}

	Block *nb = block_new();
	for (int i = 0; i < b->count; i++)
	{
		Stmt *c = b->stmts[i];
		block_push(nb, c);
		if (c->kind != ST_CASE || c->case_bind_count == 0)
		{
			continue;
		}

		const char *label = c->value->name;   /* Still an IDENT before the ordinal rewrite. */
		const char *vclass = enum_variant_class(enum_name, label);
		if (!vclass)
		{
			die(c->line, "Cannot bind payload fields: not a payload variant: ", label);
		}

		ClassInfo *vc = types_find_class(g_types, vclass);
		FieldInfo *pf[8];
		int pn = 0;
		for (int f = 0; f < vc->field_count; f++)
		{
			if (strcmp(vc->fields[f].name, "__ordinal") != 0
					&& strcmp(vc->fields[f].name, "__name") != 0)
			{
				if (pn < 8)
				{
					pf[pn++] = &vc->fields[f];
				}
			}
		}

		if (c->case_bind_count != pn)
		{
			die(c->line, "Wrong number of payload binds for variant: ", label);
		}

		char mv[64];
		snprintf(mv, sizeof(mv), "__mv%d", g_match_bind_seq++);
		Stmt *nd = stmt_new(ST_VARDECL, c->line);
		nd->is_narrow = 1;
		nd->decl_type.kind = TY_OBJECT;
		strcpy(nd->decl_type.class_name, vclass);
		snprintf(nd->decl_name, sizeof(nd->decl_name), "%s", mv);
		nd->decl_init = expr_clone(sw->cond);
		block_push(nb, nd);

		for (int k = 0; k < pn; k++)
		{
			Stmt *bd = stmt_new(ST_VARDECL, c->line);
			bd->decl_type = pf[k]->type;
			snprintf(bd->decl_name, sizeof(bd->decl_name), "%s", c->case_binds[k]);
			Expr *recv = expr_new(EX_IDENT, c->line);
			snprintf(recv->name, sizeof(recv->name), "%s", mv);
			Expr *fld = expr_new(EX_FIELD, c->line);
			snprintf(fld->name, sizeof(fld->name), "%s", pf[k]->name);
			fld->lhs = recv;
			bd->decl_init = fld;
			block_push(nb, bd);
		}
	}

	sw->then_blk = nb;
}

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
			resolve_value(st,s->decl_init,&s->decl_type,tc);
			/* A match-arm narrowing (is_narrow) assigns the base-enum subject into a
			   variant-subclass local; the matched tag guarantees the dynamic type, so
			   the normal downcast rejection is skipped. */
			if (!s->is_narrow && !assignable(&s->decl_type, &s->decl_init->type))
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

		if (s->target->kind==EX_INDEX)
		{
			/* A List subscript store `list[i] = v` becomes `list.set(i, v)`. Resolve
			   the receiver to learn its type; only List rewrites (arrays keep the
			   native indexed store). The receiver is a plain lvalue base, so the
			   re-resolve on the array fall-through is idempotent. */
			resolve_expr(st,s->target->lhs,tc);
			if (s->target->lhs->type.kind==TY_GENERIC && strcmp(s->target->lhs->type.class_name,"List")==0)
			{
				Expr *call = expr_new(EX_METHOD_CALL, s->line);
				call->lhs = s->target->lhs;
				snprintf(call->name, sizeof call->name, "set");
				expr_add_arg(call, s->target->rhs);   /* index. */
				expr_add_arg(call, s->value);          /* value (type-checked by set's resolve). */
				s->kind = ST_EXPR;
				s->expr = call;
				resolve_expr(st, s->expr, tc);
				break;
			}
		}

		resolve_expr(st,s->target,tc);
		resolve_value(st,s->value,&s->target->type,tc);
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
			resolve_value(st,s->ret_val,g_ret,tc);
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
		/* Splice payload binds into match arms before the arm bodies resolve. */
		if (s->is_match && is_enum_switch)
		{
			inject_match_binds(s, s->cond->type.class_name);
		}
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

		/* `match` is exhaustive: with no default arm, every enum constant must
		   have an arm. Report the first uncovered constant. (seen[] holds the
		   case ordinals after the enum-label rewrite above.) */
		if (s->is_match && ndefault==0)
		{
			if (!is_enum_switch)
			{
				die(s->line,"A match without a default must be over an enum.",NULL);
			}

			int total=enum_count_of(s->cond->type.class_name);
			for (int ord=0; ord<total; ord++)
			{
				int covered=0;
				for (int j=0; j<nseen; j++)
				{
					if (seen[j]==ord) { covered=1; break; }
				}

				if (!covered)
				{
					die(s->line,"Non-exhaustive match; missing case: ",
						enum_const_name(s->cond->type.class_name,ord));
				}
			}
		}

		break;
	}
	case ST_CASE:
	case ST_DEFAULT:
		break;   /* Resolved as part of the enclosing switch body. */
	case ST_SELECT:
	{
		if (s->sel_arm_count==0)
		{
			die(s->line,"select needs at least one send/receive arm.",NULL);
		}

		/* No default => a blocking select (cooperatively yields and retries until
		   an arm is ready). With a default it is non-blocking. */
		for (int i=0; i<s->sel_arm_count; i++)
		{
			SelectArm *a=&s->sel_arms[i];
			resolve_expr(st,a->chan,tc);
			if (a->chan->type.kind!=TY_CHANNEL)
			{
				die(a->chan->line,"select arm operand must be a channel.",NULL);
			}

			TypeRef elem=*a->chan->type.elem;
			if (a->is_send)
			{
				resolve_value(st,a->send_val,&elem,tc);
				if (!assignable(&elem,&a->send_val->type))
				{
					die(a->chan->line,"select send arm value type mismatch.",NULL);
				}
			}
			else if (a->bind[0])
			{
				a->bind_type=elem;
				a->bind_offset=sym_add(st,a->bind,elem)->offset;
			}

			resolve_block(st,a->body,tc);
		}

		if (s->else_blk)
		{
			resolve_block(st,s->else_blk,tc);
		}

		break;
	}
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

/* ---- Combinator desugaring ----------------------------------------------------
   A statement-level collection combinator over a literal expression-body lambda is
   rewritten into an equivalent `foreach` loop before resolution, so the loop flows
   through the same hoisting + register promotion a hand-written loop gets (the
   closure path, cg_combinator, stays for nested-expression uses and function-value
   arguments). For example:

       sum = xs.reduce(0, (acc, x) => acc + (x + base));
   becomes
       sum = 0;
       foreach (T __cbN in xs) { sum = sum + (__cbN + base); }

   map/filter build a fresh List and add per element; forEach (expression body)
   drops the result. Captures stay ordinary in-scope reads (base above); the
   element parameter is renamed to a fresh loop variable so it never clashes. */

static int g_desugar_seq;

static void ident_rename(Expr *e, const char *from, const char *to)
{
	if (!e)
	{
		return;
	}

	if (e->kind == EX_IDENT && strcmp(e->name, from) == 0)
	{
		snprintf(e->name, sizeof(e->name), "%s", to);
	}

	ident_rename(e->lhs, from, to);
	ident_rename(e->rhs, from, to);
	for (int i = 0; i < e->arg_count; i++)
	{
		ident_rename(e->args[i], from, to);
	}
}

static Expr *mk_ident(const char *name, int line)
{
	Expr *e = expr_new(EX_IDENT, line);
	snprintf(e->name, sizeof(e->name), "%s", name);
	return e;
}

static Expr *mk_new_list(TypeRef T, int line)
{
	Expr *e = expr_new(EX_NEWGEN, line);
	memset(&e->type, 0, sizeof(e->type));
	e->type.kind = TY_GENERIC;
	snprintf(e->type.class_name, sizeof(e->type.class_name), "List");
	e->type.elem = typeref_box(T);
	return e;
}

/* Replace b->stmts[i] with the k statements in out[], growing the array as needed. */
static void block_splice(Block *b, int i, Stmt **out, int k)
{
	int need = b->count - 1 + k;
	if (need > b->cap)
	{
		int nc = b->cap > 0 ? b->cap : 4;
		while (nc < need)
		{
			nc *= 2;
		}

		Stmt **n = calloc(nc, sizeof(Stmt*));
		memcpy(n, b->stmts, b->count * sizeof(Stmt*));
		b->stmts = n;
		b->cap = nc;
	}

	for (int j = b->count - 1; j > i; j--)
	{
		b->stmts[j + (k - 1)] = b->stmts[j];
	}

	for (int j = 0; j < k; j++)
	{
		b->stmts[i + j] = out[j];
	}

	b->count = need;
}

/* If statement s is a desugarable statement-level combinator, write the equivalent
   statements into out[] (at most 2) and return their count; else return 0. */
static int desugar_combinator(SymTable *st, Stmt *s, const char *tc, Stmt **out)
{
	Expr *C = NULL, *target = NULL;
	int is_vardecl = 0;
	if (s->kind == ST_EXPR)
	{
		C = s->expr;
	}
	else if (s->kind == ST_ASSIGN)
	{
		C = s->value;
		target = s->target;
	}
	else if (s->kind == ST_VARDECL)
	{
		C = s->decl_init;
		is_vardecl = 1;
	}
	else
	{
		return 0;
	}

	if (!C || C->kind != EX_METHOD_CALL)
	{
		return 0;
	}

	const char *nm = C->name;
	int is_filter = strcmp(nm,"filter")==0;
	int is_foreach = strcmp(nm,"forEach")==0, is_reduce = strcmp(nm,"reduce")==0;
	int is_map = 0;   /* map keeps the cg_combinator path (a presized indexed fill). */
	if (!(is_filter || is_foreach || is_reduce))
	{
		return 0;
	}

	/* forEach is a void statement; the others must be the whole right-hand side and
	   assign to a plain name. */
	if (is_foreach && s->kind != ST_EXPR)
	{
		return 0;
	}

	if (!is_foreach && s->kind == ST_EXPR)
	{
		return 0;
	}

	if (s->kind == ST_ASSIGN && target->kind != EX_IDENT)
	{
		return 0;
	}

	int lam_idx = is_reduce ? 1 : 0;
	if (C->arg_count <= lam_idx || C->args[lam_idx]->kind != EX_LAMBDA)
	{
		return 0;
	}

	LambdaInfo *lam = C->args[lam_idx]->lam;
	if (lam->is_block)
	{
		return 0;   /* Expression body only; a block body keeps the closure path. */
	}

	/* Element type from the receiver: resolve a throwaway clone so the foreach
	   resolves the original exactly once. The clone's resolution may register
	   lambdas (a receiver like xs.filter(...)) -- roll the lambda registry back
	   afterwards on every path so those throwaway lambdas are not emitted twice. */
	int lam_save = g_lam_count, seq_save = g_lambda_seq;
	Expr *rc = expr_clone(C->lhs);
	resolve_expr(st, rc, tc);
	TypeKind rk = rc->type.kind;
	char tmpl[64];
	snprintf(tmpl, sizeof(tmpl), "%s", rc->type.class_name);
	int have_elem = rc->type.elem != NULL;
	TypeRef T;
	memset(&T, 0, sizeof(T));
	if (have_elem) { T = typeref_deepcopy(rc->type.elem); }
	g_lam_count = lam_save;
	g_lambda_seq = seq_save;

	if (rk != TY_GENERIC || !have_elem)
	{
		return 0;
	}

	if (strcmp(tmpl,"Box")==0 || strcmp(tmpl,"Set")==0 || strcmp(tmpl,"PriorityQueue")==0
			|| strcmp(tmpl,"TreeSet")==0 || strcmp(tmpl,"TreeMap")==0)
	{
		return 0;
	}

	int line = s->line;

	char xv[32];
	snprintf(xv, sizeof(xv), "__cb%d", g_desugar_seq++);
	const char *elemParam = is_reduce ? lam->params[1].name : lam->params[0].name;

	char sink[64];
	if (is_vardecl)
	{
		snprintf(sink, sizeof(sink), "%s", s->decl_name);
	}
	else if (target)
	{
		snprintf(sink, sizeof(sink), "%s", target->name);
	}
	else
	{
		sink[0] = '\0';
	}

	Stmt *fe = stmt_new(ST_FOREACH, line);
	fe->decl_type = T;
	snprintf(fe->decl_name, sizeof(fe->decl_name), "%s", xv);
	fe->fe_val_type.kind = TY_VOID;
	fe->fe_val_name[0] = '\0';
	fe->expr = C->lhs;
	fe->then_blk = block_new();

	int n = 0;

	/* The shared initializer: `sink = <init>` as a vardecl or an assignment. */
	if (!is_foreach)
	{
		Expr *init = is_reduce ? C->args[0] : mk_new_list(T, line);
		if (is_vardecl)
		{
			Stmt *vd = stmt_new(ST_VARDECL, line);
			vd->decl_type = s->decl_type;
			snprintf(vd->decl_name, sizeof(vd->decl_name), "%s", s->decl_name);
			vd->decl_init = init;
			out[n++] = vd;
		}
		else
		{
			Stmt *as = stmt_new(ST_ASSIGN, line);
			as->target = mk_ident(sink, line);
			as->value = init;
			out[n++] = as;
		}

		/* Pre-size the result to the source length so the fill never reallocates
		   (only when the receiver is a plain name -- reading its `size` twice is
		   then side-effect free). */
		if ((is_map || is_filter) && C->lhs->kind == EX_IDENT)
		{
			Expr *sz = expr_new(EX_FIELD, line);
			snprintf(sz->name, sizeof(sz->name), "size");
			sz->lhs = expr_clone(C->lhs);
			Expr *rv = expr_new(EX_METHOD_CALL, line);
			snprintf(rv->name, sizeof(rv->name), "reserve");
			rv->lhs = mk_ident(sink, line);
			expr_add_arg(rv, sz);
			Stmt *rs = stmt_new(ST_EXPR, line);
			rs->expr = rv;
			out[n++] = rs;
		}
	}

	if (is_reduce)
	{
		Expr *body = expr_clone(lam->body_expr);
		ident_rename(body, lam->params[0].name, sink);   /* acc -> sink. */
		ident_rename(body, elemParam, xv);               /* x   -> loop var. */
		Stmt *bs = stmt_new(ST_ASSIGN, line);
		bs->target = mk_ident(sink, line);
		bs->value = body;
		block_push(fe->then_blk, bs);
	}
	else if (is_map)
	{
		Expr *body = expr_clone(lam->body_expr);
		ident_rename(body, elemParam, xv);
		Expr *add = expr_new(EX_METHOD_CALL, line);
		snprintf(add->name, sizeof(add->name), "add");
		add->lhs = mk_ident(sink, line);
		expr_add_arg(add, body);
		Stmt *es = stmt_new(ST_EXPR, line);
		es->expr = add;
		block_push(fe->then_blk, es);
	}
	else if (is_filter)
	{
		Expr *body = expr_clone(lam->body_expr);
		ident_rename(body, elemParam, xv);
		Expr *add = expr_new(EX_METHOD_CALL, line);
		snprintf(add->name, sizeof(add->name), "add");
		add->lhs = mk_ident(sink, line);
		expr_add_arg(add, mk_ident(xv, line));
		Stmt *es = stmt_new(ST_EXPR, line);
		es->expr = add;
		Stmt *iff = stmt_new(ST_IF, line);
		iff->cond = body;
		iff->then_blk = block_new();
		block_push(iff->then_blk, es);
		block_push(fe->then_blk, iff);
	}
	else   /* forEach, expression body. */
	{
		Expr *body = expr_clone(lam->body_expr);
		ident_rename(body, lam->params[0].name, xv);
		Stmt *es = stmt_new(ST_EXPR, line);
		es->expr = body;
		block_push(fe->then_blk, es);
	}

	out[n++] = fe;
	return n;
}

static void resolve_block(SymTable *st, Block *b, const char *tc)
{
	for (int i=0; i<b->count; i++)
	{
		Stmt *out[3];
		int k = desugar_combinator(st, b->stmts[i], tc, out);
		if (k > 0)
		{
			block_splice(b, i, out, k);
			for (int j = 0; j < k; j++)
			{
				resolve_stmt(st, b->stmts[i + j], tc);
			}

			i += k - 1;
			continue;
		}

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

	/* An expression-body lambda may be inlined into the enclosing frame at a
	   combinator call site, so its body's temps count here too. */
	if (e->kind == EX_LAMBDA && e->lam && !e->lam->is_block)
	{
		c = frame_expr_depth(e->lam->body_expr, depth_out, args_out);
		if (c > ch) { ch = c; }
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
	case EX_METHOD_CALL:
		/* A collection combinator (map/filter/forEach/reduce) reserves a working
		   block plus a per-call argument block simultaneously (cg_combinator). */
		if (strcmp(e->name,"map")==0 || strcmp(e->name,"filter")==0
				|| strcmp(e->name,"forEach")==0 || strcmp(e->name,"reduce")==0)
		{
			return 112;
		}

		return 48;
	case EX_CALL:
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

	/* An expression-body lambda may be inlined into the enclosing frame at a
	   combinator call site, so its body's scratch counts here too. */
	if (e->kind == EX_LAMBDA && e->lam && !e->lam->is_block)
	{
		c = frame_scratch_bytes(e->lam->body_expr, max_out);
		if (c > child) { child = c; }
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
	if (s->kind==ST_SELECT)
	{
		if (*a < 2)
		{
			*a = 2;   /* The try_send/try_recv calls each pass two outgoing args. */
		}

		for (int i=0; i<s->sel_arm_count; i++)
		{
			frame_expr_depth(s->sel_arms[i].chan, d, a);
			frame_expr_depth(s->sel_arms[i].send_val, d, a);
			frame_scratch_bytes(s->sel_arms[i].chan, sc);
			frame_scratch_bytes(s->sel_arms[i].send_val, sc);
			frame_block(s->sel_arms[i].body, d, a, sc);
		}
	}
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

		/* A value array marshals to its element-data pointer (a C buffer). Object and
		   string arrays hold pointers C cannot use as a flat buffer, so reject them. */
		for (int i=0; i<f->param_count; i++)
		{
			TypeRef *pt = &f->params[i].type;
			if (pt->kind==TY_ARRAY && (!pt->elem || ty_is_managed(pt->elem->kind)))
			{
				die(0,"extern array parameter must be a value array (object/string arrays cannot marshal to a C buffer).",NULL);
			}

			/* A callback (TY_FUNC) parameter's signature must be C-ABI-native scalars:
			   no managed types (received as a raw long + fromCString/fromCBytes), no
			   nested function types. */
			if (pt->kind==TY_FUNC)
			{
				TypeKind rk = pt->elem ? pt->elem->kind : TY_VOID;
				if (ty_is_managed(rk) || rk==TY_FUNC)
				{
					die(0,"callback return type must be a scalar.",NULL);
				}

				for (int k=0; k<pt->targ_count; k++)
				{
					TypeKind ak = pt->targs[k]->kind;
					if (ty_is_managed(ak) || ak==TY_FUNC)
					{
						die(0,"callback parameter type must be a scalar.",NULL);
					}
				}
			}
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
	constprop_annotate(f);   /* Rewrite single-assignment literal-scalar reads to the literal (frees their registers). */
	if (!f->is_lambda)
	{
		promote_annotate(f);   /* Choose scalar locals to keep in r12..r15 (codegen consults the map). A lambda's captures have no AST definition (a prologue seeds their slots from env), so promotion would read an unloaded register -- keep lambda bodies on the stack. */
	}

	nonneg_annotate(f);   /* Flag divides with a provably non-negative dividend. */
	bce_annotate(f);   /* Flag array indexes provably in [0, length) so codegen drops the check. */
}

void resolve_program(TypeTable *tt, Unit **units, int unit_count)
{
	g_lam_count = 0;     /* Reset the lambda registry: a process may compile more than once (the test harness). */
	g_lambda_seq = 0;
	g_desugar_seq = 0;
	types_compute_shared_set(tt,units,unit_count);   /* Decide which types get atomic refcounts / op gating before resolving bodies. */

	for (int i=0; i<unit_count; i++)
	{
		Unit *u=units[i];
		for (int k=0; k<u->func_count; k++)
		{
			resolve_func(tt,u->funcs[k],NULL);
		}

		for (int ci=0; ci<u->class_count; ci++)
		{
			ClassDecl *d=u->klasses[ci];
			for (int k=0; k<d->method_count; k++)
			{
				/* A static method has no `this`: resolve it like a free function. */
				int mstatic = d->methods[k]->is_static || d->is_static;
				resolve_func(tt,d->methods[k], mstatic ? NULL : d->name);
			}

			for (int ci=0; ci<d->ctor_count; ci++)
			{
				resolve_func(tt,d->ctors[ci],d->name);
			}

			/* Static field initializers live outside any function body; resolve
			   each in an empty scope and type-check against the field. Instance
			   field initializers were already lowered to constructor assignments
			   by fieldinit_expand, so any init left here belongs to a static
			   field and is type-checked through the normal assignment path. */
			for (int k=0; k<d->field_count; k++)
			{
				if (d->fields[k].init)
				{
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
