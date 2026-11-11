#ifndef MINIDB_LEXER_H
#define MINIDB_LEXER_H

#include "common.h"

typedef enum {
    TK_EOF,
    TK_IDENT,  /* text = lower-cased identifier or keyword */
    TK_INT,    /* ival */
    TK_STRING, /* text = unescaped contents */
    TK_LPAREN,
    TK_RPAREN,
    TK_COMMA,
    TK_SEMI,
    TK_STAR,
    TK_EQ,
    TK_NE,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE,
    TK_ERROR /* text = error message */
} TokType;

typedef struct {
    TokType type;
    char text[MAX_STR + 1];
    int64_t ival;
    int line, col; /* 1-based position of the token's first character */
} Token;

typedef struct {
    const char *src;
    size_t pos;
    int line, col;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);

#endif
