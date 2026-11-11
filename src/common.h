#ifndef MINIDB_COMMON_H
#define MINIDB_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_NAME 64                       /* identifier buffer (63 chars + NUL) */
#define MAX_COLS 32                       /* columns per table */
#define MAX_STR 255                       /* longest string literal / VARCHAR */
#define MAX_RECSIZE (MAX_COLS * (MAX_STR + 1))

typedef enum { T_INT, T_VARCHAR } ColType;

/* Growable string buffer used for JSON and table output. */
typedef struct {
    char *p;
    size_t len, cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(StrBuf *sb, const char *s); /* appends a quoted, escaped JSON string */

/* malloc/realloc that abort on out-of-memory: the engine has no way to continue. */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

double now_ms(void);

#endif
