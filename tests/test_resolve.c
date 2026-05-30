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
	ASSERT_INT(assign->target->anno_int, 16);
	ASSERT_INT(assign->target->type.kind, TY_INT);
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
	for (int i=0; i<2; i++) types_register_unit_names(&g_tt,units[i]);
	for (int i=0; i<2; i++) types_register_unit_members(&g_tt,units[i]);
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
	RUN(test_method_call_slot_and_class);
	ast_free_all();
	SUMMARY();
	return 0;
}
