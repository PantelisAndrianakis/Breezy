#include "parser.h"
#include "grow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Convert an integer-literal token's text to its value, honouring the radix
   prefix: 0x/0X is hex, 0b/0B is binary, anything else is decimal. A bare
   leading zero is decimal (not octal), matching the lexer. */
static long long parse_int_text(const char *s)
{
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		return strtoll(s, NULL, 16);
	}

	if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
	{
		return strtoll(s + 2, NULL, 2);
	}

	return strtoll(s, NULL, 10);
}

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
		fprintf(stderr, "line %d: Expected '%s', got '%s'\n",
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

static Expr *parse_or(Parser *p);
static Expr *parse_xor(Parser *p);
static Expr *parse_and(Parser *p);
static Expr *parse_bitor(Parser *p);
static Expr *parse_bitxor(Parser *p);
static Expr *parse_bitand(Parser *p);
static Expr *parse_comparison(Parser *p);
static Expr *parse_shift(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_multiplicative(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);
static int   parse_args(Parser *p, Expr *e);
static int   parse_type(Parser *p, TypeRef *out);
static int   parse_base_type(Parser *p, TypeRef *out);
static int   scalar_type_kind(TokenType t, TypeKind *out);
static Block *parse_block(Parser *p);
static Expr *parse_lambda(Parser *p);
static int   looks_like_lambda(Parser *p);

Expr *parse_expr(Parser *p)
{
	return parse_or(p);
}

/* Logical OR - loosest operator. Left-associative, boolean. */
static Expr *parse_or(Parser *p)
{
	Expr *left = parse_xor(p);
	while (check(p,TOKEN_OR))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_xor(p);
		left=e;
	}
	return left;
}

/* Logical XOR - between OR and AND. Left-associative, boolean. */
static Expr *parse_xor(Parser *p)
{
	Expr *left = parse_and(p);
	while (check(p,TOKEN_XOR))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_and(p);
		left=e;
	}
	return left;
}

/* Logical AND - tightest logical level, looser than the bitwise operators.
   Left-associative, boolean. */
static Expr *parse_and(Parser *p)
{
	Expr *left = parse_bitor(p);
	while (check(p,TOKEN_AND))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_bitor(p);
		left=e;
	}
	return left;
}

/* Bitwise OR - loosest of the bitwise operators, looser than comparison.
   Left-associative. */
static Expr *parse_bitor(Parser *p)
{
	Expr *left = parse_bitxor(p);
	while (check(p,TOKEN_PIPE))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_bitxor(p);
		left=e;
	}
	return left;
}

/* Bitwise XOR - between OR and AND. Left-associative. */
static Expr *parse_bitxor(Parser *p)
{
	Expr *left = parse_bitand(p);
	while (check(p,TOKEN_CARET))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_bitand(p);
		left=e;
	}
	return left;
}

/* Bitwise AND - tightest bitwise level, still looser than the comparisons.
   Left-associative. */
static Expr *parse_bitand(Parser *p)
{
	Expr *left = parse_comparison(p);
	while (check(p,TOKEN_AMP))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_comparison(p);
		left=e;
	}
	return left;
}

static int is_cmp(TokenType t)
{
	return t==TOKEN_EQ||t==TOKEN_NEQ||t==TOKEN_LT||t==TOKEN_GT||t==TOKEN_LTE||t==TOKEN_GTE;
}

