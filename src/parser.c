#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void advance(Parser *p)
{
	p->cur = p->peek;
	p->peek = lexer_next(&p->lex);
}
static int  check(Parser *p, TokenType tt)
{
	return p->cur.type == tt;
}
static int  match(Parser *p, TokenType tt)
{
	if (!check(p,tt)) return 0;
	advance(p);
	return 1;
}
static Token expect(Parser *p, TokenType tt)
{
	if (p->cur.type != tt)
	{
		fprintf(stderr, "line %d: expected '%s', got '%s'\n",
		        p->cur.line, token_type_name(tt), token_type_name(p->cur.type));
		exit(1);
	}
	Token t = p->cur;
	advance(p);
	return t;
}

void parser_init(Parser *p, const char *src)
{
	lexer_init(&p->lex, src);
	p->peek = lexer_next(&p->lex);
	advance(p);
}

static Expr *parse_comparison(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_multiplicative(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);
static int   parse_args(Parser *p, Expr **out);

Expr *parse_expr(Parser *p)
{
	return parse_comparison(p);
}

static int is_cmp(TokenType t)
{
	return t==TOKEN_EQ||t==TOKEN_NEQ||t==TOKEN_LT||t==TOKEN_GT||t==TOKEN_LTE||t==TOKEN_GTE;
}

static Expr *parse_comparison(Parser *p)
{
	Expr *left = parse_additive(p);
	if (is_cmp(p->cur.type))
	{
		int op = p->cur.type, line = p->cur.line;
		advance(p);
		Expr *e = expr_new(EX_BINARY, line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_additive(p);
		return e;
	}
	return left;
}
static Expr *parse_additive(Parser *p)
{
	Expr *left = parse_multiplicative(p);
	while (check(p,TOKEN_PLUS)||check(p,TOKEN_MINUS))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_multiplicative(p);
		left=e;
	}
	return left;
}
static Expr *parse_multiplicative(Parser *p)
{
	Expr *left = parse_unary(p);
	while (check(p,TOKEN_STAR)||check(p,TOKEN_SLASH))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_unary(p);
		left=e;
	}
	return left;
}
static Expr *parse_unary(Parser *p)
{
	if (check(p,TOKEN_MINUS))
	{
		int line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_UNARY,line);
		e->op=TOKEN_MINUS;
		e->lhs=parse_unary(p);
		return e;
	}
	return parse_postfix(p);
}
static Expr *parse_postfix(Parser *p)
{
	Expr *e = parse_primary(p);
	while (check(p,TOKEN_DOT))
	{
		int line=p->cur.line;
		advance(p);
		Token name = expect(p, TOKEN_IDENT);
		if (check(p,TOKEN_LPAREN))
		{
			advance(p);
			Expr *call = expr_new(EX_METHOD_CALL, line);
			strcpy(call->name, name.text);
			call->lhs = e;
			call->arg_count = parse_args(p, call->args);
			expect(p, TOKEN_RPAREN);
			e = call;
		}
		else
		{
			Expr *f = expr_new(EX_FIELD, line);
			strcpy(f->name, name.text);
			f->lhs = e;
			e = f;
		}
	}
	return e;
}
static int parse_args(Parser *p, Expr **out)
{
	int n = 0;
	if (check(p,TOKEN_RPAREN)) return 0;
	do
	{
		if (n >= 8)
		{
			fprintf(stderr,"line %d: too many args\n",p->cur.line);
			exit(1);
		}
		out[n++] = parse_expr(p);
	}
	while (match(p,TOKEN_COMMA));
	return n;
}
static Expr *parse_primary(Parser *p)
{
	int line = p->cur.line;
	if (check(p,TOKEN_INT_LIT))
	{
		Expr *e=expr_new(EX_INT,line);
		e->int_val=strtol(p->cur.text,NULL,10);
		advance(p);
		return e;
	}
	if (check(p,TOKEN_THIS))
	{
		advance(p);
		return expr_new(EX_THIS,line);
	}
	if (check(p,TOKEN_NEW))
	{
		advance(p);
		Token cls=expect(p,TOKEN_IDENT);
		expect(p,TOKEN_LPAREN);
		expect(p,TOKEN_RPAREN);
		Expr *e=expr_new(EX_NEW,line);
		strcpy(e->name,cls.text);
		return e;
	}
	if (check(p,TOKEN_IDENT))
	{
		Token id=p->cur;
		advance(p);
		if (check(p,TOKEN_LPAREN))
		{
			advance(p);
			Expr *e=expr_new(EX_CALL,line);
			strcpy(e->name,id.text);
			e->arg_count=parse_args(p,e->args);
			expect(p,TOKEN_RPAREN);
			return e;
		}
		Expr *e=expr_new(EX_IDENT,line);
		strcpy(e->name,id.text);
		return e;
	}
	if (match(p,TOKEN_LPAREN))
	{
		Expr *e=parse_expr(p);
		expect(p,TOKEN_RPAREN);
		return e;
	}
	fprintf(stderr,"line %d: unexpected token '%s'\n", line, token_type_name(p->cur.type));
	exit(1);
}
