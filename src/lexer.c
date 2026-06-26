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
	{"switch",TOKEN_SWITCH},{"case",TOKEN_CASE},{"default",TOKEN_DEFAULT},{"match",TOKEN_MATCH},{"select",TOKEN_SELECT},{"asm",TOKEN_ASM},{"class",TOKEN_CLASS},
	{"extends",TOKEN_EXTENDS},{"new",TOKEN_NEW},{"this",TOKEN_THIS},{"extern",TOKEN_EXTERN},{"blocking",TOKEN_BLOCKING},
	{"record",TOKEN_RECORD},
	{"interface",TOKEN_INTERFACE},{"implements",TOKEN_IMPLEMENTS},{"enum",TOKEN_ENUM},{"static",TOKEN_STATIC},
	{"byte",TOKEN_BYTE},{"short",TOKEN_SHORT},{"long",TOKEN_LONG},
	{"ubyte",TOKEN_UBYTE},{"ushort",TOKEN_USHORT},{"uint",TOKEN_UINT},
	{"ulong",TOKEN_ULONG},{"bool",TOKEN_BOOL},
	{"float",TOKEN_FLOAT},{"double",TOKEN_DOUBLE},{"f64x2",TOKEN_F64X2},{"f32x4",TOKEN_F32X4},{"i32x4",TOKEN_I32X4},{"f64x4",TOKEN_F64X4},{"f32x8",TOKEN_F32X8},{"i32x8",TOKEN_I32X8},{"string",TOKEN_STRING},
	{"map",TOKEN_MAP},{"channel",TOKEN_CHANNEL},
	{"and",TOKEN_AND},{"or",TOKEN_OR},{"xor",TOKEN_XOR},{"not",TOKEN_NOT},
	{"true",TOKEN_TRUE},{"false",TOKEN_FALSE},{"null",TOKEN_NULL},{NULL,0}
};

void lexer_init(Lexer *l, const char *src)
{
	l->src = src;
	l->pos = 0;
	l->line = 1;
	l->col = 1;
	l->file = "<source>";
}

static int g_json_diag;   /* When set, diagnostics are emitted as one JSON object on stdout. */

void lexer_diag_json(int on)
{
	g_json_diag = on;
}

/* Print s to stdout with JSON string escaping for the few chars that need it. */
static void json_str(const char *s)
{
	for (; s && *s; s++)
	{
		switch (*s)
		{
		case '"':
			fputs("\\\"", stdout);
			break;
		case '\\':
			fputs("\\\\", stdout);
			break;
		case '\n':
			fputs("\\n", stdout);
			break;
		case '\t':
			fputs("\\t", stdout);
			break;
		default:
			fputc(*s, stdout);
			break;
		}
	}
}

