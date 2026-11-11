#include "test.h"

/* Many threads insert and select against one DB at the same time, the way the
   API server's workers do. Build with `make SAN=thread unit` to have
   ThreadSanitizer check the locking. */
#include <pthread.h>
#include <stdatomic.h>

#include "db.h"

#define WRITERS 8
#define READERS 4
#define PER_WRITER 2500

static DB *db;
static atomic_int failures;
static atomic_bool writers_done;
static unsigned char seen[WRITERS * PER_WRITER + 1]; /* seen[id] = times handed out */
static pthread_mutex_t seen_mu = PTHREAD_MUTEX_INITIALIZER;

static void *writer(void *arg) {
    int w = (int)(intptr_t)arg;
    char sql[128];
    Result r;
    for (int i = 0; i < PER_WRITER; i++) {
        result_init(&r);
        snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('w%d_%d', %d, 'w%d@example.com')", w,
                 i, w, w);
        if (db_execute(db, sql, 0, &r) != 0 || r.insert_id < 1 ||
            r.insert_id > WRITERS * PER_WRITER) {
            atomic_fetch_add(&failures, 1);
        } else {
            pthread_mutex_lock(&seen_mu);
            seen[r.insert_id]++;
            pthread_mutex_unlock(&seen_mu);
        }
        result_free(&r);
    }
    return NULL;
}

/* Readers run index lookups, range scans and full scans while rows are being
   added. Every row they see must be complete and self-consistent. */
static void *reader(void *arg) {
    unsigned seed = (unsigned)(intptr_t)arg;
    char sql[128];
    Result r;
    while (!atomic_load(&writers_done)) {
        result_init(&r);
        int kind = rand_r(&seed) % 3;
        int id = 1 + rand_r(&seed) % (WRITERS * PER_WRITER);
        if (kind == 0) snprintf(sql, sizeof sql, "SELECT * FROM users WHERE id = %d", id);
        else if (kind == 1) snprintf(sql, sizeof sql, "SELECT * FROM users WHERE id >= %d LIMIT 50", id);
        else snprintf(sql, sizeof sql, "SELECT * FROM users WHERE age = %d LIMIT 50", id % WRITERS);

        if (db_execute(db, sql, 0, &r) != 0) atomic_fetch_add(&failures, 1);
        for (size_t row = 0; row < r.nrows; row++) {
            const uint8_t *rec = r.rows + row * (size_t)r.schema->recsize;
            int64_t age = rec_get_int(rec, &r.schema->cols[2]);
            const char *name = rec_get_str(rec, &r.schema->cols[1]);
            /* name is "w<age>_<n>": a torn or half-written row would break this. */
            if (name[0] != 'w' || atoi(name + 1) != age || age < 0 || age >= WRITERS)
                atomic_fetch_add(&failures, 1);
            if (kind == 0 && rec_get_int(rec, &r.schema->cols[0]) != id)
                atomic_fetch_add(&failures, 1);
        }
        result_free(&r);
    }
    return NULL;
}

int main(void) {
    char dir[64], err[256];
    make_data_dir(dir, sizeof dir, "users", USERS_SCHEMA);
    db = db_open(dir, err, sizeof err);
    if (!db) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    pthread_t wt[WRITERS], rt[READERS];
    for (intptr_t i = 0; i < WRITERS; i++) pthread_create(&wt[i], NULL, writer, (void *)i);
    for (intptr_t i = 0; i < READERS; i++) pthread_create(&rt[i], NULL, reader, (void *)(i + 1));
    for (int i = 0; i < WRITERS; i++) pthread_join(wt[i], NULL);
    atomic_store(&writers_done, true);
    for (int i = 0; i < READERS; i++) pthread_join(rt[i], NULL);

    CHECK(atomic_load(&failures) == 0);

    /* Every id from 1..N was handed out exactly once: no lost or duplicate ids. */
    int bad = 0;
    for (int id = 1; id <= WRITERS * PER_WRITER; id++)
        if (seen[id] != 1) bad++;
    CHECK(bad == 0);

    Result r;
    result_init(&r);
    CHECK(db_execute(db, "SELECT COUNT(*) FROM users", 0, &r) == 0);
    CHECK(r.count == WRITERS * PER_WRITER);
    result_free(&r);

    /* A full scan and the index agree on what is stored. */
    for (int w = 0; w < WRITERS; w++) {
        char sql[96];
        snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM users WHERE age = %d", w);
        result_init(&r);
        CHECK(db_execute(db, sql, 0, &r) == 0 && r.count == PER_WRITER);
        result_free(&r);
    }
    result_init(&r);
    CHECK(db_execute(db, "SELECT COUNT(*) FROM users WHERE id >= 1", 0, &r) == 0);
    CHECK(r.count == WRITERS * PER_WRITER);
    result_free(&r);

    db_close(db);
    remove_data_dir(dir);
    TEST_REPORT("test_concurrency");
}
