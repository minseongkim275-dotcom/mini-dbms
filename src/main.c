/* minidb: command-line SQL processor.
     minidb -f script.sql      run every statement in a file
     minidb -e "SELECT ..."    run statements given on the command line
     minidb                    interactive prompt (or statements piped on stdin) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"

typedef struct {
    DB *db;
    bool timing;
    bool quiet;
    int errors;
} Session;

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-d DATA_DIR] [-f FILE.sql | -e \"SQL\"] [-t] [-q]\n"
            "  -d DIR   directory holding <table>.schema / <table>.tbl (default: data)\n"
            "  -f FILE  execute the SQL statements in FILE\n"
            "  -e SQL   execute the given SQL statements\n"
            "  -t       print the execution plan and time after each statement\n"
            "  -q       do not print a line for each successful INSERT (bulk loads)\n"
            "With neither -f nor -e, statements are read from standard input.\n",
            prog);
}

/* Runs every statement in `sql`. A failing statement is reported and the
   rest still run. */
static void run_sql(Session *s, const char *sql) {
    Lexer lx;
    lexer_init(&lx, sql);
    Stmt *st = malloc(sizeof *st);
    if (!st) abort();
    char err[512];
    int rc;
    while ((rc = parse_statement(&lx, st, err, sizeof err)) != 0) {
        Result r;
        result_init(&r);
        if (rc < 0) snprintf(r.error, sizeof r.error, "%s", err);
        else db_exec_stmt(s->db, st, 0, &r);

        StrBuf out;
        sb_init(&out);
        result_format_table(&r, &out);
        if (r.ok && s->quiet && r.type == STMT_INSERT) {
            /* bulk load: stay silent */
        } else if (r.ok) {
            fputs(out.p, stdout);
            if (s->timing && !r.explain)
                printf("[%s%s%.3f ms]\n", r.plan ? r.plan : "", r.plan ? ", " : "", r.elapsed_ms);
        } else {
            fflush(stdout); /* keep stdout and stderr in order when both are redirected */
            fputs(out.p, stderr);
            s->errors++;
        }
        fflush(stdout);
        sb_free(&out);
        result_free(&r);
    }
    free(st);
}

static char *read_all(FILE *f) {
    StrBuf sb;
    sb_init(&sb);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_append(&sb, buf, n);
    return sb.p;
}

/* True when the text typed so far ends a statement: its last non-blank
   character is ';' and it is not inside a string literal. */
static bool statement_complete(const char *s) {
    bool in_string = false;
    char last = '\0';
    for (; *s; s++) {
        if (*s == '\'') in_string = !in_string;
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') last = *s;
    }
    return !in_string && last == ';';
}

static void repl(Session *s) {
    bool tty = isatty(STDIN_FILENO);
    if (tty) puts("minidb - end each statement with ';'. Type .exit to quit.");
    StrBuf pending;
    sb_init(&pending);
    char line[8192];
    for (;;) {
        if (tty) {
            fputs(pending.len ? "   ...> " : "minidb> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, stdin)) break;
        if (pending.len == 0 &&
            (strncmp(line, ".exit", 5) == 0 || strncmp(line, ".quit", 5) == 0))
            break;
        sb_puts(&pending, line);
        if (!tty) continue; /* piped input: run it all at the end */
        if (statement_complete(pending.p)) {
            run_sql(s, pending.p);
            pending.len = 0;
            pending.p[0] = '\0';
        }
    }
    if (pending.len) run_sql(s, pending.p);
    sb_free(&pending);
}

int main(int argc, char **argv) {
    const char *dir = "data", *file = NULL, *expr = NULL;
    Session s = {0};
    int opt;
    while ((opt = getopt(argc, argv, "d:f:e:tqh")) != -1) {
        switch (opt) {
        case 'q': s.quiet = true; break;
        case 'd': dir = optarg; break;
        case 'f': file = optarg; break;
        case 'e': expr = optarg; break;
        case 't': s.timing = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc || (file && expr)) {
        usage(argv[0]);
        return 2;
    }

    char err[512];
    s.db = db_open(dir, err, sizeof err);
    if (!s.db) {
        fprintf(stderr, "ERROR: %s\n", err);
        return 1;
    }

    if (file) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            fprintf(stderr, "ERROR: cannot open %s\n", file);
            db_close(s.db);
            return 1;
        }
        char *sql = read_all(f);
        fclose(f);
        run_sql(&s, sql);
        free(sql);
    } else if (expr) {
        run_sql(&s, expr);
    } else {
        repl(&s);
    }

    db_close(s.db);
    return s.errors ? 1 : 0;
}
