#include "test.h"

#include "lexer.h"

static Token first(const char *src) {
    Lexer lx;
    lexer_init(&lx, src);
    return lexer_next(&lx);
}

static void test_token_sequence(void) {
    Lexer lx;
    lexer_init(&lx, "SELECT id, Name FROM users WHERE age >= 30;");
    TokType want[] = {TK_IDENT, TK_IDENT, TK_COMMA, TK_IDENT, TK_IDENT, TK_IDENT,
                      TK_IDENT, TK_IDENT, TK_GE,    TK_INT,   TK_SEMI,  TK_EOF};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) CHECK(lexer_next(&lx).type == want[i]);
    CHECK(lexer_next(&lx).type == TK_EOF); /* EOF repeats */
}

static void test_identifiers_are_lowercased(void) {
    Token t = first("  SeLeCt ");
    CHECK(t.type == TK_IDENT);
    CHECK_STR(t.text, "select");
    CHECK_STR(first("_a1_B").text, "_a1_b");
}

static void test_integers(void) {
    CHECK(first("42").ival == 42);
    CHECK(first("-17").ival == -17);
    CHECK(first("0").ival == 0);
    CHECK(first("9223372036854775807").ival == INT64_MAX);
    CHECK(first("-9223372036854775808").ival == INT64_MIN);
    Token t = first("9223372036854775808");
    CHECK(t.type == TK_ERROR);
    CHECK_HAS(t.text, "out of range");
    CHECK(first("99999999999999999999999999999999999999999").type == TK_ERROR);
    CHECK(first("12abc").type == TK_ERROR);
    CHECK(first("- 5").type == TK_ERROR); /* a lone '-' is not a token */
}

static void test_strings(void) {
    Token t = first("'hello world'");
    CHECK(t.type == TK_STRING);
    CHECK_STR(t.text, "hello world");
    CHECK_STR(first("''").text, "");
    CHECK_STR(first("'O''Brien'").text, "O'Brien");
    CHECK_STR(first("''''").text, "'");
    CHECK_STR(first("'a;b -- c'").text, "a;b -- c"); /* no comment inside a string */
    CHECK_STR(first("'한글'").text, "한글");

    t = first("'never closed");
    CHECK(t.type == TK_ERROR);
    CHECK_HAS(t.text, "unterminated");
}

static void test_string_length_limit(void) {
    char src[400];
    memset(src, 'x', sizeof src);
    src[0] = '\'';
    src[256] = '\''; /* 255 characters */
    src[257] = '\0';
    Token t = first(src);
    CHECK(t.type == TK_STRING);
    CHECK(strlen(t.text) == 255);

    src[256] = 'x';
    src[257] = '\''; /* 256 characters */
    src[258] = '\0';
    CHECK(first(src).type == TK_ERROR);
}

static void test_identifier_length_limit(void) {
    char src[80];
    memset(src, 'a', 64);
    src[63] = '\0';
    CHECK(first(src).type == TK_IDENT);
    src[63] = 'a';
    src[64] = '\0';
    CHECK(first(src).type == TK_ERROR);
}

static void test_operators(void) {
    CHECK(first("=").type == TK_EQ);
    CHECK(first("!=").type == TK_NE);
    CHECK(first("<>").type == TK_NE);
    CHECK(first("<").type == TK_LT);
    CHECK(first("<=").type == TK_LE);
    CHECK(first(">").type == TK_GT);
    CHECK(first(">=").type == TK_GE);
    CHECK(first("*").type == TK_STAR);
    CHECK(first("!").type == TK_ERROR);
    CHECK(first("@").type == TK_ERROR);
}

static void test_comments_and_positions(void) {
    Lexer lx;
    lexer_init(&lx, "-- header\n  select -- trailing\n\tfrom");
    Token a = lexer_next(&lx), b = lexer_next(&lx);
    CHECK_STR(a.text, "select");
    CHECK(a.line == 2 && a.col == 3);
    CHECK_STR(b.text, "from");
    CHECK(b.line == 3 && b.col == 2);
    CHECK(lexer_next(&lx).type == TK_EOF);

    CHECK(first("").type == TK_EOF);
    CHECK(first("   \n\t ").type == TK_EOF);
    CHECK(first("-- only a comment").type == TK_EOF);
}

int main(void) {
    RUN(test_token_sequence);
    RUN(test_identifiers_are_lowercased);
    RUN(test_integers);
    RUN(test_strings);
    RUN(test_string_length_limit);
    RUN(test_identifier_length_limit);
    RUN(test_operators);
    RUN(test_comments_and_positions);
    TEST_REPORT("test_lexer");
}
