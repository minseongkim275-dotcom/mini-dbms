#ifndef MINIDB_TABLE_H
#define MINIDB_TABLE_H

#include <pthread.h>

#include "bptree.h"
#include "common.h"

typedef struct {
    char name[MAX_NAME];
    ColType type;
    int maxlen; /* VARCHAR(n): n. Unused for INT. */
    int size;   /* bytes on disk: INT = 8, VARCHAR(n) = n + 1 */
    int offset; /* byte offset inside a record */
} Column;

typedef struct {
    int ncols;
    Column cols[MAX_COLS];
    int recsize;
} Schema;

/* One table = one schema file + one data file of fixed-length records,
   plus an in-memory B+ tree mapping id -> file offset of the record. */
typedef struct Table {
    char name[MAX_NAME];
    Schema schema;
    int fd;
    int64_t end;     /* file offset where the next record goes */
    int64_t nrows;
    int64_t next_id; /* id handed to the next inserted row */
    BPTree *index;
    pthread_rwlock_t lock; /* readers: SELECT, writer: INSERT. Taken by the caller. */
} Table;

/* Record-scan callback. Return false to stop early. */
typedef bool (*ScanFn)(const uint8_t *rec, int64_t offset, void *ctx);

int schema_load(const char *path, Schema *s, char *err, size_t errlen);
int schema_find(const Schema *s, const char *name); /* column index or -1 */

/* Opens <dir>/<name>.schema and <dir>/<name>.tbl (created if missing) and
   rebuilds the index from the data file. Returns NULL on error. */
Table *table_open(const char *dir, const char *name, char *err, size_t errlen);
void table_close(Table *t);

/* Assigns the next id (written into rec's id column), appends the record and
   indexes it. Returns 0, or -1 with `err` set. Requires the write lock. */
int table_append(Table *t, uint8_t *rec, int64_t *id, char *err, size_t errlen);

/* Reads the record stored at `offset`. Returns 0 or -1. */
int table_read(const Table *t, int64_t offset, uint8_t *rec);

/* Calls fn for every record in file order. Returns 0, or -1 on an I/O error. */
int table_scan(const Table *t, ScanFn fn, void *ctx);

int64_t rec_get_int(const uint8_t *rec, const Column *c);
const char *rec_get_str(const uint8_t *rec, const Column *c);

#endif
