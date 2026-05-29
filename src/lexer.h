#ifndef LEXER_H
#define LEXER_H

typedef enum {
    TOKEN_EOF = 0, TOKEN_IDENT, TOKEN_INT_LIT,
    TOKEN_VOID, TOKEN_INT, TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_RETURN,
    TOKEN_CLASS, TOKEN_EXTENDS, TOKEN_NEW, TOKEN_THIS,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
    TOKEN_ASSIGN, TOKEN_EQ, TOKEN_NEQ, TOKEN_LT, TOKEN_GT, TOKEN_LTE, TOKEN_GTE,
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_SEMICOLON, TOKEN_COMMA, TOKEN_DOT
} TokenType;

typedef struct { TokenType type; char text[256]; int line; } Token;
typedef struct { const char *src; int pos; int line; } Lexer;

void        lexer_init(Lexer *l, const char *src);
Token       lexer_next(Lexer *l);
const char *token_type_name(TokenType t);

#endif
