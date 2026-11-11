#define _GNU_SOURCE
#include "table.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Data file layout: a 16-byte header followed by fixed-length records.
     bytes 0-3   magic "MDB1"
     bytes 4-7   record size (uint32, host byte order)
     bytes 8-15  reserved (zero)
   Integers are stored in host byte order, so a data file is not portable
   between little- and big-endian machines. */
#define HEADER_SIZE 16
static const char MAGIC[4] = {'M', 'D', 'B', '1'};

#define SCAN_BUF_BYTES (256 * 1024)
#define PATH_BUF 4096

int64_t rec_get_int(const uint8_t *rec, const Column *c) {
    int64_t v;
    memcpy(&v, rec + c->offset, sizeof v);
    return v;
}

const char *rec_get_str(const uint8_t *rec, const Column *c) {
    return (const char *)rec + c->offset; /* always NUL-terminated on disk */
}

/* ---- schema -------------------------------------------------------------- */

int schema_find(const Schema *s, const char *name) {
    for (int i = 0; i < s->ncols; i++)
        if (strcmp(s->cols[i].name, name) == 0) return i;
    return -1;
}

static int schema_fail(char *err, size_t errlen, const char *path, int line, const char *msg) {
    snprintf(err, errlen, "%s:%d: %s", path, line, msg);
    return -1;
}

int schema_load(const char *path, Schema *s, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open schema file %s: %s", path, strerror(errno));
        return -1;
    }
    memset(s, 0, sizeof *s);
    char line[512];
    int lineno = 0, rc = 0;

    while (rc == 0 && fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char name[128], type[128], extra[8];
        int n = sscanf(line, " %127s %127[^\n]", name, type);
        if (n <= 0) continue; /* blank or comment-only line */
        if (n != 2) {
            rc = schema_fail(err, errlen, path, lineno, "expected '<column> <type>'");
            break;
        }
        if (s->ncols == MAX_COLS) {
            rc = schema_fail(err, errlen, path, lineno, "too many columns");
            break;
        }
        if (strlen(name) >= MAX_NAME || !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
            rc = schema_fail(err, errlen, path, lineno, "invalid column name");
            break;
        }
        for (char *p = name; *p; p++) {
            if (!(isalnum((unsigned char)*p) || *p == '_'))
                rc = schema_fail(err, errlen, path, lineno, "invalid column name");
            *p = (char)tolower((unsigned char)*p);
        }
        for (char *p = type; *p; p++) *p = (char)toupper((unsigned char)*p);
        for (char *p = type + strlen(type); p > type && isspace((unsigned char)p[-1]);) *--p = '\0';
        if (rc) break;

        Column *c = &s->cols[s->ncols];
        int len;
        if (strcmp(type, "INT") == 0) {
            c->type = T_INT;
            c->size = 8;
        } else if (sscanf(type, "VARCHAR ( %d ) %7s", &len, extra) == 1 && strchr(type, ')')) {
            if (len < 1 || len > MAX_STR) {
                rc = schema_fail(err, errlen, path, lineno, "VARCHAR length must be 1..255");
                break;
            }
            c->type = T_VARCHAR;
            c->maxlen = len;
            c->size = len + 1;
        } else {
            rc = schema_fail(err, errlen, path, lineno, "unknown type (use INT or VARCHAR(n))");
            break;
        }
        if (schema_find(s, name) >= 0) {
            rc = schema_fail(err, errlen, path, lineno, "duplicate column name");
            break;
        }
        snprintf(c->name, sizeof c->name, "%s", name);
        c->offset = s->recsize;
        s->recsize += c->size;
        s->ncols++;
    }
    fclose(f);
    if (rc) return rc;
    if (s->ncols == 0 || strcmp(s->cols[0].name, "id") != 0 || s->cols[0].type != T_INT) {
        snprintf(err, errlen, "%s: the first column must be 'id INT'", path);
        return -1;
    }
    return 0;
}

/* ---- file I/O ------------------------------------------------------------ */

static int pread_full(int fd, void *buf, size_t n, off_t off) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1; /* error or unexpected end of file */
        p += r;
        n -= (size_t)r;
        off += r;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t n, off_t off) {
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = pwrite(fd, p, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
        off += r;
    }
    return 0;
}

int table_read(const Table *t, int64_t offset, uint8_t *rec) {
    return pread_full(t->fd, rec, (size_t)t->schema.recsize, (off_t)offset);
}

