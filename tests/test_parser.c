#include "test.h"

#include "parser.h"

static Stmt st;
static char err[512];

static int parse(const char *sql) { return parse_single(sql, &st, err, sizeof err); }

static void test_insert(void) {
    CHECK(parse("INSERT INTO users VALUES ('alice', 30, 'a@x.com');") == 0);
    CHECK(st.type == STMT_INSERT);
    CHECK_STR(st.table, "users");
    CHECK(st.ncols == 0 && st.nvalues == 3);
    CHECK(st.values[0].type == T_VARCHAR);
    CHECK_STR(st.values[0].s, "alice");
    CHECK(st.values[1].type == T_INT && st.values[1].i == 30);

    CHECK(parse("insert into Users (Name, AGE) values ('bob', -1)") == 0); /* no ';' needed */
    CHECK_STR(st.table, "users");
    CHECK(st.ncols == 2 && st.nvalues == 2);
    CHECK_STR(st.cols[0], "name");
    CHECK_STR(st.cols[1], "age");
    CHECK(st.values[1].i == -1);
}

static void test_select(void) {
    CHECK(parse("SELECT * FROM users") == 0);
    CHECK(st.type == STMT_SELECT && st.star && !st.has_where && !st.has_limit && !st.count_star);

    CHECK(parse("SELECT id, name FROM users WHERE id = 7 LIMIT 3;") == 0);
    CHECK(!st.star && st.ncols == 2);
    CHECK(st.has_where && st.where.op == OP_EQ && st.where.value.i == 7);
    CHECK_STR(st.where.col, "id");
    CHECK(st.has_limit && st.limit == 3);

    CHECK(parse("SELECT name FROM users WHERE name <> 'x y'") == 0);
    CHECK(st.where.op == OP_NE && st.where.value.type == T_VARCHAR);
    CHECK_STR(st.where.value.s, "x y");

    const char *ops[] = {"=", "!=", "<", "<=", ">", ">="};
    CmpOp want[] = {OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE};
    for (int i = 0; i < 6; i++) {
        char sql[64];
        snprintf(sql, sizeof sql, "select * from t where a %s 1", ops[i]);
        CHECK(parse(sql) == 0 && st.where.op == want[i]);
    }
}

static void test_count_and_explain(void) {
    CHECK(parse("SELECT COUNT(*) FROM users WHERE age > 20") == 0);
    CHECK(st.count_star && st.ncols == 0 && st.has_where);

    CHECK(parse("SELECT count FROM t") == 0); /* a column may be called count */
    CHECK(!st.count_star && st.ncols == 1);
    CHECK_STR(st.cols[0], "count");

    CHECK(parse("EXPLAIN SELECT * FROM users WHERE id = 1") == 0);
    CHECK(st.explain && st.type == STMT_SELECT);

    CHECK(parse("EXPLAIN INSERT INTO users VALUES (1)") != 0);
    CHECK_HAS(err, "EXPLAIN supports only SELECT");
}

static void test_syntax_errors(void) {
    CHECK(parse("") != 0);
    CHECK_HAS(err, "empty statement");
    CHECK(parse("  ;; -- nothing\n") != 0);

    CHECK(parse("SELEC * FROM users") != 0);
    CHECK_HAS(err, "line 1, col 1");
    CHECK_HAS(err, "expected SELECT or INSERT");

    CHECK(parse("SELECT * users") != 0);
    CHECK_HAS(err, "expected FROM");
    CHECK(parse("SELECT FROM users") != 0);
    CHECK(parse("SELECT * FROM") != 0);
    CHECK_HAS(err, "end of input");
    CHECK(parse("SELECT a, FROM t") != 0);
    CHECK(parse("SELECT * FROM t WHERE") != 0);
    CHECK(parse("SELECT * FROM t WHERE a") != 0);
    CHECK_HAS(err, "comparison operator");
    CHECK(parse("SELECT * FROM t WHERE a = ") != 0);
    CHECK(parse("SELECT * FROM t WHERE a = b") != 0); /* column-to-column not supported */
    CHECK(parse("SELECT * FROM t LIMIT -1") != 0);
    CHECK(parse("SELECT * FROM t LIMIT x") != 0);
    CHECK(parse("SELECT * FROM t extra") != 0);
    CHECK_HAS(err, "end of statement");

    CHECK(parse("INSERT users VALUES (1)") != 0);
    CHECK_HAS(err, "expected INTO");
    CHECK(parse("INSERT INTO users VALUES ()") != 0);
    CHECK(parse("INSERT INTO users VALUES (1,)") != 0);
    CHECK(parse("INSERT INTO users VALUES (1") != 0);
    CHECK(parse("INSERT INTO users (a,) VALUES (1)") != 0);
    CHECK(parse("INSERT INTO users () VALUES (1)") != 0);
    CHECK(parse("INSERT INTO users VALUES (1) (2)") != 0);

    CHECK(parse("SELECT * FROM select") != 0);
    CHECK_HAS(err, "reserved word");

    CHECK(parse("SELECT * FROM t WHERE a = 'oops") != 0);
    CHECK_HAS(err, "unterminated string");
    CHECK(parse("SELECT *\nFROM t\nWHERE a # 1") != 0);
    CHECK_HAS(err, "line 3, col 9");
}

static void test_one_statement_only(void) {
    CHECK(parse("SELECT * FROM a; SELECT * FROM b;") != 0);
    CHECK_HAS(err, "only one statement");
    CHECK(parse("SELECT * FROM a;;; -- trailing comment") == 0);
}

static void test_too_many_values(void) {
    char sql[512] = "INSERT INTO t VALUES (";
    for (int i = 0; i < MAX_COLS + 1; i++) strcat(sql, i ? ",1" : "1");
    strcat(sql, ")");
    CHECK(parse(sql) != 0);
    CHECK_HAS(err, "too many values");
}

/* A script is parsed statement by statement, and a bad statement does not
   stop the ones after it. */
static void test_script_recovery(void) {
    Lexer lx;
    lexer_init(&lx, "INSERT INTO t VALUES (1);\nBOGUS stuff here;\nSELECT * FROM t; ");
    CHECK(parse_statement(&lx, &st, err, sizeof err) == 1 && st.type == STMT_INSERT);
    CHECK(parse_statement(&lx, &st, err, sizeof err) == -1);
    CHECK_HAS(err, "line 2");
    CHECK(parse_statement(&lx, &st, err, sizeof err) == 1 && st.type == STMT_SELECT);
    CHECK(parse_statement(&lx, &st, err, sizeof err) == 0);
    CHECK(parse_statement(&lx, &st, err, sizeof err) == 0);
}

int main(void) {
    RUN(test_insert);
    RUN(test_select);
    RUN(test_count_and_explain);
    RUN(test_syntax_errors);
    RUN(test_one_statement_only);
    RUN(test_too_many_values);
    RUN(test_script_recovery);
    TEST_REPORT("test_parser");
}
