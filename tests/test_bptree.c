#include "test.h"

#include "bptree.h"

static void test_empty(void) {
    BPTree *t = bpt_create();
    int64_t v;
    CHECK(bpt_count(t) == 0 && bpt_height(t) == 1);
    CHECK(!bpt_search(t, 1, &v));
    CHECK(bpt_range(t, INT64_MIN, INT64_MAX, NULL, NULL) == 0);
    CHECK(bpt_validate(t));
    bpt_destroy(t);
    bpt_destroy(NULL);
}

static void test_insert_and_search(void) {
    BPTree *t = bpt_create();
    for (int64_t k = 1; k <= 10; k++) CHECK(bpt_insert(t, k, k * 100) == 0);
    int64_t v = 0;
    CHECK(bpt_search(t, 7, &v) && v == 700);
    CHECK(bpt_search(t, 1, &v) && v == 100);
    CHECK(bpt_search(t, 10, NULL));
    CHECK(!bpt_search(t, 0, &v) && !bpt_search(t, 11, &v));
    CHECK(bpt_count(t) == 10);
    bpt_destroy(t);
}

static void test_duplicate_rejected(void) {
    BPTree *t = bpt_create();
    CHECK(bpt_insert(t, 5, 1) == 0);
    CHECK(bpt_insert(t, 5, 2) == -1);
    int64_t v;
    CHECK(bpt_search(t, 5, &v) && v == 1); /* original value kept */
    CHECK(bpt_count(t) == 1);
    bpt_destroy(t);
}

/* Exactly at the split boundary: ORDER-1 keys fit in one leaf, one more splits it. */
static void test_first_split(void) {
    BPTree *t = bpt_create();
    for (int64_t k = 1; k < BPT_ORDER; k++) bpt_insert(t, k, k);
    CHECK(bpt_height(t) == 1 && bpt_validate(t));
    bpt_insert(t, BPT_ORDER, BPT_ORDER);
    CHECK(bpt_height(t) == 2 && bpt_validate(t));
    for (int64_t k = 1; k <= BPT_ORDER; k++) CHECK(bpt_search(t, k, NULL));
    bpt_destroy(t);
}

static void check_all(BPTree *t, int64_t n) {
    CHECK(bpt_count(t) == (size_t)n);
    CHECK(bpt_validate(t));
    int missing = 0, wrong = 0;
    for (int64_t k = 1; k <= n; k++) {
        int64_t v;
        if (!bpt_search(t, k, &v)) missing++;
        else if (v != k * 3) wrong++;
    }
    CHECK(missing == 0 && wrong == 0);
    CHECK(!bpt_search(t, 0, NULL) && !bpt_search(t, n + 1, NULL));
}

/* Different insertion orders force splits at the right edge, the left edge
   and in the middle of the tree. */
static void test_many_ascending(void) {
    const int64_t n = 200000;
    BPTree *t = bpt_create();
    for (int64_t k = 1; k <= n; k++) bpt_insert(t, k, k * 3);
    check_all(t, n);
    CHECK(bpt_height(t) >= 3);
    bpt_destroy(t);
}

static void test_many_descending(void) {
    const int64_t n = 200000;
    BPTree *t = bpt_create();
    for (int64_t k = n; k >= 1; k--) bpt_insert(t, k, k * 3);
    check_all(t, n);
    bpt_destroy(t);
}

static void test_many_shuffled(void) {
    const int64_t n = 200000;
    int64_t *keys = malloc((size_t)n * sizeof *keys);
    for (int64_t i = 0; i < n; i++) keys[i] = i + 1;
    srand(12345);
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = (int64_t)(((uint64_t)rand() << 15 ^ (uint64_t)rand()) % (uint64_t)(i + 1));
        int64_t tmp = keys[i];
        keys[i] = keys[j];
        keys[j] = tmp;
    }
    BPTree *t = bpt_create();
    for (int64_t i = 0; i < n; i++) CHECK(bpt_insert(t, keys[i], keys[i] * 3) == 0);
    check_all(t, n);
    for (int64_t i = 0; i < 100; i++) CHECK(bpt_insert(t, keys[i], 0) == -1);
    CHECK(bpt_count(t) == (size_t)n);
    free(keys);
    bpt_destroy(t);
}

typedef struct {
    int64_t prev, sum;
    size_t n, stop_after;
    bool ordered;
} Visit;

static bool visit(int64_t key, int64_t value, void *ctx) {
    Visit *v = ctx;
    if (v->n > 0 && key <= v->prev) v->ordered = false;
    v->prev = key;
    v->sum += value;
    v->n++;
    return v->stop_after == 0 || v->n < v->stop_after;
}

static void test_range(void) {
    BPTree *t = bpt_create();
    for (int64_t k = 2; k <= 2000; k += 2) bpt_insert(t, k, 1); /* even keys only */

    Visit v = {.ordered = true};
    CHECK(bpt_range(t, 100, 200, visit, &v) == 51); /* 100,102,...,200 */
    CHECK(v.n == 51 && v.ordered && v.sum == 51);

    v = (Visit){.ordered = true};
    CHECK(bpt_range(t, 101, 199, visit, &v) == 49); /* bounds between keys */
    v = (Visit){.ordered = true};
    CHECK(bpt_range(t, INT64_MIN, INT64_MAX, visit, &v) == 1000 && v.ordered);
    v = (Visit){.ordered = true};
    CHECK(bpt_range(t, 500, 500, visit, &v) == 1);
    CHECK(bpt_range(t, 501, 501, visit, &v) == 0);  /* absent key */
    CHECK(bpt_range(t, 200, 100, visit, &v) == 0);  /* lo > hi */
    CHECK(bpt_range(t, 2001, 9999, visit, &v) == 0); /* past the last key */
    CHECK(bpt_range(t, -50, 1, visit, &v) == 0);     /* before the first key */

    v = (Visit){.ordered = true, .stop_after = 5}; /* callback stops the scan */
    CHECK(bpt_range(t, 2, 2000, visit, &v) == 5);
    bpt_destroy(t);
}

static void test_extreme_keys(void) {
    BPTree *t = bpt_create();
    CHECK(bpt_insert(t, INT64_MAX, 1) == 0);
    CHECK(bpt_insert(t, INT64_MIN, 2) == 0);
    CHECK(bpt_insert(t, 0, 3) == 0);
    CHECK(bpt_insert(t, -1, 4) == 0);
    int64_t v;
    CHECK(bpt_search(t, INT64_MIN, &v) && v == 2);
    CHECK(bpt_search(t, INT64_MAX, &v) && v == 1);
    Visit vis = {.ordered = true};
    CHECK(bpt_range(t, INT64_MIN, INT64_MAX, visit, &vis) == 4 && vis.ordered);
    CHECK(bpt_validate(t));
    bpt_destroy(t);
}

int main(void) {
    RUN(test_empty);
    RUN(test_insert_and_search);
    RUN(test_duplicate_rejected);
    RUN(test_first_split);
    RUN(test_many_ascending);
    RUN(test_many_descending);
    RUN(test_many_shuffled);
    RUN(test_range);
    RUN(test_extreme_keys);
    TEST_REPORT("test_bptree");
}
