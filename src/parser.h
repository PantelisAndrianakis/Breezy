#ifndef PARSER_H
#define PARSER_H
#include "lexer.h"
#include "ast.h"

typedef struct
{
	Lexer lex;
	Token cur;
	Token peek;
	Token peek2;
} Parser;

void  parser_init(Parser *p, const char *src);
Expr *parse_expr(Parser *p);
Unit *parse_unit(Parser *p);

#endif