static Expr *parse_comparison(Parser *p)
{
	Expr *left = parse_shift(p);
	int op = 0;
	if (is_cmp(p->cur.type))
	{
		op = p->cur.type;
	}
	else if (p->cur.type==TOKEN_IDENT && strcmp(p->cur.text,"equals")==0)
	{
		op = TOKEN_EQ;   /* Soft keyword: == in infix position only. */
	}
	else if (p->cur.type==TOKEN_IDENT && strcmp(p->cur.text,"differs")==0)
	{
		op = TOKEN_NEQ;  /* Soft keyword: != in infix position only. */
	}

	if (op)
	{
		int line = p->cur.line;
		advance(p);
		Expr *e = expr_new(EX_BINARY, line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_shift(p);
		return e;
	}
	return left;
}

/* Shift level: binds looser than '+'/'-' but tighter than the comparisons,
   matching C/Java. `a + b << c` is `(a + b) << c`; `a << b < c` is
   `(a << b) < c`. Left-associative. */
static Expr *parse_shift(Parser *p)
{
	Expr *left = parse_additive(p);
	while (check(p,TOKEN_SHL)||check(p,TOKEN_SHR))
	{
		int op=p->cur.type, line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_BINARY,line);
		e->op=op;
		e->lhs=left;
		e->rhs=parse_additive(p);
		left=e;
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
	while (check(p,TOKEN_STAR)||check(p,TOKEN_SLASH)||check(p,TOKEN_PERCENT))
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
	if (check(p,TOKEN_LPAREN) && scalar_type_kind(p->peek.type,&ck) && ck != TY_VOID
			&& !looks_like_lambda(p))   /* (int n) => ... is a lambda, not a cast. */
	{
		int line=p->cur.line;
		advance(p);                 /* Consume '('. */
		Expr *e=expr_new(EX_CAST,line);
		parse_type(p,&e->type);     /* Cast target lives in the result type slot. */
		expect(p,TOKEN_RPAREN);
		e->lhs=parse_unary(p);
		return e;
	}
	if (check(p,TOKEN_NOT))
	{
		int line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_UNARY,line);
		e->op=TOKEN_NOT;
		e->lhs=parse_unary(p);
		return e;
	}
	if (check(p,TOKEN_TILDE))
	{
		int line=p->cur.line;
		advance(p);
		Expr *e=expr_new(EX_UNARY,line);
		e->op=TOKEN_TILDE;
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
		/* `map` is a type keyword but also a collection combinator method name; accept
		   it (and only it) as a member name after a dot. */
		Token name;
		if (check(p,TOKEN_MAP))
		{
			name = p->cur;
			strcpy(name.text, "map");
			advance(p);
		}
		else
		{
			name = expect(p, TOKEN_IDENT);
		}

		if (check(p,TOKEN_LPAREN))
		{
			advance(p);
			Expr *call = expr_new(EX_METHOD_CALL, line);
			strcpy(call->name, name.text);
			call->lhs = e;
			call->arg_count = parse_args(p, call);
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

static int parse_args(Parser *p, Expr *e)
{
	if (check(p,TOKEN_RPAREN))
	{
		return 0;
	}
	do
	{
		expr_add_arg(e, parse_expr(p));
	}
	while (match(p,TOKEN_COMMA));
	return e->arg_count;
}

/* The fixed set of compiler-known static namespaces. 5c/5d extend this. */
static int is_namespace(const char *name)
{
	return strcmp(name,"Math")==0 || strcmp(name,"Clock")==0 || strcmp(name,"Random")==0 || strcmp(name,"Regex")==0 || strcmp(name,"File")==0 || strcmp(name,"System")==0 || strcmp(name,"Network")==0 || strcmp(name,"Graphics")==0 || strcmp(name,"Ffi")==0 || strcmp(name,"Log")==0 || strcmp(name,"Xml")==0 || strcmp(name,"Json")==0 || strcmp(name,"Http")==0;
}

/* A '(' opens a lambda parameter list (rather than a grouped expression) iff the
   token after the matching ')' is '=>'. Scans a parser copy, non-destructive. */
static int looks_like_lambda(Parser *p)
{
	if (p->cur.type != TOKEN_LPAREN)
	{
		return 0;
	}

	Parser t = *p;
	int depth = 0;
	for (;;)
	{
		if (t.cur.type == TOKEN_EOF)
		{
			return 0;
		}

		if (t.cur.type == TOKEN_LPAREN)
		{
			depth++;
		}
		else if (t.cur.type == TOKEN_RPAREN)
		{
			if (--depth == 0)
			{
				advance(&t);
				return t.cur.type == TOKEN_FATARROW;
			}
		}

		advance(&t);
	}
}

/* True if the next lambda parameter carries an explicit type annotation (vs a
   bare inferred name). */
static int lambda_param_typed(Parser *p)
{
	TypeKind k;
	if (scalar_type_kind(p->cur.type,&k))
	{
		return 1;
	}

	if (check(p,TOKEN_STRING) || check(p,TOKEN_MAP) || check(p,TOKEN_CHANNEL) || check(p,TOKEN_LPAREN))
	{
		return 1;   /* string / map<> / channel<> / (P)->R param type. */
	}

	if (check(p,TOKEN_IDENT)
			&& (p->peek.type==TOKEN_IDENT || p->peek.type==TOKEN_LT || p->peek.type==TOKEN_LBRACKET))
	{
		return 1;   /* Type name / generic / array, followed by the param name. */
	}

	return 0;
}

/* Lambda literal: `name => body` or `(p, ...) => body`, where body is a single
   expression or a `{ ... }` block. Parameter types are optional (inferred from
   the expected function type at resolve). */
static Expr *parse_lambda(Parser *p)
{
	Expr *e = expr_new(EX_LAMBDA, p->cur.line);
	e->lam = lambda_new();
	if (check(p,TOKEN_IDENT))
	{
		TypeRef none;
		memset(&none,0,sizeof(none));
		lambda_add_param(e->lam, p->cur.text, 0, none);
		advance(p);
	}
	else
	{
		expect(p,TOKEN_LPAREN);
		if (!check(p,TOKEN_RPAREN))
		{
			do
			{
				TypeRef pt;
				memset(&pt,0,sizeof(pt));
				int has = 0;
				if (lambda_param_typed(p))
				{
					parse_type(p,&pt);
					has = 1;
				}

				Token nm = expect(p,TOKEN_IDENT);
				lambda_add_param(e->lam, nm.text, has, pt);
			}
			while (match(p,TOKEN_COMMA));
		}

		expect(p,TOKEN_RPAREN);
	}

	expect(p,TOKEN_FATARROW);
	if (check(p,TOKEN_LBRACE))
	{
		e->lam->is_block = 1;
		e->lam->body_block = parse_block(p);
	}
	else
	{
		e->lam->is_block = 0;
		e->lam->body_expr = parse_expr(p);
	}

	return e;
}

static Expr *parse_primary(Parser *p)
{
	int line = p->cur.line;
	if ((check(p,TOKEN_IDENT) && p->peek.type==TOKEN_FATARROW)
			|| (check(p,TOKEN_LPAREN) && looks_like_lambda(p)))
	{
		return parse_lambda(p);
	}

	if (check(p,TOKEN_INT_LIT))
	{
		Expr *e=expr_new(EX_INT,line);
		e->int_val=parse_int_text(p->cur.text);
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
	if (check(p,TOKEN_NULL))
	{
		Expr *e=expr_new(EX_NULL,line);
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
		if (et.kind==TY_CHANNEL)
		{
			expect(p,TOKEN_LPAREN);
			Expr *e=expr_new(EX_NEWCHANNEL,line);
			e->type=et;                       /* Carries elem (T). */
			expr_add_arg(e, parse_expr(p));   /* Capacity. */
			expect(p,TOKEN_RPAREN);
			return e;
		}
		if (et.kind==TY_GENERIC)
		{
			Expr *e=expr_new(EX_NEWGEN,line);
			e->type=et;                       /* class_name (template) + elem or targs. */
			expect(p,TOKEN_LPAREN);
			if (et.targ_count>0 && !check(p,TOKEN_RPAREN))
			{
				do
				{
					expr_add_arg(e, parse_expr(p));
				}
				while (match(p,TOKEN_COMMA));
			}

			expect(p,TOKEN_RPAREN);
			return e;
		}
		if (check(p,TOKEN_LBRACKET))
		{
			advance(p);                       /* '[' */
			Expr *e=expr_new(EX_NEWARRAY,line);
			e->lhs=parse_expr(p);             /* The count. */
			expect(p,TOKEN_RBRACKET);
			e->type.kind=TY_ARRAY;
			e->type.elem=typeref_box(et);
			return e;
		}

		expect(p,TOKEN_LPAREN);
		Expr *e=expr_new(EX_NEW,line);
		strcpy(e->name,et.class_name);        /* Object: et is an IDENT class. */
		if (!check(p,TOKEN_RPAREN))
		{
			e->arg_count=parse_args(p,e);   /* Constructor arguments. */
		}

		expect(p,TOKEN_RPAREN);
		return e;
	}
	if (check(p,TOKEN_IDENT) && is_namespace(p->cur.text) && p->peek.type==TOKEN_DOT)
	{
		char ns[64];
		strcpy(ns,p->cur.text);
		advance(p);                 /* Namespace. */
		expect(p,TOKEN_DOT);
		Token m=expect(p,TOKEN_IDENT);
		if (check(p,TOKEN_LPAREN))
		{
			expect(p,TOKEN_LPAREN);
			Expr *e=expr_new(EX_CALL,line);
			snprintf(e->name,sizeof e->name,"%.31s.%.31s",ns,m.text);   /* Bounded to fit name[64] (ns/method are short). */
			e->arg_count=parse_args(p,e);
			expect(p,TOKEN_RPAREN);
			return e;
		}

		/* No call parens: a namespace constant, e.g. File.READONLY. Build a field
		   access (lhs = the namespace identifier); resolve folds it to a literal. */
		Expr *id=expr_new(EX_IDENT,line);
		strcpy(id->name,ns);
		Expr *e=expr_new(EX_FIELD,line);
		e->lhs=id;
		strcpy(e->name,m.text);
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
			e->arg_count=parse_args(p,e);
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
	fprintf(stderr,"line %d: Unexpected token '%s'\n", line, token_type_name(p->cur.type));
	exit(1);
}

static Block *parse_block(Parser *p);
static Stmt  *parse_statement(Parser *p);
static Stmt  *parse_try(Parser *p);

/* Maps a scalar/void type-keyword token to its TypeKind, or returns 0. */
static int scalar_type_kind(TokenType t, TypeKind *out)
{
	switch (t)
	{
	case TOKEN_VOID:
		*out=TY_VOID;
		return 1;
	case TOKEN_BOOL:
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

/* The fixed set of compiler-known generic templates. */
static int is_generic_template(const char *name)
{
	return strcmp(name,"Box")==0
	       || strcmp(name,"List")==0
	       || strcmp(name,"Stack")==0
	       || strcmp(name,"Queue")==0
	       || strcmp(name,"Deque")==0
	       || strcmp(name,"ArrayDeque")==0
	       || strcmp(name,"Set")==0
	       || strcmp(name,"PriorityQueue")==0
	       || strcmp(name,"TreeSet")==0
	       || strcmp(name,"TreeMap")==0;
}

/* Consume the single '>' that closes a generic type. Nested generics such as
   map<Box,map<Box,int>> end in a '>>' which the lexer scans as one TOKEN_SHR
   (the shift operator); here we split it - consuming one '>' by demoting the
   current token to a lone '>' that the enclosing generic's close then consumes.
   This is the standard generics-vs-shift disambiguation (cf. javac, Roslyn). */
static void expect_gt(Parser *p)
{
	if (p->cur.type==TOKEN_SHR)
	{
		p->cur.type=TOKEN_GT;   /* One '>' consumed; one '>' remains as cur. */
		return;
	}

	expect(p,TOKEN_GT);
}

/* Parse a base (non-array) type: scalar / string / map / generic / IDENT class. */
static int parse_base_type(Parser *p, TypeRef *out)
{
	TypeKind k;
	out->elem=NULL;
	out->elem2=NULL;
	out->targs=NULL;
	out->targ_cap=0;
	out->targ_count=0;   /* Cleared for every base type; only user generics set it. */
	/* A known template name immediately followed by '<' is a generic. Since
	   parse_base_type is only reached in type position, an unknown IDENT before
	   '<' is a clear error, not a downstream parse failure. */
	if (check(p,TOKEN_IDENT) && p->peek.type==TOKEN_LT)
	{
		char tmpl[64];
		strcpy(tmpl,p->cur.text);
		int builtin = is_generic_template(tmpl);
		advance(p);                 /* Template / generic-class name. */
		expect(p,TOKEN_LT);
		out->kind=TY_GENERIC;
		strcpy(out->class_name,tmpl);
		out->elem=NULL;
		out->elem2=NULL;
		out->targ_count=0;
		if (builtin)
		{
			TypeRef el;
			parse_type(p,&el);
			out->elem=typeref_box(el);   /* Built-in templates carry T in elem (4e/4f). */
			if (strcmp(tmpl,"TreeMap")==0)
			{
				expect(p,TOKEN_COMMA);
				TypeRef val;
				parse_type(p,&val);
				out->elem2=typeref_box(val);   /* TreeMap<K,V>: V in elem2 (like map<K,V>). */
			}
		}
		else
		{
			/* User-generic application: Name<T1, T2, ...>; validated whole-program. */
			do
			{
				TypeRef a;
				parse_type(p,&a);
				out->targs=grow_ensure(out->targs,out->targ_count,&out->targ_cap,sizeof(*out->targs));
				out->targs[out->targ_count++]=typeref_box(a);
			}
			while (match(p,TOKEN_COMMA));
		}

		expect_gt(p);
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
		expect_gt(p);
		out->kind=TY_MAP;
		out->class_name[0]='\0';
		out->elem=typeref_box(key);
		out->elem2=typeref_box(val);
		return 1;
	}
	if (check(p,TOKEN_CHANNEL))
	{
		advance(p);
		expect(p,TOKEN_LT);
		TypeRef el;
		parse_type(p,&el);
		expect_gt(p);
		out->kind=TY_CHANNEL;
		out->class_name[0]='\0';
		out->elem=typeref_box(el);
		out->elem2=NULL;
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
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Timer")==0)
	{
		out->kind=TY_TIMER;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Entry")==0)
	{
		out->kind=TY_ENTRY;
		out->class_name[0]='\0';
		out->elem=NULL;        /* K/V are supplied by the iterable in resolve. */
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Listener")==0)
	{
		out->kind=TY_LISTENER;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Socket")==0)
	{
		out->kind=TY_SOCKET;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"TlsSocket")==0)
	{
		out->kind=TY_TLSSOCKET;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"TlsListener")==0)
	{
		out->kind=TY_TLSLISTENER;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Surface")==0)
	{
		out->kind=TY_SURFACE;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"GlSurface")==0)
	{
		out->kind=TY_GLSURFACE;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"MappedFile")==0)
	{
		out->kind=TY_MAPPEDFILE;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"UdpSocket")==0)
	{
		out->kind=TY_UDPSOCKET;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Datagram")==0)
	{
		out->kind=TY_DATAGRAM;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"JsonValue")==0)
	{
		out->kind=TY_JSONVALUE;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"HttpRequest")==0)
	{
		out->kind=TY_HTTPREQUEST;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"HttpResponse")==0)
	{
		out->kind=TY_HTTPRESPONSE;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"XmlNode")==0)
	{
		out->kind=TY_XMLNODE;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}

	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"FileChannel")==0)
	{
		out->kind=TY_FILECHANNEL;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"FileWriter")==0)
	{
		out->kind=TY_FILEWRITER;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
		advance(p);
		return 1;
	}
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"Logger")==0)
	{
		out->kind=TY_LOGGER;
		out->class_name[0]='\0';
		out->elem=NULL;
		out->elem2=NULL;
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
	/* Arrow function type: (P1, P2, ...) -> R. The leading '(' only begins a
	   function type in type position (Breezy has no grouped types), so this is
	   unambiguous. The internal shape matches the FFI TY_FUNC: elem = return,
	   targs = parameters. */
	if (check(p,TOKEN_LPAREN))
	{
		memset(out,0,sizeof(*out));
		out->kind=TY_FUNC;
		advance(p);   /* '(' */
		if (!check(p,TOKEN_RPAREN))
		{
			do
			{
				TypeRef pt;
				memset(&pt,0,sizeof(pt));
				parse_type(p,&pt);
				typeref_add_targ(out,pt);
			}
			while (check(p,TOKEN_COMMA) && (advance(p),1));
		}

		expect(p,TOKEN_RPAREN);
		expect(p,TOKEN_ARROW);
		TypeRef ret;
		memset(&ret,0,sizeof(ret));
		parse_type(p,&ret);
		out->elem=typeref_box(ret);
		return 1;
	}

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

/* A '(' begins an arrow function type (vardecl) rather than a parenthesized/cast
   expression iff the token after the matching ')' is '->'. Scans a copy of the
   parser so the real token stream is untouched (Parser holds its lexer by value
   and token text points into the persistent source). */
static int looks_like_fn_type(Parser *p)
{
	if (p->cur.type != TOKEN_LPAREN)
	{
		return 0;
	}

	Parser t = *p;
	int depth = 0;
	for (;;)
	{
		if (t.cur.type == TOKEN_EOF)
		{
			return 0;
		}

		if (t.cur.type == TOKEN_LPAREN)
		{
			depth++;
		}
		else if (t.cur.type == TOKEN_RPAREN)
		{
			if (--depth == 0)
			{
				advance(&t);
				return t.cur.type == TOKEN_ARROW;
			}
		}

		advance(&t);
	}
}

static int starts_vardecl(Parser *p)
{
	if (check(p,TOKEN_LPAREN) && looks_like_fn_type(p))
	{
		return 1;
	}
	TypeKind k;
	if (scalar_type_kind(p->cur.type, &k) && k != TY_VOID)
	{
		return 1;
	}
	if (check(p,TOKEN_STRING))
	{
		return 1;
	}
	/* `map<...>` / `channel<...>` declarations start unambiguously with the keyword. */
	if (check(p,TOKEN_MAP) || check(p,TOKEN_CHANNEL))
	{
		return 1;
	}
	/* `Box<...>` / `Pair<...>` etc.: any IDENT followed by '<' begins a generic
	   variable declaration (Breezy has no bare comparison statements). */
	if (check(p,TOKEN_IDENT) && p->peek.type == TOKEN_LT)
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
	case TOKEN_PERCENT_ASSIGN:
		*op=TOKEN_PERCENT;
		return 1;
	case TOKEN_SHL_ASSIGN:
		*op=TOKEN_SHL;
		return 1;
	case TOKEN_SHR_ASSIGN:
		*op=TOKEN_SHR;
		return 1;
	case TOKEN_AMP_ASSIGN:
		*op=TOKEN_AMP;
		return 1;
	case TOKEN_PIPE_ASSIGN:
		*op=TOKEN_PIPE;
		return 1;
	case TOKEN_CARET_ASSIGN:
		*op=TOKEN_CARET;
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
			fprintf(stderr,"line %d: Invalid assignment target.\n",line);
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
			fprintf(stderr,"line %d: Invalid assignment target.\n",line);
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
	/* A bare identifier is an enum constant label (case RED:); the resolver maps
	   it to the operand enum's ordinal. */
	if (check(p,TOKEN_IDENT))
	{
		Token id=expect(p,TOKEN_IDENT);
		Expr *e=expr_new(EX_IDENT,line);
		strcpy(e->name,id.text);
		return e;
	}

	if (check(p,TOKEN_TRUE) || check(p,TOKEN_FALSE))
	{
		Expr *e=expr_new(EX_BOOL,line);
		e->int_val = check(p,TOKEN_TRUE) ? 1 : 0;
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

	/* A float literal is parsed only so the resolver can reject float/double
	   switches with a clear operand-level message (Java forbids them). */
	if (check(p,TOKEN_FLOAT_LIT))
	{
		Expr *e=expr_new(EX_FLOAT,line);
		e->float_val=strtod(p->cur.text,NULL);
		strcpy(e->int_suffix,p->cur.suffix);
		advance(p);
		return e;
	}

	int neg=0;
	if (check(p,TOKEN_MINUS))
	{
		neg=1;
		advance(p);
	}

	Token t=expect(p,TOKEN_INT_LIT);
	Expr *e=expr_new(EX_INT,line);
	e->int_val=parse_int_text(t.text);
	if (neg)
	{
		e->int_val=-e->int_val;
	}

	return e;
}

/* match (subject) { LABEL => stmt-or-block  ... [default => ...] }
   Lowered to the same ST_SWITCH node as `switch`, with two differences enforced
   later: every arm ends in an implicit break (no fallthrough), and the resolver
   checks the arms exhaustively cover the enum (or a default is present). */
static Stmt *parse_match(Parser *p)
{
	int line=p->cur.line;
	advance(p);                       /* Consume 'match'. */
	Stmt *s=stmt_new(ST_SWITCH,line);
	s->is_match=1;
	expect(p,TOKEN_LPAREN);
	s->cond=parse_expr(p);
	expect(p,TOKEN_RPAREN);
	expect(p,TOKEN_LBRACE);
	s->then_blk=block_new();
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		int cl=p->cur.line;
		if (check(p,TOKEN_DEFAULT))
		{
			advance(p);
			expect(p,TOKEN_FATARROW);
			block_push(s->then_blk,stmt_new(ST_DEFAULT,cl));
		}
		else
		{
			Stmt *c=stmt_new(ST_CASE,cl);
			c->value=parse_case_const(p);
			expect(p,TOKEN_FATARROW);
			block_push(s->then_blk,c);
		}

		if (check(p,TOKEN_LBRACE))
		{
			Block *grp=parse_block(p);
			for (int i=0; i<grp->count; i++)
			{
				block_push(s->then_blk,grp->stmts[i]);
			}
		}
		else
		{
			block_push(s->then_blk,parse_statement(p));
		}

		block_push(s->then_blk,stmt_new(ST_BREAK,cl));   /* Implicit per-arm break: no fallthrough. */
	}

	expect(p,TOKEN_RBRACE);
	return s;
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
				fprintf(stderr,"line %d: Statement before first case in switch.\n",p->cur.line);
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
	s->fe_val_type.kind=TY_VOID;
	s->fe_val_name[0]='\0';
	if (match(p,TOKEN_COMMA))
	{
		parse_type(p,&s->fe_val_type);
		Token vn=expect(p,TOKEN_IDENT);
		strcpy(s->fe_val_name,vn.text);
	}

	expect(p,TOKEN_IN);
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
			fprintf(stderr,"line %d: Invalid assignment target.\n",line);
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
			fprintf(stderr,"line %d: Invalid assignment target.\n",line);
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

static Stmt *parse_try(Parser *p)
{
	int line=p->cur.line;
	advance(p);                              /* Consume 'try'. */
	Stmt *s=stmt_new(ST_TRY,line);
	s->then_blk=parse_block(p);
	s->else_blk=block_new();
	do
	{
		int cline=p->cur.line;
		expect(p,TOKEN_CATCH);
		expect(p,TOKEN_LPAREN);
		Stmt *c=stmt_new(ST_CATCH,cline);
		parse_type(p,&c->decl_type);
		Token name=expect(p,TOKEN_IDENT);
		strcpy(c->decl_name,name.text);
		expect(p,TOKEN_RPAREN);
		c->then_blk=parse_block(p);
		block_push(s->else_blk,c);
	}
	while (check(p,TOKEN_CATCH));
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
	if (check(p,TOKEN_MATCH))
	{
		return parse_match(p);
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
	if (check(p,TOKEN_THROW))
	{
		int line=p->cur.line;
		advance(p);
		Stmt *s=stmt_new(ST_THROW,line);
		s->expr=parse_expr(p);
		expect(p,TOKEN_SEMICOLON);
		return s;
	}
	if (check(p,TOKEN_TRY))
	{
		return parse_try(p);
	}
	if (check(p,TOKEN_SPAWN))
	{
		int line=p->cur.line;
		advance(p);
		Stmt *s=stmt_new(ST_SPAWN,line);
		s->expr=parse_expr(p);
		if (s->expr->kind != EX_CALL)
		{
			fprintf(stderr,"line %d: Spawn expects a function call.\n", line);
			exit(1);
		}

		expect(p,TOKEN_SEMICOLON);
		return s;
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

/* Parse one parameter — its type, name, and an optional "= <literal>" default
   value (literal constants only). A default is substituted when the argument is
   omitted at a call site (see fill_default_args in the resolver). */
static void parse_one_param(Parser *p, Param *pm)
{
	parse_type(p,&pm->type);
	if (check(p,TOKEN_LPAREN))
	{
		/* C function-pointer type: ret(paramtype, ...). parse_type put the return
		   type in pm->type; wrap it into a TY_FUNC carrying the parameter types.
		   Unambiguous: a '(' immediately after a base type (before the parameter
		   name) only occurs for a function type. */
		TypeRef ret = pm->type;
		TypeRef fn;
		memset(&fn,0,sizeof(fn));
		fn.kind = TY_FUNC;
		fn.elem = typeref_box(ret);
		advance(p);   /* '(' */
		if (!check(p,TOKEN_RPAREN))
		{
			do
			{
				TypeRef pt;
				memset(&pt,0,sizeof(pt));
				parse_type(p,&pt);
				typeref_add_targ(&fn,pt);
			}
			while (match(p,TOKEN_COMMA));
		}
		expect(p,TOKEN_RPAREN);
		pm->type = fn;
	}
	Token pn=expect(p,TOKEN_IDENT);
	strcpy(pm->name,pn.text);
	if (match(p,TOKEN_ASSIGN))
	{
		Expr *d=parse_primary(p);
		if (d->kind!=EX_INT && d->kind!=EX_FLOAT && d->kind!=EX_BOOL && d->kind!=EX_STR)
		{
			fprintf(stderr,"line %d: A default parameter value must be a literal constant.\n",d->line);
			exit(1);
		}

		pm->def=d;
	}
}

static Func *parse_extern(Parser *p)
{
	expect(p,TOKEN_EXTERN);
	Func *f=func_new();
	f->is_extern=1;
	if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,"dynamic")==0)
	{
		advance(p);
		f->is_dynamic=1;
	}
	if (match(p,TOKEN_BLOCKING))
	{
		f->is_blocking=1;
	}

	parse_type(p,&f->ret_type);
	Token name=expect(p,TOKEN_IDENT);
	strcpy(f->name,name.text);
	expect(p,TOKEN_LPAREN);
	if (!check(p,TOKEN_RPAREN))
	{
		do
		{
			if (check(p,TOKEN_DOT))
			{
				/* Trailing `...` (three TOKEN_DOT): a variadic C function. Only valid
				   as the final entry in an extern parameter list. */
				expect(p,TOKEN_DOT);
				expect(p,TOKEN_DOT);
				expect(p,TOKEN_DOT);
				f->is_variadic=1;
				break;
			}

			Param *pm=func_add_param(f);
			parse_one_param(p,pm);
		}
		while (match(p,TOKEN_COMMA));
	}
	expect(p,TOKEN_RPAREN);
	expect(p,TOKEN_SEMICOLON);   /* No body. */
	f->body=NULL;
	return f;
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
			Param *pm=func_add_param(f);
			parse_one_param(p,pm);
		}
		while (match(p,TOKEN_COMMA));
	}
	expect(p,TOKEN_RPAREN);
	f->body=parse_block(p);
	return f;
}

static InterfaceDecl *parse_interface(Parser *p)
{
	expect(p,TOKEN_INTERFACE);
	InterfaceDecl *itf=interface_new();
	Token name=expect(p,TOKEN_IDENT);
	strcpy(itf->name,name.text);
	expect(p,TOKEN_LBRACE);
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		Func *m=func_new();
		parse_type(p,&m->ret_type);
		Token mn=expect(p,TOKEN_IDENT);
		strcpy(m->name,mn.text);
		expect(p,TOKEN_LPAREN);
		if (!check(p,TOKEN_RPAREN))
		{
			do
			{

				Param *pm=func_add_param(m);
				parse_one_param(p,pm);
			}
			while (match(p,TOKEN_COMMA));
		}

		expect(p,TOKEN_RPAREN);
		expect(p,TOKEN_SEMICOLON);   /* Signature only, no body. */
		m->body=NULL;
		itf->methods=grow_ensure(itf->methods,itf->method_count,&itf->methods_cap,sizeof(*itf->methods));
		itf->methods[itf->method_count++]=m;
	}

	expect(p,TOKEN_RBRACE);
	return itf;
}

