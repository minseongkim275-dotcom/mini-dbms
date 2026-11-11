#include "db.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_TABLES 64

struct DB {
    char dir[4096];
    pthread_mutex_t mu; /* guards tables[] / ntables */
    Table *tables[MAX_TABLES];
    int ntables;
    atomic_long selects, inserts, errors, index_scans, range_scans, full_scans;
};

DB *db_open(const char *dir, char *err, size_t errlen) {
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, errlen, "data directory '%s' does not exist", dir);
        return NULL;
    }
    if (strlen(dir) >= sizeof((DB *)0)->dir - MAX_NAME - 16) {
        snprintf(err, errlen, "data directory path is too long");
        return NULL;
    }
    DB *db = xmalloc(sizeof *db);
    memset(db, 0, sizeof *db);
    snprintf(db->dir, sizeof db->dir, "%s", dir);
    pthread_mutex_init(&db->mu, NULL);
    return db;
}

void db_close(DB *db) {
    if (!db) return;
    for (int i = 0; i < db->ntables; i++) table_close(db->tables[i]);
    pthread_mutex_destroy(&db->mu);
    free(db);
}

void db_stats(DB *db, DBStats *out) {
    out->selects = atomic_load(&db->selects);
    out->inserts = atomic_load(&db->inserts);
    out->errors = atomic_load(&db->errors);
    out->index_scans = atomic_load(&db->index_scans);
    out->range_scans = atomic_load(&db->range_scans);
    out->full_scans = atomic_load(&db->full_scans);
}

/* Tables are opened on first use and stay open until db_close. Table names
   come from the lexer, so they contain only [a-z0-9_] and cannot escape the
   data directory. */
static Table *get_table(DB *db, const char *name, char *err, size_t errlen) {
    Table *t = NULL;
    pthread_mutex_lock(&db->mu);
    for (int i = 0; i < db->ntables && !t; i++)
        if (strcmp(db->tables[i]->name, name) == 0) t = db->tables[i];
    if (!t) {
        if (db->ntables == MAX_TABLES) {
            snprintf(err, errlen, "too many open tables (max %d)", MAX_TABLES);
        } else {
            t = table_open(db->dir, name, err, errlen);
            if (t) db->tables[db->ntables++] = t;
        }
    }
    pthread_mutex_unlock(&db->mu);
    return t;
}

void result_init(Result *r) { memset(r, 0, sizeof *r); }

void result_free(Result *r) {
    free(r->rows);
    r->rows = NULL;
    r->nrows = r->cap = 0;
}

static int fail(Result *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(Result *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof r->error, fmt, ap);
    va_end(ap);
    r->ok = false;
    return -1;
}

static const char *type_name(ColType t) { return t == T_INT ? "INT" : "VARCHAR"; }

/* ---- INSERT -------------------------------------------------------------- */

static int encode_value(const Column *c, const Value *v, uint8_t *rec, Result *r) {
    if (v->type != c->type)
        return fail(r, "type mismatch for column '%s': expected %s but got %s", c->name,
                    type_name(c->type), type_name(v->type));
    if (c->type == T_INT) {
        memcpy(rec + c->offset, &v->i, sizeof v->i);
    } else {
        size_t len = strlen(v->s);
        if (len > (size_t)c->maxlen)
            return fail(r, "value too long for column '%s': %zu bytes, VARCHAR(%d)", c->name, len,
                        c->maxlen);
        memcpy(rec + c->offset, v->s, len); /* rest of the field is already zero */
    }
    return 0;
}

