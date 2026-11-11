#ifndef MINIDB_HTTP_H
#define MINIDB_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char method[8];
    char path[128];      /* without the query string */
    long content_length; /* -1 when the header is absent */
    bool json;           /* Content-Type: application/json */
} HttpRequest;

/* Parses the request line and headers. `head` must be NUL-terminated and hold
   everything up to (not necessarily including) the blank line.
   Returns 0, or -1 if the request is malformed. */
int http_parse_head(const char *head, HttpRequest *req);

/* Finds the top-level string member `key` in a JSON object and writes its
   decoded value to `out`. Returns 0, or -1 if the text is not a JSON object,
   the key is missing or not a string, or the value does not fit. */
int json_get_string(const char *json, const char *key, char *out, size_t outcap);

#endif
