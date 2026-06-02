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
	if (check(p,TOKEN_PLUSPLUS) || check(p,TOKEN_MINUSMINUS))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_INCDEC,line);
		e->op=op;
		e->lhs=parse_unary(p);
		return e;
	}
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
	if (check(p,TOKEN_PLUSPLUS) || check(p,TOKEN_MINUSMINUS))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *pe=expr_new(EX_INCDEC,line);
		pe->op=op;
		pe->lhs=e;
		e=pe;
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

/* The fixed set of compiler-known static namespaces. 5c/5d extend this. */
static int is_namespace(const char *name)
{
	return strcmp(name,"Math")==0 || strcmp(name,"Clock")==0 || strcmp(name,"Random")==0;
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
		if (et.kind==TY_GENERIC)
		{
			expect(p,TOKEN_LPAREN);
			expect(p,TOKEN_RPAREN);
			Expr *e=expr_new(EX_NEWGEN,line);
			e->type=et;                       /* Carries class_name (template) + elem (T). */
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
	if (check(p,TOKEN_IDENT) && is_namespace(p->cur.text) && p->peek.type==TOKEN_DOT)
	{
		char ns[64];
		strcpy(ns,p->cur.text);
		advance(p);                 /* namespace */
		expect(p,TOKEN_DOT);
		Token m=expect(p,TOKEN_IDENT);
		expect(p,TOKEN_LPAREN);
		Expr *e=expr_new(EX_CALL,line);
		snprintf(e->name,sizeof e->name,"%s.%s",ns,m.text);
		e->arg_count=parse_args(p,e->args);
		expect(p,TOKEN_RPAREN);
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

/* The fixed set of compiler-known generic templates. Part 4f extends this. */
static int is_generic_template(const char *name)
{
	return strcmp(name,"Box")==0
		   || strcmp(name,"List")==0
		   || strcmp(name,"Stack")==0
		   || strcmp(name,"Queue")==0
		   || strcmp(name,"Deque")==0
		   || strcmp(name,"ArrayDeque")==0
		   || strcmp(name,"Set")==0;
}

/* Parse a base (non-array) type: scalar / string / map / generic / IDENT class. */
static int parse_base_type(Parser *p, TypeRef *out)
{
	TypeKind k;
	out->elem=NULL;
	out->elem2=NULL;
	/* A known template name immediately followed by '<' is a generic. Since
	   parse_base_type is only reached in type position, an unknown IDENT before
	   '<' is a clear error, not a downstream parse failure. */
	if (check(p,TOKEN_IDENT) && p->peek.type==TOKEN_LT)
	{
		if (!is_generic_template(p->cur.text))
		{
			fprintf(stderr,"line %d: unknown generic template '%s' (user-defined generics are not supported)\n",
					p->cur.line, p->cur.text);
			exit(1);
		}

		char tmpl[64];
		strcpy(tmpl,p->cur.text);
		advance(p);                 /* Template name. */
		expect(p,TOKEN_LT);
		TypeRef el;
		parse_type(p,&el);
		expect(p,TOKEN_GT);
		out->kind=TY_GENERIC;
		strcpy(out->class_name,tmpl);
		out->elem=typeref_box(el);
		out->elem2=NULL;
		return 1;
	}
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
	/* `Box<...>` etc.: a known template name followed by '<'. */
	if (check(p,TOKEN_IDENT) && is_generic_template(p->cur.text) && p->peek.type == TOKEN_LT)
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

/* A statement with no trailing ';' (a for-loop init/post clause): a var-decl, an
   assignment, or a bare expression. */
static int compound_to_binop(TokenType t, int *op)
{
	switch (t)
	{
	case TOKEN_PLUS_ASSIGN:
		*op=TOKEN_PLUS;
		return 1;
	case TOKEN_MINUS_ASSIGN:
		*op=TOKEN_MINUS;
		return 1;
	case TOKEN_STAR_ASSIGN:
		*op=TOKEN_STAR;
		return 1;
	case TOKEN_SLASH_ASSIGN:
		*op=TOKEN_SLASH;
		return 1;
	default:
		return 0;
	}
}

/* Build `target = (target OP rhs)` from a compound assignment. The target node
   is shared as both the store target and the binary's left operand. */
static Stmt *make_compound_assign(int line, Expr *target, int binop, Expr *rhs)
{
	Expr *bin=expr_new(EX_BINARY,line);
	bin->op=binop;
	bin->lhs=target;
	bin->rhs=rhs;
	Stmt *s=stmt_new(ST_ASSIGN,line);
	s->target=target;
	s->value=bin;
	return s;
}

static Stmt *parse_simple_stmt(Parser *p)
{
	int line=p->cur.line;
	if (starts_vardecl(p))
	{
		Stmt *s=stmt_new(ST_VARDECL,line);
		parse_type(p,&s->decl_type);
		Token name=expect(p,TOKEN_IDENT);
		strcpy(s->decl_name,name.text);
		if (match(p,TOKEN_ASSIGN))
		{
			s->decl_init=parse_expr(p);
		}

		return s;
	}

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
		return s;
	}

	int binop;
	if (compound_to_binop(p->cur.type, &binop))
	{
		if (first->kind!=EX_IDENT && first->kind!=EX_FIELD && first->kind!=EX_INDEX)
		{
			fprintf(stderr,"line %d: invalid assignment target\n",line);
			exit(1);
		}

		advance(p);
		return make_compound_assign(line, first, binop, parse_expr(p));
	}

	Stmt *s=stmt_new(ST_EXPR,line);
	s->expr=first;
	return s;
}

static Stmt *parse_for(Parser *p)
{
	int line=p->cur.line;
	advance(p);                       /* Consume 'for'. */
	Stmt *s=stmt_new(ST_FOR,line);
	expect(p,TOKEN_LPAREN);
	s->for_init=parse_simple_stmt(p);
	expect(p,TOKEN_SEMICOLON);
	s->cond=parse_expr(p);
	expect(p,TOKEN_SEMICOLON);
	s->for_post=parse_simple_stmt(p);
	expect(p,TOKEN_RPAREN);
	s->then_blk=parse_block(p);
	return s;
}

static Expr *parse_case_const(Parser *p)
{
	int line=p->cur.line;
	int neg=0;
	if (check(p,TOKEN_MINUS))
	{
		neg=1;
		advance(p);
	}

	Token t=expect(p,TOKEN_INT_LIT);
	Expr *e=expr_new(EX_INT,line);
	e->int_val=strtoll(t.text,NULL,10);
	if (neg)
	{
		e->int_val=-e->int_val;
	}

	return e;
}

static Stmt *parse_switch(Parser *p)
{
	int line=p->cur.line;
	advance(p);                       /* Consume 'switch'. */
	Stmt *s=stmt_new(ST_SWITCH,line);
	expect(p,TOKEN_LPAREN);
	s->cond=parse_expr(p);
	expect(p,TOKEN_RPAREN);
	expect(p,TOKEN_LBRACE);
	s->then_blk=block_new();
	int seen_label=0;
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		if (check(p,TOKEN_CASE))
		{
			int cl=p->cur.line;
			advance(p);
			Stmt *c=stmt_new(ST_CASE,cl);
			c->value=parse_case_const(p);
			expect(p,TOKEN_COLON);
			block_push(s->then_blk,c);
			seen_label=1;
		}
		else if (check(p,TOKEN_DEFAULT))
		{
			int cl=p->cur.line;
			advance(p);
			expect(p,TOKEN_COLON);
			block_push(s->then_blk,stmt_new(ST_DEFAULT,cl));
			seen_label=1;
		}
		else if (check(p,TOKEN_LBRACE))
		{
			Block *grp=parse_block(p);            /* Java/K&R braces: flatten. */
			for (int i=0; i<grp->count; i++)
			{
				block_push(s->then_blk,grp->stmts[i]);
			}
		}
		else
		{
			if (!seen_label)
			{
				fprintf(stderr,"line %d: statement before first case in switch\n",p->cur.line);
				exit(1);
			}

			block_push(s->then_blk,parse_statement(p));
		}
	}

	expect(p,TOKEN_RBRACE);
	return s;
}

static Stmt *parse_foreach(Parser *p)
{
	int line=p->cur.line;
	advance(p);                       /* Consume 'foreach'. */
	Stmt *s=stmt_new(ST_FOREACH,line);
	expect(p,TOKEN_LPAREN);
	parse_type(p,&s->decl_type);
	Token name=expect(p,TOKEN_IDENT);
	strcpy(s->decl_name,name.text);
	expect(p,TOKEN_COLON);
	s->expr=parse_expr(p);
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
	int binop;
	if (compound_to_binop(p->cur.type, &binop))
	{
		if (first->kind!=EX_IDENT && first->kind!=EX_FIELD && first->kind!=EX_INDEX)
		{
			fprintf(stderr,"line %d: invalid assignment target\n",line);
			exit(1);
		}

		advance(p);
		Stmt *s=make_compound_assign(line, first, binop, parse_expr(p));
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
	if (check(p,TOKEN_FOREACH))
	{
		return parse_foreach(p);
	}
	if (check(p,TOKEN_FOR))
	{
		return parse_for(p);
	}
	if (check(p,TOKEN_SWITCH))
	{
		return parse_switch(p);
	}
	if (check(p,TOKEN_RETURN))
	{
		return parse_return(p);
	}
	if (check(p,TOKEN_BREAK))
	{
		int line=p->cur.line;
		advance(p);
		expect(p,TOKEN_SEMICOLON);
		return stmt_new(ST_BREAK,line);
	}
	if (check(p,TOKEN_CONTINUE))
	{
		int line=p->cur.line;
		advance(p);
		expect(p,TOKEN_SEMICOLON);
		return stmt_new(ST_CONTINUE,line);
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
