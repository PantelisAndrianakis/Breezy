#include "lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const struct
{
	const char *kw;
	TokenType tt;
} KEYWORDS[] =
{
	{"void",TOKEN_VOID},{"int",TOKEN_INT},{"if",TOKEN_IF},{"else",TOKEN_ELSE},
	{"while",TOKEN_WHILE},{"foreach",TOKEN_FOREACH},{"for",TOKEN_FOR},{"in",TOKEN_IN},{"return",TOKEN_RETURN},{"throw",TOKEN_THROW},{"try",TOKEN_TRY},{"catch",TOKEN_CATCH},{"spawn",TOKEN_SPAWN},
	{"break",TOKEN_BREAK},{"continue",TOKEN_CONTINUE},
	{"switch",TOKEN_SWITCH},{"case",TOKEN_CASE},{"default",TOKEN_DEFAULT},{"class",TOKEN_CLASS},
	{"extends",TOKEN_EXTENDS},{"new",TOKEN_NEW},{"this",TOKEN_THIS},
	{"byte",TOKEN_BYTE},{"short",TOKEN_SHORT},{"long",TOKEN_LONG},
	{"ubyte",TOKEN_UBYTE},{"ushort",TOKEN_USHORT},{"uint",TOKEN_UINT},
	{"ulong",TOKEN_ULONG},{"boolean",TOKEN_BOOLEAN},
	{"float",TOKEN_FLOAT},{"double",TOKEN_DOUBLE},{"string",TOKEN_STRING},
	{"map",TOKEN_MAP},{"channel",TOKEN_CHANNEL},
	{"true",TOKEN_TRUE},{"false",TOKEN_FALSE},{NULL,0}
};

void lexer_init(Lexer *l, const char *src)
{
	l->src = src;
	l->pos = 0;
	l->line = 1;
}

static char peek_ch(Lexer *l)
{
	return l->src[l->pos];
}

static char next_ch(Lexer *l)
{
	char c = l->src[l->pos++];
	if (c == '\n')
	{
		l->line++;
	}

	return c;
}

/* Finish a float literal whose mantissa already occupies t->text[0..i): scan an
   optional exponent (e[+/-]?digits) and an optional 'f' suffix, then mark it. */
static Token finish_float(Lexer *l, Token *t, int i)
{
	if (peek_ch(l) == 'e' || peek_ch(l) == 'E')
	{
		t->text[i++] = next_ch(l);
		if (peek_ch(l) == '+' || peek_ch(l) == '-')
		{
			t->text[i++] = next_ch(l);
		}

		while (isdigit((unsigned char)peek_ch(l)) && i < 255)
		{
			t->text[i++] = next_ch(l);
		}
	}

	t->text[i] = '\0';
	if (peek_ch(l) == 'f' || peek_ch(l) == 'F')
	{
		next_ch(l);
		t->suffix[0] = 'f';
		t->suffix[1] = '\0';
	}

	t->type = TOKEN_FLOAT_LIT;
	return *t;
}

