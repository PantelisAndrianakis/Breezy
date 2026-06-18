#include "test_framework.h"
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "types.h"
#include "resolve.h"
#include "grow.h"

static TypeTable g_tt;
static Unit *build1(const char *s)
{
	static Parser ps;
	static Unit *u;
	types_init(&g_tt);
	types_register_builtins(&g_tt);
	parser_init(&ps,s);
	u=parse_unit(&ps);
	types_register_unit_names(&g_tt,u);
	types_register_interfaces(&g_tt,u);
	types_register_all_members(&g_tt,&u,1);
	resolve_program(&g_tt,&u,1);
	return u;
}

/* Build several source files, run the generics pass, register + resolve. */
static TypeTable *build_generic(const char **srcs, int n)
{
	static Parser ps[16];
	int units_cap=0;
	Unit **units=grow_reserve(NULL,n,&units_cap,sizeof(*units));
	for (int i=0; i<n; i++)
	{
		parser_init(&ps[i],srcs[i]);
		units[i]=parse_unit(&ps[i]);
	}

	int total=n;
	generics_expand(&units,&total,&units_cap);
	types_init(&g_tt);
	types_register_builtins(&g_tt);
	for (int i=0; i<total; i++)
	{
		types_register_unit_names(&g_tt,units[i]);
	}
	for (int i=0; i<total; i++)
	{
		types_register_interfaces(&g_tt,units[i]);
	}
	types_register_all_members(&g_tt,units,total);

	resolve_program(&g_tt,units,total);
	return &g_tt;
}

/* Full front-end lowering: enums first, then generics, then register + resolve. */
static Unit *g_prog_units[64];
static int   g_prog_total;

static Func *prog_find_func(const char *name)
{
	for (int i=0; i<g_prog_total; i++)
	{
		for (int j=0; j<g_prog_units[i]->func_count; j++)
		{
			if (strcmp(g_prog_units[i]->funcs[j]->name,name)==0)
			{
				return g_prog_units[i]->funcs[j];
			}
		}
	}

	return NULL;
}

static TypeTable *build_program(const char **srcs, int n)
{
	static Parser ps[16];
	int units_cap=0;
	Unit **units=grow_reserve(NULL,n,&units_cap,sizeof(*units));
	for (int i=0; i<n; i++)
	{
		parser_init(&ps[i],srcs[i]);
		units[i]=parse_unit(&ps[i]);
	}

	int total=n;
	enums_expand(&units,&total,&units_cap);
	generics_expand(&units,&total,&units_cap);
	for (int i=0; i<total; i++)
	{
		g_prog_units[i]=units[i];
	}

	g_prog_total=total;
	types_init(&g_tt);
	types_register_builtins(&g_tt);
	for (int i=0; i<total; i++)
	{
		types_register_unit_names(&g_tt,units[i]);
	}
	for (int i=0; i<total; i++)
	{
		types_register_interfaces(&g_tt,units[i]);
	}
	types_register_all_members(&g_tt,units,total);

	resolve_program(&g_tt,units,total);
	return &g_tt;
}

static void test_enum_lowers_to_class(void)
{
	const char *srcs[] =
	{
		"enum Color { RED(255,0,0), GREEN(0,255,0), BLUE(0,0,255); int r; int g; int b;"
		" Color(int r, int g, int b) { this.r = r; this.g = g; this.b = b; } }",
		"void main() { }"
	};
	TypeTable *tt = build_program(srcs, 2);
	ClassInfo *c = types_find_class(tt,"Color");
	ASSERT_INT(c != NULL, 1);
	/* Hidden fields first, then user fields. */
	ASSERT_INT(types_find_field(c,"__ordinal")->type.kind, TY_INT);
	ASSERT_INT(types_find_field(c,"__name")->type.kind, TY_STRING);
	ASSERT_INT(types_find_field(c,"r")->type.kind, TY_INT);
	/* Registry. */
	ASSERT_INT(enum_is("Color"), 1);
	ASSERT_INT(enum_ordinal("Color","GREEN"), 1);
	ASSERT_INT(enum_count_of("Color"), 3);
}

static void test_bitwise_typing(void)
{
	const char *srcs[] =
	{
		"int fand() { return 6 & 3; }"
		"int forr() { return 6 | 1; }"
		"int fxor() { return 5 ^ 1; }"
		"int fnot() { return ~0; }"
		"void main() { }"
	};
	build_program(srcs, 1);
	Func *a = prog_find_func("fand");
	ASSERT_INT(a->body->stmts[0]->ret_val->kind, EX_BINARY);
	ASSERT_INT(a->body->stmts[0]->ret_val->op, TOKEN_AMP);
	ASSERT_INT(a->body->stmts[0]->ret_val->type.kind, TY_INT);
	Func *o = prog_find_func("forr");
	ASSERT_INT(o->body->stmts[0]->ret_val->op, TOKEN_PIPE);
	ASSERT_INT(o->body->stmts[0]->ret_val->type.kind, TY_INT);
	Func *x = prog_find_func("fxor");
	ASSERT_INT(x->body->stmts[0]->ret_val->op, TOKEN_CARET);
	ASSERT_INT(x->body->stmts[0]->ret_val->type.kind, TY_INT);
	Func *n = prog_find_func("fnot");
	ASSERT_INT(n->body->stmts[0]->ret_val->kind, EX_UNARY);
	ASSERT_INT(n->body->stmts[0]->ret_val->op, TOKEN_TILDE);
	ASSERT_INT(n->body->stmts[0]->ret_val->type.kind, TY_INT);
}

