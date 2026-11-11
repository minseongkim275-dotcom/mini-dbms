/* bench: inserts N rows through the SQL engine, then compares lookups that use
   the B+ tree index (WHERE id ...) with lookups that scan the whole file.

     bench [N] [-k]      N defaults to 1,000,000; -k keeps the data directory */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"

static void die(const char *msg) {
    fprintf(stderr, "bench: %s\n", msg);
    exit(1);
}

static void must(DB *db, const char *sql, Result *r) {
    result_free(r);
    result_init(r);
    if (db_execute(db, sql, 0, r) != 0) {
        fprintf(stderr, "bench: %s\n  -> %s\n", sql, r->error);
        exit(1);
    }
}

/* Runs `runs` queries produced from `fmt` with a random id each, and returns
   the average time per query in milliseconds. */
static double time_queries(DB *db, const char *fmt, int64_t n, int runs, const char *want_plan,
                           size_t want_rows) {
    char sql[256];
    Result r;
    result_init(&r);
    double total = 0;
    for (int i = 0; i < runs; i++) {
        int64_t k = 1 + (int64_t)(((uint64_t)rand() << 16 ^ (uint64_t)rand()) % (uint64_t)n);
        snprintf(sql, sizeof sql, fmt, k);
        double t0 = now_ms();
        must(db, sql, &r);
        total += now_ms() - t0;
        if (strcmp(r.plan, want_plan) != 0) die("unexpected plan");
        if (r.nrows != want_rows) die("unexpected row count");
    }
    result_free(&r);
    return total / runs;
}

int main(int argc, char **argv) {
    int64_t n = 1000000;
    bool keep = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) keep = true;
        else n = atoll(argv[i]);
    }
    if (n < 1000) die("N must be at least 1000");

    char dir[] = "/tmp/minidb_bench_XXXXXX";
    if (!mkdtemp(dir)) die("cannot create a temporary directory");
    char path[256], err[512];
    snprintf(path, sizeof path, "%s/users.schema", dir);
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write the schema file");
    fputs("id INT\nname VARCHAR(32)\nage INT\nemail VARCHAR(64)\n", f);
    fclose(f);

    DB *db = db_open(dir, err, sizeof err);
    if (!db) die(err);
    Result r;
    result_init(&r);
    char sql[256];

    printf("== minidb benchmark: %" PRId64 " rows ==\n\n", n);

    /* 1. Bulk insert, one INSERT statement per row, through the SQL parser. */
    double t0 = now_ms();
    for (int64_t i = 1; i <= n; i++) {
        snprintf(sql, sizeof sql,
                 "INSERT INTO users VALUES ('user_%" PRId64 "', %d, 'user_%" PRId64
                 "@example.com')",
                 i, (int)(i * 7919 % 80) + 10, i);
        must(db, sql, &r);
    }
    double insert_ms = now_ms() - t0;
    printf("INSERT x %" PRId64 "            : %9.1f ms total  (%.0f rows/s)\n", n, insert_ms,
           n / (insert_ms / 1000.0));

    must(db, "SELECT COUNT(*) FROM users", &r);
    if (r.count != n) die("row count mismatch after insert");

    /* 2. Point lookups. */
    srand(42);
    double by_id = time_queries(db, "SELECT * FROM users WHERE id = %" PRId64, n, 10000,
                                PLAN_INDEX, 1);
    double by_name = time_queries(db, "SELECT * FROM users WHERE name = 'user_%" PRId64 "'", n, 10,
                                  PLAN_FULL, 1);
    double by_email = time_queries(
        db, "SELECT * FROM users WHERE email = 'user_%" PRId64 "@example.com'", n, 10, PLAN_FULL, 1);

    /* 3. Range of 1000 ids: leaf chain vs. scanning for the same rows. */
    snprintf(sql, sizeof sql, "SELECT * FROM users WHERE id > %" PRId64, n - 1000);
    t0 = now_ms();
    must(db, sql, &r);
    double range_ms = now_ms() - t0;
    if (r.nrows != 1000 || strcmp(r.plan, PLAN_RANGE) != 0) die("range query failed");

    printf("\n%-34s %-18s %14s\n", "query", "plan", "avg time");
    printf("%-34s %-18s %11.4f ms\n", "WHERE id = ?    (10000 runs)", "INDEX SCAN", by_id);
    printf("%-34s %-18s %11.4f ms\n", "WHERE name = ?  (10 runs)", "FULL SCAN", by_name);
    printf("%-34s %-18s %11.4f ms\n", "WHERE email = ? (10 runs)", "FULL SCAN", by_email);
    printf("%-34s %-18s %11.4f ms\n", "WHERE id > N-1000 (1000 rows)", "INDEX RANGE SCAN",
           range_ms);
    printf("\nindex lookup is %.0fx faster than a linear scan on name\n", by_name / by_id);

    /* 4. Reopen: the index is rebuilt from the data file. */
    db_close(db);
    db = db_open(dir, err, sizeof err);
    if (!db) die(err);
    t0 = now_ms();
    must(db, "SELECT * FROM users WHERE id = 1", &r); /* first use opens the table */
    printf("reopen + index rebuild              : %9.1f ms\n", now_ms() - t0);
    if (r.nrows != 1) die("lookup after reopen failed");

    result_free(&r);
    db_close(db);
    if (keep) {
        printf("\ndata kept in %s\n", dir);
    } else {
        snprintf(path, sizeof path, "%s/users.schema", dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/users.tbl", dir);
        unlink(path);
        rmdir(dir);
    }
    return 0;
}
