#include "test.h"

#include "db.h"

static DB *db;
static Result r;

static int run(const char *sql) {
    result_free(&r);
    result_init(&r);
    return db_execute(db, sql, 0, &r);
}

static int64_t cell_int(size_t row, int k) {
    const uint8_t *rec = r.rows + row * (size_t)r.schema->recsize;
    return rec_get_int(rec, &r.schema->cols[r.proj[k]]);
}

static const char *cell_str(size_t row, int k) {
    const uint8_t *rec = r.rows + row * (size_t)r.schema->recsize;
    return rec_get_str(rec, &r.schema->cols[r.proj[k]]);
}

static void test_empty_table(void) {
    CHECK(run("SELECT * FROM users") == 0 && r.nrows == 0);
    CHECK(run("SELECT COUNT(*) FROM users") == 0 && r.count == 0);
    CHECK(run("SELECT * FROM users WHERE id = 1") == 0 && r.nrows == 0);
    CHECK(run("SELECT * FROM users WHERE id > 0") == 0 && r.nrows == 0);
    CHECK(run("SELECT * FROM users WHERE name = 'x'") == 0 && r.nrows == 0);
}

static void test_insert_assigns_ids(void) {
    CHECK(run("INSERT INTO users VALUES ('alice', 30, 'alice@example.com')") == 0);
    CHECK(r.insert_id == 1);
    CHECK(run("INSERT INTO users VALUES ('bob', 25, 'bob@example.com')") == 0 && r.insert_id == 2);
    CHECK(run("INSERT INTO users (age, name) VALUES (41, 'carol')") == 0 && r.insert_id == 3);
    CHECK(run("INSERT INTO users VALUES ('dave', 25, 'dave@example.com')") == 0);
    CHECK(run("INSERT INTO users VALUES ('', -5, '')") == 0 && r.insert_id == 5);
    CHECK(run("SELECT COUNT(*) FROM users") == 0 && r.count == 5);
    CHECK_STR(r.plan, PLAN_META);
}

static void test_select_projection(void) {
    CHECK(run("SELECT * FROM users") == 0);
    CHECK(r.nrows == 5 && r.nproj == 4);
    CHECK(cell_int(0, 0) == 1);
    CHECK_STR(cell_str(0, 1), "alice");
    CHECK_STR(cell_str(2, 3), ""); /* omitted column defaults to '' */
    CHECK(cell_int(4, 2) == -5);

    CHECK(run("SELECT email, id, id FROM users WHERE id = 2") == 0); /* reorder and repeat */
    CHECK(r.nrows == 1 && r.nproj == 3);
    CHECK_STR(cell_str(0, 0), "bob@example.com");
    CHECK(cell_int(0, 1) == 2 && cell_int(0, 2) == 2);
}

static void test_index_plans(void) {
    CHECK(run("SELECT * FROM users WHERE id = 3") == 0);
    CHECK_STR(r.plan, PLAN_INDEX);
    CHECK(r.nrows == 1 && r.examined == 1); /* one record read, not five */
    CHECK_STR(cell_str(0, 1), "carol");

    CHECK(run("SELECT * FROM users WHERE id = 999") == 0 && r.nrows == 0 && r.examined == 0);
    CHECK(run("SELECT * FROM users WHERE id = -1") == 0 && r.nrows == 0);

    CHECK(run("SELECT id FROM users WHERE id > 3") == 0);
    CHECK_STR(r.plan, PLAN_RANGE);
    CHECK(r.nrows == 2 && r.examined == 2 && cell_int(0, 0) == 4 && cell_int(1, 0) == 5);
    CHECK(run("SELECT id FROM users WHERE id >= 3") == 0 && r.nrows == 3);
    CHECK(run("SELECT id FROM users WHERE id < 3") == 0 && r.nrows == 2 && cell_int(1, 0) == 2);
    CHECK(run("SELECT id FROM users WHERE id <= 3") == 0 && r.nrows == 3);
    CHECK(run("SELECT id FROM users WHERE id < 1") == 0 && r.nrows == 0);
    CHECK(run("SELECT id FROM users WHERE id > 5") == 0 && r.nrows == 0);
    CHECK(run("SELECT COUNT(*) FROM users WHERE id >= 2") == 0 && r.count == 4);

    /* Bounds at the edge of int64 must not overflow. */
    CHECK(run("SELECT id FROM users WHERE id > 9223372036854775807") == 0 && r.nrows == 0);
    CHECK(run("SELECT id FROM users WHERE id < -9223372036854775808") == 0 && r.nrows == 0);
    CHECK(run("SELECT id FROM users WHERE id <= 9223372036854775807") == 0 && r.nrows == 5);
    CHECK(run("SELECT id FROM users WHERE id >= -9223372036854775808") == 0 && r.nrows == 5);

    CHECK(run("SELECT id FROM users WHERE id != 3") == 0);
    CHECK_STR(r.plan, PLAN_FULL);
    CHECK(r.nrows == 4);
}