static int exec_insert(DB *db, const Stmt *st, Result *r) {
    Table *t = get_table(db, st->table, r->error, sizeof r->error);
    if (!t) return -1;
    const Schema *s = &t->schema;

    /* Map each supplied value to a column. Without a column list the values
       fill every column except id, in schema order. */
    int target[MAX_COLS];
    int ntargets;
    if (st->ncols == 0) {
        ntargets = s->ncols - 1;
        for (int i = 0; i < ntargets; i++) target[i] = i + 1;
    } else {
        bool seen[MAX_COLS] = {false};
        ntargets = st->ncols;
        for (int i = 0; i < st->ncols; i++) {
            int c = schema_find(s, st->cols[i]);
            if (c < 0)
                return fail(r, "column '%s' does not exist in table '%s'", st->cols[i], t->name);
            if (c == 0) return fail(r, "column 'id' is assigned automatically and cannot be set");
            if (seen[c]) return fail(r, "column '%s' is listed more than once", st->cols[i]);
            seen[c] = true;
            target[i] = c;
        }
    }
    if (st->nvalues != ntargets)
        return fail(r, "expected %d value(s) but got %d", ntargets, st->nvalues);

    /* Columns left out of the column list keep the zeroed default (0 or ''). */
    uint8_t rec[MAX_RECSIZE];
    memset(rec, 0, (size_t)s->recsize);
    for (int i = 0; i < ntargets; i++)
        if (encode_value(&s->cols[target[i]], &st->values[i], rec, r) != 0) return -1;

    pthread_rwlock_wrlock(&t->lock);
    int rc = table_append(t, rec, &r->insert_id, r->error, sizeof r->error);
    pthread_rwlock_unlock(&t->lock);
    if (rc != 0) return -1;
    atomic_fetch_add(&db->inserts, 1);
    return 0;
}

/* ---- SELECT -------------------------------------------------------------- */

typedef struct {
    const Table *t;
    Result *r;
    const Column *wcol; /* WHERE column, or NULL when every record matches */
    CmpOp op;
    const Value *wval;
    bool has_limit;
    int64_t limit;
    size_t max_rows;
    bool io_error;
} SelectCtx;

static bool matches(const SelectCtx *c, const uint8_t *rec) {
    if (!c->wcol) return true;
    int cmp;
    if (c->wcol->type == T_INT) {
        int64_t a = rec_get_int(rec, c->wcol), b = c->wval->i;
        cmp = (a > b) - (a < b);
    } else {
        cmp = strcmp(rec_get_str(rec, c->wcol), c->wval->s);
    }
    switch (c->op) {
    case OP_EQ: return cmp == 0;
    case OP_NE: return cmp != 0;
    case OP_LT: return cmp < 0;
    case OP_LE: return cmp <= 0;
    case OP_GT: return cmp > 0;
    case OP_GE: return cmp >= 0;
    }
    return false;
}

/* Handles one candidate record. Returns false when the scan can stop. */
static bool consider(SelectCtx *c, const uint8_t *rec) {
    Result *r = c->r;
    r->examined++;
    if (!matches(c, rec)) return true;
    if (r->count_star) {
        r->count++;
        return true;
    }
    if (c->has_limit && (int64_t)r->nrows >= c->limit) return false;
    if (c->max_rows && r->nrows >= c->max_rows) {
        r->truncated = true;
        return false;
    }
    size_t recsize = (size_t)c->t->schema.recsize;
    if (r->nrows == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->rows = xrealloc(r->rows, r->cap * recsize);
    }
    memcpy(r->rows + r->nrows * recsize, rec, recsize);
    r->nrows++;
    return !(c->has_limit && (int64_t)r->nrows >= c->limit);
}

static bool on_scan(const uint8_t *rec, int64_t offset, void *ctx) {
    (void)offset;
    return consider(ctx, rec);
}

static bool on_index(int64_t key, int64_t offset, void *ctx) {
    (void)key;
    SelectCtx *c = ctx;
    uint8_t rec[MAX_RECSIZE];
    if (table_read(c->t, offset, rec) != 0) {
        c->io_error = true;
        return false;
    }
    return consider(c, rec);
}

