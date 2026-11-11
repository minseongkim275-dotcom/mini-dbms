#include "test.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "table.h"

static char err[512];

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static int load(const char *text, Schema *s) {
    char path[] = "/tmp/minidb_schema_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    write_file(path, text);
    int rc = schema_load(path, s, err, sizeof err);
    unlink(path);
    return rc;
}

static void test_schema_load(void) {
    Schema s;
    CHECK(load("# users\nid INT\n\nName  varchar(32)   # display name\nage int\n", &s) == 0);
    CHECK(s.ncols == 3);
    CHECK_STR(s.cols[1].name, "name"); /* lower-cased */
    CHECK(s.cols[0].type == T_INT && s.cols[0].size == 8 && s.cols[0].offset == 0);
    CHECK(s.cols[1].type == T_VARCHAR && s.cols[1].maxlen == 32 && s.cols[1].size == 33);
    CHECK(s.cols[1].offset == 8 && s.cols[2].offset == 41);
    CHECK(s.recsize == 49);
    CHECK(schema_find(&s, "age") == 2 && schema_find(&s, "nope") == -1);
    CHECK(load("id INT\nv VARCHAR ( 255 )\n", &s) == 0);
}

static void test_schema_errors(void) {
    Schema s;
    CHECK(load("", &s) != 0);
    CHECK_HAS(err, "first column must be 'id INT'");
    CHECK(load("name VARCHAR(8)\n", &s) != 0);
    CHECK(load("id VARCHAR(8)\n", &s) != 0);
    CHECK(load("id INT\nname\n", &s) != 0);
    CHECK(load("id INT\nname TEXT\n", &s) != 0);
    CHECK_HAS(err, "unknown type");
    CHECK(load("id INT\nname VARCHAR(0)\n", &s) != 0);
    CHECK(load("id INT\nname VARCHAR(256)\n", &s) != 0);
    CHECK(load("id INT\nname VARCHAR(8\n", &s) != 0);
    CHECK(load("id INT\nname VARCHAR(8)x\n", &s) != 0);
    CHECK(load("id INT\nage INT\nAGE INT\n", &s) != 0);
    CHECK_HAS(err, "duplicate");
    CHECK(load("id INT\n9lives INT\n", &s) != 0);
    CHECK(load("id INT\nbad-name INT\n", &s) != 0);
    CHECK_HAS(err, ":2:"); /* line number reported */

    char big[2048] = "id INT\n";
    for (int i = 0; i < MAX_COLS; i++) sprintf(big + strlen(big), "c%d INT\n", i);
    CHECK(load(big, &s) != 0);
    CHECK_HAS(err, "too many columns");
    CHECK(schema_load("/no/such/file.schema", &s, err, sizeof err) != 0);
}

static void make_rec(const Table *t, uint8_t *rec, const char *name, int64_t age) {
    memset(rec, 0, (size_t)t->schema.recsize);
    strcpy((char *)rec + t->schema.cols[1].offset, name);
    memcpy(rec + t->schema.cols[2].offset, &age, sizeof age);
}

typedef struct {
    int n;
    int64_t last_id;
    int stop_after;
} Count;

static bool count_rec(const uint8_t *rec, int64_t offset, void *ctx) {
    (void)offset;
    Count *c = ctx;
    memcpy(&c->last_id, rec, 8);
    c->n++;
    return c->stop_after == 0 || c->n < c->stop_after;
}