static ClassDecl *parse_class(Parser *p)
{
	advance(p);
	ClassDecl *c=class_new();
	Token name=expect(p,TOKEN_IDENT);
	strcpy(c->name,name.text);
	if (match(p,TOKEN_LT))
	{
		do
		{
			c->type_params       = grow_ensure(c->type_params,       c->type_param_count, &c->type_param_cap,        sizeof(*c->type_params));
			c->type_param_bounds = grow_ensure(c->type_param_bounds, c->type_param_count, &c->type_param_bounds_cap, sizeof(*c->type_param_bounds));
			c->type_param_bounds[c->type_param_count][0]='\0';   /* Default: no bound. */
			Token tp=expect(p,TOKEN_IDENT);
			strcpy(c->type_params[c->type_param_count],tp.text);
			if (match(p,TOKEN_COLON))
			{
				Token b=expect(p,TOKEN_IDENT);
				strcpy(c->type_param_bounds[c->type_param_count],b.text);
			}

			c->type_param_count++;
		}
		while (match(p,TOKEN_COMMA));
		expect_gt(p);
	}

	if (match(p,TOKEN_EXTENDS))
	{
		Token par=expect(p,TOKEN_IDENT);
		strcpy(c->parent_name,par.text);
		c->has_parent=1;
	}

	if (match(p,TOKEN_IMPLEMENTS))
	{
		do
		{
			Token in=expect(p,TOKEN_IDENT);
			c->implements=grow_ensure(c->implements,c->implements_count,&c->implements_cap,sizeof(*c->implements));
			strcpy(c->implements[c->implements_count++],in.text);
		}
		while (match(p,TOKEN_COMMA));
	}

	expect(p,TOKEN_LBRACE);
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		int member_static = match(p,TOKEN_STATIC);   /* `static` field/method modifier. */
		/* Constructor: the class name immediately followed by '(' (no return type). */
		if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,c->name)==0 && p->peek.type==TOKEN_LPAREN)
		{
			advance(p);                 /* Class name. */
			Func *f=func_new();
			f->ret_type.kind=TY_VOID;
			strcpy(f->name,c->name);
			expect(p,TOKEN_LPAREN);
			if (!check(p,TOKEN_RPAREN))
			{
				do
				{
					Param *pm=func_add_param(f);
					parse_one_param(p,pm);
				}
				while (match(p,TOKEN_COMMA));
			}

			expect(p,TOKEN_RPAREN);
			f->body=parse_block(p);
			if (c->ctor_count>=8)
			{
				fprintf(stderr,"Too many constructors.\n");
				exit(1);
			}
			class_add_ctor(c,f);
			c->ctor=c->ctors[0];   /* Legacy alias. */
			continue;
		}

		TypeRef ty;
		if (!parse_type(p,&ty))
		{
			fprintf(stderr,"line %d: Expected member type.\n",p->cur.line);
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
					Param *pm=func_add_param(f);
					parse_one_param(p,pm);
				}
				while (match(p,TOKEN_COMMA));
			}
			expect(p,TOKEN_RPAREN);
			f->body=parse_block(p);
			f->is_static=member_static;
			class_add_method(c,f);
		}
		else
		{
			Expr *init=NULL;
			if (match(p,TOKEN_ASSIGN))
			{
				init=parse_expr(p);
			}

			expect(p,TOKEN_SEMICOLON);
			Field *fl=class_add_field(c);
			fl->type=ty;
			strcpy(fl->name,mname.text);
			fl->is_static=member_static;
			fl->init=init;
		}
	}
	expect(p,TOKEN_RBRACE);
	return c;
}

