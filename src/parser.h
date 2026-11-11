#ifndef MINIDB_PARSER_H
#define MINIDB_PARSER_H

#include "common.h"
#include "lexer.h"

typedef enum { STMT_INSERT, STMT_SELECT } StmtType;
typedef enum { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE } CmpOp;

typedef struct {
    ColType type;
    int64_t i;
    char s[MAX_STR + 1];
} Value;

typedef struct {
    char col[MAX_NAME];
    CmpOp op;
    Value value;
} Condition;

/* One parsed statement. Which fields are meaningful depends on `type`. */
typedef struct {
    StmtType type;
    bool explain;
    char table[MAX_NAME];

    /* INSERT: optional column list (ncols == 0 means "all columns except id").
       SELECT: projection list (unused when star or count_star is set). */
    int ncols;
    char cols[MAX_COLS][MAX_NAME];

    /* INSERT */
    int nvalues;
    Value values[MAX_COLS];

    /* SELECT */
    bool star;
    bool count_star;
    bool has_where;
    Condition where;
    bool has_limit;
    int64_t limit;
} Stmt;

/* Parses the next statement from the lexer.
   Returns 1 if a statement was parsed, 0 if the input is exhausted, and -1 on
   a syntax error (message in `err`). After an error the lexer is positioned
   just past the next ';', so the caller can keep going with the next statement. */
int parse_statement(Lexer *lx, Stmt *out, char *err, size_t errlen);

/* Parses a string that must contain exactly one statement. Returns 0 or -1. */
int parse_single(const char *sql, Stmt *out, char *err, size_t errlen);

#endif