static void test_full_scan(void) {
    CHECK(run("SELECT name FROM users WHERE age = 25") == 0);
    CHECK_STR(r.plan, PLAN_FULL);
    CHECK(r.nrows == 2 && r.examined == 5);
    CHECK_STR(cell_str(0, 0), "bob");
    CHECK_STR(cell_str(1, 0), "dave");

    CHECK(run("SELECT name FROM users WHERE age < 0") == 0 && r.nrows == 1);
    CHECK(run("SELECT name FROM users WHERE name = 'carol'") == 0 && r.nrows == 1);
    CHECK(run("SELECT name FROM users WHERE name = 'CAROL'") == 0 && r.nrows == 0); /* case-sensitive */
    CHECK(run("SELECT name FROM users WHERE name >= 'c'") == 0 && r.nrows == 2);
    CHECK(run("SELECT name FROM users WHERE name = ''") == 0 && r.nrows == 1);
    CHECK(run("SELECT COUNT(*) FROM users WHERE age != 25") == 0 && r.count == 3);
}

static void test_limit(void) {
    CHECK(run("SELECT * FROM users LIMIT 2") == 0 && r.nrows == 2 && r.examined == 2);
    CHECK(run("SELECT * FROM users LIMIT 0") == 0 && r.nrows == 0);
    CHECK(run("SELECT * FROM users LIMIT 100") == 0 && r.nrows == 5);
    CHECK(run("SELECT * FROM users WHERE id >= 2 LIMIT 2") == 0 && r.nrows == 2);
    CHECK(cell_int(1, 0) == 3);
    CHECK(run("SELECT * FROM users WHERE age = 25 LIMIT 1") == 0 && r.nrows == 1);
}

static void test_max_rows_cap(void) {
    result_free(&r);
    result_init(&r);
    CHECK(db_execute(db, "SELECT * FROM users", 3, &r) == 0);
    CHECK(r.nrows == 3 && r.truncated);
    result_free(&r);
    result_init(&r);
    CHECK(db_execute(db, "SELECT * FROM users", 5, &r) == 0);
    CHECK(r.nrows == 5 && !r.truncated); /* exactly at the cap is not truncation */
    result_free(&r);
    result_init(&r);
    CHECK(db_execute(db, "SELECT COUNT(*) FROM users WHERE age > 0", 1, &r) == 0 && r.count == 4);
}

static void test_explain(void) {
    CHECK(run("EXPLAIN SELECT * FROM users WHERE id = 1") == 0);
    CHECK(r.explain && r.nrows == 0);
    CHECK_STR(r.plan, PLAN_INDEX);
    CHECK(run("EXPLAIN SELECT * FROM users WHERE id <= 1") == 0);
    CHECK_STR(r.plan, PLAN_RANGE);
    CHECK(run("EXPLAIN SELECT * FROM users WHERE email = 'x'") == 0);
    CHECK_STR(r.plan, PLAN_FULL);
    CHECK(run("EXPLAIN SELECT * FROM users") == 0);
    CHECK_STR(r.plan, PLAN_FULL);
}

static void test_semantic_errors(void) {
    CHECK(run("SELECT * FROM ghost") != 0);
    CHECK_HAS(r.error, "table 'ghost' does not exist");
    CHECK(run("INSERT INTO ghost VALUES (1)") != 0);
    CHECK(run("SELECT nope FROM users") != 0);
    CHECK_HAS(r.error, "column 'nope' does not exist");
    CHECK(run("SELECT * FROM users WHERE nope = 1") != 0);
    CHECK(run("SELECT * FROM users WHERE age = 'old'") != 0);
    CHECK_HAS(r.error, "type mismatch");
    CHECK(run("SELECT * FROM users WHERE name = 5") != 0);
    CHECK(run("SELECT * FROM users WHERE id = '1'") != 0);

    CHECK(run("INSERT INTO users VALUES ('x', 1)") != 0);
    CHECK_HAS(r.error, "expected 3 value(s) but got 2");
    CHECK(run("INSERT INTO users VALUES ('x', 1, 'e', 'extra')") != 0);
    CHECK(run("INSERT INTO users VALUES (1, 'x', 'e')") != 0);
    CHECK_HAS(r.error, "type mismatch for column 'name'");
    CHECK(run("INSERT INTO users (id, name) VALUES (99, 'x')") != 0);
    CHECK_HAS(r.error, "assigned automatically");
    CHECK(run("INSERT INTO users (name, name) VALUES ('a', 'b')") != 0);
    CHECK_HAS(r.error, "more than once");
    CHECK(run("INSERT INTO users (nope) VALUES (1)") != 0);
    CHECK(run("INSERT INTO users (name, age) VALUES ('a')") != 0);

    /* None of the failed statements above may have inserted or consumed an id. */
    CHECK(run("SELECT COUNT(*) FROM users") == 0 && r.count == 5);
    CHECK(run("SELECT nonsense") != 0);
    CHECK_HAS(r.error, "syntax error");
}