static void test_logical_typing(void)
{
	const char *srcs[] =
	{
		"bool fand() { return true and false; }"
		"bool forr() { return true or false; }"
		"bool fxor() { return true xor false; }"
		"bool fnot() { return not true; }"
		"void main() { }"
	};
	build_program(srcs, 1);
	Func *a = prog_find_func("fand");
	ASSERT_INT(a->body->stmts[0]->ret_val->op, TOKEN_AND);
	ASSERT_INT(a->body->stmts[0]->ret_val->type.kind, TY_BOOL);
	Func *o = prog_find_func("forr");
	ASSERT_INT(o->body->stmts[0]->ret_val->type.kind, TY_BOOL);
	Func *x = prog_find_func("fxor");
	ASSERT_INT(x->body->stmts[0]->ret_val->type.kind, TY_BOOL);
	Func *n = prog_find_func("fnot");
	ASSERT_INT(n->body->stmts[0]->ret_val->kind, EX_UNARY);
	ASSERT_INT(n->body->stmts[0]->ret_val->op, TOKEN_NOT);
	ASSERT_INT(n->body->stmts[0]->ret_val->type.kind, TY_BOOL);
}

static void test_enum_static_access(void)
{
	const char *srcs[] =
	{
		"enum Color { RED, GREEN, BLUE; }",
		"void main() { Color c; c = Color.RED; int o; o = c.ordinal(); string n; n = c.name();"
		" Color[] all; all = Color.values(); Color v; v = Color.valueOf(\"GREEN\"); }"
	};
	build_program(srcs, 2);
	Func *m = prog_find_func("main");
	ASSERT_INT(m != NULL, 1);
	/* c = Color.RED -> Color (object). */
	ASSERT_INT(m->body->stmts[1]->value->type.kind, TY_OBJECT);
	ASSERT_STR(m->body->stmts[1]->value->type.class_name, "Color");
	/* o = c.ordinal() -> int;  n = c.name() -> string. */
	ASSERT_INT(m->body->stmts[3]->value->type.kind, TY_INT);
	ASSERT_INT(m->body->stmts[5]->value->type.kind, TY_STRING);
	/* all = Color.values() -> Color[];  v = Color.valueOf(..) -> Color. */
	ASSERT_INT(m->body->stmts[7]->value->type.kind, TY_ARRAY);
	ASSERT_INT(m->body->stmts[7]->value->type.elem->kind, TY_OBJECT);
	ASSERT_STR(m->body->stmts[7]->value->type.elem->class_name, "Color");
	ASSERT_INT(m->body->stmts[9]->value->type.kind, TY_OBJECT);
	ASSERT_STR(m->body->stmts[9]->value->type.class_name, "Color");
}

static void test_static_member_resolves(void)
{
	const char *srcs[] =
	{
		"class C { static int total = 0; static int peek() { return C.total; } int id;"
		" C() { C.total = C.total + 1; this.id = C.total; } }",
		"void main() { C a; a = new C(); int t; t = C.total; int p; p = C.peek(); }"
	};
	build_program(srcs, 2);
	Func *mn = prog_find_func("main");
	ASSERT_INT(mn != NULL, 1);
	ASSERT_INT(mn->body->stmts[3]->value->type.kind, TY_INT);   /* t = C.total */
	ASSERT_INT(mn->body->stmts[5]->value->type.kind, TY_INT);   /* p = C.peek() */
	/* Static access is annotated for codegen with the sentinel + class name. */
	ASSERT_INT(mn->body->stmts[3]->value->anno_int, -1);
	ASSERT_STR(mn->body->stmts[3]->value->anno_str, "C");
}

static void test_generic_lowers_to_class(void)
{
	const char *srcs[] =
	{
		"class Wrap<T> { T value; void set(T v) { this.value = v; } T get() { return this.value; } }",
		"void main() { Wrap<int> b; b = new Wrap<int>(); b.set(7); print(b.get()); }"
	};
	TypeTable *tt = build_generic(srcs, 2);
	ASSERT_INT(types_find_class(tt,"Wrap$int") != NULL, 1);  /* synthesized */
	ASSERT_INT(types_find_class(tt,"Wrap") == NULL, 1);      /* template excluded */
	MethodInfo *m = types_find_method(types_find_class(tt,"Wrap$int"),"get");
	ASSERT_INT(m->ret_type.kind, TY_INT);                    /* T -> int */
}

static void test_generic_multi_param_lowers(void)
{
	const char *srcs[] =
	{
		"class Pair<K, V> { K k; V v; }",
		"void main() { Pair<int, string> p; p = new Pair<int, string>(); }"
	};
	TypeTable *tt = build_generic(srcs, 2);
	ClassInfo *c = types_find_class(tt,"Pair$int$string");
	ASSERT_INT(c != NULL, 1);
	ASSERT_INT(types_find_field(c,"k")->type.kind, TY_INT);
	ASSERT_INT(types_find_field(c,"v")->type.kind, TY_STRING);
}

