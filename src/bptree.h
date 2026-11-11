#ifndef MINIDB_BPTREE_H
#define MINIDB_BPTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* In-memory B+ tree mapping a unique int64 key to an int64 value.
   Internal nodes hold only separator keys; all values live in the leaves,
   and the leaves are chained left-to-right for range scans. */

#define BPT_ORDER 64 /* max children of an internal node; a node holds at most ORDER-1 keys */

typedef struct BPTree BPTree;

/* Range-scan callback. Return false to stop the scan early. */
typedef bool (*BptVisit)(int64_t key, int64_t value, void *ctx);

BPTree *bpt_create(void);
void bpt_destroy(BPTree *t);

/* Returns 0 on success, -1 if the key already exists (tree unchanged). */
int bpt_insert(BPTree *t, int64_t key, int64_t value);

bool bpt_search(const BPTree *t, int64_t key, int64_t *value);

/* Visits every key in [lo, hi] in ascending order. Returns the number visited. */
size_t bpt_range(const BPTree *t, int64_t lo, int64_t hi, BptVisit visit, void *ctx);

size_t bpt_count(const BPTree *t);
int bpt_height(const BPTree *t); /* 1 = the root is a leaf */

/* Checks every structural invariant. Used by the tests. */
bool bpt_validate(const BPTree *t);

#endif