static void test_varchar_boundary(void) {
    char sql[256], name[64];
    memset(name, 'n', 32);
    name[32] = '\0'; /* exactly VARCHAR(32) */
    snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('%s', 1, 'e')", name);
    CHECK(run(sql) == 0 && r.insert_id == 6);
    CHECK(run("SELECT name FROM users WHERE id = 6") == 0);
    CHECK_STR(cell_str(0, 0), name);

    name[32] = 'n';
    name[33] = '\0'; /* one byte too long */
    snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('%s', 1, 'e')", name);
    CHECK(run(sql) != 0);
    CHECK_HAS(r.error, "value too long for column 'name'");

    CHECK(run("INSERT INTO users VALUES ('O''Brien', 1, 'quote''s')") == 0 && r.insert_id == 7);
    CHECK(run("SELECT name FROM users WHERE name = 'O''Brien'") == 0 && r.nrows == 1);
    CHECK(run("INSERT INTO users VALUES ('한글 이름', 9223372036854775807, 'k')") == 0);
    CHECK(run("SELECT name, age FROM users WHERE id = 8") == 0);
    CHECK_STR(cell_str(0, 0), "한글 이름");
    CHECK(cell_int(0, 1) == INT64_MAX);
}

static void test_formatting(void) {
    StrBuf sb;
    sb_init(&sb);
    CHECK(run("SELECT id, name FROM users WHERE id = 7") == 0);
    result_format_json(&r, &sb);
    CHECK_HAS(sb.p, "\"columns\":[\"id\",\"name\"],\"rows\":[[7,\"O'Brien\"]],\"row_count\":1");
    CHECK_HAS(sb.p, "\"ok\":true");
    CHECK_HAS(sb.p, "\"truncated\":false");

    sb.len = 0;
    result_format_table(&r, &sb);
    CHECK_HAS(sb.p, "| id | name    |\n");
    CHECK_HAS(sb.p, "| 7  | O'Brien |\n");
    CHECK_HAS(sb.p, "(1 row)\n");

    sb.len = 0;
    run("SELECT * FROM ghost");
    result_format_json(&r, &sb);
    CHECK_STR(sb.p, "{\"ok\":false,\"error\":\"table 'ghost' does not exist\"}");

    sb.len = 0;
    sb_json_str(&sb, "a\"b\\c\n\t\x01");
    CHECK_STR(sb.p, "\"a\\\"b\\\\c\\n\\t\\u0001\"");
    sb_free(&sb);
}

static void test_persistence_and_stats(const char *dir) {
    DBStats s;
    db_stats(db, &s);
    CHECK(s.inserts == 8 && s.errors > 0 && s.index_scans > 0 && s.full_scans > 0);

    char err[256];
    db_close(db);
    db = db_open(dir, err, sizeof err);
    CHECK(db != NULL);
    CHECK(run("SELECT COUNT(*) FROM users") == 0 && r.count == 8);
    CHECK(run("SELECT name FROM users WHERE id = 3") == 0 && r.nrows == 1);
    CHECK_STR(r.plan, PLAN_INDEX);
    CHECK_STR(cell_str(0, 0), "carol");
    CHECK(run("INSERT INTO users VALUES ('next', 1, 'n')") == 0 && r.insert_id == 9);
}

int main(void) {
    char dir[64], err[256];
    CHECK(db_open("/no/such/dir", err, sizeof err) == NULL);
    make_data_dir(dir, sizeof dir, "users", USERS_SCHEMA);
    db = db_open(dir, err, sizeof err);
    if (!db) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    result_init(&r);

    RUN(test_empty_table);
    RUN(test_insert_assigns_ids);
    RUN(test_select_projection);
    RUN(test_index_plans);
    RUN(test_full_scan);
    RUN(test_limit);
    RUN(test_max_rows_cap);
    RUN(test_explain);
    RUN(test_semantic_errors);
    RUN(test_varchar_boundary);
    RUN(test_formatting);
    test_persistence_and_stats(dir);

    result_free(&r);
    db_close(db);
    remove_data_dir(dir);
    TEST_REPORT("test_db");
}