static void test_generic_bound_ok(void)
{
	const char *srcs[] =
	{
		"interface Speaker { string speak(); }",
		"class Dog implements Speaker { string speak() { return \"woof\"; } }",
		"class Caller<T: Speaker> { T who; string call() { return this.who.speak(); } }",
		"void main() { Caller<Dog> c; c = new Caller<Dog>(); }"
	};
	TypeTable *tt = build_generic(srcs, 4);
	ClassInfo *c = types_find_class(tt,"Caller$Dog");
	ASSERT_INT(c != NULL, 1);
	/* who: T -> Dog; the substituted body this.who.speak() resolves to string. */
	ASSERT_INT(types_find_method(c,"call")->ret_type.kind, TY_STRING);
}

static void test_interface_call_resolves(void)
{
	Unit *u=build1("interface Speaker { string speak(); }"
				   " class Dog implements Speaker { string speak() { return \"woof\"; } }"
				   " void main() { Speaker s; s = new Dog(); string r; r = s.speak(); }");
	Func *main=u->funcs[0];
	Expr *call=main->body->stmts[3]->value;   /* r = s.speak() */
	ASSERT_INT(call->type.kind, TY_STRING);   /* speak() -> string. */
	ASSERT_INT(call->anno_int, 0);            /* Interface slot 0 (reserved [0..K), K=1). */
	/* Dog's speak() sits at the same reserved interface slot. */
	ClassInfo *dog=types_find_class(&g_tt,"Dog");
	MethodInfo *sp=types_find_method(dog,"speak");
	ASSERT_INT(sp->vtable_slot, 0);
}

static void test_extern_call_resolves(void)
{
	Func *f=build1("extern long strlen(string s); extern int abs(int n);"
				   " void main() { long n; n = strlen(\"hi\"); int a; a = abs(-3); }")->funcs[2];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_LONG);   /* strlen(string) -> long. */
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_INT);    /* abs(int) -> int. */
}

static void test_extern_blocking_flag(void)
{
	Unit *u=build1("extern blocking long read_db(long h); void main() { long n; n = read_db(1); }");
	(void)u;
	FuncInfo *fi=types_find_func(&g_tt,"read_db");
	ASSERT_INT(fi->is_extern, 1);
	ASSERT_INT(fi->is_blocking, 1);
}

static void test_local_int_offset(void)
{
	Func *f=build1("void main() { int x; x = 5; }")->funcs[0];
	Stmt *assign=f->body->stmts[1];
	ASSERT_INT(assign->target->kind, EX_IDENT);
	ASSERT_INT(assign->target->type.kind, TY_INT);
	ASSERT_INT(assign->target->anno_int, 8);
	ASSERT(f->frame_size >= 8);
}

static void test_field_resolves_offset(void)
{
	Func *m=build1("class A { int age; void set() { this.age = 3; } }")->klasses[0]->methods[0];
	Stmt *assign=m->body->stmts[0];
	ASSERT_INT(assign->target->kind, EX_FIELD);
	ASSERT_INT(assign->target->anno_int, 24);
	ASSERT_INT(assign->target->type.kind, TY_INT);
}

static void test_int_literal_widths(void)
{
	Func *f=build1("void main() { int a; long n; uint u; ulong g;"
				   " a = 1; n = 5L; u = 3u; g = 9uL; }")->funcs[0];
	ASSERT_INT(f->body->stmts[4]->value->type.kind, TY_INT);
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_LONG);
	ASSERT_INT(f->body->stmts[6]->value->type.kind, TY_UINT);
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_ULONG);
}

static void test_bool_literal_type(void)
{
	Func *f=build1("void main() { bool t; t = true; }")->funcs[0];
	Stmt *assign=f->body->stmts[1];
	ASSERT_INT(assign->value->kind, EX_BOOL);
	ASSERT_INT(assign->value->type.kind, TY_BOOL);
}

static void test_arithmetic_widens_to_wider_operand(void)
{
	Func *f=build1("void main() { byte a; long n; long r; r = a + n; }")->funcs[0];
	Stmt *assign=f->body->stmts[3];
	ASSERT_INT(assign->value->kind, EX_BINARY);
	ASSERT_INT(assign->value->type.kind, TY_LONG);
}

static void test_comparison_is_bool(void)
{
	Func *f=build1("void main() { int x; bool r; x = 0; r = x < 2; }")->funcs[0];
	Stmt *assign=f->body->stmts[3];
	ASSERT_INT(assign->value->kind, EX_BINARY);
	ASSERT_INT(assign->value->type.kind, TY_BOOL);
}

static void test_implicit_widening_init(void)
{
	Func *f=build1("void main() { byte a; int x = a; }")->funcs[0];
	Stmt *decl=f->body->stmts[1];
	ASSERT_INT(decl->decl_type.kind, TY_INT);
	ASSERT_INT(decl->decl_init->type.kind, TY_BYTE);
}

static void test_cast_result_type(void)
{
	Func *f=build1("void main() { int x; byte b; x = 300; b = (byte)x; }")->funcs[0];
	Stmt *assign=f->body->stmts[3];
	ASSERT_INT(assign->value->kind, EX_CAST);
	ASSERT_INT(assign->value->type.kind, TY_BYTE);
	ASSERT_INT(assign->value->lhs->type.kind, TY_INT);
}

