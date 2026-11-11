#include "bptree.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"

#define MAX_KEYS (BPT_ORDER - 1)

/* The key array has one spare slot so a node can overflow by one key
   before it is split. */
typedef struct Node {
    bool leaf;
    int n; /* number of keys */
    int64_t keys[BPT_ORDER];
    union {
        struct Node *child[BPT_ORDER + 1]; /* internal: n + 1 children */
        struct {
            int64_t values[BPT_ORDER];
            struct Node *next; /* right sibling */
        } leaf;
    } u;
} Node;

struct BPTree {
    Node *root;
    size_t count;
    int height;
};

static Node *node_new(bool leaf) {
    Node *n = xmalloc(sizeof *n);
    n->leaf = leaf;
    n->n = 0;
    if (leaf) n->u.leaf.next = NULL;
    return n;
}

/* First index i with keys[i] >= key. */
static int lower_bound(const Node *n, int64_t key) {
    int lo = 0, hi = n->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (n->keys[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* First index i with keys[i] > key. In an internal node this is the child to
   descend into: child[i] covers keys in [keys[i-1], keys[i]). */
static int upper_bound(const Node *n, int64_t key) {
    int lo = 0, hi = n->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (n->keys[mid] <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

BPTree *bpt_create(void) {
    BPTree *t = xmalloc(sizeof *t);
    t->root = node_new(true);
    t->count = 0;
    t->height = 1;
    return t;
}

static void node_free(Node *n) {
    if (!n->leaf)
        for (int i = 0; i <= n->n; i++) node_free(n->u.child[i]);
    free(n);
}

void bpt_destroy(BPTree *t) {
    if (!t) return;
    node_free(t->root);
    free(t);
}

static const Node *find_leaf(const BPTree *t, int64_t key) {
    const Node *n = t->root;
    while (!n->leaf) n = n->u.child[upper_bound(n, key)];
    return n;
}

bool bpt_search(const BPTree *t, int64_t key, int64_t *value) {
    const Node *leaf = find_leaf(t, key);
    int i = lower_bound(leaf, key);
    if (i == leaf->n || leaf->keys[i] != key) return false;
    if (value) *value = leaf->u.leaf.values[i];
    return true;
}

typedef enum { INS_OK, INS_SPLIT, INS_DUP } InsertResult;

/* Inserts into the subtree rooted at `n`. When the node overflows it is split:
   the upper half moves to a new right sibling and the separator key that the
   parent must add is returned through `up_key` / `up_right`. */
static InsertResult insert_rec(Node *n, int64_t key, int64_t value, int64_t *up_key,
                               Node **up_right) {
    if (n->leaf) {
        int i = lower_bound(n, key);
        if (i < n->n && n->keys[i] == key) return INS_DUP;

        memmove(&n->keys[i + 1], &n->keys[i], (size_t)(n->n - i) * sizeof(int64_t));
        memmove(&n->u.leaf.values[i + 1], &n->u.leaf.values[i],
                (size_t)(n->n - i) * sizeof(int64_t));
        n->keys[i] = key;
        n->u.leaf.values[i] = value;
        n->n++;
        if (n->n <= MAX_KEYS) return INS_OK;

        /* Leaf split: the separator is COPIED up, because the key must stay in
           the leaf level where the values are. */
        Node *right = node_new(true);
        int keep = n->n / 2;
        right->n = n->n - keep;
        memcpy(right->keys, &n->keys[keep], (size_t)right->n * sizeof(int64_t));
        memcpy(right->u.leaf.values, &n->u.leaf.values[keep], (size_t)right->n * sizeof(int64_t));
        n->n = keep;
        right->u.leaf.next = n->u.leaf.next;
        n->u.leaf.next = right;
        *up_key = right->keys[0];
        *up_right = right;
        return INS_SPLIT;
    }

    int i = upper_bound(n, key);
    int64_t child_key;
    Node *child_right;
    InsertResult r = insert_rec(n->u.child[i], key, value, &child_key, &child_right);
    if (r != INS_SPLIT) return r;

    /* The child split: add its separator and new right sibling here. */
    memmove(&n->keys[i + 1], &n->keys[i], (size_t)(n->n - i) * sizeof(int64_t));
    memmove(&n->u.child[i + 2], &n->u.child[i + 1], (size_t)(n->n - i) * sizeof(Node *));
    n->keys[i] = child_key;
    n->u.child[i + 1] = child_right;
    n->n++;
    if (n->n <= MAX_KEYS) return INS_OK;

    /* Internal split: the middle key MOVES up and is kept in neither half. */
    Node *right = node_new(false);
    int mid = n->n / 2;
    right->n = n->n - mid - 1;
    memcpy(right->keys, &n->keys[mid + 1], (size_t)right->n * sizeof(int64_t));
    memcpy(right->u.child, &n->u.child[mid + 1], (size_t)(right->n + 1) * sizeof(Node *));
    *up_key = n->keys[mid];
    *up_right = right;
    n->n = mid;
    return INS_SPLIT;
}

int bpt_insert(BPTree *t, int64_t key, int64_t value) {
    int64_t up_key;
    Node *up_right;
    InsertResult r = insert_rec(t->root, key, value, &up_key, &up_right);
    if (r == INS_DUP) return -1;
    if (r == INS_SPLIT) {
        /* The root itself split: the tree grows one level taller. */
        Node *root = node_new(false);
        root->n = 1;
        root->keys[0] = up_key;
        root->u.child[0] = t->root;
        root->u.child[1] = up_right;
        t->root = root;
        t->height++;
    }
    t->count++;
    return 0;
}

size_t bpt_range(const BPTree *t, int64_t lo, int64_t hi, BptVisit visit, void *ctx) {
    if (lo > hi) return 0;
    size_t visited = 0;
    const Node *leaf = find_leaf(t, lo);
    int i = lower_bound(leaf, lo);
    while (leaf) {
        for (; i < leaf->n; i++) {
            if (leaf->keys[i] > hi) return visited;
            visited++;
            if (!visit(leaf->keys[i], leaf->u.leaf.values[i], ctx)) return visited;
        }
        leaf = leaf->u.leaf.next;
        i = 0;
    }
    return visited;
}

size_t bpt_count(const BPTree *t) { return t->count; }
int bpt_height(const BPTree *t) { return t->height; }

/* ---- validation ---------------------------------------------------------- */

typedef struct {
    int leaf_depth; /* depth of the first leaf seen; all others must match */
    size_t keys;
    const Node *root;
} Check;

static bool check_node(const Node *n, int depth, bool has_lo, int64_t lo, bool has_hi, int64_t hi,
                       Check *c) {
    if (n->n > MAX_KEYS) return false;
    if (n != c->root) {
        int min = n->leaf ? BPT_ORDER / 2 : (BPT_ORDER - 1) / 2;
        if (n->n < min) return false;
    } else if (!n->leaf && n->n < 1) {
        return false;
    }
    for (int i = 0; i < n->n; i++) {
        if (i > 0 && n->keys[i - 1] >= n->keys[i]) return false; /* strictly ascending */
        if (has_lo && n->keys[i] < lo) return false;
        if (has_hi && n->keys[i] >= hi) return false;
    }
    if (n->leaf) {
        if (c->leaf_depth < 0) c->leaf_depth = depth;
        if (depth != c->leaf_depth) return false; /* all leaves at the same depth */
        c->keys += (size_t)n->n;
        return true;
    }
    for (int i = 0; i <= n->n; i++) {
        bool clo = i > 0 || has_lo, chi = i < n->n || has_hi;
        int64_t vlo = i > 0 ? n->keys[i - 1] : lo, vhi = i < n->n ? n->keys[i] : hi;
        if (!check_node(n->u.child[i], depth + 1, clo, vlo, chi, vhi, c)) return false;
    }
    return true;
}

bool bpt_validate(const BPTree *t) {
    Check c = {.leaf_depth = -1, .keys = 0, .root = t->root};
    if (!check_node(t->root, 1, false, 0, false, 0, &c)) return false;
    if (c.keys != t->count || c.leaf_depth != t->height) return false;

    /* The leaf chain must visit every key once, in ascending order. */
    const Node *leaf = t->root;
    while (!leaf->leaf) leaf = leaf->u.child[0];
    size_t seen = 0;
    bool have_prev = false;
    int64_t prev = 0;
    for (; leaf; leaf = leaf->u.leaf.next) {
        for (int i = 0; i < leaf->n; i++) {
            if (have_prev && leaf->keys[i] <= prev) return false;
            prev = leaf->keys[i];
            have_prev = true;
            seen++;
        }
    }
    return seen == t->count;
}
