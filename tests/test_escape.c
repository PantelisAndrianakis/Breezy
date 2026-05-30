#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "escape.h"

static TypeTable g_tt;

static Func *build(const char *body)
{
	static Parser ps[2];
	static Unit *u[2];
	const char *cell = "class Cell { int v; }";
	types_init(&g_tt);
	parser_init(&ps[0], cell);
	u[0]=parse_unit(&ps[0]);
	parser_init(&ps[1], body);
	u[1]=parse_unit(&ps[1]);
	types_register_unit_names(&g_tt,u[0]);
	types_register_unit_names(&g_tt,u[1]);
	types_register_unit_members(&g_tt,u[0]);
	types_register_unit_members(&g_tt,u[1]);
	resolve_program(&g_tt,u,2);
	Func *f=u[1]->funcs[0];
	escape_annotate(&g_tt,f);
	return f;
}

/* The first EX_NEW produced into a local by a flat statement list. */
static Expr *first_new(Func *f)
{
	for (int i=0; i<f->body->count; i++)
	{
		Stmt *s=f->body->stmts[i];
		if (s->kind==ST_VARDECL && s->decl_init && s->decl_init->kind==EX_NEW)
		{
			return s->decl_init;
		}

		if (s->kind==ST_ASSIGN && s->value && s->value->kind==EX_NEW)
		{
			return s->value;
		}
	}

	return NULL;
}

static void test_local_only_is_stack(void)
{
	Func *f=build("void main() { Cell c; c = new Cell(); c.v = 3; print(c.v); }");
	Expr *n=first_new(f);
	ASSERT(n!=NULL);
	ASSERT_INT(n->anno_stack, 1);
	ASSERT(f->stack_alloc_bytes >= 24);    /* object_size(Cell) = 16 + 1*8. */
}

static void test_returned_escapes(void)
{
	Func *f=build("Cell make() { Cell c; c = new Cell(); return c; }");
	Expr *n=first_new(f);
	ASSERT(n!=NULL);
	ASSERT_INT(n->anno_stack, 0);          /* Heap: c is returned. */
}

static void test_field_store_escapes(void)
{
	Func *f=build("Cell make2() { Cell a; Cell b; a = new Cell(); b = new Cell(); a.v = 1; return a; }");
	Expr *a_new=f->body->stmts[2]->value;  /* a = new Cell(): a is returned -> heap. */
	Expr *b_new=f->body->stmts[3]->value;  /* b = new Cell(): local-only -> stack. */
	ASSERT_INT(a_new->anno_stack, 0);
	ASSERT_INT(b_new->anno_stack, 1);
}

int main(void)
{
	printf("Escape analysis tests\n");
	RUN(test_local_only_is_stack);
	RUN(test_returned_escapes);
	RUN(test_field_store_escapes);
	ast_free_all();
	SUMMARY();
	return 0;
}
