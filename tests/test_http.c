#include "test.h"

#include "http.h"

static void test_parse_head(void) {
    HttpRequest req;
    CHECK(http_parse_head("POST /query HTTP/1.1\r\nHost: localhost\r\n"
                          "content-length:  42 \r\nContent-Type: application/json; charset=utf-8",
                          &req) == 0);
    CHECK_STR(req.method, "POST");
    CHECK_STR(req.path, "/query");
    CHECK(req.content_length == 42 && req.json);

    CHECK(http_parse_head("GET /health?verbose=1 HTTP/1.0", &req) == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.path, "/health"); /* query string dropped */
    CHECK(req.content_length == -1 && !req.json);

    CHECK(http_parse_head("POST /query HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 0",
                          &req) == 0);
    CHECK(req.content_length == 0 && !req.json);
}

static void test_parse_head_errors(void) {
    HttpRequest req;
    CHECK(http_parse_head("", &req) != 0);
    CHECK(http_parse_head("GET", &req) != 0);
    CHECK(http_parse_head("GET /health", &req) != 0);          /* no version */
    CHECK(http_parse_head("GET /health FTP/1.1", &req) != 0);
    CHECK(http_parse_head("get /health HTTP/1.1", &req) != 0); /* methods are upper case */
    CHECK(http_parse_head("VERYLONGMETHOD / HTTP/1.1", &req) != 0);
    CHECK(http_parse_head("GET health HTTP/1.1", &req) != 0);  /* path must start with / */
    CHECK(http_parse_head("GET / HTTP/1.1\r\nBroken header line", &req) != 0);
    CHECK(http_parse_head("POST / HTTP/1.1\r\nContent-Length: abc", &req) != 0);
    CHECK(http_parse_head("POST / HTTP/1.1\r\nContent-Length: -5", &req) != 0);
    CHECK(http_parse_head("POST / HTTP/1.1\r\nContent-Length: 12x", &req) != 0);
    CHECK(http_parse_head("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999", &req) != 0);
    CHECK(http_parse_head("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2", &req) != 0);

    char longpath[400] = "GET /";
    memset(longpath + 5, 'a', 300);
    strcpy(longpath + 305, " HTTP/1.1");
    CHECK(http_parse_head(longpath, &req) != 0);
}

static void test_json_get_string(void) {
    char out[128];
    CHECK(json_get_string("{\"sql\":\"SELECT 1\"}", "sql", out, sizeof out) == 0);
    CHECK_STR(out, "SELECT 1");
    CHECK(json_get_string(" { \"a\" : 1 , \"b\":[1,{\"sql\":\"inner\"}], \"n\":null,\n"
                          "\"sql\" : \"x\" , \"z\":true }",
                          "sql", out, sizeof out) == 0);
    CHECK_STR(out, "x"); /* only the top-level member counts */

    CHECK(json_get_string("{\"sql\":\"a\\\"b\\\\c\\n\\t\\/\\u0041\\ud55c\\uD83D\\uDE00\"}", "sql",
                          out, sizeof out) == 0);
    CHECK_STR(out, "a\"b\\c\n\t/A한😀");
    CHECK(json_get_string("{\"sql\":\"\"}", "sql", out, sizeof out) == 0);
    CHECK_STR(out, "");
}

static void test_json_errors(void) {
    char out[16];
    CHECK(json_get_string("", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("SELECT 1", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("[\"sql\"]", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"other\":\"x\"}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":5}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":null}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":\"unterminated", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\" \"x\"}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":\"bad \\q escape\"}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":\"\\u12\"}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":\"\\ud83d alone\"}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":\"\\u0000\"}", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"a\":[1,2", "sql", out, sizeof out) != 0);
    CHECK(json_get_string("{\"sql\":\"0123456789abcdef\"}", "sql", out, sizeof out) != 0); /* 16 + NUL */
    CHECK(json_get_string("{\"sql\":\"0123456789abcde\"}", "sql", out, sizeof out) == 0);
}

int main(void) {
    RUN(test_parse_head);
    RUN(test_parse_head_errors);
    RUN(test_json_get_string);
    RUN(test_json_errors);
    TEST_REPORT("test_http");
}