static int exec_select(DB *db, const Stmt *st, size_t max_rows, Result *r) {
    Table *t = get_table(db, st->table, r->error, sizeof r->error);
    if (!t) return -1;
    const Schema *s = &t->schema;
    r->schema = s;
    r->count_star = st->count_star;

    if (st->star) {
        r->nproj = s->ncols;
        for (int i = 0; i < s->ncols; i++) r->proj[i] = i;
    } else if (!st->count_star) {
        r->nproj = st->ncols;
        for (int i = 0; i < st->ncols; i++) {
            r->proj[i] = schema_find(s, st->cols[i]);
            if (r->proj[i] < 0)
                return fail(r, "column '%s' does not exist in table '%s'", st->cols[i], t->name);
        }
    }

    SelectCtx c = {.t = t, .r = r, .max_rows = max_rows};
    /* LIMIT 0 on a row query returns nothing; it has no meaning for COUNT(*). */
    c.has_limit = st->has_limit && !st->count_star;
    c.limit = st->limit;
    int wi = -1;
    if (st->has_where) {
        wi = schema_find(s, st->where.col);
        if (wi < 0)
            return fail(r, "column '%s' does not exist in table '%s'", st->where.col, t->name);
        if (s->cols[wi].type != st->where.value.type)
            return fail(r, "type mismatch in WHERE: column '%s' is %s but the value is %s",
                        s->cols[wi].name, type_name(s->cols[wi].type),
                        type_name(st->where.value.type));
        c.wcol = &s->cols[wi];
        c.op = st->where.op;
        c.wval = &st->where.value;
    }

    /* Choose the access path. Only conditions on id (column 0) can use the
       index; `id != v` matches almost everything, so it scans. */
    enum { P_META, P_POINT, P_RANGE, P_FULL } path = P_FULL;
    if (wi == 0 && st->where.op == OP_EQ) path = P_POINT;
    else if (wi == 0 && st->where.op != OP_NE) path = P_RANGE;
    else if (!st->has_where && st->count_star) path = P_META;

    static const char *const PLAN[] = {PLAN_META, PLAN_INDEX, PLAN_RANGE, PLAN_FULL};
    r->plan = PLAN[path];
    if (st->explain) return 0;
    if (c.has_limit && c.limit == 0) return 0;

    int rc = 0;
    int64_t v = st->has_where ? st->where.value.i : 0;
    pthread_rwlock_rdlock(&t->lock);
    switch (path) {
    case P_META:
        r->count = t->nrows;
        break;
    case P_POINT: {
        int64_t offset;
        atomic_fetch_add(&db->index_scans, 1);
        if (bpt_search(t->index, v, &offset)) on_index(v, offset, &c);
        break;
    }
    case P_RANGE: {
        int64_t lo = INT64_MIN, hi = INT64_MAX;
        bool empty = false;
        switch (st->where.op) {
        case OP_LT: empty = v == INT64_MIN; hi = v - !empty; break;
        case OP_LE: hi = v; break;
        case OP_GT: empty = v == INT64_MAX; lo = v + !empty; break;
        default: lo = v; break; /* OP_GE */
        }
        atomic_fetch_add(&db->range_scans, 1);
        c.wcol = NULL; /* the range already guarantees the condition */
        if (!empty) bpt_range(t->index, lo, hi, on_index, &c);
        break;
    }
    case P_FULL:
        atomic_fetch_add(&db->full_scans, 1);
        rc = table_scan(t, on_scan, &c);
        break;
    }
    pthread_rwlock_unlock(&t->lock);

    if (rc != 0 || c.io_error) return fail(r, "failed to read table '%s'", t->name);
    atomic_fetch_add(&db->selects, 1);
    return 0;
}

/* ---- entry points -------------------------------------------------------- */

int db_exec_stmt(DB *db, const Stmt *st, size_t max_rows, Result *r) {
    double start = now_ms();
    r->ok = true;
    r->type = st->type;
    r->explain = st->explain;
    int rc = st->type == STMT_INSERT ? exec_insert(db, st, r) : exec_select(db, st, max_rows, r);
    r->ok = rc == 0;
    if (rc != 0) atomic_fetch_add(&db->errors, 1);
    r->elapsed_ms = now_ms() - start;
    return rc;
}

int db_execute(DB *db, const char *sql, size_t max_rows, Result *r) {
    Stmt *st = xmalloc(sizeof *st); /* a Stmt is ~25 KB: keep it off the stack */
    int rc;
    if (parse_single(sql, st, r->error, sizeof r->error) != 0) {
        r->ok = false;
        atomic_fetch_add(&db->errors, 1);
        rc = -1;
    } else {
        rc = db_exec_stmt(db, st, max_rows, r);
    }
    free(st);
    return rc;
}
