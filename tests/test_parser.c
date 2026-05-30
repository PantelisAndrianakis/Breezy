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

static Unit *parse_unit_str(const char *s)
{
	static Parser up;
	parser_init(&up,s);
	return parse_unit(&up);
}

static void test_parse_function_with_vardecl(void)
{
	Unit *u = parse_unit_str("void main() { int x; x = 42; }");
	ASSERT(u->klass == NULL);
	ASSERT_INT(u->func_count, 1);
	Func *f = u->funcs[0];
	ASSERT_STR(f->name, "main");
	ASSERT_INT(f->ret_type.kind, TY_VOID);
	ASSERT_INT(f->body->count, 2);
	ASSERT_INT(f->body->stmts[0]->kind, ST_VARDECL);
	ASSERT_STR(f->body->stmts[0]->decl_name, "x");
	ASSERT_INT(f->body->stmts[1]->kind, ST_ASSIGN);
}

static void test_parse_if_else(void)
{
	Unit *u = parse_unit_str("void m() { if (x < 1) { x = 1; } else { x = 2; } }");
	Stmt *s = u->funcs[0]->body->stmts[0];
	ASSERT_INT(s->kind, ST_IF);
	ASSERT_INT(s->cond->kind, EX_BINARY);
	ASSERT_INT(s->then_blk->count, 1);
	ASSERT(s->else_blk != NULL);
	ASSERT_INT(s->else_blk->count, 1);
}

static void test_parse_class_with_inheritance(void)
{
	Unit *u = parse_unit_str("class Dog extends Animal { int age; void speak() { age = 1; } }");
	ASSERT(u->klass != NULL);
	ClassDecl *c = u->klass;
	ASSERT_STR(c->name, "Dog");
	ASSERT_INT(c->has_parent, 1);
	ASSERT_STR(c->parent_name, "Animal");
	ASSERT_INT(c->field_count, 1);
	ASSERT_STR(c->fields[0].name, "age");
	ASSERT_INT(c->method_count, 1);
	ASSERT_STR(c->methods[0]->name, "speak");
}

static void test_parse_method_with_param(void)
{
	Unit *u = parse_unit_str("class A { void init(int a) { } }");
	Func *m = u->klass->methods[0];
	ASSERT_INT(m->param_count, 1);
	ASSERT_INT(m->params[0].type.kind, TY_INT);
	ASSERT_STR(m->params[0].name, "a");
}

int main(void)
{
	printf("Parser (expr) tests\n");
	RUN(test_int_literal);
	RUN(test_precedence);
	RUN(test_method_call);
	RUN(test_field_access);
	RUN(test_new);
	RUN(test_parse_function_with_vardecl);
	RUN(test_parse_if_else);
	RUN(test_parse_class_with_inheritance);
	RUN(test_parse_method_with_param);
	ast_free_all();
	SUMMARY();
	return 0;
}
