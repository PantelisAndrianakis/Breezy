#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void advance(Parser *p)
{
	p->cur = p->peek;
	p->peek = p->peek2;
	p->peek2 = lexer_next(&p->lex);
}

static int  check(Parser *p, TokenType tt)
{
	return p->cur.type == tt;
}

static int  match(Parser *p, TokenType tt)
{
	if (!check(p,tt))
	{
		return 0;
	}
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
	p->peek2 = lexer_next(&p->lex);
	advance(p);
}

static Expr *parse_comparison(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_multiplicative(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);
static int   parse_args(Parser *p, Expr **out);
static int   parse_type(Parser *p, TypeRef *out);
static int   parse_base_type(Parser *p, TypeRef *out);
static int   scalar_type_kind(TokenType t, TypeKind *out);

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
	TypeKind ck;
	if (check(p,TOKEN_LPAREN) && scalar_type_kind(p->peek.type,&ck) && ck != TY_VOID)
	{
		int line=p->cur.line;
		advance(p);                 /* consume '('. */
		Expr *e=expr_new(EX_CAST,line);
		parse_type(p,&e->type);     /* cast target lives in the result type slot. */
		expect(p,TOKEN_RPAREN);
		e->lhs=parse_unary(p);
		return e;
	}
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
	while (check(p,TOKEN_DOT) || check(p,TOKEN_LBRACKET))
	{
		int line=p->cur.line;
		if (check(p,TOKEN_LBRACKET))
		{
			advance(p);
			Expr *ix=expr_new(EX_INDEX,line);
			ix->lhs=e;
			ix->rhs=parse_expr(p);
			expect(p,TOKEN_RBRACKET);
			e=ix;
			continue;
		}

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
	if (check(p,TOKEN_RPAREN))
	{
		return 0;
	}
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
		e->int_val=strtoll(p->cur.text,NULL,10);
		strcpy(e->int_suffix,p->cur.suffix);
		advance(p);
		return e;
	}
	if (check(p,TOKEN_FLOAT_LIT))
	{
		Expr *e=expr_new(EX_FLOAT,line);
		e->float_val=strtod(p->cur.text,NULL);
		strcpy(e->int_suffix,p->cur.suffix);   /* "f" → float, "" → double. */
		advance(p);
		return e;
	}
	if (check(p,TOKEN_STR_LIT))
	{
		Expr *e=expr_new(EX_STR,line);
		strcpy(e->str_val,p->cur.text);
		advance(p);
		return e;
	}
	if (check(p,TOKEN_TRUE) || check(p,TOKEN_FALSE))
	{
		Expr *e=expr_new(EX_BOOL,line);
		e->int_val = check(p,TOKEN_TRUE) ? 1 : 0;
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
		TypeRef et;
		parse_base_type(p,&et);
		if (et.kind==TY_MAP)
		{
			expect(p,TOKEN_LPAREN);
			expect(p,TOKEN_RPAREN);
			Expr *e=expr_new(EX_NEWMAP,line);
			e->type=et;                       /* Carries elem (key) + elem2 (value). */
			return e;
		}
		if (check(p,TOKEN_LBRACKET))
		{
			advance(p);                       /* '[' */
			Expr *e=expr_new(EX_NEWARRAY,line);
			e->lhs=parse_expr(p);             /* the count */
			expect(p,TOKEN_RBRACKET);
			e->type.kind=TY_ARRAY;
			e->type.elem=typeref_box(et);
			return e;
		}

		expect(p,TOKEN_LPAREN);
		expect(p,TOKEN_RPAREN);
		Expr *e=expr_new(EX_NEW,line);
		strcpy(e->name,et.class_name);        /* object: et is an IDENT class */
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

static Block *parse_block(Parser *p);
static Stmt  *parse_statement(Parser *p);

/* Maps a scalar/void type-keyword token to its TypeKind, or returns 0. */
static int scalar_type_kind(TokenType t, TypeKind *out)
{
	switch (t)
	{
	case TOKEN_VOID:
		*out=TY_VOID;
		return 1;
	case TOKEN_BOOLEAN:
		*out=TY_BOOL;
		return 1;
	case TOKEN_BYTE:
		*out=TY_BYTE;
		return 1;
	case TOKEN_SHORT:
		*out=TY_SHORT;
		return 1;
	case TOKEN_INT:
		*out=TY_INT;
		return 1;
	case TOKEN_LONG:
		*out=TY_LONG;
		return 1;
	case TOKEN_UBYTE:
		*out=TY_UBYTE;
		return 1;
	case TOKEN_USHORT:
		*out=TY_USHORT;
		return 1;
	case TOKEN_UINT:
		*out=TY_UINT;
		return 1;
	case TOKEN_ULONG:
		*out=TY_ULONG;
		return 1;
	case TOKEN_FLOAT:
		*out=TY_FLOAT;
		return 1;
	case TOKEN_DOUBLE:
		*out=TY_DOUBLE;
		return 1;
	default:
		return 0;
	}
}

/* Parse a base (non-array) type: scalar / string / IDENT class. */
static int parse_base_type(Parser *p, TypeRef *out)
{
	TypeKind k;
	out->elem=NULL;
	out->elem2=NULL;
	if (check(p,TOKEN_MAP))
	{
		advance(p);
		expect(p,TOKEN_LT);
		TypeRef key, val;
		parse_type(p,&key);
		expect(p,TOKEN_COMMA);
		parse_type(p,&val);
		expect(p,TOKEN_GT);
		out->kind=TY_MAP;
		out->class_name[0]='\0';
		out->elem=typeref_box(key);
		out->elem2=typeref_box(val);
		return 1;
	}
	if (scalar_type_kind(p->cur.type, &k))
	{
		out->kind=k;
		out->class_name[0]='\0';
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_STRING))
	{
		out->kind=TY_STRING;
		out->class_name[0]='\0';
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT))
	{
		out->kind=TY_OBJECT;
		strcpy(out->class_name,p->cur.text);
		advance(p);
		return 1;
	}
	return 0;
}