static void test_append_read_scan_reopen(void) {
    char dir[64];
    make_data_dir(dir, sizeof dir, "users", USERS_SCHEMA);

    Table *t = table_open(dir, "users", err, sizeof err);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(t->nrows == 0 && t->next_id == 1);

    Count c = {0};
    CHECK(table_scan(t, count_rec, &c) == 0 && c.n == 0); /* empty table */

    uint8_t rec[MAX_RECSIZE], back[MAX_RECSIZE];
    int64_t id = 0;
    for (int i = 1; i <= 5000; i++) { /* several scan chunks */
        char name[32];
        snprintf(name, sizeof name, "user_%d", i);
        make_rec(t, rec, name, i % 90);
        CHECK(table_append(t, rec, &id, err, sizeof err) == 0 && id == i);
    }
    CHECK(t->nrows == 5000 && t->next_id == 5001);

    int64_t off;
    CHECK(bpt_search(t->index, 4321, &off));
    CHECK(table_read(t, off, back) == 0);
    CHECK(rec_get_int(back, &t->schema.cols[0]) == 4321);
    CHECK_STR(rec_get_str(back, &t->schema.cols[1]), "user_4321");
    CHECK(rec_get_int(back, &t->schema.cols[2]) == 4321 % 90);

    c = (Count){0};
    CHECK(table_scan(t, count_rec, &c) == 0 && c.n == 5000 && c.last_id == 5000);
    c = (Count){.stop_after = 7};
    CHECK(table_scan(t, count_rec, &c) == 0 && c.n == 7); /* early stop */
    table_close(t);

    /* Reopening rebuilds the index from the file and continues the id sequence. */
    t = table_open(dir, "users", err, sizeof err);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(t->nrows == 5000 && t->next_id == 5001);
    CHECK(bpt_count(t->index) == 5000 && bpt_validate(t->index));
    CHECK(bpt_search(t->index, 1, &off) && table_read(t, off, back) == 0);
    CHECK_STR(rec_get_str(back, &t->schema.cols[1]), "user_1");
    make_rec(t, rec, "late", 1);
    CHECK(table_append(t, rec, &id, err, sizeof err) == 0 && id == 5001);
    table_close(t);
    remove_data_dir(dir);
}

static void test_missing_table(void) {
    char dir[64];
    make_data_dir(dir, sizeof dir, "users", USERS_SCHEMA);
    CHECK(table_open(dir, "ghost", err, sizeof err) == NULL);
    CHECK_HAS(err, "table 'ghost' does not exist");
    remove_data_dir(dir);
}

/* A crash in the middle of a write leaves a partial record at the end of the
   file. It must be ignored on open and overwritten by the next insert. */
static void test_torn_tail_is_ignored(void) {
    char dir[64], path[128];
    make_data_dir(dir, sizeof dir, "users", USERS_SCHEMA);
    snprintf(path, sizeof path, "%s/users.tbl", dir);

    Table *t = table_open(dir, "users", err, sizeof err);
    uint8_t rec[MAX_RECSIZE];
    int64_t id;
    for (int i = 0; i < 3; i++) {
        make_rec(t, rec, "ok", i);
        table_append(t, rec, &id, err, sizeof err);
    }
    int recsize = t->schema.recsize;
    table_close(t);

    FILE *f = fopen(path, "ab");
    fwrite("garbage-partial-record", 1, 22, f);
    fclose(f);

    t = table_open(dir, "users", err, sizeof err);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(t->nrows == 3 && t->next_id == 4);
    make_rec(t, rec, "after", 9);
    CHECK(table_append(t, rec, &id, err, sizeof err) == 0 && id == 4);
    table_close(t);

    struct stat st;
    stat(path, &st);
    CHECK(st.st_size == 16 + 4 * recsize); /* the garbage was overwritten */
    t = table_open(dir, "users", err, sizeof err);
    CHECK(t != NULL && t->nrows == 4);
    table_close(t);
    remove_data_dir(dir);
}

static void test_rejects_foreign_or_mismatched_file(void) {
    char dir[64], path[128];
    make_data_dir(dir, sizeof dir, "users", USERS_SCHEMA);
    snprintf(path, sizeof path, "%s/users.tbl", dir);

    write_file(path, "this is definitely not a minidb data file");
    CHECK(table_open(dir, "users", err, sizeof err) == NULL);
    CHECK_HAS(err, "not a minidb data file");
    write_file(path, "MDB"); /* shorter than the header */
    CHECK(table_open(dir, "users", err, sizeof err) == NULL);
    unlink(path);

    /* Data written under one schema must not be read under another. */
    Table *t = table_open(dir, "users", err, sizeof err);
    uint8_t rec[MAX_RECSIZE];
    int64_t id;
    make_rec(t, rec, "x", 1);
    table_append(t, rec, &id, err, sizeof err);
    table_close(t);
    snprintf(path, sizeof path, "%s/users.schema", dir);
    write_file(path, "id INT\nname VARCHAR(10)\n");
    CHECK(table_open(dir, "users", err, sizeof err) == NULL);
    CHECK_HAS(err, "schema changed");
    remove_data_dir(dir);
}

int main(void) {
    RUN(test_schema_load);
    RUN(test_schema_errors);
    RUN(test_append_read_scan_reopen);
    RUN(test_missing_table);
    RUN(test_torn_tail_is_ignored);
    RUN(test_rejects_foreign_or_mismatched_file);
    TEST_REPORT("test_table");
}
