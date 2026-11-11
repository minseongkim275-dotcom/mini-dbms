/* minidb-server: HTTP API in front of the SQL engine.

     POST /query   body = SQL text, or {"sql": "..."} with Content-Type: application/json
     GET  /health
     GET  /stats

   One thread accepts connections and hands each one to a thread pool through a
   bounded queue. Each worker reads one request, runs it against the shared DB
   and closes the connection. When the queue is full the accept thread answers
   503 itself, so overload never blocks accepting. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "db.h"
#include "http.h"
#include "threadpool.h"

#define MAX_HEAD 8192            /* request line + headers */
#define MAX_BODY (1024 * 1024)   /* request body */
#define IO_TIMEOUT_SEC 5

typedef struct {
    DB *db;
    ThreadPool *pool;
    size_t max_rows;
    int nthreads, queue_cap;
    atomic_long requests, rejected;
    double started_ms;
} Server;

typedef struct {
    Server *srv;
    int fd;
} Conn;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---- socket helpers ------------------------------------------------------ */

static int send_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, buf, n, MSG_NOSIGNAL); /* no SIGPIPE if the client left */
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static void respond(int fd, int status, const char *json) {
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, reason(status), strlen(json));
    if (send_all(fd, head, (size_t)n) == 0) send_all(fd, json, strlen(json));
}

static void respond_error(int fd, int status, const char *msg) {
    StrBuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"ok\":false,\"error\":");
    sb_json_str(&sb, msg);
    sb_puts(&sb, "}");
    respond(fd, status, sb.p);
    sb_free(&sb);
}

/* Reads until `need` bytes are in buf. Returns 0, -1 if the peer closed or an
   error occurred, -2 on timeout. */
static int recv_some(int fd, char *buf, size_t cap, size_t *len) {
    for (;;) {
        ssize_t r = recv(fd, buf + *len, cap - *len, 0);
        if (r > 0) {
            *len += (size_t)r;
            return 0;
        }
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
    }
}

/* ---- request handling ---------------------------------------------------- */

static void handle_query(Server *srv, int fd, const HttpRequest *req, char *body) {
    char *sql = body;
    char *decoded = NULL;
    if (req->json) {
        size_t cap = strlen(body) + 1; /* decoded text is never longer than the JSON */
        decoded = xmalloc(cap);
        if (json_get_string(body, "sql", decoded, cap) != 0) {
            free(decoded);
            respond_error(fd, 400, "body must be a JSON object with a string member \"sql\"");
            return;
        }
        sql = decoded;
    }

    Result r;
    result_init(&r);
    db_execute(srv->db, sql, srv->max_rows, &r);
    StrBuf out;
    sb_init(&out);
    result_format_json(&r, &out);
    respond(fd, r.ok ? 200 : 400, out.p);
    sb_free(&out);
    result_free(&r);
    free(decoded);
}

static void handle_stats(Server *srv, int fd) {
    DBStats s;
    db_stats(srv->db, &s);
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb,
              "{\"uptime_sec\":%.1f,\"requests\":%ld,\"rejected_503\":%ld,"
              "\"threads\":%d,\"active\":%d,\"queued\":%d,\"queue_capacity\":%d,"
              "\"selects\":%ld,\"inserts\":%ld,\"errors\":%ld,"
              "\"index_scans\":%ld,\"range_scans\":%ld,\"full_scans\":%ld}",
              (now_ms() - srv->started_ms) / 1000.0, atomic_load(&srv->requests),
              atomic_load(&srv->rejected), srv->nthreads, tp_active(srv->pool),
              tp_queued(srv->pool), srv->queue_cap, s.selects, s.inserts, s.errors, s.index_scans,
              s.range_scans, s.full_scans);
    respond(fd, 200, sb.p);
    sb_free(&sb);
}