Token lexer_next(Lexer *l)
{
	Token t;
	t.text[0] = '\0';
	t.suffix[0] = '\0';
	for (;;)
	{
		while (peek_ch(l) && isspace((unsigned char)peek_ch(l)))
		{
			next_ch(l);
		}

		if (peek_ch(l) == '/' && l->src[l->pos+1] == '/')
		{
			while (peek_ch(l) && peek_ch(l) != '\n')
			{
				next_ch(l);
			}
		}
		else if (peek_ch(l) == '/' && l->src[l->pos+1] == '*')
		{
			next_ch(l);   /* Consume '/'. */
			next_ch(l);   /* Consume '*'. */
			while (peek_ch(l) && !(peek_ch(l) == '*' && l->src[l->pos+1] == '/'))
			{
				next_ch(l);
			}

			if (peek_ch(l))
			{
				next_ch(l);   /* Consume '*'. */
				next_ch(l);   /* Consume '/'. */
			}
		}
		else
		{
			break;
		}
	}

	t.line = l->line;
	char c = peek_ch(l);
	if (!c)
	{
		t.type = TOKEN_EOF;
		return t;
	}

	if (isalpha((unsigned char)c) || c == '_')
	{
		int i = 0;
		while ((isalnum((unsigned char)peek_ch(l)) || peek_ch(l) == '_') && i < 255)
		{
			t.text[i++] = next_ch(l);
		}

		t.text[i] = '\0';
		t.type = TOKEN_IDENT;
		for (int k = 0; KEYWORDS[k].kw; k++)
		{
			if (strcmp(t.text, KEYWORDS[k].kw) == 0)
			{
				t.type = KEYWORDS[k].tt;
				break;
			}
		}

		return t;
	}

	if (isdigit((unsigned char)c))
	{
		int i = 0;
		while (isdigit((unsigned char)peek_ch(l)) && i < 255)
		{
			t.text[i++] = next_ch(l);
		}

		if (peek_ch(l) == '.' || peek_ch(l) == 'e' || peek_ch(l) == 'E')
		{
			if (peek_ch(l) == '.')
			{
				t.text[i++] = next_ch(l);
				while (isdigit((unsigned char)peek_ch(l)) && i < 255)
				{
					t.text[i++] = next_ch(l);
				}
			}

			return finish_float(l, &t, i);
		}

		t.text[i] = '\0';
		/* Capture a trailing run of width/sign suffix letters (u/U, l/L). */
		int s = 0;
		while ((peek_ch(l) == 'u' || peek_ch(l) == 'U'
				|| peek_ch(l) == 'l' || peek_ch(l) == 'L') && s < 3)
		{
			t.suffix[s++] = next_ch(l);
		}

		t.suffix[s] = '\0';
		t.type = TOKEN_INT_LIT;
		return t;
	}

	if (c == '"')
	{
		next_ch(l);                 /* Consume the opening quote. */
		int i = 0;
		while (peek_ch(l) && peek_ch(l) != '"' && i < 255)
		{
			char d = next_ch(l);
			if (d == '\\')
			{
				char e = next_ch(l);
				switch (e)
				{
				case 'n':
					d = '\n';
					break;
				case 't':
					d = '\t';
					break;
				case '\\':
					d = '\\';
					break;
				case '"':
					d = '"';
					break;
				default:
					fprintf(stderr, "line %d: Bad string escape '\\%c'\n", l->line, e);
					exit(1);
				}
			}

			t.text[i++] = d;
		}

		if (peek_ch(l) != '"')
		{
			fprintf(stderr, "line %d: Unterminated string literal.\n", l->line);
			exit(1);
		}

		next_ch(l);                 /* Consume the closing quote. */
		t.text[i] = '\0';
		t.type = TOKEN_STR_LIT;
		return t;
	}

	next_ch(l);
	t.text[0] = c;
	t.text[1] = '\0';
	switch (c)
	{
	case '+':
		if (peek_ch(l)=='+')
		{
			next_ch(l);
			t.type=TOKEN_PLUSPLUS;
		}
		else if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_PLUS_ASSIGN;
		}
		else
		{
			t.type=TOKEN_PLUS;
		}

		return t;
	case '-':
		if (peek_ch(l)=='-')
		{
			next_ch(l);
			t.type=TOKEN_MINUSMINUS;
		}
		else if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_MINUS_ASSIGN;
		}
		else
		{
			t.type=TOKEN_MINUS;
		}

		return t;
	case '*':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_STAR_ASSIGN;
		}
		else
		{
			t.type=TOKEN_STAR;
		}

		return t;
	case '/':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_SLASH_ASSIGN;
		}
		else
		{
			t.type=TOKEN_SLASH;
		}

		return t;
	case '(':
		t.type = TOKEN_LPAREN;
		return t;
	case ')':
		t.type = TOKEN_RPAREN;
		return t;
	case '{':
		t.type = TOKEN_LBRACE;
		return t;
	case '}':
		t.type = TOKEN_RBRACE;
		return t;
	case '[':
		t.type = TOKEN_LBRACKET;
		return t;
	case ']':
		t.type = TOKEN_RBRACKET;
		return t;
	case ';':
		t.type = TOKEN_SEMICOLON;
		return t;
	case ',':
		t.type = TOKEN_COMMA;
		return t;
	case ':':
		t.type = TOKEN_COLON;
		return t;
	case '.':
		if (isdigit((unsigned char)peek_ch(l)))
		{
			int i = 1;   /* t.text[0] already holds '.'. */
			while (isdigit((unsigned char)peek_ch(l)) && i < 255)
			{
				t.text[i++] = next_ch(l);
			}

			return finish_float(l, &t, i);
		}

		t.type = TOKEN_DOT;
		return t;
	case '<':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_LTE;
		}
		else
		{
			t.type=TOKEN_LT;
		}

		return t;
	case '>':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_GTE;
		}
		else
		{
			t.type=TOKEN_GT;
		}

		return t;
	case '=':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_EQ;
		}
		else
		{
			t.type=TOKEN_ASSIGN;
		}

		return t;
	case '!':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_NEQ;
		}
		else
		{
			fprintf(stderr,"line %d: Unexpected '!'\n",l->line);
			exit(1);
		}

		return t;
	default:
		fprintf(stderr,"line %d: Unexpected char '%c'\n",l->line,c);
		exit(1);
	}
}

