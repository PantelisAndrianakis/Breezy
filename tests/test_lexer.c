#include "test_framework.h"
#include "lexer.h"

static void test_keywords(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "void int if else while return class extends new this");
	TokenType e[] = {TOKEN_VOID,TOKEN_INT,TOKEN_IF,TOKEN_ELSE,TOKEN_WHILE,
	                 TOKEN_RETURN,TOKEN_CLASS,TOKEN_EXTENDS,TOKEN_NEW,TOKEN_THIS,TOKEN_EOF
	                };
	for (int i = 0; i < 11; i++)
	{
		t = lexer_next(&l);
		ASSERT_INT(t.type, e[i]);
	}
}

static void test_idents_and_ints(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "myVar _x foo123 0 42 1000");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_IDENT);
	ASSERT_STR(t.text, "myVar");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_IDENT);
	ASSERT_STR(t.text, "_x");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_IDENT);
	ASSERT_STR(t.text, "foo123");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "0");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "42");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "1000");
}

static void test_operators(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "+ - * / = == != < > <= >= . , ; ( ) { }");
	TokenType e[] = {TOKEN_PLUS,TOKEN_MINUS,TOKEN_STAR,TOKEN_SLASH,TOKEN_ASSIGN,
	                 TOKEN_EQ,TOKEN_NEQ,TOKEN_LT,TOKEN_GT,TOKEN_LTE,TOKEN_GTE,
	                 TOKEN_DOT,TOKEN_COMMA,TOKEN_SEMICOLON,
	                 TOKEN_LPAREN,TOKEN_RPAREN,TOKEN_LBRACE,TOKEN_RBRACE,TOKEN_EOF
	                };
	for (int i = 0; i < 19; i++)
	{
		t = lexer_next(&l);
		ASSERT_INT(t.type, e[i]);
	}
}

static void test_comment_and_lines(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "int // ignore me\nx");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT);
	ASSERT_INT(t.line, 1);
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_IDENT);
	ASSERT_INT(t.line, 2);
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_EOF);
}

int main(void)
{
	printf("Lexer tests\n");
	RUN(test_keywords);
	RUN(test_idents_and_ints);
	RUN(test_operators);
	RUN(test_comment_and_lines);
	SUMMARY();
	return 0;
}
