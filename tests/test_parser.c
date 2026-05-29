#include "test_framework.h"
#include "parser.h"

static Expr *parse_str(const char *s)
{
	static Parser p;
	parser_init(&p, s);
	return parse_expr(&p);
}

static void test_int_literal(void)
{
	Expr *e = parse_str("42");
	ASSERT_INT(e->kind, EX_INT);
	ASSERT_INT(e->int_val, 42);
}

static void test_precedence(void)
{
	Expr *e = parse_str("1 + 2 * 3");
	ASSERT_INT(e->kind, EX_BINARY);
	ASSERT_INT(e->op, TOKEN_PLUS);
	ASSERT_INT(e->lhs->int_val, 1);
	ASSERT_INT(e->rhs->kind, EX_BINARY);
	ASSERT_INT(e->rhs->op, TOKEN_STAR);
}

static void test_method_call(void)
{
	Expr *e = parse_str("d.speak()");
	ASSERT_INT(e->kind, EX_METHOD_CALL);
	ASSERT_STR(e->name, "speak");
	ASSERT_INT(e->lhs->kind, EX_IDENT);
	ASSERT_STR(e->lhs->name, "d");
	ASSERT_INT(e->arg_count, 0);
}

static void test_field_access(void)
{
	Expr *e = parse_str("this.age");
	ASSERT_INT(e->kind, EX_FIELD);
	ASSERT_STR(e->name, "age");
	ASSERT_INT(e->lhs->kind, EX_THIS);
}

static void test_new(void)
{
	Expr *e = parse_str("new Dog()");
	ASSERT_INT(e->kind, EX_NEW);
	ASSERT_STR(e->name, "Dog");
}

int main(void)
{
	printf("Parser (expr) tests\n");
	RUN(test_int_literal);
	RUN(test_precedence);
	RUN(test_method_call);
	RUN(test_field_access);
	RUN(test_new);
	ast_free_all();
	SUMMARY();
	return 0;
}