const char *token_type_name(TokenType t)
{
	switch (t)
	{
	case TOKEN_EOF:
		return "EOF";
	case TOKEN_IDENT:
		return "IDENT";
	case TOKEN_INT_LIT:
		return "INT_LIT";
	case TOKEN_VOID:
		return "void";
	case TOKEN_INT:
		return "int";
	case TOKEN_IF:
		return "if";
	case TOKEN_ELSE:
		return "else";
	case TOKEN_WHILE:
		return "while";
	case TOKEN_FOREACH:
		return "foreach";
	case TOKEN_FOR:
		return "for";
	case TOKEN_IN:
		return "in";
	case TOKEN_BREAK:
		return "break";
	case TOKEN_CONTINUE:
		return "continue";
	case TOKEN_SWITCH:
		return "switch";
	case TOKEN_CASE:
		return "case";
	case TOKEN_DEFAULT:
		return "default";
	case TOKEN_RETURN:
		return "return";
	case TOKEN_THROW:
		return "throw";
	case TOKEN_TRY:
		return "try";
	case TOKEN_CATCH:
		return "catch";
	case TOKEN_SPAWN:
		return "spawn";
	case TOKEN_CLASS:
		return "class";
	case TOKEN_EXTENDS:
		return "extends";
	case TOKEN_NEW:
		return "new";
	case TOKEN_THIS:
		return "this";
	case TOKEN_BYTE:
		return "byte";
	case TOKEN_SHORT:
		return "short";
	case TOKEN_LONG:
		return "long";
	case TOKEN_UBYTE:
		return "ubyte";
	case TOKEN_USHORT:
		return "ushort";
	case TOKEN_UINT:
		return "uint";
	case TOKEN_ULONG:
		return "ulong";
	case TOKEN_BOOLEAN:
		return "boolean";
	case TOKEN_FLOAT:
		return "float";
	case TOKEN_DOUBLE:
		return "double";
	case TOKEN_FLOAT_LIT:
		return "FLOAT_LIT";
	case TOKEN_STR_LIT:
		return "STR_LIT";
	case TOKEN_STRING:
		return "string";
	case TOKEN_MAP:
		return "map";
	case TOKEN_CHANNEL:
		return "channel";
	case TOKEN_LBRACKET:
		return "[";
	case TOKEN_RBRACKET:
		return "]";
	case TOKEN_TRUE:
		return "true";
	case TOKEN_FALSE:
		return "false";
	case TOKEN_PLUS:
		return "+";
	case TOKEN_MINUS:
		return "-";
	case TOKEN_PLUSPLUS:
		return "++";
	case TOKEN_MINUSMINUS:
		return "--";
	case TOKEN_PLUS_ASSIGN:
		return "+=";
	case TOKEN_MINUS_ASSIGN:
		return "-=";
	case TOKEN_STAR_ASSIGN:
		return "*=";
	case TOKEN_SLASH_ASSIGN:
		return "/=";
	case TOKEN_STAR:
		return "*";
	case TOKEN_SLASH:
		return "/";
	case TOKEN_ASSIGN:
		return "=";
	case TOKEN_EQ:
		return "==";
	case TOKEN_NEQ:
		return "!=";
	case TOKEN_LT:
		return "<";
	case TOKEN_GT:
		return ">";
	case TOKEN_LTE:
		return "<=";
	case TOKEN_GTE:
		return ">=";
	case TOKEN_LPAREN:
		return "(";
	case TOKEN_RPAREN:
		return ")";
	case TOKEN_LBRACE:
		return "{";
	case TOKEN_RBRACE:
		return "}";
	case TOKEN_SEMICOLON:
		return ";";
	case TOKEN_COMMA:
		return ",";
	case TOKEN_DOT:
		return ".";
	case TOKEN_COLON:
		return ":";
	default:
		return "UNKNOWN";
	}
}
