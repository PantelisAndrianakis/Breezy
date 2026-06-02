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

static void test_scalar_keywords(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "byte short long ubyte ushort uint ulong boolean true false");
	TokenType e[] = {TOKEN_BYTE,TOKEN_SHORT,TOKEN_LONG,TOKEN_UBYTE,TOKEN_USHORT,
					 TOKEN_UINT,TOKEN_ULONG,TOKEN_BOOLEAN,TOKEN_TRUE,TOKEN_FALSE,TOKEN_EOF
					};
	for (int i = 0; i < 11; i++)
	{
		t = lexer_next(&l);
		ASSERT_INT(t.type, e[i]);
	}
}

static void test_int_suffixes(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "42 7L 9u 5uL 6Lu");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "42");
	ASSERT_STR(t.suffix, "");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "7");
	ASSERT_STR(t.suffix, "L");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "9");
	ASSERT_STR(t.suffix, "u");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "5");
	ASSERT_STR(t.suffix, "uL");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_INT_LIT);
	ASSERT_STR(t.text, "6");
	ASSERT_STR(t.suffix, "Lu");
}

static void test_float_keywords(void)
{
	Lexer l;
	lexer_init(&l, "float double");
	ASSERT_INT(lexer_next(&l).type, TOKEN_FLOAT);
	ASSERT_INT(lexer_next(&l).type, TOKEN_DOUBLE);
}

static void test_float_literals(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "1.5 1.5f 1e9 .5 2.");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_FLOAT_LIT);
	ASSERT_STR(t.text, "1.5");
	ASSERT_STR(t.suffix, "");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_FLOAT_LIT);
	ASSERT_STR(t.text, "1.5");
	ASSERT_STR(t.suffix, "f");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_FLOAT_LIT);
	ASSERT_STR(t.text, "1e9");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_FLOAT_LIT);
	ASSERT_STR(t.text, ".5");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_FLOAT_LIT);
	ASSERT_STR(t.text, "2.");
}

static void test_string_literal(void)
{
	Lexer l;
	Token t;
	lexer_init(&l, "\"hello\" \"a\\nb\"");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_STR_LIT);
	ASSERT_STR(t.text, "hello");
	t = lexer_next(&l);
	ASSERT_INT(t.type, TOKEN_STR_LIT);
	ASSERT_STR(t.text, "a\nb");
}

static void test_brackets(void)
{
	Lexer l;
	lexer_init(&l, "[ ]");
	ASSERT_INT(lexer_next(&l).type, TOKEN_LBRACKET);
	ASSERT_INT(lexer_next(&l).type, TOKEN_RBRACKET);
}

static void test_map_keyword(void)
{
	Lexer l;
	lexer_init(&l, "map");
	ASSERT_INT(lexer_next(&l).type, TOKEN_MAP);
}

static void test_foreach_and_colon(void)
{
	Lexer l;
	lexer_init(&l, "foreach :");
	ASSERT_INT(lexer_next(&l).type, TOKEN_FOREACH);
	ASSERT_INT(lexer_next(&l).type, TOKEN_COLON);
}

static void test_incdec_operators(void)
{
	Lexer l;
	lexer_init(&l, "++ --");
	ASSERT_INT(lexer_next(&l).type, TOKEN_PLUSPLUS);
	ASSERT_INT(lexer_next(&l).type, TOKEN_MINUSMINUS);
}

static void test_break_continue_keywords(void)
{
	Lexer l;
	lexer_init(&l, "break continue");
	ASSERT_INT(lexer_next(&l).type, TOKEN_BREAK);
	ASSERT_INT(lexer_next(&l).type, TOKEN_CONTINUE);
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
	RUN(test_scalar_keywords);
	RUN(test_float_keywords);
	RUN(test_float_literals);
	RUN(test_string_literal);
	RUN(test_brackets);
	RUN(test_map_keyword);
	RUN(test_foreach_and_colon);
	RUN(test_incdec_operators);
	RUN(test_break_continue_keywords);
	RUN(test_int_suffixes);
	RUN(test_idents_and_ints);
	RUN(test_operators);
	RUN(test_comment_and_lines);
	SUMMARY();
	return 0;
}