static void serve(Server *srv, int fd) {
    char head[MAX_HEAD + 1];
    size_t len = 0;
    char *blank = NULL;

    /* Read until the blank line that ends the headers. */
    while (!blank) {
        if (len == MAX_HEAD) {
            respond_error(fd, 431, "request headers too large");
            return;
        }
        int rc = recv_some(fd, head, MAX_HEAD, &len);
        if (rc == -2) respond_error(fd, 408, "timed out waiting for the request");
        if (rc != 0) return;
        head[len] = '\0';
        if (strlen(head) != len) { /* a NUL byte inside the headers */
            respond_error(fd, 400, "malformed request");
            return;
        }
        blank = strstr(head, "\r\n\r\n");
    }
    size_t head_len = (size_t)(blank - head) + 4;
    *blank = '\0';

    HttpRequest req;
    if (http_parse_head(head, &req) != 0) {
        respond_error(fd, 400, "malformed request");
        return;
    }
    atomic_fetch_add(&srv->requests, 1);

    bool is_get = strcmp(req.method, "GET") == 0, is_post = strcmp(req.method, "POST") == 0;
    if (strcmp(req.path, "/health") == 0) {
        if (is_get) respond(fd, 200, "{\"status\":\"ok\"}");
        else respond_error(fd, 405, "use GET /health");
    } else if (strcmp(req.path, "/stats") == 0) {
        if (is_get) handle_stats(srv, fd);
        else respond_error(fd, 405, "use GET /stats");
    } else if (strcmp(req.path, "/query") != 0) {
        respond_error(fd, 404, "not found (endpoints: POST /query, GET /health, GET /stats)");
    } else if (!is_post) {
        respond_error(fd, 405, "use POST /query");
    } else if (req.content_length < 0) {
        respond_error(fd, 411, "Content-Length is required");
    } else if (req.content_length > MAX_BODY) {
        respond_error(fd, 413, "request body too large (max 1 MB)");
    } else {
        size_t need = (size_t)req.content_length;
        size_t have = len - head_len;
        if (have > need) have = need;
        char *body = xmalloc(need + 1);
        memcpy(body, head + head_len, have);
        int rc = 0;
        while (have < need && (rc = recv_some(fd, body, need, &have)) == 0) {}
        if (rc == -2) respond_error(fd, 408, "timed out waiting for the request body");
        if (rc == 0) {
            body[need] = '\0';
            if (strlen(body) != need) respond_error(fd, 400, "request body contains a NUL byte");
            else handle_query(srv, fd, &req, body);
        }
        free(body);
    }
}

static void handle_conn(void *arg) {
    Conn *c = arg;
    struct timeval tv = {.tv_sec = IO_TIMEOUT_SEC};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    serve(c->srv, c->fd);
    close(c->fd);
    free(c);
}

/* ---- main ---------------------------------------------------------------- */

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 512) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p PORT] [-d DATA_DIR] [-t THREADS] [-q QUEUE] [-m MAX_ROWS]\n"
            "  -p PORT      listen port (default 8080)\n"
            "  -d DIR       data directory (default: data)\n"
            "  -t THREADS   worker threads (default 8)\n"
            "  -q QUEUE     pending-connection queue size (default 256)\n"
            "  -m MAX_ROWS  most rows returned by one SELECT (default 10000)\n",
            prog);
}

int main(int argc, char **argv) {
    int port = 8080;
    const char *dir = "data";
    Server srv = {.max_rows = 10000, .nthreads = 8, .queue_cap = 256};
    int opt;
    while ((opt = getopt(argc, argv, "p:d:t:q:m:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'd': dir = optarg; break;
        case 't': srv.nthreads = atoi(optarg); break;
        case 'q': srv.queue_cap = atoi(optarg); break;
        case 'm': srv.max_rows = (size_t)atol(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (port < 1 || port > 65535 || srv.nthreads < 1 || srv.nthreads > 1024 || srv.queue_cap < 1 ||
        srv.max_rows < 1) {
        usage(argv[0]);
        return 2;
    }

    char err[512];
    srv.db = db_open(dir, err, sizeof err);
    if (!srv.db) {
        fprintf(stderr, "ERROR: %s\n", err);
        return 1;
    }
    int lfd = listen_on(port);
    if (lfd < 0) {
        fprintf(stderr, "ERROR: cannot listen on port %d: %s\n", port, strerror(errno));
        db_close(srv.db);
        return 1;
    }
    srv.pool = tp_create(srv.nthreads, srv.queue_cap);
    if (!srv.pool) {
        fprintf(stderr, "ERROR: cannot start %d worker threads\n", srv.nthreads);
        close(lfd);
        db_close(srv.db);
        return 1;
    }
    srv.started_ms = now_ms();

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("minidb-server listening on port %d (%d threads, queue %d, data dir '%s')\n", port,
           srv.nthreads, srv.queue_cap, dir);
    fflush(stdout);

    /* poll() with a timeout lets the loop notice g_stop promptly. */
    while (!g_stop) {
        struct pollfd pfd = {.fd = lfd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) continue;
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        Conn *c = xmalloc(sizeof *c);
        c->srv = &srv;
        c->fd = fd;
        if (tp_submit(srv.pool, handle_conn, c) != 0) {
            atomic_fetch_add(&srv.rejected, 1);
            respond_error(fd, 503, "server is busy, try again");
            close(fd);
            free(c);
        }
    }

    printf("shutting down: finishing queued requests...\n");
    fflush(stdout);
    close(lfd);
    tp_destroy(srv.pool); /* drains the queue, then joins the workers */
    db_close(srv.db);
    printf("bye\n");
    return 0;
}