/* A base type followed by zero or more empty `[]` suffixes → array types. Only
   an empty `[]` is consumed here (the `[5]` of `new T[5]` and the `[i]` of an
   index are left for their own parsers). */
static int parse_type(Parser *p, TypeRef *out)
{
	if (!parse_base_type(p,out))
	{
		return 0;
	}

	while (check(p,TOKEN_LBRACKET) && p->peek.type==TOKEN_RBRACKET)
	{
		advance(p);   /* '[' */
		advance(p);   /* ']' */
		TypeRef elem = *out;
		out->kind=TY_ARRAY;
		out->class_name[0]='\0';
		out->elem=typeref_box(elem);
	}

	return 1;
}

static int starts_vardecl(Parser *p)
{
	TypeKind k;
	if (scalar_type_kind(p->cur.type, &k) && k != TY_VOID)
	{
		return 1;
	}
	if (check(p,TOKEN_STRING))
	{
		return 1;
	}
	/* `map<...>` declarations start unambiguously with the map keyword. */
	if (check(p,TOKEN_MAP))
	{
		return 1;
	}
	if (check(p,TOKEN_IDENT) && p->peek.type == TOKEN_IDENT)
	{
		return 1;
	}
	/* `Foo[] a;` — an object-array declaration (vs. the index expression `foo[i]`). */
	if (check(p,TOKEN_IDENT) && p->peek.type == TOKEN_LBRACKET && p->peek2.type == TOKEN_RBRACKET)
	{
		return 1;
	}
	return 0;
}

static Stmt *parse_vardecl(Parser *p)
{
	int line=p->cur.line;
	Stmt *s=stmt_new(ST_VARDECL,line);
	parse_type(p,&s->decl_type);
	Token name=expect(p,TOKEN_IDENT);
	strcpy(s->decl_name,name.text);
	if (match(p,TOKEN_ASSIGN))
	{
		s->decl_init=parse_expr(p);
	}
	expect(p,TOKEN_SEMICOLON);
	return s;
}

static Stmt *parse_if(Parser *p)
{
	int line=p->cur.line;
	advance(p);
	Stmt *s=stmt_new(ST_IF,line);
	expect(p,TOKEN_LPAREN);
	s->cond=parse_expr(p);
	expect(p,TOKEN_RPAREN);
	s->then_blk=parse_block(p);
	if (match(p,TOKEN_ELSE))
	{
		s->else_blk=parse_block(p);
	}
	return s;
}

static Stmt *parse_while(Parser *p)
{
	int line=p->cur.line;
	advance(p);
	Stmt *s=stmt_new(ST_WHILE,line);
	expect(p,TOKEN_LPAREN);
	s->cond=parse_expr(p);
	expect(p,TOKEN_RPAREN);
	s->then_blk=parse_block(p);
	return s;
}

static Stmt *parse_return(Parser *p)
{
	int line=p->cur.line;
	advance(p);
	Stmt *s=stmt_new(ST_RETURN,line);
	if (!check(p,TOKEN_SEMICOLON))
	{
		s->ret_val=parse_expr(p);
	}
	expect(p,TOKEN_SEMICOLON);
	return s;
}

