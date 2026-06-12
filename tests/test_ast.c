#include "test_framework.h"
#include "ast.h"

static void test_expr_int(void)
{
	Expr *e = expr_new(EX_INT, 5);
	e->int_val = 42;
	ASSERT_INT(e->kind, EX_INT);
	ASSERT_INT(e->line, 5);
	ASSERT_INT(e->int_val, 42);
}

static void test_block_push_grows(void)
{
	Block *b = block_new();
	for (int i = 0; i < 50; i++)
	{
		block_push(b, stmt_new(ST_EXPR, i));
	}
	ASSERT_INT(b->count, 50);
	ASSERT_INT(b->stmts[49]->line, 49);
}

static void test_class_holds_methods(void)
{
	ClassDecl *c = class_new();
	strcpy(c->name, "Dog");
	class_add_method(c, func_new());
	strcpy(c->methods[0]->name, "speak");
	ASSERT_STR(c->name, "Dog");
	ASSERT_STR(c->methods[0]->name, "speak");
}

int main(void)
{
	printf("AST tests\n");
	RUN(test_expr_int);
	RUN(test_block_push_grows);
	RUN(test_class_holds_methods);
	ast_free_all();
	SUMMARY();
	return 0;
}
