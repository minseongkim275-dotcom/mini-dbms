#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    Lexer *lx;
    Token cur; /* one token of lookahead */
    char *err;
    size_t errlen;
    bool failed;
} Parser;

static const char *const KEYWORDS[] = {"select", "insert", "into",  "values", "from",
                                       "where",  "limit",  "explain", NULL};

static bool is_keyword(const char *s) {
    for (int i = 0; KEYWORDS[i]; i++)
        if (strcmp(s, KEYWORDS[i]) == 0) return true;
    return false;
}

static void advance(Parser *p) { p->cur = lexer_next(p->lx); }

static bool fail(Parser *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool fail(Parser *p, const char *fmt, ...) {
    if (p->failed) return false; /* keep the first error */
    p->failed = true;
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(p->err, p->errlen, "syntax error at line %d, col %d: %s", p->cur.line, p->cur.col,
             msg);
    return false;
}

static void describe(const Token *t, char *buf, size_t n) {
    switch (t->type) {
    case TK_EOF: snprintf(buf, n, "end of input"); break;
    case TK_STRING: snprintf(buf, n, "string '%.40s'", t->text); break;
    default: snprintf(buf, n, "'%.40s'", t->text); break;
    }
}

static bool unexpected(Parser *p, const char *wanted) {
    if (p->cur.type == TK_ERROR) return fail(p, "%s", p->cur.text);
    char found[64];
    describe(&p->cur, found, sizeof found);
    return fail(p, "expected %s but found %s", wanted, found);
}

static bool at_keyword(const Parser *p, const char *kw) {
    return p->cur.type == TK_IDENT && strcmp(p->cur.text, kw) == 0;
}

static bool expect_keyword(Parser *p, const char *kw) {
    if (!at_keyword(p, kw)) {
        char wanted[32];
        snprintf(wanted, sizeof wanted, "%s", kw);
        for (char *c = wanted; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        return unexpected(p, wanted);
    }
    advance(p);
    return true;
}

static bool expect(Parser *p, TokType type, const char *wanted) {
    if (p->cur.type != type) return unexpected(p, wanted);
    advance(p);
    return true;
}

static bool parse_name(Parser *p, char *out, const char *what) {
    if (p->cur.type != TK_IDENT) return unexpected(p, what);
    if (is_keyword(p->cur.text))
        return fail(p, "'%s' is a reserved word and cannot be used as a %s", p->cur.text, what);
    snprintf(out, MAX_NAME, "%.*s", MAX_NAME - 1, p->cur.text);
    advance(p);
    return true;
}

static bool parse_value(Parser *p, Value *v) {
    if (p->cur.type == TK_INT) {
        v->type = T_INT;
        v->i = p->cur.ival;
        v->s[0] = '\0';
    } else if (p->cur.type == TK_STRING) {
        v->type = T_VARCHAR;
        v->i = 0;
        snprintf(v->s, sizeof v->s, "%s", p->cur.text);
    } else {
        return unexpected(p, "a value (integer or 'string')");
    }
    advance(p);
    return true;
}

static bool parse_insert(Parser *p, Stmt *st) {
    st->type = STMT_INSERT;
    advance(p); /* INSERT */
    if (!expect_keyword(p, "into")) return false;
    if (!parse_name(p, st->table, "table name")) return false;

    if (p->cur.type == TK_LPAREN) {
        advance(p);
        do {
            if (st->ncols == MAX_COLS) return fail(p, "too many columns (max %d)", MAX_COLS);
            if (!parse_name(p, st->cols[st->ncols], "column name")) return false;
            st->ncols++;
        } while (p->cur.type == TK_COMMA && (advance(p), true));
        if (!expect(p, TK_RPAREN, "',' or ')'")) return false;
    }

    if (!expect_keyword(p, "values")) return false;
    if (!expect(p, TK_LPAREN, "'('")) return false;
    do {
        if (st->nvalues == MAX_COLS) return fail(p, "too many values (max %d)", MAX_COLS);
        if (!parse_value(p, &st->values[st->nvalues])) return false;
        st->nvalues++;
    } while (p->cur.type == TK_COMMA && (advance(p), true));
    return expect(p, TK_RPAREN, "',' or ')'");
}

static bool parse_select(Parser *p, Stmt *st) {
    st->type = STMT_SELECT;
    advance(p); /* SELECT */

    if (p->cur.type == TK_STAR) {
        st->star = true;
        advance(p);
    } else {
        bool first = true;
        do {
            /* COUNT(*) is only recognised as the sole item; a column may still be
               called "count" as long as it is not followed by '('. */
            if (first && at_keyword(p, "count")) {
                advance(p);
                if (p->cur.type == TK_LPAREN) {
                    advance(p);
                    if (!expect(p, TK_STAR, "'*'")) return false;
                    if (!expect(p, TK_RPAREN, "')'")) return false;
                    st->count_star = true;
                    break;
                }
                snprintf(st->cols[st->ncols++], MAX_NAME, "count");
            } else {
                if (st->ncols == MAX_COLS) return fail(p, "too many columns (max %d)", MAX_COLS);
                if (!parse_name(p, st->cols[st->ncols], "column name, '*' or COUNT(*)"))
                    return false;
                st->ncols++;
            }
            first = false;
        } while (p->cur.type == TK_COMMA && (advance(p), true));
    }

    if (!expect_keyword(p, "from")) return false;
    if (!parse_name(p, st->table, "table name")) return false;

    if (at_keyword(p, "where")) {
        advance(p);
        st->has_where = true;
        if (!parse_name(p, st->where.col, "column name")) return false;
        switch (p->cur.type) {
        case TK_EQ: st->where.op = OP_EQ; break;
        case TK_NE: st->where.op = OP_NE; break;
        case TK_LT: st->where.op = OP_LT; break;
        case TK_LE: st->where.op = OP_LE; break;
        case TK_GT: st->where.op = OP_GT; break;
        case TK_GE: st->where.op = OP_GE; break;
        default: return unexpected(p, "a comparison operator (=, !=, <, <=, >, >=)");
        }
        advance(p);
        if (!parse_value(p, &st->where.value)) return false;
    }

    if (at_keyword(p, "limit")) {
        advance(p);
        if (p->cur.type != TK_INT || p->cur.ival < 0)
            return unexpected(p, "a non-negative integer after LIMIT");
        st->has_limit = true;
        st->limit = p->cur.ival;
        advance(p);
    }
    return true;
}

static bool parse_body(Parser *p, Stmt *st) {
    if (at_keyword(p, "explain")) {
        st->explain = true;
        advance(p);
        if (!at_keyword(p, "select")) {
            if (at_keyword(p, "insert")) return fail(p, "EXPLAIN supports only SELECT");
            return unexpected(p, "SELECT after EXPLAIN");
        }
    }
    bool ok;
    if (at_keyword(p, "select")) ok = parse_select(p, st);
    else if (at_keyword(p, "insert")) ok = parse_insert(p, st);
    else return unexpected(p, "SELECT or INSERT");
    if (!ok) return false;
    if (p->cur.type != TK_SEMI && p->cur.type != TK_EOF)
        return unexpected(p, "';' or end of statement");
    return true;
}

int parse_statement(Lexer *lx, Stmt *out, char *err, size_t errlen) {
    Parser p = {.lx = lx, .err = err, .errlen = errlen, .failed = false};
    if (errlen) err[0] = '\0';

    advance(&p);
    while (p.cur.type == TK_SEMI) advance(&p); /* empty statements */
    if (p.cur.type == TK_EOF) return 0;

    memset(out, 0, sizeof *out);
    if (parse_body(&p, out)) return 1;

    /* Error recovery: resume after the next ';'. */
    while (p.cur.type != TK_SEMI && p.cur.type != TK_EOF) advance(&p);
    return -1;
}

int parse_single(const char *sql, Stmt *out, char *err, size_t errlen) {
    Lexer lx;
    lexer_init(&lx, sql);
    int rc = parse_statement(&lx, out, err, errlen);
    if (rc < 0) return -1;
    if (rc == 0) {
        snprintf(err, errlen, "empty statement");
        return -1;
    }
    Stmt extra;
    char err2[64];
    if (parse_statement(&lx, &extra, err2, sizeof err2) != 0) {
        snprintf(err, errlen, "only one statement is allowed per request");
        return -1;
    }
    return 0;
}
