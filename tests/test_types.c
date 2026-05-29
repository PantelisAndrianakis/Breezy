#include "test_framework.h"
#include "parser.h"
#include "types.h"

static void build(TypeTable *tt, Unit **units, const char **srcs, int n)
{
	static Parser ps[8];
	types_init(tt);
	for (int i=0; i<n; i++)
	{
		parser_init(&ps[i],srcs[i]);
		units[i]=parse_unit(&ps[i]);
	}
	for (int i=0; i<n; i++) types_register_unit_names(tt, units[i]);
	for (int i=0; i<n; i++) types_register_unit_members(tt, units[i]);
}
static void test_field_offsets_and_size(void)
{
	TypeTable tt;
	Unit *u[1];
	const char *s[]= {"class Foo { int x; int y; }"};
	build(&tt,u,s,1);
	ClassInfo *c=types_find_class(&tt,"Foo");
	ASSERT(c!=NULL);
	ASSERT_INT(types_find_field(c,"x")->offset, 8);
	ASSERT_INT(types_find_field(c,"y")->offset, 16);
	ASSERT_INT(c->object_size, 24);
}
static void test_method_slot(void)
{
	TypeTable tt;
	Unit *u[1];
	const char *s[]= {"class Animal { void speak() { } }"};
	build(&tt,u,s,1);
	ClassInfo *c=types_find_class(&tt,"Animal");
	MethodInfo *m=types_find_method(c,"speak");
	ASSERT_INT(m->vtable_slot, 0);
	ASSERT_STR(m->asm_label, "Animal__speak");
}
static void test_override_reuses_slot(void)
{
	TypeTable tt;
	Unit *u[2];
	const char *s[]= {"class Animal { void speak() { } }",
	                  "class Dog extends Animal { void speak() { } }"
	                 };
	build(&tt,u,s,2);
	ClassInfo *dog=types_find_class(&tt,"Dog");
	MethodInfo *m=types_find_method(dog,"speak");
	ASSERT_INT(m->vtable_slot, 0);
	ASSERT_STR(m->asm_label, "Dog__speak");
	ASSERT_STR(m->owner_class, "Dog");
}
static void test_inherited_field_offset(void)
{
	TypeTable tt;
	Unit *u[2];
	const char *s[]= {"class Animal { int age; }","class Dog extends Animal { int breed; }"};
	build(&tt,u,s,2);
	ClassInfo *dog=types_find_class(&tt,"Dog");
	ASSERT_INT(types_find_field(dog,"age")->offset, 8);
	ASSERT_INT(types_find_field(dog,"breed")->offset, 16);
	ASSERT_INT(dog->object_size, 24);
}
int main(void)
{
	printf("Type table tests\n");
	RUN(test_field_offsets_and_size);
	RUN(test_method_slot);
	RUN(test_override_reuses_slot);
	RUN(test_inherited_field_offset);
	ast_free_all();
	SUMMARY();
	return 0;
}
