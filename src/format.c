#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "db.h"

/* Terminal columns a UTF-8 string occupies: CJK and Hangul take two. */
static int display_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { cp = '?'; len = 1; }
        int i = 1;
        for (; i < len && (p[i] & 0xC0) == 0x80; i++) cp = (cp << 6) | (p[i] & 0x3F);
        p += i; /* a truncated sequence still advances past what was there */
        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                    (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                    (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
    }
    return w;
}

static const char *cell_text(const Result *r, size_t row, int k, char *buf, size_t n) {
    const Column *c = &r->schema->cols[r->proj[k]];
    const uint8_t *rec = r->rows + row * (size_t)r->schema->recsize;
    if (c->type == T_VARCHAR) return rec_get_str(rec, c);
    snprintf(buf, n, "%" PRId64, rec_get_int(rec, c));
    return buf;
}

static void rule(StrBuf *out, const int *width, int n) {
    for (int k = 0; k < n; k++) {
        sb_puts(out, "+");
        for (int i = 0; i < width[k] + 2; i++) sb_puts(out, "-");
    }
    sb_puts(out, "+\n");
}

static void cell(StrBuf *out, const char *text, int width) {
    sb_puts(out, "| ");
    sb_puts(out, text);
    for (int i = display_width(text); i < width + 1; i++) sb_puts(out, " ");
}

void result_format_table(const Result *r, StrBuf *out) {
    char buf[32];
    if (!r->ok) {
        sb_printf(out, "ERROR: %s\n", r->error);
        return;
    }
    if (r->explain) {
        sb_printf(out, "plan: %s\n", r->plan);
        return;
    }
    if (r->type == STMT_INSERT) {
        sb_printf(out, "inserted 1 row (id = %" PRId64 ")\n", r->insert_id);
        return;
    }
    if (r->count_star) {
        int w = snprintf(buf, sizeof buf, "%" PRId64, r->count);
        if (w < 8) w = 8;
        rule(out, &w, 1);
        cell(out, "count(*)", w);
        sb_puts(out, "|\n");
        rule(out, &w, 1);
        cell(out, buf, w);
        sb_puts(out, "|\n");
        rule(out, &w, 1);
        return;
    }

    int width[MAX_COLS];
    for (int k = 0; k < r->nproj; k++) width[k] = display_width(r->schema->cols[r->proj[k]].name);
    for (size_t row = 0; row < r->nrows; row++)
        for (int k = 0; k < r->nproj; k++) {
            int w = display_width(cell_text(r, row, k, buf, sizeof buf));
            if (w > width[k]) width[k] = w;
        }

    if (r->nrows > 0) {
        rule(out, width, r->nproj);
        for (int k = 0; k < r->nproj; k++) cell(out, r->schema->cols[r->proj[k]].name, width[k]);
        sb_puts(out, "|\n");
        rule(out, width, r->nproj);
        for (size_t row = 0; row < r->nrows; row++) {
            for (int k = 0; k < r->nproj; k++)
                cell(out, cell_text(r, row, k, buf, sizeof buf), width[k]);
            sb_puts(out, "|\n");
        }
        rule(out, width, r->nproj);
    }
    sb_printf(out, "(%zu row%s)\n", r->nrows, r->nrows == 1 ? "" : "s");
}

void result_format_json(const Result *r, StrBuf *out) {
    if (!r->ok) {
        sb_puts(out, "{\"ok\":false,\"error\":");
        sb_json_str(out, r->error);
        sb_puts(out, "}");
        return;
    }
    sb_puts(out, "{\"ok\":true,");
    if (r->explain) {
        sb_puts(out, "\"type\":\"explain\",\"plan\":");
        sb_json_str(out, r->plan);
        sb_puts(out, "}");
        return;
    }
    if (r->type == STMT_INSERT) {
        sb_printf(out, "\"type\":\"insert\",\"id\":%" PRId64 ",\"rows_affected\":1", r->insert_id);
    } else {
        sb_puts(out, "\"type\":\"select\",\"columns\":[");
        if (r->count_star) {
            sb_printf(out, "\"count(*)\"],\"rows\":[[%" PRId64 "]],\"row_count\":1", r->count);
        } else {
            for (int k = 0; k < r->nproj; k++) {
                if (k) sb_puts(out, ",");
                sb_json_str(out, r->schema->cols[r->proj[k]].name);
            }
            sb_puts(out, "],\"rows\":[");
            for (size_t row = 0; row < r->nrows; row++) {
                sb_puts(out, row ? ",[" : "[");
                const uint8_t *rec = r->rows + row * (size_t)r->schema->recsize;
                for (int k = 0; k < r->nproj; k++) {
                    const Column *c = &r->schema->cols[r->proj[k]];
                    if (k) sb_puts(out, ",");
                    if (c->type == T_INT) sb_printf(out, "%" PRId64, rec_get_int(rec, c));
                    else sb_json_str(out, rec_get_str(rec, c));
                }
                sb_puts(out, "]");
            }
            sb_printf(out, "],\"row_count\":%zu,\"truncated\":%s", r->nrows,
                      r->truncated ? "true" : "false");
        }
        sb_puts(out, ",\"plan\":");
        sb_json_str(out, r->plan);
        sb_printf(out, ",\"rows_examined\":%" PRId64, r->examined);
    }
    sb_printf(out, ",\"elapsed_ms\":%.3f}", r->elapsed_ms);
}