int table_scan(const Table *t, ScanFn fn, void *ctx) {
    size_t recsize = (size_t)t->schema.recsize;
    size_t per_chunk = SCAN_BUF_BYTES / recsize;
    if (per_chunk == 0) per_chunk = 1;
    uint8_t *buf = xmalloc(per_chunk * recsize);

    int rc = 0;
    int64_t off = HEADER_SIZE;
    while (off < t->end) {
        size_t left = (size_t)((t->end - off) / (int64_t)recsize);
        size_t n = left < per_chunk ? left : per_chunk;
        if (pread_full(t->fd, buf, n * recsize, (off_t)off) != 0) {
            rc = -1;
            break;
        }
        bool stop = false;
        for (size_t i = 0; i < n && !stop; i++)
            stop = !fn(buf + i * recsize, off + (int64_t)(i * recsize), ctx);
        if (stop) break;
        off += (int64_t)(n * recsize);
    }
    free(buf);
    return rc;
}

/* ---- open / close -------------------------------------------------------- */

typedef struct {
    Table *t;
    bool duplicate;
} BuildCtx;

static bool index_record(const uint8_t *rec, int64_t offset, void *ctx) {
    BuildCtx *b = ctx;
    int64_t id = rec_get_int(rec, &b->t->schema.cols[0]);
    if (bpt_insert(b->t->index, id, offset) != 0) {
        b->duplicate = true;
        return false;
    }
    if (id >= b->t->next_id) b->t->next_id = id + 1;
    b->t->nrows++;
    return true;
}

Table *table_open(const char *dir, const char *name, char *err, size_t errlen) {
    char path[PATH_BUF];
    snprintf(path, sizeof path, "%s/%s.schema", dir, name);
    if (access(path, F_OK) != 0) {
        snprintf(err, errlen, "table '%s' does not exist", name);
        return NULL;
    }

    Table *t = xmalloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->fd = -1;
    t->next_id = 1;
    snprintf(t->name, sizeof t->name, "%s", name);
    if (schema_load(path, &t->schema, err, errlen) != 0) goto fail;

    snprintf(path, sizeof path, "%s/%s.tbl", dir, name);
    t->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (t->fd < 0) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        goto fail;
    }
    struct stat st;
    if (fstat(t->fd, &st) != 0) {
        snprintf(err, errlen, "cannot stat %s: %s", path, strerror(errno));
        goto fail;
    }

    uint8_t header[HEADER_SIZE] = {0};
    uint32_t recsize = (uint32_t)t->schema.recsize;
    if (st.st_size == 0) {
        memcpy(header, MAGIC, 4);
        memcpy(header + 4, &recsize, 4);
        if (pwrite_full(t->fd, header, HEADER_SIZE, 0) != 0) {
            snprintf(err, errlen, "cannot write %s: %s", path, strerror(errno));
            goto fail;
        }
        t->end = HEADER_SIZE;
    } else {
        uint32_t stored;
        if (st.st_size < HEADER_SIZE || pread_full(t->fd, header, HEADER_SIZE, 0) != 0 ||
            memcmp(header, MAGIC, 4) != 0) {
            snprintf(err, errlen, "%s is not a minidb data file", path);
            goto fail;
        }
        memcpy(&stored, header + 4, 4);
        if (stored != recsize) {
            snprintf(err, errlen,
                     "%s was written with record size %u but the schema says %u "
                     "(the schema changed after data was inserted)",
                     path, stored, recsize);
            goto fail;
        }
        /* A record cut short by a crash mid-write is ignored: the next insert
           overwrites it. */
        int64_t whole = ((int64_t)st.st_size - HEADER_SIZE) / recsize;
        t->end = HEADER_SIZE + whole * recsize;
    }

    t->index = bpt_create();
    BuildCtx b = {.t = t, .duplicate = false};
    if (table_scan(t, index_record, &b) != 0) {
        snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        goto fail;
    }
    if (b.duplicate) {
        snprintf(err, errlen, "%s is corrupt: duplicate id", path);
        goto fail;
    }
    /* glibc's default rwlock prefers readers: a steady stream of SELECTs would
       keep the read lock held forever and INSERTs would never get in. Prefer
       writers so a waiting INSERT blocks new readers until it has run. */
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&t->lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    return t;

fail:
    if (t->fd >= 0) close(t->fd);
    bpt_destroy(t->index);
    free(t);
    return NULL;
}

void table_close(Table *t) {
    if (!t) return;
    fsync(t->fd);
    close(t->fd);
    bpt_destroy(t->index);
    pthread_rwlock_destroy(&t->lock);
    free(t);
}

int table_append(Table *t, uint8_t *rec, int64_t *id, char *err, size_t errlen) {
    int64_t new_id = t->next_id;
    memcpy(rec, &new_id, sizeof new_id); /* id is always column 0 at offset 0 */

    if (pwrite_full(t->fd, rec, (size_t)t->schema.recsize, (off_t)t->end) != 0) {
        snprintf(err, errlen, "write to table '%s' failed: %s", t->name, strerror(errno));
        return -1; /* nothing is advanced, so a partial write gets overwritten later */
    }
    bpt_insert(t->index, new_id, t->end); /* cannot collide: ids only grow */
    t->end += t->schema.recsize;
    t->nrows++;
    t->next_id++;
    *id = new_id;
    return 0;
}
