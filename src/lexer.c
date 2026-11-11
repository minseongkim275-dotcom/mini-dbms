#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
}

static int peekc(const Lexer *lx) { return (unsigned char)lx->src[lx->pos]; }
static int peek2(const Lexer *lx) {
    return lx->src[lx->pos] ? (unsigned char)lx->src[lx->pos + 1] : 0;
}

static int nextc(Lexer *lx) {
    int c = peekc(lx);
    if (!c) return 0;
    lx->pos++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static bool is_ident_start(int c) { return isalpha(c) || c == '_'; }
static bool is_ident_char(int c) { return isalnum(c) || c == '_'; }

static void skip_space_and_comments(Lexer *lx) {
    for (;;) {
        int c = peekc(lx);
        if (isspace(c)) {
            nextc(lx);
        } else if (c == '-' && peek2(lx) == '-') {
            while (peekc(lx) && peekc(lx) != '\n') nextc(lx);
        } else {
            return;
        }
    }
}

static Token error_tok(Token t, const char *msg) {
    t.type = TK_ERROR;
    snprintf(t.text, sizeof t.text, "%s", msg);
    return t;
}

static Token lex_ident(Lexer *lx, Token t) {
    size_t n = 0;
    while (is_ident_char(peekc(lx))) {
        int c = nextc(lx);
        if (n < MAX_NAME) t.text[n] = (char)tolower(c);
        n++;
    }
    if (n >= MAX_NAME) return error_tok(t, "identifier too long (max 63 characters)");
    t.text[n] = '\0';
    t.type = TK_IDENT;
    return t;
}

static Token lex_number(Lexer *lx, Token t) {
    char buf[32];
    size_t n = 0;
    bool too_long = false;
    if (peekc(lx) == '-') buf[n++] = (char)nextc(lx);
    while (isdigit(peekc(lx))) {
        int c = nextc(lx);
        if (n < sizeof buf - 1) buf[n++] = (char)c;
        else too_long = true;
    }
    if (is_ident_char(peekc(lx))) {
        while (is_ident_char(peekc(lx))) nextc(lx);
        return error_tok(t, "invalid number");
    }
    buf[n] = '\0';
    errno = 0;
    long long v = strtoll(buf, NULL, 10);
    if (too_long || errno == ERANGE) return error_tok(t, "integer out of range");
    t.type = TK_INT;
    t.ival = v;
    snprintf(t.text, sizeof t.text, "%s", buf);
    return t;
}

static Token lex_string(Lexer *lx, Token t) {
    size_t n = 0;
    bool too_long = false;
    nextc(lx); /* opening quote */
    for (;;) {
        int c = peekc(lx);
        if (!c) return error_tok(t, "unterminated string literal");
        nextc(lx);
        if (c == '\'') {
            if (peekc(lx) != '\'') break; /* closing quote */
            nextc(lx);                    /* '' is an escaped quote */
        }
        if (n < MAX_STR) t.text[n++] = (char)c;
        else too_long = true;
    }
    if (too_long) return error_tok(t, "string literal too long (max 255 bytes)");
    t.text[n] = '\0';
    t.type = TK_STRING;
    return t;
}

Token lexer_next(Lexer *lx) {
    skip_space_and_comments(lx);

    Token t;
    t.type = TK_EOF;
    t.text[0] = '\0';
    t.ival = 0;
    t.line = lx->line;
    t.col = lx->col;

    int c = peekc(lx);
    if (!c) return t;
    if (is_ident_start(c)) return lex_ident(lx, t);
    if (isdigit(c) || (c == '-' && isdigit(peek2(lx)))) return lex_number(lx, t);
    if (c == '\'') return lex_string(lx, t);

    nextc(lx);
    t.text[0] = (char)c;
    t.text[1] = '\0';
    switch (c) {
    case '(': t.type = TK_LPAREN; return t;
    case ')': t.type = TK_RPAREN; return t;
    case ',': t.type = TK_COMMA; return t;
    case ';': t.type = TK_SEMI; return t;
    case '*': t.type = TK_STAR; return t;
    case '=': t.type = TK_EQ; return t;
    case '!':
        if (peekc(lx) == '=') {
            nextc(lx);
            t.type = TK_NE;
            return t;
        }
        break;
    case '<':
        if (peekc(lx) == '=') {
            nextc(lx);
            t.type = TK_LE;
        } else if (peekc(lx) == '>') {
            nextc(lx);
            t.type = TK_NE;
        } else {
            t.type = TK_LT;
        }
        return t;
    case '>':
        if (peekc(lx) == '=') {
            nextc(lx);
            t.type = TK_GE;
        } else {
            t.type = TK_GT;
        }
        return t;
    default: break;
    }

    char msg[64];
    if (isprint(c)) snprintf(msg, sizeof msg, "unexpected character '%c'", c);
    else snprintf(msg, sizeof msg, "unexpected byte 0x%02x", c);
    return error_tok(t, msg);
}