static void test_float_literal_types(void)
{
	Func *f=build1("void main() { double d; float g; d = 1.5; g = 1.5f; }")->funcs[0];
	ASSERT_INT(f->body->stmts[2]->value->type.kind, TY_DOUBLE);
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_FLOAT);
}

static void test_double_arithmetic_type(void)
{
	Func *f=build1("void main() { double a; double b; double r; r = a + b; }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_DOUBLE);
}

static void test_int_promotes_to_double(void)
{
	Func *f=build1("void main() { int n; double d; double r; r = n + d; }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_DOUBLE);
}

static void test_implicit_int_to_double_init(void)
{
	Func *f=build1("void main() { int n; double d = n; }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->decl_init->type.kind, TY_INT);
	ASSERT_INT(f->body->stmts[1]->decl_type.kind, TY_DOUBLE);
}

static void test_float_compare_is_bool(void)
{
	Func *f=build1("void main() { double a; double b; bool r; r = a < b; }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);
}

static void test_string_literal_type(void)
{
	Func *f = build1("void main() { string s; s = \"x\"; }")->funcs[0];
	Stmt *a = f->body->stmts[1];
	ASSERT_INT(a->value->type.kind, TY_STRING);
}

static void test_xmlnode_type(void)
{
	Func *f = build1("void main() { XmlNode n; }")->funcs[0];
	ASSERT_INT(f->body->stmts[0]->decl_type.kind, TY_XMLNODE);
}

static void test_jsonvalue_type(void)
{
	Func *f = build1("void main() { JsonValue v; }")->funcs[0];
	ASSERT_INT(f->body->stmts[0]->decl_type.kind, TY_JSONVALUE);
}

static void test_httprequest_type(void)
{
	Func *f = build1("void main() { HttpRequest r; }")->funcs[0];
	ASSERT_INT(f->body->stmts[0]->decl_type.kind, TY_HTTPREQUEST);
}

static void test_httpresponse_type(void)
{
	Func *f = build1("void main() { HttpResponse r; }")->funcs[0];
	ASSERT_INT(f->body->stmts[0]->decl_type.kind, TY_HTTPRESPONSE);
}

static void test_concat_is_string(void)
{
	Func *f = build1("void main() { string s; s = \"a\" + \"b\"; }")->funcs[0];
	Stmt *a = f->body->stmts[1];
	ASSERT_INT(a->value->kind, EX_BINARY);
	ASSERT_INT(a->value->type.kind, TY_STRING);
}

static void test_length_is_int(void)
{
	Func *f = build1("void main() { string s; int n; s = \"hi\"; n = length(s); }")->funcs[0];
	Stmt *a = f->body->stmts[3];
	ASSERT_INT(a->value->type.kind, TY_INT);
}

static void test_newarray_type(void)
{
	Func *f=build1("void main() { int[] a; a = new int[3]; }")->funcs[0];
	Stmt *as=f->body->stmts[1];
	ASSERT_INT(as->value->kind, EX_NEWARRAY);
	ASSERT_INT(as->value->type.kind, TY_ARRAY);
	ASSERT_INT(as->value->type.elem->kind, TY_INT);
}

static void test_index_element_type(void)
{
	Func *f=build1("void main() { int[] a; int x; a = new int[3]; x = a[0]; }")->funcs[0];
	Stmt *as=f->body->stmts[3];
	ASSERT_INT(as->value->kind, EX_INDEX);
	ASSERT_INT(as->value->type.kind, TY_INT);
}

static void test_array_length_is_int(void)
{
	Func *f=build1("void main() { int[] a; int n; a = new int[3]; n = a.length; }")->funcs[0];
	Stmt *as=f->body->stmts[3];
	ASSERT_INT(as->value->type.kind, TY_INT);
}

static void test_newmap_type(void)
{
	Func *f=build1("void main() { map<string,int> d; d = new map<string,int>(); }")->funcs[0];
	Stmt *as=f->body->stmts[1];
	ASSERT_INT(as->value->kind, EX_NEWMAP);
	ASSERT_INT(as->value->type.kind, TY_MAP);
}

static void test_map_get_and_size_types(void)
{
	Func *f=build1("void main() { map<int,int> d; int x; int n; "
				   "d = new map<int,int>(); d.put(1, 9); x = d.get(1); n = d.size; }")->funcs[0];
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_INT);   /* d.get(1) */
	ASSERT_INT(f->body->stmts[6]->value->type.kind, TY_INT);   /* d.size */
}

static void test_incdec_type(void)
{
	Func *f=build1("void main() { int i; i = 0; i++; }")->funcs[0];
	Stmt *st=f->body->stmts[2];
	ASSERT_INT(st->kind, ST_EXPR);
	ASSERT_INT(st->expr->kind, EX_INCDEC);
	ASSERT_INT(st->expr->type.kind, TY_INT);
}

static void test_foreach_array_elem_type(void)
{
	Func *f=build1("void main() { int[] a; int s; a = new int[3]; s = 0; "
				   "foreach (int x in a) { s = s + x; } }")->funcs[0];
	Stmt *fe=f->body->stmts[4];
	ASSERT_INT(fe->kind, ST_FOREACH);
	ASSERT_INT(fe->decl_type.kind, TY_INT);
	ASSERT(fe->decl_offset > 0);
	ASSERT(fe->fe_index_offset != fe->decl_offset);
}

static void test_foreach_map_key_type(void)
{
	Func *f=build1("void main() { map<string,int> m; m = new map<string,int>(); "
				   "foreach (string k in m) { print(k); } }")->funcs[0];
	Stmt *fe=f->body->stmts[2];
	ASSERT_INT(fe->kind, ST_FOREACH);
	ASSERT_INT(fe->decl_type.kind, TY_STRING);
}

static void test_for_resolve(void)
{
	Func *f=build1("void main() { int n; n = 0; for (int i = 0; i <= 10; i++) { n = n + i; } }")->funcs[0];
	Stmt *fr=f->body->stmts[2];
	ASSERT_INT(fr->kind, ST_FOR);
	ASSERT_INT(fr->cond->type.kind, TY_BOOL);
}

static void test_break_in_loop_ok(void)
{
	Func *f=build1("void main() { int i; i = 0; while (i < 3) { break; } }")->funcs[0];
	ASSERT_INT(f->body->stmts[2]->then_blk->stmts[0]->kind, ST_BREAK);
}

static void test_newgen_type(void)
{
	Func *f=build1("void main() { Box<int> b; b = new Box<int>(); }")->funcs[0];
	Stmt *as=f->body->stmts[1];
	ASSERT_INT(as->value->kind, EX_NEWGEN);
	ASSERT_INT(as->value->type.kind, TY_GENERIC);
}

static void test_box_method_types(void)
{
	Func *f=build1("void main() { Box<int> b; int x; bool c; "
				   "b = new Box<int>(); b.set(7); x = b.get(); c = b.contains(7); }")->funcs[0];
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_INT);    /* b.get() */
	ASSERT_INT(f->body->stmts[6]->value->type.kind, TY_BOOL);   /* b.contains(7) */
}

static void test_list_method_types(void)
{
	Func *f=build1("void main() { List<int> l; int x; int n; bool c; "
				   "l = new List<int>(); l.add(5); x = l.get(0); n = l.size; c = l.contains(5); }")->funcs[0];
	ASSERT_INT(f->body->stmts[6]->value->type.kind, TY_INT);    /* l.get(0) */
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_INT);    /* l.size */
	ASSERT_INT(f->body->stmts[8]->value->type.kind, TY_BOOL);   /* l.contains(5) */
}

static void test_set_string_ok(void)
{
	Func *f=build1("void main() { Set<string> s; s = new Set<string>(); s.add(\"x\"); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->kind, EX_NEWGEN);
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_GENERIC);
}

static void test_math_types(void)
{
	Func *f=build1("void main() { double r; int a; "
				   "r = Math.sqrt(16.0); a = Math.abs(-5); }")->funcs[0];
	ASSERT_INT(f->body->stmts[2]->value->type.kind, TY_DOUBLE);   /* sqrt -> double */
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_INT);      /* abs(int) -> int */
}

