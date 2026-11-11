#ifndef MINIDB_DB_H
#define MINIDB_DB_H

#include "common.h"
#include "parser.h"
#include "table.h"

/* The engine entry point used by both the CLI and the API server.
   All functions are safe to call from multiple threads at once. */

typedef struct DB DB;

/* Outcome of one statement. Initialise with result_init, release with result_free. */
typedef struct {
    bool ok;
    char error[512];

    StmtType type;
    bool explain;    /* only `plan` is filled in */
    bool count_star; /* `count` holds the answer */

    /* SELECT rows: `nrows` whole records stored back to back in `rows`, each
       `schema->recsize` bytes. `proj` lists the column indexes to show. */
    const Schema *schema;
    int nproj;
    int proj[MAX_COLS];
    uint8_t *rows;
    size_t nrows, cap;
    bool truncated; /* stopped at max_rows; more rows matched */

    int64_t count;     /* COUNT(*) */
    int64_t insert_id; /* INSERT */

    const char *plan;  /* how a SELECT was (or would be) executed */
    int64_t examined;  /* records read from the data file */
    double elapsed_ms;
} Result;

typedef struct {
    long selects, inserts, errors;
    long index_scans, range_scans, full_scans;
} DBStats;

#define PLAN_INDEX "INDEX SCAN (B+Tree point lookup on id)"
#define PLAN_RANGE "INDEX RANGE SCAN (B+Tree leaf chain on id)"
#define PLAN_FULL "FULL SCAN (linear)"
#define PLAN_META "METADATA (row counter)"

DB *db_open(const char *dir, char *err, size_t errlen);
void db_close(DB *db);

void result_init(Result *r);
void result_free(Result *r);

/* Executes one parsed statement. `max_rows` caps the rows kept in the result
   (0 = no cap). Returns 0 on success, -1 on error (r->error is set). */
int db_exec_stmt(DB *db, const Stmt *st, size_t max_rows, Result *r);

/* Parses and executes a string holding exactly one statement. */
int db_execute(DB *db, const char *sql, size_t max_rows, Result *r);

void db_stats(DB *db, DBStats *out);

/* Output formats (format.c). */
void result_format_table(const Result *r, StrBuf *out);
void result_format_json(const Result *r, StrBuf *out);

#endif
