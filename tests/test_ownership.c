#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "ownership.h"

static TypeTable g_tt;
static Func *build(const char *s)
{
	static Parser ps[2];
	static Unit *u[2];
	const char *animal = "class Animal { int age; }";
	types_init(&g_tt);
	parser_init(&ps[0], animal);
	u[0] = parse_unit(&ps[0]);
	parser_init(&ps[1], s);
	u[1] = parse_unit(&ps[1]);
	types_register_unit_names(&g_tt, u[0]);
	types_register_unit_names(&g_tt, u[1]);
	types_register_unit_members(&g_tt, u[0]);
	types_register_unit_members(&g_tt, u[1]);
	resolve_program(&g_tt, u, 2);
	Func *f = u[1]->funcs[0];
	ownership_annotate(f);
	return f;
}

static void test_collects_object_locals(void)
{
	/* Two object locals (a and b) are collected; the int local n is ignored. */
	Func *f = build("void main() { int n; Animal a; Animal b; n = 1; }");
	ASSERT_INT(f->obj_local_count, 2);
}

static void test_no_object_locals(void)
{
	Func *f = build("void main() { int x; int y; }");
	ASSERT_INT(f->obj_local_count, 0);
}

int main(void)
{
	printf("Ownership pass tests\n");
	RUN(test_collects_object_locals);
	RUN(test_no_object_locals);
	ast_free_all();
	SUMMARY();
	return 0;
}
