#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"

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
	types_register_unit_members(&g_tt,u);
	resolve_program(&g_tt,&u,1);
	return u;
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
	Func *m=build1("class A { int age; void set() { this.age = 3; } }")->klass->methods[0];
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
	Func *f=build1("void main() { boolean t; t = true; }")->funcs[0];
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

static void test_comparison_is_boolean(void)
{
	Func *f=build1("void main() { int x; boolean r; x = 0; r = x < 2; }")->funcs[0];
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

static void test_float_compare_is_boolean(void)
{
	Func *f=build1("void main() { double a; double b; boolean r; r = a < b; }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);
}

static void test_string_literal_type(void)
{
	Func *f = build1("void main() { string s; s = \"x\"; }")->funcs[0];
	Stmt *a = f->body->stmts[1];
	ASSERT_INT(a->value->type.kind, TY_STRING);
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
	Func *f=build1("void main() { Box<int> b; int x; boolean c; "
				   "b = new Box<int>(); b.set(7); x = b.get(); c = b.contains(7); }")->funcs[0];
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_INT);    /* b.get() */
	ASSERT_INT(f->body->stmts[6]->value->type.kind, TY_BOOL);   /* b.contains(7) */
}

static void test_list_method_types(void)
{
	Func *f=build1("void main() { List<int> l; int x; int n; boolean c; "
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
	Func *f=build1("void main() { int a; double d; boolean b; "
				   "a = Random.get(10); d = Random.nextDouble(); b = Random.nextBoolean(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_INT);     /* get(int) -> int */
	ASSERT_INT(f->body->stmts[4]->value->type.kind, TY_DOUBLE);  /* nextDouble -> double */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_BOOL);    /* nextBoolean -> boolean */
}

static void test_string_method_types(void)
{
	Func *f=build1("void main() { string s; s = \"hi\"; boolean b; b = s.contains(\"h\"); int i; i = s.indexOf(\"i\"); int n; n = s.length(); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);   /* contains -> boolean */
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_INT);    /* indexOf -> int */
	ASSERT_INT(f->body->stmts[7]->value->type.kind, TY_INT);    /* length -> int */
}

static void test_regex_types(void)
{
	Func *f=build1("void main() { string p; string t; p = \"a+\"; t = \"aa\"; "
				   "boolean b; b = Regex.matches(p, t); string m; m = Regex.find(p, t); }")->funcs[0];
	ASSERT_INT(f->body->stmts[5]->value->type.kind, TY_BOOL);    /* matches -> boolean */
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
	Func *f=build1("void main() { map<string,int> m; boolean a; boolean b;"
				   " a = m.containsKey(\"x\"); b = m.containsValue(5); }")->funcs[0];
	ASSERT_INT(f->body->stmts[3]->value->type.kind, TY_BOOL);   /* containsKey -> bool */
	ASSERT_INT(f->body->stmts[4]->value->type.kind, TY_BOOL);   /* containsValue -> bool */
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
		types_register_unit_members(&g_tt,units[i]);
	}
	resolve_program(&g_tt,units,2);
	Func *go=units[1]->klass->methods[1];
	Stmt *call=go->body->stmts[0];
	ASSERT_INT(call->kind, ST_EXPR);
	ASSERT_INT(call->expr->kind, EX_METHOD_CALL);
	ASSERT_INT(call->expr->anno_int, 0);
	ASSERT_STR(call->expr->anno_str, "Dog");
}

int main(void)
{
	printf("Resolver tests\n");
	RUN(test_local_int_offset);
	RUN(test_field_resolves_offset);
	RUN(test_int_literal_widths);
	RUN(test_bool_literal_type);
	RUN(test_arithmetic_widens_to_wider_operand);
	RUN(test_comparison_is_boolean);
	RUN(test_implicit_widening_init);
	RUN(test_cast_result_type);
	RUN(test_float_literal_types);
	RUN(test_double_arithmetic_type);
	RUN(test_int_promotes_to_double);
	RUN(test_implicit_int_to_double_init);
	RUN(test_float_compare_is_boolean);
	RUN(test_string_literal_type);
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
	RUN(test_random_types);
	RUN(test_string_method_types);
	RUN(test_regex_types);
	RUN(test_switch_resolves);
	RUN(test_throw_resolves);
	RUN(test_try_catch_resolves);
	RUN(test_catch_index_oob_resolves);
	RUN(test_method_call_slot_and_class);
	ast_free_all();
	SUMMARY();
	return 0;
}
