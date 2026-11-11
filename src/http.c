#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- request head -------------------------------------------------------- */

static const char *skip_blanks(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int http_parse_head(const char *head, HttpRequest *req) {
    memset(req, 0, sizeof *req);
    req->content_length = -1;

    /* Request line: METHOD SP request-target SP HTTP/1.x */
    const char *sp1 = strchr(head, ' ');
    const char *eol = strpbrk(head, "\r\n");
    if (!eol) eol = head + strlen(head);
    if (!sp1 || sp1 > eol) return -1;
    size_t mlen = (size_t)(sp1 - head);
    if (mlen == 0 || mlen >= sizeof req->method) return -1;
    for (size_t i = 0; i < mlen; i++)
        if (!isupper((unsigned char)head[i])) return -1;
    memcpy(req->method, head, mlen);

    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(eol - target));
    if (!sp2 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) return -1;
    size_t plen = strcspn(target, " ?"); /* drop the query string */
    if (target[0] != '/' || plen >= sizeof req->path) return -1;
    memcpy(req->path, target, plen);

    /* Headers: one "Name: value" per line. */
    const char *line = eol;
    while (*line) {
        if (*line == '\r') line++;
        if (*line == '\n') line++;
        const char *end = strpbrk(line, "\r\n");
        if (!end) end = line + strlen(line);
        if (end == line) break; /* blank line: end of headers */

        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (!colon) return -1;
        size_t nlen = (size_t)(colon - line);
        const char *value = skip_blanks(colon + 1);

        if (nlen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
            if (!isdigit((unsigned char)*value)) return -1;
            char *stop;
            errno = 0;
            long n = strtol(value, &stop, 10);
            if (errno == ERANGE || skip_blanks(stop) != end) return -1;
            if (req->content_length >= 0 && req->content_length != n) return -1; /* conflicting */
            req->content_length = n;
        } else if (nlen == 12 && strncasecmp(line, "Content-Type", 12) == 0) {
            req->json = strncasecmp(value, "application/json", 16) == 0;
        }
        line = end;
    }
    return 0;
}

/* ---- minimal JSON -------------------------------------------------------- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int hex4(const char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)p[i];
        if (!isxdigit(c)) return -1;
        v = v * 16 + (uint32_t)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    *out = v;
    return 0;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses the JSON string starting at the opening quote `p`. The decoded text
   is written to `out` when it is non-NULL. Returns the position after the
   closing quote, or NULL on a syntax error or overflow. */
static const char *parse_string(const char *p, char *out, size_t outcap) {
    size_t n = 0;
    if (*p++ != '"') return NULL;
    while (*p != '"') {
        char buf[4];
        size_t len = 1;
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) return NULL; /* also catches the terminating NUL */
        if (c != '\\') {
            buf[0] = (char)c;
            p++;
        } else {
            p++;
            switch (*p) {
            case '"': buf[0] = '"'; break;
            case '\\': buf[0] = '\\'; break;
            case '/': buf[0] = '/'; break;
            case 'b': buf[0] = '\b'; break;
            case 'f': buf[0] = '\f'; break;
            case 'n': buf[0] = '\n'; break;
            case 'r': buf[0] = '\r'; break;
            case 't': buf[0] = '\t'; break;
            case 'u': {
                uint32_t cp, low;
                if (hex4(p + 1, &cp) != 0) return NULL;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) { /* surrogate pair */
                    if (p[1] != '\\' || p[2] != 'u' || hex4(p + 3, &low) != 0 || low < 0xDC00 ||
                        low > 0xDFFF)
                        return NULL;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    p += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return NULL;
                }
                if (cp == 0) return NULL; /* cannot be represented in a C string */
                len = utf8_encode(cp, buf);
                break;
            }
            default: return NULL;
            }
            p++;
        }
        if (out) {
            if (n + len >= outcap) return NULL;
            memcpy(out + n, buf, len);
        }
        n += len;
    }
    if (out) out[n] = '\0';
    return p + 1;
}

/* Skips one JSON value of any type. Structure is only checked loosely: this
   is used for members we do not care about. */
static const char *skip_value(const char *p) {
    int depth = 0;
    do {
        p = skip_ws(p);
        if (*p == '"') {
            p = parse_string(p, NULL, 0);
            if (!p) return NULL;
        } else if (*p == '{' || *p == '[') {
            depth++;
            p++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0) return NULL;
            depth--;
            p++;
        } else if (*p == ',' || *p == ':') {
            if (depth == 0) return NULL;
            p++;
        } else if (*p == '\0') {
            return NULL;
        } else {
            const char *start = p; /* number, true, false or null */
            while (*p && !strchr(",:{}[]\" \t\r\n", *p)) p++;
            if (p == start) return NULL;
        }
    } while (depth > 0);
    return p;
}

int json_get_string(const char *json, const char *key, char *out, size_t outcap) {
    char name[64];
    const char *p = skip_ws(json);
    if (*p++ != '{') return -1;
    p = skip_ws(p);
    if (*p == '}') return -1;
    for (;;) {
        p = skip_ws(p);
        const char *after = parse_string(p, name, sizeof name);
        if (!after) {
            /* A key too long for `name` cannot be the one we want: skip it. */
            after = parse_string(p, NULL, 0);
            if (!after) return -1;
            name[0] = '\0';
        }
        p = skip_ws(after);
        if (*p++ != ':') return -1;
        p = skip_ws(p);
        if (strcmp(name, key) == 0) {
            if (*p != '"') return -1;
            return parse_string(p, out, outcap) ? 0 : -1;
        }
        p = skip_value(p);
        if (!p) return -1;
        p = skip_ws(p);
        if (*p == '}') return -1; /* key not present */
        if (*p++ != ',') return -1;
    }
}