static Stmt *parse_assign_or_expr(Parser *p)
{
	int line=p->cur.line;
	Expr *first=parse_expr(p);
	if (match(p,TOKEN_ASSIGN))
	{
		if (first->kind!=EX_IDENT && first->kind!=EX_FIELD && first->kind!=EX_INDEX)
		{
			fprintf(stderr,"line %d: invalid assignment target\n",line);
			exit(1);
		}
		Stmt *s=stmt_new(ST_ASSIGN,line);
		s->target=first;
		s->value=parse_expr(p);
		expect(p,TOKEN_SEMICOLON);
		return s;
	}
	Stmt *s=stmt_new(ST_EXPR,line);
	s->expr=first;
	expect(p,TOKEN_SEMICOLON);
	return s;
}

static Stmt *parse_statement(Parser *p)
{
	if (starts_vardecl(p))
	{
		return parse_vardecl(p);
	}
	if (check(p,TOKEN_IF))
	{
		return parse_if(p);
	}
	if (check(p,TOKEN_WHILE))
	{
		return parse_while(p);
	}
	if (check(p,TOKEN_RETURN))
	{
		return parse_return(p);
	}
	return parse_assign_or_expr(p);
}

static Block *parse_block(Parser *p)
{
	expect(p,TOKEN_LBRACE);
	Block *b=block_new();
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		block_push(b, parse_statement(p));
	}
	expect(p,TOKEN_RBRACE);
	return b;
}

static Func *parse_function(Parser *p)
{
	Func *f=func_new();
	parse_type(p,&f->ret_type);
	Token name=expect(p,TOKEN_IDENT);
	strcpy(f->name,name.text);
	expect(p,TOKEN_LPAREN);
	if (!check(p,TOKEN_RPAREN))
	{
		do
		{
			if (f->param_count>=8)
			{
				fprintf(stderr,"too many params\n");
				exit(1);
			}
			Param *pm=&f->params[f->param_count++];
			parse_type(p,&pm->type);
			Token pn=expect(p,TOKEN_IDENT);
			strcpy(pm->name,pn.text);
		}
		while (match(p,TOKEN_COMMA));
	}
	expect(p,TOKEN_RPAREN);
	f->body=parse_block(p);
	return f;
}

static ClassDecl *parse_class(Parser *p)
{
	advance(p);
	ClassDecl *c=class_new();
	Token name=expect(p,TOKEN_IDENT);
	strcpy(c->name,name.text);
	if (match(p,TOKEN_EXTENDS))
	{
		Token par=expect(p,TOKEN_IDENT);
		strcpy(c->parent_name,par.text);
		c->has_parent=1;
	}
	expect(p,TOKEN_LBRACE);
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		TypeRef ty;
		if (!parse_type(p,&ty))
		{
			fprintf(stderr,"line %d: expected member type\n",p->cur.line);
			exit(1);
		}
		Token mname=expect(p,TOKEN_IDENT);
		if (check(p,TOKEN_LPAREN))
		{
			Func *f=func_new();
			f->ret_type=ty;
			strcpy(f->name,mname.text);
			advance(p);
			if (!check(p,TOKEN_RPAREN))
			{
				do
				{
					if (f->param_count>=8)
					{
						fprintf(stderr,"too many params\n");
						exit(1);
					}
					Param *pm=&f->params[f->param_count++];
					parse_type(p,&pm->type);
					Token pn=expect(p,TOKEN_IDENT);
					strcpy(pm->name,pn.text);
				}
				while (match(p,TOKEN_COMMA));
			}
			expect(p,TOKEN_RPAREN);
			f->body=parse_block(p);
			if (c->method_count>=32)
			{
				fprintf(stderr,"too many methods\n");
				exit(1);
			}
			c->methods[c->method_count++]=f;
		}
		else
		{
			expect(p,TOKEN_SEMICOLON);
			if (c->field_count>=32)
			{
				fprintf(stderr,"too many fields\n");
				exit(1);
			}
			c->fields[c->field_count].type=ty;
			strcpy(c->fields[c->field_count].name,mname.text);
			c->field_count++;
		}
	}
	expect(p,TOKEN_RBRACE);
	return c;
}

Unit *parse_unit(Parser *p)
{
	Unit *u=unit_new();
	while (!check(p,TOKEN_EOF))
	{
		if (check(p,TOKEN_CLASS))
		{
			if (u->klass)
			{
				fprintf(stderr,"line %d: only one class per file\n",p->cur.line);
				exit(1);
			}
			u->klass=parse_class(p);
		}
		else
		{
			if (u->func_count>=8)
			{
				fprintf(stderr,"too many top-level functions\n");
				exit(1);
			}
			u->funcs[u->func_count++]=parse_function(p);
		}
	}
	return u;
}
