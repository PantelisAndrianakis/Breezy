#ifndef LEXER_H
#define LEXER_H

typedef enum
{
	TOKEN_EOF = 0, TOKEN_IDENT, TOKEN_INT_LIT,
	TOKEN_VOID, TOKEN_INT, TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOREACH, TOKEN_FOR, TOKEN_RETURN,
	TOKEN_BREAK, TOKEN_CONTINUE,                         /* loop control */
	TOKEN_SWITCH, TOKEN_CASE, TOKEN_DEFAULT,             /* switch */
	TOKEN_CLASS, TOKEN_EXTENDS, TOKEN_NEW, TOKEN_THIS,
	TOKEN_BYTE, TOKEN_SHORT, TOKEN_LONG,                 /* signed width keywords  */
	TOKEN_UBYTE, TOKEN_USHORT, TOKEN_UINT, TOKEN_ULONG,  /* unsigned width keywords */
	TOKEN_BOOLEAN, TOKEN_TRUE, TOKEN_FALSE,              /* boolean type + literals */
	TOKEN_FLOAT, TOKEN_DOUBLE, TOKEN_FLOAT_LIT,          /* float/double types + literal */
	TOKEN_STR_LIT,                                       /* "..." string literal */
	TOKEN_STRING,                                        /* `string` type keyword */
	TOKEN_MAP,                                           /* `map` type keyword */
	TOKEN_LBRACKET, TOKEN_RBRACKET,                      /* [ ] */
	TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
	TOKEN_PLUSPLUS, TOKEN_MINUSMINUS,                    /* ++ -- */
	TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN, TOKEN_STAR_ASSIGN, TOKEN_SLASH_ASSIGN,   /* += -= *= /= */
	TOKEN_ASSIGN, TOKEN_EQ, TOKEN_NEQ, TOKEN_LT, TOKEN_GT, TOKEN_LTE, TOKEN_GTE,
	TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE,
	TOKEN_SEMICOLON, TOKEN_COMMA, TOKEN_DOT, TOKEN_COLON
} TokenType;

typedef struct
{
	TokenType type;
	char text[256];
	char suffix[4];   /* TOKEN_INT_LIT: literal suffix ("", "L", "u", "uL", "Lu"). */
	int line;
} Token;
typedef struct
{
	const char *src;
	int pos;
	int line;
} Lexer;

void        lexer_init(Lexer *l, const char *src);
Token       lexer_next(Lexer *l);
const char *token_type_name(TokenType t);

#endif