/* True when the token stream after a variant's '(' opens a typed parameter
   (`double r`, `Tree left`, `List<int> xs`, `int[] a`) rather than a value
   argument (`255`, `FOO`). Distinguishes a payload variant `Circle(double r)`
   from a singleton-constant argument list `RED(255, 0, 0)`. */
static int variant_is_payload(Parser *p)
{
	TypeKind k;
	if (scalar_type_kind(p->cur.type,&k))
	{
		return 1;
	}

	if (check(p,TOKEN_STRING) || check(p,TOKEN_MAP) || check(p,TOKEN_CHANNEL))
	{
		return 1;
	}

	if (check(p,TOKEN_IDENT)
			&& (p->peek.type==TOKEN_IDENT || p->peek.type==TOKEN_LBRACKET || p->peek.type==TOKEN_LT))
	{
		return 1;
	}

	return 0;
}

/* enum Name [implements I, J] { CONST [ (args) ] [ { overrides } ] (, CONST)*
   [ ; shared-fields-and-methods ] }. Modeled on parse_class for the members. */
static EnumDecl *parse_enum(Parser *p)
{
	advance(p);                 /* 'enum' */
	EnumDecl *e=enum_new();
	Token name=expect(p,TOKEN_IDENT);
	strcpy(e->name,name.text);
	if (check(p,TOKEN_EXTENDS))
	{
		fprintf(stderr,"line %d: Enums may not extend a class.\n",p->cur.line);
		exit(1);
	}

	if (match(p,TOKEN_IMPLEMENTS))
	{
		do
		{
			Token in=expect(p,TOKEN_IDENT);
			e->implements=grow_ensure(e->implements,e->implements_count,&e->implements_cap,sizeof(*e->implements));
			strcpy(e->implements[e->implements_count++],in.text);
		}
		while (match(p,TOKEN_COMMA));
	}

	expect(p,TOKEN_LBRACE);
	/* Constants: IDENT [ (args) ] [ { method overrides } ], comma-separated. */
	if (check(p,TOKEN_IDENT))
	{
		do
		{
			e->constants=grow_ensure(e->constants,e->constant_count,&e->constants_cap,sizeof(*e->constants));
			EnumConstant *c=&e->constants[e->constant_count];
			Token cn=expect(p,TOKEN_IDENT);
			strcpy(c->name,cn.text);
			if (match(p,TOKEN_LPAREN))
			{
				if (!check(p,TOKEN_RPAREN))
				{
					if (variant_is_payload(p))
					{
						/* Payload variant: typed fields become a constructible
						   subclass (a sum-type case). */
						do
						{
							if (c->payload_count>=8)
							{
								fprintf(stderr,"Too many variant fields.\n");
								exit(1);
							}
							Param pm;
							memset(&pm,0,sizeof(pm));
							parse_one_param(p,&pm);
							Field *fld=&c->payload[c->payload_count++];
							fld->type=pm.type;
							strcpy(fld->name,pm.name);
						}
						while (match(p,TOKEN_COMMA));
					}
					else
					{
						do
						{
							if (c->arg_count>=8)
							{
								fprintf(stderr,"Too many constant arguments.\n");
								exit(1);
							}
							c->args[c->arg_count++]=parse_expr(p);
						}
						while (match(p,TOKEN_COMMA));
					}
				}

				expect(p,TOKEN_RPAREN);
			}

			if (check(p,TOKEN_LBRACE))
			{
				advance(p);             /* '{' of the per-constant body. */
				while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
				{
					if (c->override_count>=8)
					{
						fprintf(stderr,"Too many constant overrides.\n");
						exit(1);
					}
					TypeRef rt;
					parse_type(p,&rt);
					Func *f=func_new();
					f->ret_type=rt;
					Token mn=expect(p,TOKEN_IDENT);
					strcpy(f->name,mn.text);
					expect(p,TOKEN_LPAREN);
					if (!check(p,TOKEN_RPAREN))
					{
						do
						{
							Param *pm=func_add_param(f);
							parse_one_param(p,pm);
						}
						while (match(p,TOKEN_COMMA));
					}

					expect(p,TOKEN_RPAREN);
					f->body=parse_block(p);
					c->overrides[c->override_count++]=f;
				}

				expect(p,TOKEN_RBRACE);
			}

			e->constant_count++;
		}
		while (match(p,TOKEN_COMMA));
	}

	match(p,TOKEN_SEMICOLON);   /* Optional ';' before shared members. */
	while (!check(p,TOKEN_RBRACE) && !check(p,TOKEN_EOF))
	{
		/* Constructor: IDENT == enum name followed by '('. */
		if (check(p,TOKEN_IDENT) && strcmp(p->cur.text,e->name)==0 && p->peek.type==TOKEN_LPAREN)
		{
			advance(p);
			Func *f=func_new();
			f->ret_type.kind=TY_VOID;
			strcpy(f->name,e->name);
			expect(p,TOKEN_LPAREN);
			if (!check(p,TOKEN_RPAREN))
			{
				do
				{
					Param *pm=func_add_param(f);
					parse_one_param(p,pm);
				}
				while (match(p,TOKEN_COMMA));
			}

			expect(p,TOKEN_RPAREN);
			f->body=parse_block(p);
			e->ctor=f;
			continue;
		}

		TypeRef ty;
		if (!parse_type(p,&ty))
		{
			fprintf(stderr,"line %d: Expected member type.\n",p->cur.line);
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
					Param *pm=func_add_param(f);
					parse_one_param(p,pm);
				}
				while (match(p,TOKEN_COMMA));
			}

			expect(p,TOKEN_RPAREN);
			f->body=parse_block(p);
			e->methods=grow_ensure(e->methods,e->method_count,&e->methods_cap,sizeof(*e->methods));
			e->methods[e->method_count++]=f;
		}
		else
		{
			expect(p,TOKEN_SEMICOLON);
			e->fields=grow_ensure(e->fields,e->field_count,&e->fields_cap,sizeof(*e->fields));
			e->fields[e->field_count].type=ty;
			strcpy(e->fields[e->field_count].name,mname.text);
			e->field_count++;
		}
	}

	expect(p,TOKEN_RBRACE);
	return e;
}

