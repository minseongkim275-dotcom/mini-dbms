/* Minimal unit-test helpers: no external framework needed. */
#ifndef MINIDB_TEST_H
#define MINIDB_TEST_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int t_checks, t_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        t_checks++;                                                              \
        if (!(cond)) {                                                           \
            t_failed++;                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

#define CHECK_STR(actual, expected)                                              \
    do {                                                                         \
        const char *a_ = (actual), *e_ = (expected);                             \
        t_checks++;                                                              \
        if (strcmp(a_, e_) != 0) {                                               \
            t_failed++;                                                          \
            fprintf(stderr, "  FAIL %s:%d: got \"%s\", expected \"%s\"\n",       \
                    __FILE__, __LINE__, a_, e_);                                 \
        }                                                                        \
    } while (0)

#define CHECK_HAS(haystack, needle)                                              \
    do {                                                                         \
        const char *h_ = (haystack), *n_ = (needle);                             \
        t_checks++;                                                              \
        if (!strstr(h_, n_)) {                                                   \
            t_failed++;                                                          \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" does not contain \"%s\"\n",    \
                    __FILE__, __LINE__, h_, n_);                                 \
        }                                                                        \
    } while (0)

#define RUN(fn) fn()

#define TEST_REPORT(name)                                                        \
    do {                                                                         \
        printf("%-18s %s (%d checks, %d failed)\n", name,                        \
               t_failed ? "FAILED" : "ok", t_checks, t_failed);                  \
        return t_failed ? 1 : 0;                                                 \
    } while (0)

/* Creates a fresh data directory holding <table>.schema with the given text. */
__attribute__((unused)) static void make_data_dir(char *dir, size_t n, const char *table,
                                                  const char *schema) {
    snprintf(dir, n, "/tmp/minidb_test_XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        exit(1);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.schema", dir, table);
    FILE *f = fopen(path, "w");
    fputs(schema, f);
    fclose(f);
}

__attribute__((unused)) static void remove_data_dir(const char *dir) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
}

#define USERS_SCHEMA "id INT\nname VARCHAR(32)\nage INT\nemail VARCHAR(64)\n"

#endif
