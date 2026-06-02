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

static void test_bool_literal(void)
{
	Expr *t = parse_str("true");
	ASSERT_INT(t->kind, EX_BOOL);
	ASSERT_INT(t->int_val, 1);
	Expr *f = parse_str("false");
	ASSERT_INT(f->kind, EX_BOOL);
	ASSERT_INT(f->int_val, 0);
}

static void test_cast(void)
{
	Expr *e = parse_str("(byte)x");
	ASSERT_INT(e->kind, EX_CAST);
	ASSERT_INT(e->type.kind, TY_BYTE);
	ASSERT_INT(e->lhs->kind, EX_IDENT);
	ASSERT_STR(e->lhs->name, "x");
}

static void test_float_literal(void)
{
	Expr *e = parse_str("1.5");
	ASSERT_INT(e->kind, EX_FLOAT);
	ASSERT(e->float_val > 1.4 && e->float_val < 1.6);
}

static void test_float_cast(void)
{
	Expr *e = parse_str("(float)x");
	ASSERT_INT(e->kind, EX_CAST);
	ASSERT_INT(e->type.kind, TY_FLOAT);
}

static void test_new_array(void)
{
	Expr *e = parse_str("new int[5]");
	ASSERT_INT(e->kind, EX_NEWARRAY);
	ASSERT_INT(e->type.kind, TY_ARRAY);
	ASSERT_INT(e->type.elem->kind, TY_INT);
	ASSERT_INT(e->lhs->kind, EX_INT);
}

static void test_index_and_length(void)
{
	Expr *e = parse_str("a[2]");
	ASSERT_INT(e->kind, EX_INDEX);
	ASSERT_INT(e->lhs->kind, EX_IDENT);
	ASSERT_INT(e->rhs->kind, EX_INT);
	Expr *n = parse_str("a.length");
	ASSERT_INT(n->kind, EX_FIELD);
	ASSERT_STR(n->name, "length");
}

static void test_string_expr(void)
{
	Expr *e = parse_str("\"hi\"");
	ASSERT_INT(e->kind, EX_STR);
	ASSERT_STR(e->str_val, "hi");
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

static void test_parse_scalar_vardecls(void)
{
	Unit *u = parse_unit_str("void m() { ubyte b; long n; boolean f; }");
	Block *body = u->funcs[0]->body;
	ASSERT_INT(body->count, 3);
	ASSERT_INT(body->stmts[0]->decl_type.kind, TY_UBYTE);
	ASSERT_INT(body->stmts[1]->decl_type.kind, TY_LONG);
	ASSERT_INT(body->stmts[2]->decl_type.kind, TY_BOOL);
}

static void test_parse_double_vardecl(void)
{
	Unit *u = parse_unit_str("void m() { double d; float f; }");
	Block *b = u->funcs[0]->body;
	ASSERT_INT(b->stmts[0]->decl_type.kind, TY_DOUBLE);
	ASSERT_INT(b->stmts[1]->decl_type.kind, TY_FLOAT);
}

static void test_array_type_decls(void)
{
	Unit *u = parse_unit_str("void m() { int[] a; Foo[] b; }");
	Block *body = u->funcs[0]->body;
	ASSERT_INT(body->stmts[0]->decl_type.kind, TY_ARRAY);
	ASSERT_INT(body->stmts[0]->decl_type.elem->kind, TY_INT);
	ASSERT_INT(body->stmts[1]->decl_type.kind, TY_ARRAY);   /* object array via peek2 */
	ASSERT_INT(body->stmts[1]->decl_type.elem->kind, TY_OBJECT);
}

static void test_map_type_decl(void)
{
	Unit *u = parse_unit_str("void m() { map<string,int> d; }");
	TypeRef *t = &u->funcs[0]->body->stmts[0]->decl_type;
	ASSERT_INT(t->kind, TY_MAP);
	ASSERT_INT(t->elem->kind, TY_STRING);
	ASSERT_INT(t->elem2->kind, TY_INT);
}

static void test_new_map(void)
{
	Expr *e = parse_str("new map<int,int>()");
	ASSERT_INT(e->kind, EX_NEWMAP);
	ASSERT_INT(e->type.elem->kind, TY_INT);
	ASSERT_INT(e->type.elem2->kind, TY_INT);
}

static void test_incdec_parse(void)
{
	Expr *post = parse_str("i++");
	ASSERT_INT(post->kind, EX_INCDEC);
	ASSERT_INT(post->op, TOKEN_PLUSPLUS);
	ASSERT_INT(post->lhs->kind, EX_IDENT);
	Expr *pre = parse_str("--i");
	ASSERT_INT(pre->kind, EX_INCDEC);
	ASSERT_INT(pre->op, TOKEN_MINUSMINUS);
}

static void test_break_continue_stmt(void)
{
	Unit *u = parse_unit_str("void m() { while (true) { break; continue; } }");
	Block *body = u->funcs[0]->body->stmts[0]->then_blk;
	ASSERT_INT(body->stmts[0]->kind, ST_BREAK);
	ASSERT_INT(body->stmts[1]->kind, ST_CONTINUE);
}

static void test_foreach_stmt(void)
{
	Unit *u = parse_unit_str("void m() { int[] a; foreach (int x : a) { print(x); } }");
	Stmt *fe = u->funcs[0]->body->stmts[1];
	ASSERT_INT(fe->kind, ST_FOREACH);
	ASSERT_INT(fe->decl_type.kind, TY_INT);
	ASSERT_STR(fe->decl_name, "x");
	ASSERT_INT(fe->expr->kind, EX_IDENT);
	ASSERT_INT(fe->then_blk->count, 1);
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
	RUN(test_bool_literal);
	RUN(test_cast);
	RUN(test_float_literal);
	RUN(test_float_cast);
	RUN(test_string_expr);
	RUN(test_new_array);
	RUN(test_index_and_length);
	RUN(test_precedence);
	RUN(test_method_call);
	RUN(test_field_access);
	RUN(test_new);
	RUN(test_parse_function_with_vardecl);
	RUN(test_parse_scalar_vardecls);
	RUN(test_parse_double_vardecl);
	RUN(test_array_type_decls);
	RUN(test_map_type_decl);
	RUN(test_new_map);
	RUN(test_incdec_parse);
	RUN(test_break_continue_stmt);
	RUN(test_foreach_stmt);
	RUN(test_parse_if_else);
	RUN(test_parse_class_with_inheritance);
	RUN(test_parse_method_with_param);
	ast_free_all();
	SUMMARY();
	return 0;
}