Unit *parse_unit(Parser *p)
{
	Unit *u=unit_new();
	while (!check(p,TOKEN_EOF))
	{
		if (check(p,TOKEN_INTERFACE))
		{
			u->interfaces=grow_ensure(u->interfaces,u->interface_count,&u->interfaces_cap,sizeof(*u->interfaces));
			u->interfaces[u->interface_count++]=parse_interface(p);
		}
		else if (check(p,TOKEN_EXTERN))
		{
			u->funcs=grow_ensure(u->funcs,u->func_count,&u->funcs_cap,sizeof(*u->funcs));
			u->funcs[u->func_count++]=parse_extern(p);
		}
		else if (check(p,TOKEN_STATIC) && p->peek.type==TOKEN_CLASS)
		{
			advance(p);                 /* 'static'; parse_class consumes 'class'. */
			ClassDecl *c=parse_class(p);
			c->is_static=1;
			unit_add_class(u,c);
		}
		else if (check(p,TOKEN_CLASS))
		{
			unit_add_class(u,parse_class(p));
		}
		else if (check(p,TOKEN_RECORD))
		{
			ClassDecl *c=parse_class(p);   /* Same body grammar; parse_class consumes the leading keyword. */
			c->is_record=1;
			unit_add_class(u,c);
		}
		else if (check(p,TOKEN_ENUM))
		{
			u->enums=grow_ensure(u->enums,u->enum_count,&u->enums_cap,sizeof(*u->enums));
			u->enums[u->enum_count++]=parse_enum(p);
		}
		else
		{
			u->funcs=grow_ensure(u->funcs,u->func_count,&u->funcs_cap,sizeof(*u->funcs));
			u->funcs[u->func_count++]=parse_function(p);
		}
	}
	return u;
}