void lexer_diag(const char *src, const char *file, int line, int col,
				const char *msg, const char *arg)
{
	if (!file)
	{
		file = "<source>";
	}

	if (g_json_diag)
	{
		/* endCol = the 1-based column just past the offending token, so an editor can
		   draw the squiggle across the whole token rather than one character. The token
		   text is not passed in, so derive its extent from the source: an identifier /
		   number word at `col` spans to its end; punctuation stays one character. */
		int endcol = col > 0 ? col + 1 : 0;
		if (src && col > 0)
		{
			const char *lp = src;
			for (int ln = 1; ln < line && *lp; lp++)
			{
				if (*lp == '\n')
				{
					ln++;
				}
			}
			const char *lend = lp;
			while (*lend && *lend != '\n')
			{
				lend++;
			}
			if (col - 1 < (int)(lend - lp))
			{
				const char *t = lp + (col - 1);
				if ((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z') || *t == '_')
				{
					const char *e = t;
					while (e < lend && ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z')
										|| (*e >= '0' && *e <= '9') || *e == '_'))
					{
						e++;
					}
					endcol = col + (int)(e - t);
				}
			}
		}

		fputs("{\"file\":\"", stdout);
		json_str(file);
		fprintf(stdout, "\",\"line\":%d,\"col\":%d,\"endCol\":%d,\"message\":\"", line, col, endcol);
		json_str(msg);
		json_str(arg);
		fputs("\"}\n", stdout);
		exit(1);
	}

	if (col > 0)
	{
		fprintf(stderr, "%s:%d:%d: %s%s\n", file, line, col, msg, arg ? arg : "");
	}
	else
	{
		fprintf(stderr, "%s:%d: %s%s\n", file, line, msg, arg ? arg : "");
	}

	/* Without a source buffer there is no line to echo. */
	if (!src)
	{
		exit(1);
	}

	/* Find the start of the offending line and print it with a caret. */
	const char *p = src;
	for (int ln = 1; ln < line && *p; p++)
	{
		if (*p == '\n')
		{
			ln++;
		}
	}

	const char *end = p;
	while (*end && *end != '\n')
	{
		end++;
	}

	fprintf(stderr, "  %.*s\n", (int)(end - p), p);
	if (col > 0)
	{
		fputs("  ", stderr);
		for (int i = 1; i < col && p[i - 1]; i++)
		{
			/* Preserve tabs so the caret lines up under the source. */
			fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
		}

		fprintf(stderr, "^\n");
	}

	exit(1);
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
		l->col = 1;
	}
	else
	{
		l->col++;
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
	else if (peek_ch(l) == 'd' || peek_ch(l) == 'D')
	{
		next_ch(l);
		t->suffix[0] = 'd';   /* Explicit double; same type a bare float literal already gets. */
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
			/* Block comments nest: track depth so an inner block comment does not
			   close the outer one early. An unterminated comment runs to EOF, the
			   same as a single-level one. */
			int depth = 1;
			while (peek_ch(l) && depth > 0)
			{
				if (peek_ch(l) == '/' && l->src[l->pos+1] == '*')
				{
					next_ch(l);   /* Consume '/'. */
					next_ch(l);   /* Consume '*'. */
					depth++;
				}
				else if (peek_ch(l) == '*' && l->src[l->pos+1] == '/')
				{
					next_ch(l);   /* Consume '*'. */
					next_ch(l);   /* Consume '/'. */
					depth--;
				}
				else
				{
					next_ch(l);
				}
			}
		}
		else
		{
			break;
		}
	}

	t.line = l->line;
	t.col = l->col;
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
		/* Radix-prefixed literals: 0x.. (hex) and 0b.. (binary). The whole token
		   text including the prefix is kept; the parser picks the conversion base. */
		if (c == '0' && (l->src[l->pos + 1] == 'x' || l->src[l->pos + 1] == 'X'))
		{
			t.text[i++] = next_ch(l);   /* The '0'. */
			t.text[i++] = next_ch(l);   /* The 'x'/'X'. */
			while (isxdigit((unsigned char)peek_ch(l)) && i < 255)
			{
				t.text[i++] = next_ch(l);
			}

			t.text[i] = '\0';
			t.suffix[0] = '\0';
			t.type = TOKEN_INT_LIT;
			return t;
		}

		if (c == '0' && (l->src[l->pos + 1] == 'b' || l->src[l->pos + 1] == 'B'))
		{
			t.text[i++] = next_ch(l);   /* The '0'. */
			t.text[i++] = next_ch(l);   /* The 'b'/'B'. */
			while ((peek_ch(l) == '0' || peek_ch(l) == '1') && i < 255)
			{
				t.text[i++] = next_ch(l);
			}

			t.text[i] = '\0';
			t.suffix[0] = '\0';
			t.type = TOKEN_INT_LIT;
			return t;
		}

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
				case 'r':
					d = '\r';
					break;
				case '\\':
					d = '\\';
					break;
				case '"':
					d = '"';
					break;
				default:
				{
					char esc[3] = { '\\', e, '\0' };
					lexer_diag(l->src, l->file, l->line, l->col - 1, "Bad string escape: ", esc);
				}
				}
			}

			t.text[i++] = d;
		}

		if (peek_ch(l) != '"')
		{
			lexer_diag(l->src, l->file, t.line, t.col, "Unterminated string literal.", NULL);
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
		else if (peek_ch(l)=='>')
		{
			next_ch(l);
			t.type=TOKEN_ARROW;             /* -> function-type separator. */
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
	case '%':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_PERCENT_ASSIGN;
		}
		else
		{
			t.type=TOKEN_PERCENT;
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
	case '?':
		t.type = TOKEN_QUESTION;
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
		if (peek_ch(l)=='<')
		{
			next_ch(l);
			if (peek_ch(l)=='=')
			{
				next_ch(l);
				t.type=TOKEN_SHL_ASSIGN;
			}
			else
			{
				t.type=TOKEN_SHL;
			}
		}
		else if (peek_ch(l)=='>')
		{
			next_ch(l);
			t.type=TOKEN_NEQ;   /* '<>' (less-or-greater) is a synonym for '!='. */
		}
		else if (peek_ch(l)=='=')
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
		if (peek_ch(l)=='>')
		{
			next_ch(l);
			if (peek_ch(l)=='=')
			{
				next_ch(l);
				t.type=TOKEN_SHR_ASSIGN;
			}
			else
			{
				t.type=TOKEN_SHR;
			}
		}
		else if (peek_ch(l)=='=')
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
		else if (peek_ch(l)=='>')
		{
			next_ch(l);
			t.type=TOKEN_FATARROW;          /* => lambda arrow. */
		}
		else
		{
			t.type=TOKEN_ASSIGN;
		}

		return t;
	case '&':
		if (peek_ch(l)=='&')
		{
			next_ch(l);
			t.type=TOKEN_AND;
		}
		else if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_AMP_ASSIGN;
		}
		else
		{
			t.type=TOKEN_AMP;
		}

		return t;
	case '|':
		if (peek_ch(l)=='|')
		{
			next_ch(l);
			t.type=TOKEN_OR;
		}
		else if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_PIPE_ASSIGN;
		}
		else
		{
			t.type=TOKEN_PIPE;
		}

		return t;
	case '^':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_CARET_ASSIGN;
		}
		else
		{
			t.type=TOKEN_CARET;
		}

		return t;
	case '~':
		t.type=TOKEN_TILDE;
		return t;
	case '!':
		if (peek_ch(l)=='=')
		{
			next_ch(l);
			t.type=TOKEN_NEQ;
		}
		else
		{
			t.type=TOKEN_NOT;
		}

		return t;
	default:
	{
		char ch[2] = { c, '\0' };
		lexer_diag(l->src, l->file, l->line, l->col, "Unexpected char: ", ch);
	}
	}

	return t;   /* Unreachable: lexer_diag exits. */
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
	case TOKEN_MATCH:
		return "match";
	case TOKEN_SELECT:
		return "select";
	case TOKEN_ASM:
		return "asm";
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
	case TOKEN_RECORD:
		return "record";
	case TOKEN_INTERFACE:
		return "interface";
	case TOKEN_IMPLEMENTS:
		return "implements";
	case TOKEN_ENUM:
		return "enum";
	case TOKEN_STATIC:
		return "static";
	case TOKEN_EXTERN:
		return "extern";
	case TOKEN_BLOCKING:
		return "blocking";
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
	case TOKEN_BOOL:
		return "bool";
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
	case TOKEN_NULL:
		return "null";
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
	case TOKEN_PERCENT_ASSIGN:
		return "%=";
	case TOKEN_SHL_ASSIGN:
		return "<<=";
	case TOKEN_SHR_ASSIGN:
		return ">>=";
	case TOKEN_STAR:
		return "*";
	case TOKEN_SLASH:
		return "/";
	case TOKEN_PERCENT:
		return "%";
	case TOKEN_SHL:
		return "<<";
	case TOKEN_SHR:
		return ">>";
	case TOKEN_AMP:
		return "&";
	case TOKEN_PIPE:
		return "|";
	case TOKEN_CARET:
		return "^";
	case TOKEN_TILDE:
		return "~";
	case TOKEN_AMP_ASSIGN:
		return "&=";
	case TOKEN_PIPE_ASSIGN:
		return "|=";
	case TOKEN_CARET_ASSIGN:
		return "^=";
	case TOKEN_AND:
		return "and";
	case TOKEN_OR:
		return "or";
	case TOKEN_XOR:
		return "xor";
	case TOKEN_NOT:
		return "not";
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