static void test_random_types(void)
{
	Func *f=build1("void main() { int a; double d; bool b; "
				   "a = Random.get(10); d = Random.nextDouble(); b = Random.nextBool(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_INT);     /* get(int) -> int */
	ASSERT_INT(f->body->stmts[4]->value->type.kind, TY_DOUBLE);  /* nextDouble -> double */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_BOOL);    /* nextBool -> bool */
}

static void test_string_method_types(void)
{
	Func *f=build1("void main() { string s; s = \"hi\"; bool b; b = s.contains(\"h\"); int i; i = s.indexOf(\"i\"); int n; n = s.length(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);   /* contains -> bool */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_INT);    /* indexOf -> int */
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_INT);    /* length -> int */
}

static void test_regex_types(void)
{
	Func *f=build1("void main() { string p; string t; p = \"a+\"; t = \"aa\"; "
				   "bool b; b = Regex.matches(p, t); string m; m = Regex.find(p, t); }")->funcs[0];
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_BOOL);    /* matches -> bool */
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_STRING);  /* find -> string */
}

static void test_clock_type(void)
{
	Func *f=build1("void main() { long t; t = Clock.currentTimeMillis(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_LONG);
}

static void test_clock_date_types(void)
{
	Func *f=build1("void main() { long t; t = Clock.currentTimeMillis(); string a; a = Clock.getDateString(t); string b; b = Clock.getDateString(t, \"yyyy\"); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_STRING);   /* getDateString(t) */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_STRING);   /* getDateString(t, fmt) */
}

static void test_map_contains(void)
{
	Func *f=build1("void main() { map<string,int> m; bool a; bool b;"
				   " a = m.containsKey(\"x\"); b = m.containsValue(5); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);   /* containsKey -> bool */
	ASSERT_INT(f->body->stmts[4]->value->type.kind, TY_BOOL);   /* containsValue -> bool */
}

static void test_map_keys_values_types(void)
{
	Func *f=build1("void main() { map<string,int> m; string[] ks; int[] vs;"
				   " ks = m.getKeys(); vs = m.getValues(); }")->funcs[0];
	Stmt *sk=f->body->stmts[3], *sv=f->body->stmts[4];
	ASSERT_INT(sk->value->type.kind, TY_ARRAY);
	ASSERT_INT(sk->value->type.elem->kind, TY_STRING);
	ASSERT_INT(sv->value->type.kind, TY_ARRAY);
	ASSERT_INT(sv->value->type.elem->kind, TY_INT);
}

static void test_foreach_pair_resolves(void)
{
	Func *f=build1("void main() { map<string,int> m; foreach (string k, int v in m) { print(v); } }")->funcs[0];
	Stmt *s=f->body->stmts[1];
	ASSERT_INT(s->kind, ST_FOREACH);
	ASSERT(s->fe_val_offset != 0);   /* value slot was assigned */
}

static void test_map_entries_types(void)
{
	Func *f=build1("void main() { map<string,int> m;"
				   " foreach (Entry e in m.getEntries()) { string k; int v; k = e.getKey(); v = e.getValue(); } }")->funcs[0];
	Stmt *fe=f->body->stmts[1];
	ASSERT_INT(fe->kind, ST_FOREACH);
	ASSERT_INT(fe->expr->type.kind, TY_ARRAY);          /* getEntries() -> Entry[] */
	ASSERT_INT(fe->expr->type.elem->kind, TY_ENTRY);
	ASSERT_INT(fe->decl_type.kind, TY_ENTRY);           /* loop var enriched to Entry<K,V> */
}

static void test_spawn_resolves(void)
{
	Func *f=build1("void worker() { } void main() { spawn worker(); }")->funcs[1];
	Stmt *s=f->body->stmts[0];
	ASSERT_INT(s->kind, ST_SPAWN);
	ASSERT_INT(s->expr->kind, EX_CALL);
}

static void test_channel_send_recv_types(void)
{
	Func *f=build1("void main() { channel<int> c; c = new channel<int>(2); int x; c.send(5); x = c.recv(); }")->funcs[0];
	Stmt *snd=f->body->stmts[3], *rcv=f->body->stmts[4];
	ASSERT_INT(snd->expr->type.kind, TY_VOID);    /* send -> void (ST_EXPR) */
	ASSERT_INT(rcv->value->type.kind, TY_INT);    /* recv -> elem type */
}

static void test_spawn_args_resolve(void)
{
	Func *f=build1("void worker(int id, string tag) { } void main() { spawn worker(7, \"hi\"); }")->funcs[1];
	Stmt *s=f->body->stmts[0];
	ASSERT_INT(s->kind, ST_SPAWN);
	ASSERT_INT(s->expr->arg_count, 2);
}

static void test_file_basic_types(void)
{
	Func *f=build1("void main() { bool b; b = File.exists(\"x\"); File.createFile(\"y\"); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_BOOL);   /* exists -> bool */
	ASSERT_INT(f->body->stmts[2]->expr->type.kind, TY_VOID);    /* createFile -> void */
}

static void test_file_attr_types(void)
{
	Func *f=build1("void main() { int a; a = File.READONLY; File.setAttribute(\"x\", File.HIDDEN, true); bool b; b = File.hasAttribute(\"x\", File.READONLY); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_INT);   /* File.READONLY -> int */
	ASSERT_INT(f->body->stmts[1]->value->int_val, 1);
	ASSERT_INT(f->body->stmts[4]->value->type.kind, TY_BOOL);  /* hasAttribute -> bool */
}

static void test_switch_resolves(void)
{
	Func *f=build1("void main() { int x; x = 1; switch (x) { case 1: print(x); break; case 2: break; default: break; } }")->funcs[0];
	Stmt *sw=f->body->stmts[2];
	ASSERT_INT(sw->kind, ST_SWITCH);
	ASSERT_INT(sw->cond->type.kind, TY_INT);
}

static void test_throw_resolves(void)
{
	Func *f=build1("void main() { Exception e; e = new Exception(); throw e; }")->funcs[0];
	Stmt *s=f->body->stmts[2];
	ASSERT_INT(s->kind, ST_THROW);
	ASSERT_INT(s->expr->type.kind, TY_OBJECT);
}

static void test_try_catch_resolves(void)
{
	Func *f=build1("void main() { try { print(1); } catch (Exception e) { print(2); } }")->funcs[0];
	Stmt *s=f->body->stmts[0];
	ASSERT_INT(s->kind, ST_TRY);
	ASSERT_INT(s->else_blk->stmts[0]->kind, ST_CATCH);
	ASSERT_INT(s->else_blk->stmts[0]->decl_type.kind, TY_OBJECT);
}

static void test_catch_index_oob_resolves(void)
{
	Func *f=build1("void main() { try { print(1); } catch (IndexOutOfBounds e) { print(2); } }")->funcs[0];
	Stmt *s=f->body->stmts[0];
	ASSERT_INT(s->kind, ST_TRY);
	ASSERT_STR(s->else_blk->stmts[0]->decl_type.class_name, "IndexOutOfBounds");
}

static void test_method_call_slot_and_class(void)
{
	static Parser ps[2];
	static Unit *units[2];
	const char *s[]= {"class Animal { void speak() { } }",
					  "class Dog extends Animal { void speak() { } void go() { this.speak(); } }"
					 };
	types_init(&g_tt);
	for (int i=0; i<2; i++)
	{
		parser_init(&ps[i],s[i]);
		units[i]=parse_unit(&ps[i]);
	}
	for (int i=0; i<2; i++)
	{
		types_register_unit_names(&g_tt,units[i]);
	}
	for (int i=0; i<2; i++)
	{
		types_register_interfaces(&g_tt,units[i]);
	}
	types_register_all_members(&g_tt,units,2);
	resolve_program(&g_tt,units,2);
	Func *go=units[1]->klasses[0]->methods[1];
	Stmt *call=go->body->stmts[0];
	ASSERT_INT(call->kind, ST_EXPR);
	ASSERT_INT(call->expr->kind, EX_METHOD_CALL);
	ASSERT_INT(call->expr->anno_int, 0);
	ASSERT_STR(call->expr->anno_str, "Dog");
}

static void test_shared_set_inference(void)
{
	/* worker takes a channel<Player>, so Player can cross cores -> shared.
	   Player has an Inventory field, so Inventory is shared too (transitive).
	   Loner is never channel-reachable -> not shared. */
	static Parser ps[4];
	static Unit *units[4];
	const char *s[]= {"class Inventory { int n; }",
					  "class Player { Inventory inv; }",
					  "class Loner { int x; }",
					  "void worker(channel<Player> c) { }"
					 };
	types_init(&g_tt);
	types_register_builtins(&g_tt);
	for (int i=0; i<4; i++)
	{
		parser_init(&ps[i],s[i]);
		units[i]=parse_unit(&ps[i]);
	}
	for (int i=0; i<4; i++)
	{
		types_register_unit_names(&g_tt,units[i]);
	}
	for (int i=0; i<4; i++)
	{
		types_register_interfaces(&g_tt,units[i]);
	}
	types_register_all_members(&g_tt,units,4);
	resolve_program(&g_tt,units,4);
	ASSERT_INT(types_find_class(&g_tt,"Player")->is_shared, 1);
	ASSERT_INT(types_find_class(&g_tt,"Inventory")->is_shared, 1);
	ASSERT_INT(types_find_class(&g_tt,"Loner")->is_shared, 0);
}

static void test_schedule_after_returns_timer(void)
{
	/* tick: a named zero-arg void function; scheduleAfter returns a Timer, and
	   Timer.cancel() resolves to void. */
	Unit *u = build1("void tick() {} void main() { Timer t; t = scheduleAfter(tick, 100); t.cancel(); }");
	Func *mn = u->funcs[1];
	ASSERT_INT(mn->body->stmts[0]->decl_type.kind, TY_TIMER);    /* Timer t; */
	ASSERT_INT(mn->body->stmts[1]->value->type.kind, TY_TIMER);  /* scheduleAfter(...) -> Timer. */
	ASSERT_INT(mn->body->stmts[2]->expr->type.kind, TY_VOID);    /* t.cancel() -> void. */
}

static void test_schedule_every_returns_timer(void)
{
	Unit *u = build1("void tick() {} void main() { Timer t; t = scheduleEvery(tick, 100, 50); }");
	ASSERT_INT(u->funcs[1]->body->stmts[1]->value->type.kind, TY_TIMER);
}

static void test_getclassname_resolves_string(void)
{
	Func *f=build1("class Player { int x; } void main() { Player p; p = new Player(); string n; n = p.getClassName(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_STRING);   /* obj.getClassName() -> string. */
}

static void test_system_shell_resolves_int(void)
{
	Func *f=build1("void main() { int p; p = System.shell(\"dir\"); int c; c = System.shell(\"dir\", true); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_INT);   /* Async form -> int (pid). */
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_INT);   /* Wait form -> int (exit code). */
}

static void test_network_tcp_resolves(void)
{
	Func *f=build1("void main() { Listener l; l = Network.listen(0); Socket s; s = l.accept();"
				   " byte[] b; b = s.read(64); int n; n = s.write(b); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_LISTENER);   /* Network.listen -> Listener. */
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_SOCKET);     /* listener.accept -> Socket. */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_ARRAY);      /* socket.read -> byte[]. */
	ASSERT_INT(f->body->stmts[5]->value->type.elem->kind, TY_BYTE);
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_INT);        /* socket.write -> int. */
}

static void test_filechannel_resolves(void)
{
	Func *f=build1("void main() { FileChannel c; c = File.openChannel(\"db.dat\");"
				   " byte[] b; b = c.readAt(0, 16); long s; s = c.size(); c.sync(); c.close(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_FILECHANNEL);   /* File.openChannel -> FileChannel. */
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_ARRAY);         /* readAt -> byte[]. */
	ASSERT_INT(f->body->stmts[3]->value->type.elem->kind, TY_BYTE);
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_LONG);          /* size -> long. */
}

static void test_filewriter_resolves(void)
{
	Func *f=build1("void main() { FileWriter w; w = File.openWrite(\"out.txt\");"
				   " w.write(\"hi\"); w.writeLine(\"x\"); w.flush(); w.close(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_FILEWRITER);   /* File.openWrite -> FileWriter. */
	ASSERT_INT(f->body->stmts[2]->expr->type.kind, TY_VOID);          /* w.write(...) -> void. */
}

static void test_logger_resolves(void)
{
	Func *f=build1("void main() { Logger g; g = Log.open(\"app.log\");"
				   " g.log(\"hi\"); g.close(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_LOGGER);   /* Log.open -> Logger. */
	ASSERT_INT(f->body->stmts[2]->expr->type.kind, TY_VOID);      /* g.log(...) -> void. */
}

static void test_string_parse_types(void)
{
	Func *f=build1("void main() { int a; a = \"1\".toInt(); long b; b = \"2\".toLong();"
				   " double c; c = \"3.0\".toDouble(); bool d; d = \"true\".toBool(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_INT);
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_LONG);
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_DOUBLE);
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_BOOL);
}

static void test_null_resolves(void)
{
	Func *f=build1("void main() { Socket s; s = Network.connect(\"127.0.0.1\", 1); bool b; b = (s == null);"
				   " byte[] x; x = null; }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);   /* (s == null) -> bool. */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_NULL);   /* null assigns to a managed byte[]. */
}

static void test_network_udp_resolves(void)
{
	Func *f=build1("void main() { UdpSocket u; u = Network.udp(0); Datagram d; d = u.receive();"
				   " string h; h = d.host(); int p; p = d.port(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[1]->value->type.kind, TY_UDPSOCKET);  /* Network.udp -> UdpSocket. */
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_DATAGRAM);   /* udp.receive -> Datagram. */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_STRING);     /* datagram.host -> string. */
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_INT);        /* datagram.port -> int. */
}

static void test_lambda_resolve_capture(void)
{
	/* base is captured by value; x is the parameter, its type inferred from the
	   (int)->int target. */
	Func *f = build1("void m() { int base = 10; (int)->int g = x => x + base; }")->funcs[0];
	Expr *lam = f->body->stmts[1]->decl_init;
	ASSERT_INT(lam->kind, EX_LAMBDA);
	ASSERT_INT(lam->type.kind, TY_FUNC);
	ASSERT_INT(lam->lam->param_count, 1);
	ASSERT_INT(lam->lam->params[0].type.kind, TY_INT);   /* inferred from the target. */
	ASSERT_INT(lam->lam->cap_count, 1);
	ASSERT_STR(lam->lam->caps[0].name, "base");
	ASSERT_INT(lam->lam->caps[0].type.kind, TY_INT);
	ASSERT_INT(lam->lam->caps[0].env_offset, 32);        /* first capture sits past the 32-byte header. */
}

int main(void)
{
	printf("Resolver tests\n");
	RUN(test_interface_call_resolves);
	RUN(test_enum_lowers_to_class);
	RUN(test_bitwise_typing);
	RUN(test_logical_typing);
	RUN(test_enum_static_access);
	RUN(test_static_member_resolves);
	RUN(test_generic_lowers_to_class);
	RUN(test_generic_multi_param_lowers);
	RUN(test_generic_bound_ok);
	RUN(test_extern_call_resolves);
	RUN(test_extern_blocking_flag);
	RUN(test_local_int_offset);
	RUN(test_field_resolves_offset);
	RUN(test_int_literal_widths);
	RUN(test_bool_literal_type);
	RUN(test_arithmetic_widens_to_wider_operand);
	RUN(test_comparison_is_bool);
	RUN(test_implicit_widening_init);
	RUN(test_cast_result_type);
	RUN(test_float_literal_types);
	RUN(test_double_arithmetic_type);
	RUN(test_int_promotes_to_double);
	RUN(test_implicit_int_to_double_init);
	RUN(test_float_compare_is_bool);
	RUN(test_string_literal_type);
	RUN(test_xmlnode_type);
	RUN(test_jsonvalue_type);
	RUN(test_httprequest_type);
	RUN(test_httpresponse_type);
	RUN(test_concat_is_string);
	RUN(test_length_is_int);
	RUN(test_newarray_type);
	RUN(test_index_element_type);
	RUN(test_array_length_is_int);
	RUN(test_newmap_type);
	RUN(test_map_get_and_size_types);
	RUN(test_incdec_type);
	RUN(test_break_in_loop_ok);
	RUN(test_for_resolve);
	RUN(test_foreach_array_elem_type);
	RUN(test_foreach_map_key_type);
	RUN(test_newgen_type);
	RUN(test_box_method_types);
	RUN(test_list_method_types);
	RUN(test_set_string_ok);
	RUN(test_math_types);
	RUN(test_clock_type);
	RUN(test_clock_date_types);
	RUN(test_map_contains);
	RUN(test_map_keys_values_types);
	RUN(test_foreach_pair_resolves);
	RUN(test_map_entries_types);
	RUN(test_spawn_resolves);
	RUN(test_spawn_args_resolve);
	RUN(test_channel_send_recv_types);
	RUN(test_file_basic_types);
	RUN(test_file_attr_types);
	RUN(test_random_types);
	RUN(test_string_method_types);
	RUN(test_regex_types);
	RUN(test_switch_resolves);
	RUN(test_throw_resolves);
	RUN(test_try_catch_resolves);
	RUN(test_catch_index_oob_resolves);
	RUN(test_method_call_slot_and_class);
	RUN(test_shared_set_inference);
	RUN(test_schedule_after_returns_timer);
	RUN(test_schedule_every_returns_timer);
	ast_free_all();
	RUN(test_system_shell_resolves_int);
	RUN(test_getclassname_resolves_string);
	RUN(test_network_tcp_resolves);
	RUN(test_network_udp_resolves);
	RUN(test_filechannel_resolves);
	RUN(test_filewriter_resolves);
	RUN(test_logger_resolves);
	RUN(test_string_parse_types);
	RUN(test_null_resolves);
	RUN(test_lambda_resolve_capture);
	SUMMARY();
	return 0;
}
