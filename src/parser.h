#ifndef PARSER_H
#define PARSER_H
#include "lexer.h"
#include "ast.h"

typedef struct
{
	Lexer lex;
	int is_prelude; // Built-in source keeps its string literals plain.
	Token cur;
	Token peek;
	Token peek2;
} Parser;

void  parser_init(Parser *p, const char *src);
Expr *parse_expr(Parser *p);
Unit *parse_unit(Parser *p);

#endif
