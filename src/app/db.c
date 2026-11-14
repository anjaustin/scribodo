#define _DARWIN_C_SOURCE
#include "db.h"
#include "core/crash_point.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <mysql.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include "core/deadline_clock.h"

struct Db {
    MYSQL *m;
    char err[512];
    bool broken; /* the connection was lost: it must be reopened */
    bool in_tx;  /* inside db_begin .. commit/rollback */
};

/* Client errors that mean the connection itself is gone (server restarted,
 * killed, timed out): 2002/2003 can't connect, 2006 gone away, 2013/2055
 * lost during a query. */
static void note_errno(Db *db, unsigned e)
{
    if (e == 2002 || e == 2003 || e == 2006 || e == 2013 || e == 2055) db->broken = true;
}

bool db_broken(Db *db) { return db->broken; }

int db_socket(Db *db)
{
    if (!db || !db->m) return -1;
    my_socket fd=mysql_get_socket(db->m);
    return fd < 0 || (uint64_t)fd > INT_MAX ? -1 : (int)fd;
}

/* ---- configuration ------------------------------------------------------- */

bool db_config_default(DbConfig *c, char *err, size_t errlen)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "127.0.0.1");
    const char *port = getenv("SCRIBODO_DB_PORT");
    c->port = port ? (unsigned)atoi(port) : 33061;
    snprintf(c->user, sizeof(c->user), "scribodo");
    snprintf(c->database, sizeof(c->database), "scribodo");
    c->timeout_s = 5;
    char path[1024];
    const char *dir = getenv("SCRIBODO_SECRETS");
    const char *home = getenv("HOME");
    if (dir) snprintf(path, sizeof(path), "%s/db_password", dir);
    else snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/services/db_password", home ? home : "");
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "The database password isn't set up (run tools/services.sh up)");
        return false;
    }
    size_t n = fread(c->password, 1, sizeof(c->password) - 1, f);
    fclose(f);
    c->password[n] = '\0';
    c->password[strcspn(c->password, "\r\n")] = '\0';
    return c->password[0] != '\0';
}

/* ---- connection ---------------------------------------------------------- */

Db *db_open(const DbConfig *c, char *err, size_t errlen)
{
    Db *db = calloc(1, sizeof(Db));
    if (!db) return NULL;
    db->m = mysql_init(NULL);
    if (!db->m) {
        free(db);
        snprintf(err, errlen, "Out of memory");
        return NULL;
    }
    unsigned t = c->timeout_s ? c->timeout_s : 5;
    mysql_options(db->m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
    mysql_options(db->m, MYSQL_OPT_READ_TIMEOUT, &t);
    mysql_options(db->m, MYSQL_OPT_WRITE_TIMEOUT, &t);
    mysql_options(db->m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    /* Local only: never read config files that could redirect us. */
    if (!mysql_real_connect(db->m, c->host, c->user, c->password, c->database, c->port, NULL, 0)) {
        snprintf(err, errlen, "Can't reach the project store: %s", mysql_error(db->m));
        mysql_close(db->m);
        free(db);
        return NULL;
    }
    mysql_autocommit(db->m, 1);
    /* Lock waits end as an ordinary error (2 s) well before the 5 s read
     * timeout would drop the connection (red-team 008). */
    if (mysql_query(db->m, "SET SESSION sql_mode = 'STRICT_ALL_TABLES,NO_ZERO_DATE,NO_ENGINE_SUBSTITUTION',"
                           " innodb_lock_wait_timeout = 2") != 0) {
        snprintf(err, errlen, "Project store setup failed: %s", mysql_error(db->m));
        mysql_close(db->m);
        free(db);
        return NULL;
    }
    return db;
}

static double monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(SCRIBODO_DEADLINE_CLOCK, &ts) != 0) return INFINITY;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static bool open_active(const atomic_bool *cancel, double deadline, char *err, size_t errlen)
{
    if (!isfinite(deadline) || deadline <= 0) {
        snprintf(err, errlen, "invalid deadline");
        return false;
    }
    if (cancel && atomic_load(cancel)) {
        snprintf(err, errlen, "cancelled");
        return false;
    }
    if (monotonic_ms() >= deadline) {
        snprintf(err, errlen, "timed out opening project store");
        return false;
    }
    return true;
}

/* A Connector/C timeout belongs to this suspension, not to each 20ms poll.
 * Returning 0 is an abort; successful continuations always get a wait bit. */
static int open_wait(Db *db, int wanted, const atomic_bool *cancel, double deadline, char *err, size_t errlen)
{
    double timeout = (wanted & MYSQL_WAIT_TIMEOUT) ? monotonic_ms() + mysql_get_timeout_value_ms(db->m) : INFINITY;
    for (;;) {
        if (!open_active(cancel, deadline, err, errlen)) return 0;
        double now = monotonic_ms();
        if (now >= timeout) return MYSQL_WAIT_TIMEOUT;
        double remaining = fmin(deadline, timeout) - now;
        if (remaining <= 0) continue;
        int slice = remaining >= 20 ? 20 : (int)ceil(remaining);
        short events = 0;
        if (wanted & MYSQL_WAIT_READ) events |= POLLIN;
        if (wanted & MYSQL_WAIT_WRITE) events |= POLLOUT;
        if (wanted & MYSQL_WAIT_EXCEPT) events |= POLLPRI;
        struct pollfd pfd = { .fd = db_socket(db), .events = events };
        int rc = poll(&pfd, 1, slice);
        if (!open_active(cancel, deadline, err, errlen)) return 0;
        if (rc < 0) {
            if (errno == EINTR) continue;
            snprintf(err, errlen, "project store poll: %s", strerror(errno));
            return 0;
        }
        if (rc > 0) {
            if (pfd.revents & POLLNVAL) {
                snprintf(err, errlen, "project store socket closed");
                return 0;
            }
            int ready = 0;
            if (pfd.revents & POLLIN) ready |= MYSQL_WAIT_READ;
            if (pfd.revents & POLLOUT) ready |= MYSQL_WAIT_WRITE;
            if (pfd.revents & POLLPRI) ready |= MYSQL_WAIT_EXCEPT;
            /* Let Connector/C observe EOF / the actual connection error. */
            if (pfd.revents & (POLLHUP | POLLERR)) ready |= wanted & (MYSQL_WAIT_READ | MYSQL_WAIT_WRITE | MYSQL_WAIT_EXCEPT);
            if (ready) return ready;
        }
    }
}

Db *db_open_until(const DbConfig *c, const atomic_bool *cancel, double deadline_ms, char *err, size_t errlen)
{
    if (errlen) err[0] = '\0';
    if (!open_active(cancel, deadline_ms, err, errlen)) return NULL;
    /* Connector/C's resolver and arbitrary auth plugins are synchronous even
     * in its async API. Numeric hosts avoid unbounded DNS on this local path. */
    const char *host = !strcmp(c->host, "localhost") ? "127.0.0.1" : c->host;
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (inet_pton(AF_INET, host, &ipv4) != 1 && inet_pton(AF_INET6, host, &ipv6) != 1) {
        snprintf(err, errlen, "bounded project store connection requires a numeric host");
        return NULL;
    }
    Db *db = calloc(1, sizeof(*db));
    if (!db) { snprintf(err, errlen, "Out of memory"); return NULL; }
    db->m = mysql_init(NULL);
    if (!db->m) { free(db); snprintf(err, errlen, "Out of memory"); return NULL; }
    unsigned t = c->timeout_s ? c->timeout_s : 5;
    if (mysql_options(db->m, MYSQL_OPT_NONBLOCK, NULL) ||
        mysql_options(db->m, MYSQL_OPT_CONNECT_TIMEOUT, &t) ||
        mysql_options(db->m, MYSQL_OPT_READ_TIMEOUT, &t) ||
        mysql_options(db->m, MYSQL_OPT_WRITE_TIMEOUT, &t) ||
        mysql_options(db->m, MYSQL_SET_CHARSET_NAME, "utf8mb4")) goto client_error;
    if (!open_active(cancel, deadline_ms, err, errlen)) goto abort_open;
    MYSQL *connected = NULL;
    int status = mysql_real_connect_start(&connected, db->m, host, c->user, c->password,
                                          c->database, c->port, NULL, 0);
    while (status) {
        int ready = open_wait(db, status, cancel, deadline_ms, err, errlen);
        if (!ready) goto abort_open;
        status = mysql_real_connect_cont(&connected, db->m, ready);
    }
    if (!open_active(cancel, deadline_ms, err, errlen)) goto abort_open;
    if (!connected) goto client_error;
    my_bool auto_error = 0;
    status = mysql_autocommit_start(&auto_error, db->m, 1);
    while (status) {
        int ready = open_wait(db, status, cancel, deadline_ms, err, errlen);
        if (!ready) goto abort_open;
        status = mysql_autocommit_cont(&auto_error, db->m, ready);
    }
    if (!open_active(cancel, deadline_ms, err, errlen)) goto abort_open;
    if (auto_error) goto client_error;
    int query_error = 0;
    status = mysql_query_start(&query_error, db->m,
        "SET SESSION sql_mode = 'STRICT_ALL_TABLES,NO_ZERO_DATE,NO_ENGINE_SUBSTITUTION',"
        " innodb_lock_wait_timeout = 2");
    while (status) {
        int ready = open_wait(db, status, cancel, deadline_ms, err, errlen);
        if (!ready) goto abort_open;
        status = mysql_query_cont(&query_error, db->m, ready);
    }
    if (!open_active(cancel, deadline_ms, err, errlen)) goto abort_open;
    if (query_error) goto client_error;
    return db;

client_error:
    snprintf(err, errlen, "Project store connection/setup failed: %s", mysql_error(db->m));
abort_open:
    /* Never issue a potentially blocking QUIT to an abandoned connection.
     * This also works when an async operation is currently suspended. */
    {
        int fd = db_socket(db);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
    mysql_close(db->m);
    free(db);
    return NULL;
}

void db_close(Db *db)
{
    if (!db) return;
    mysql_close(db->m);
    free(db);
}

const char *db_error(Db *db) { return db->err; }

static void set_err(Db *db, const char *what, const char *detail)
{
    snprintf(db->err, sizeof(db->err), "%s: %s", what, detail ? detail : "");
    note_errno(db, mysql_errno(db->m));
}

bool db_begin(Db *db)
{
    if (mysql_query(db->m, "START TRANSACTION") != 0) {
        set_err(db, "begin", mysql_error(db->m));
        return false;
    }
    db->in_tx = true;
    return true;
}

bool db_commit(Db *db)
{
    db->in_tx = false;
    /* Every store write is one transaction (chat append, record, revision, memory ingest): a process that dies
     * here has written nothing visible, one that dies after the commit has written all of it (GAP-004 matrix). */
    crash_point("store.before_commit");
    if (mysql_commit(db->m) != 0) {
        set_err(db, "commit", mysql_error(db->m));
        return false;
    }
    crash_point("store.after_commit");
    return true;
}

void db_rollback(Db *db)
{
    db->in_tx = false;
    mysql_rollback(db->m);
}

bool db_in_transaction(Db *db) { return db->in_tx; }

/* ---- prepared statements ------------------------------------------------- */

static MYSQL_STMT *prepare(Db *db, const char *sql, const DbArg *args, size_t nargs, MYSQL_BIND **binds_out)
{
    *binds_out = NULL;
    MYSQL_STMT *st = mysql_stmt_init(db->m);
    if (!st) {
        set_err(db, "statement", "out of memory");
        return NULL;
    }
    if (mysql_stmt_prepare(st, sql, strlen(sql)) != 0) {
        set_err(db, "prepare", mysql_stmt_error(st));
        note_errno(db, mysql_stmt_errno(st));
        mysql_stmt_close(st);
        return NULL;
    }
    if (mysql_stmt_param_count(st) != nargs) {
        set_err(db, "prepare", "wrong number of parameters");
        mysql_stmt_close(st);
        return NULL;
    }
    if (nargs) {
        MYSQL_BIND *b = calloc(nargs, sizeof(MYSQL_BIND));
        if (!b) {
            mysql_stmt_close(st);
            return NULL;
        }
        for (size_t i = 0; i < nargs; i++) {
            const DbArg *a = &args[i];
            if (a->type == 's') {
                if (a->s) {
                    b[i].buffer_type = MYSQL_TYPE_STRING;
                    b[i].buffer = (void *)a->s;
                    b[i].buffer_length = strlen(a->s);
                } else {
                    b[i].buffer_type = MYSQL_TYPE_NULL;
                }
            } else if (a->type == 'i') {
                b[i].buffer_type = MYSQL_TYPE_LONGLONG;
                b[i].buffer = (void *)&a->i;
            } else {
                b[i].buffer_type = MYSQL_TYPE_DOUBLE;
                b[i].buffer = (void *)&a->d;
            }
        }
        if (mysql_stmt_bind_param(st, b) != 0) {
            set_err(db, "bind", mysql_stmt_error(st));
            free(b);
            mysql_stmt_close(st);
            return NULL;
        }
        *binds_out = b;
    }
    return st;
}

bool db_run(Db *db, const char *sql, const DbArg *args, size_t nargs, uint64_t *affected)
{
    MYSQL_BIND *b;
    MYSQL_STMT *st = prepare(db, sql, args, nargs, &b);
    if (!st) return false;
    bool ok = mysql_stmt_execute(st) == 0;
    if (!ok) {
        set_err(db, "execute", mysql_stmt_error(st));
        note_errno(db, mysql_stmt_errno(st));
    }
    if (ok && affected) *affected = (uint64_t)mysql_stmt_affected_rows(st);
    free(b);
    mysql_stmt_close(st);
    return ok;
}

void db_rows_free(DbRows *r)
{
    if (!r) return;
    size_t count = 0;
    if (r->cells && (!r->ncols || r->nrows <= SIZE_MAX / r->ncols)) count = r->nrows * r->ncols;
    for (size_t i = 0; i < count; i++) free(r->cells[i]);
    free(r->cells);
    memset(r, 0, sizeof(*r));
}

bool db_rows_has_columns(const DbRows *r, size_t expected)
{
    return r && r->ncols == expected &&
           (r->nrows == 0 || (r->cells && r->ncols && r->nrows <= SIZE_MAX / r->ncols));
}

bool db_cell_nullable(const DbRows *r, size_t row, size_t col, const char **out)
{
    if (!r || !out || !r->cells || row >= r->nrows || col >= r->ncols || (r->ncols && row > SIZE_MAX / r->ncols)) return false;
    *out = r->cells[row * r->ncols + col];
    return true;
}

bool db_cell_text(const DbRows *r, size_t row, size_t col, const char **out)
{
    return db_cell_nullable(r, row, col, out) && *out;
}

bool db_cell_i64(const DbRows *r, size_t row, size_t col, int64_t *out)
{
    const char *value;
    if (!out || !db_cell_text(r, row, col, &value) || !value[0]) return false;
    char *end = NULL;
    errno = 0;
    intmax_t parsed = strtoimax(value, &end, 10);
    if (errno == ERANGE || !end || *end || parsed < INT64_MIN || parsed > INT64_MAX) return false;
    *out = (int64_t)parsed;
    return true;
}

bool db_cell_u64(const DbRows *r, size_t row, size_t col, uint64_t *out)
{
    const char *value;
    if (!out || !db_cell_text(r, row, col, &value) || !value[0] || value[0] == '-') return false;
    char *end = NULL;
    errno = 0;
    uintmax_t parsed = strtoumax(value, &end, 10);
    if (errno == ERANGE || !end || *end || parsed > UINT64_MAX) return false;
    *out = (uint64_t)parsed;
    return true;
}

bool db_cell_double(const DbRows *r, size_t row, size_t col, double *out)
{
    const char *value;
    if (!out || !db_cell_text(r, row, col, &value) || !value[0]) return false;
    char *end = NULL;
    errno = 0;
    double parsed = strtod(value, &end);
    if (errno == ERANGE || !end || *end || !isfinite(parsed)) return false;
    *out = parsed;
    return true;
}

bool db_query(Db *db, const char *sql, const DbArg *args, size_t nargs, DbRows *out)
{
    memset(out, 0, sizeof(*out));
    MYSQL_BIND *pb;
    MYSQL_STMT *st = prepare(db, sql, args, nargs, &pb);
    if (!st) return false;
    bool ok = mysql_stmt_execute(st) == 0;
    if (!ok) {
        set_err(db, "execute", mysql_stmt_error(st));
        note_errno(db, mysql_stmt_errno(st));
    }
    MYSQL_RES *meta = ok ? mysql_stmt_result_metadata(st) : NULL;
    size_t ncols = meta ? mysql_num_fields(meta) : 0;
    MYSQL_BIND *rb = ncols ? calloc(ncols, sizeof(MYSQL_BIND)) : NULL;
    unsigned long *lens = ncols ? calloc(ncols, sizeof(unsigned long)) : NULL;
    my_bool *nulls = ncols ? calloc(ncols, sizeof(my_bool)) : NULL;
    if (ok && ncols && (!rb || !lens || !nulls)) ok = false;
    for (size_t c = 0; ok && c < ncols; c++) {
        rb[c].buffer_type = MYSQL_TYPE_STRING; /* every column read as text; sizes found per row */
        rb[c].buffer = NULL;
        rb[c].buffer_length = 0;
        rb[c].length = &lens[c];
        rb[c].is_null = &nulls[c];
    }
    if (ok && ncols && mysql_stmt_bind_result(st, rb) != 0) {
        set_err(db, "bind result", mysql_stmt_error(st));
        ok = false;
    }
    size_t cap = 0;
    out->ncols = ncols;
    while (ok && ncols) {
        int rc = mysql_stmt_fetch(st);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == 1) {
            set_err(db, "fetch", mysql_stmt_error(st));
            note_errno(db, mysql_stmt_errno(st));
            ok = false;
            break;
        }
        if (out->nrows == cap) {
            size_t next = cap ? cap * 2 : 16;
            if (next < cap || ncols > SIZE_MAX / next / sizeof(char *)) {
                ok = false;
                break;
            }
            cap = next;
            char **cells = realloc(out->cells, cap * ncols * sizeof(*cells));
            if (!cells) {
                ok = false;
                break;
            }
            out->cells = cells;
        }
        char **row = out->cells + out->nrows * ncols;
        /* A failed allocation/fetch below leaves the whole query failed.
         * Keep every cell owned by the row either NULL or malloc'd so the
         * common cleanup path never reads or frees uninitialised memory. */
        memset(row, 0, ncols * sizeof(*row));
        bool row_ok = true;
        for (size_t c = 0; c < ncols; c++) {
            if (nulls[c]) continue;
            /* Numeric columns report no length until converted: always fetch
             * into a real buffer, and again if the value is longer. */
            if (lens[c] == ULONG_MAX) {
                row_ok = false;
                break;
            }
            unsigned long size = lens[c] > 64 ? lens[c] : 64, got = 0;
            char *v = malloc(size + 1);
            MYSQL_BIND one = { 0 };
            one.buffer_type = MYSQL_TYPE_STRING;
            one.length = &got;
            for (int attempt = 0; v && attempt < 2; attempt++) {
                one.buffer = v;
                one.buffer_length = size + 1;
                if (mysql_stmt_fetch_column(st, &one, (unsigned)c, 0) != 0) {
                    free(v);
                    v = NULL;
                    break;
                }
                if (got <= size) break;
                if (got == ULONG_MAX) {
                    free(v);
                    v = NULL;
                    break;
                }
                size = got;
                char *nv = realloc(v, size + 1);
                if (!nv) {
                    free(v);
                    v = NULL;
                }
                v = nv;
            }
            if (!v) {
                row_ok = false;
                break;
            }
            v[got <= size ? got : size] = '\0';
            row[c] = v;
        }
        if (!row_ok) {
            /* It never becomes an output row; release only its known-owned
             * cells, then let the common path clean completed rows. */
            for (size_t c = 0; c < ncols; c++) free(row[c]);
            ok = false;
            break;
        }
        out->nrows++;
    }
    if (meta) mysql_free_result(meta);
    free(rb);
    free(lens);
    free(nulls);
    free(pb);
    mysql_stmt_close(st);
    if (!ok) db_rows_free(out);
    return ok;
}

/* ---- migrations ------------------------------------------------------------- */

/* Each migration is a list of statements, applied in one transaction (DDL
 * commits implicitly in MariaDB, so each statement is also idempotent: IF
 * NOT EXISTS). */
static const char *const k_migration_001[] = {
    "CREATE TABLE IF NOT EXISTS projects ("
    " id VARCHAR(40) COLLATE utf8mb4_bin PRIMARY KEY, name VARCHAR(255) NOT NULL, path VARCHAR(2048) COLLATE utf8mb4_bin NOT NULL,"
    " path_hash CHAR(64) COLLATE utf8mb4_bin NOT NULL UNIQUE, created_at DOUBLE NOT NULL, updated_at DOUBLE NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS documents ("
    " id VARCHAR(40) COLLATE utf8mb4_bin PRIMARY KEY, project_id VARCHAR(40) COLLATE utf8mb4_bin NOT NULL,"
    " name VARCHAR(255) COLLATE utf8mb4_bin NOT NULL, current_revision_id VARCHAR(40) NULL, created_at DOUBLE NOT NULL, renamed_at DOUBLE NULL,"
    " UNIQUE KEY doc_name (project_id, name),"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS revisions ("
    " id VARCHAR(40) PRIMARY KEY, document_id VARCHAR(40) NOT NULL, hash CHAR(16) NOT NULL,"
    " content LONGTEXT NOT NULL, origin VARCHAR(24) NOT NULL, note VARCHAR(255) NOT NULL DEFAULT '',"
    " created_at DOUBLE NOT NULL, KEY rev_doc (document_id, created_at),"
    " FOREIGN KEY (document_id) REFERENCES documents(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS conversations ("
    " id VARCHAR(40) PRIMARY KEY, project_id VARCHAR(40) NOT NULL, title VARCHAR(255) NOT NULL,"
    " created_at DOUBLE NOT NULL, updated_at DOUBLE NOT NULL,"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS messages ("
    " id BIGINT AUTO_INCREMENT PRIMARY KEY, conversation_id VARCHAR(40) NOT NULL, seq INT NOT NULL,"
    " role ENUM('user','assistant') NOT NULL, content LONGTEXT NOT NULL,"
    " error TINYINT(1) NOT NULL DEFAULT 0, is_edit TINYINT(1) NOT NULL DEFAULT 0, truncated TINYINT(1) NOT NULL DEFAULT 0,"
    " created_at DOUBLE NOT NULL, UNIQUE KEY msg_seq (conversation_id, seq),"
    " FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS message_documents ("
    " message_id BIGINT NOT NULL, document_id VARCHAR(40) NOT NULL, PRIMARY KEY (message_id, document_id),"
    " FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE,"
    " FOREIGN KEY (document_id) REFERENCES documents(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS records ("
    " id VARCHAR(40) PRIMARY KEY, project_id VARCHAR(40) NOT NULL, kind ENUM('note','canon','finding') NOT NULL,"
    " created_at DOUBLE NOT NULL, KEY rec_kind (project_id, kind),"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS record_versions ("
    " record_id VARCHAR(40) NOT NULL, ver INT NOT NULL, status VARCHAR(16) NOT NULL, content LONGTEXT NOT NULL,"
    " finding_type VARCHAR(24) NULL, subject VARCHAR(255) NULL, relation VARCHAR(255) NULL, object VARCHAR(255) NULL,"
    " related_id VARCHAR(40) NULL, related_text LONGTEXT NULL, created_at DOUBLE NOT NULL,"
    " PRIMARY KEY (record_id, ver),"
    " FOREIGN KEY (record_id) REFERENCES records(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS source_refs ("
    " id BIGINT AUTO_INCREMENT PRIMARY KEY, record_id VARCHAR(40) NOT NULL, ver INT NOT NULL,"
    " kind ENUM('doc','chat') NOT NULL, document_id VARCHAR(40) NULL, revision_id VARCHAR(40) NULL,"
    " conversation_id VARCHAR(40) NULL, message_seq INT NULL, start BIGINT NOT NULL, len BIGINT NOT NULL,"
    " quote LONGTEXT NOT NULL, KEY src_rec (record_id, ver),"
    " FOREIGN KEY (record_id, ver) REFERENCES record_versions(record_id, ver) ON DELETE CASCADE,"
    " FOREIGN KEY (document_id) REFERENCES documents(id) ON DELETE SET NULL,"
    " FOREIGN KEY (revision_id) REFERENCES revisions(id) ON DELETE SET NULL,"
    " FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE SET NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS entities ("
    " id VARCHAR(40) PRIMARY KEY, project_id VARCHAR(40) NOT NULL, name VARCHAR(255) NOT NULL,"
    " norm_name VARCHAR(255) NOT NULL, type VARCHAR(24) NOT NULL DEFAULT 'entity',"
    " UNIQUE KEY ent_name (project_id, norm_name),"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS links ("
    " id BIGINT AUTO_INCREMENT PRIMARY KEY, project_id VARCHAR(40) NOT NULL, from_entity VARCHAR(40) NOT NULL,"
    " relation VARCHAR(255) NOT NULL, to_entity VARCHAR(40) NOT NULL, finding_id VARCHAR(40) NULL,"
    " KEY link_from (from_entity), KEY link_to (to_entity),"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,"
    " FOREIGN KEY (from_entity) REFERENCES entities(id) ON DELETE CASCADE,"
    " FOREIGN KEY (to_entity) REFERENCES entities(id) ON DELETE CASCADE,"
    " FOREIGN KEY (finding_id) REFERENCES records(id) ON DELETE SET NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS observer_pieces ("
    " document_id VARCHAR(40) NOT NULL, piece_hash CHAR(16) NOT NULL, PRIMARY KEY (document_id, piece_hash),"
    " FOREIGN KEY (document_id) REFERENCES documents(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS observer_chats ("
    " conversation_id VARCHAR(40) PRIMARY KEY, msg INT NOT NULL, off BIGINT NOT NULL,"
    " FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS jobs ("
    " id VARCHAR(40) PRIMARY KEY, project_id VARCHAR(40) NOT NULL, kind VARCHAR(24) NOT NULL,"
    " state VARCHAR(16) NOT NULL, attempts INT NOT NULL DEFAULT 0, last_error TEXT NULL, updated_at DOUBLE NOT NULL,"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

/* 002: what the semantic index holds (Qdrant is derived; this lets the
 * indexer skip text already embedded, and a rebuild start clean). */
static const char *const k_migration_002[] = {
    "CREATE TABLE IF NOT EXISTS semantic_marks ("
    " project_id VARCHAR(40) NOT NULL, point_key VARCHAR(191) NOT NULL, content_hash CHAR(16) NOT NULL,"
    " PRIMARY KEY (project_id, point_key),"
    " FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

/* 003 (red-team 008):
 *  - revisions get an insertion sequence: History order no longer depends on
 *    timestamps (two saves in one millisecond, or a clock step back);
 *    existing rows are numbered by time.
 *  - names and other author text compare byte for byte without pad-space
 *    ("Chapter" and "Chapter " are different names; utf8mb4_bin ignored
 *    trailing spaces). Generated ids keep utf8mb4_bin (foreign keys pin
 *    their collation, and they never contain spaces).
 * DDL commits as it goes, so each step can run again after a failure. */
static const char *const k_migration_003[] = {
    "ALTER TABLE revisions ADD COLUMN IF NOT EXISTS seq BIGINT NULL",
    "SET @scribodo_n := 0",
    "UPDATE revisions SET seq = (@scribodo_n := @scribodo_n + 1) ORDER BY created_at, id",
    "ALTER TABLE revisions MODIFY seq BIGINT NOT NULL AUTO_INCREMENT, ADD UNIQUE KEY IF NOT EXISTS rev_seq (seq),"
    " ADD KEY IF NOT EXISTS rev_doc_seq (document_id, seq)",
    "ALTER TABLE projects MODIFY name VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL, MODIFY path VARCHAR(2048) COLLATE utf8mb4_nopad_bin NOT NULL",
    "ALTER TABLE documents MODIFY name VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL",
    "ALTER TABLE conversations MODIFY title VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL",
    "ALTER TABLE revisions MODIFY note VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL DEFAULT ''",
    "ALTER TABLE record_versions MODIFY status VARCHAR(16) COLLATE utf8mb4_nopad_bin NOT NULL, MODIFY finding_type VARCHAR(24) COLLATE utf8mb4_nopad_bin NULL, MODIFY subject VARCHAR(255) COLLATE utf8mb4_nopad_bin NULL, MODIFY relation VARCHAR(255) COLLATE utf8mb4_nopad_bin NULL, MODIFY object VARCHAR(255) COLLATE utf8mb4_nopad_bin NULL",
    "ALTER TABLE entities MODIFY name VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL, MODIFY norm_name VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL, MODIFY type VARCHAR(24) COLLATE utf8mb4_nopad_bin NOT NULL DEFAULT 'entity'",
    "ALTER TABLE links MODIFY relation VARCHAR(255) COLLATE utf8mb4_nopad_bin NOT NULL",
    /* new columns default to it too (ids stay as they are: foreign keys pin them, and they never hold spaces) */
    "ALTER TABLE projects DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE documents DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE revisions DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE conversations DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE messages DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE message_documents DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE records DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE record_versions DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE source_refs DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE entities DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE links DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE observer_pieces DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE observer_chats DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE jobs DEFAULT COLLATE utf8mb4_nopad_bin",
    "ALTER TABLE semantic_marks DEFAULT COLLATE utf8mb4_nopad_bin",
    NULL,
};

/* 004: which findings mention which entity (step 3, the entity graph).
 * Ids keep utf8mb4_bin to match the columns their foreign keys name. */
static const char *const k_migration_004[] = {
    "CREATE TABLE IF NOT EXISTS entity_mentions ("
    " entity_id VARCHAR(40) COLLATE utf8mb4_bin NOT NULL, finding_id VARCHAR(40) COLLATE utf8mb4_bin NOT NULL,"
    " PRIMARY KEY (entity_id, finding_id), KEY mention_finding (finding_id),"
    " FOREIGN KEY (entity_id) REFERENCES entities(id) ON DELETE CASCADE,"
    " FOREIGN KEY (finding_id) REFERENCES records(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

/* 005: which messages went to a remote model (SOW: remote use is visibly
 * identified, also afterwards). */
static const char *const k_migration_005[] = {
    "ALTER TABLE messages ADD COLUMN IF NOT EXISTS via VARCHAR(400) COLLATE utf8mb4_nopad_bin NULL",
    NULL,
};

/* 006 (red-team 009):
 *  - one link per finding, enforced (two connections could both insert);
 *    duplicates from before are removed first;
 *  - whether a project's entity graph has been built (backfill once, not
 *    every session);
 *  - a semantic rebuild in progress (resumed if it was interrupted). */
static const char *const k_migration_006[] = {
    "DELETE l FROM links l JOIN links k ON k.finding_id = l.finding_id AND k.id < l.id",
    "ALTER TABLE links ADD UNIQUE KEY IF NOT EXISTS link_finding (finding_id)",
    "ALTER TABLE projects ADD COLUMN IF NOT EXISTS graph_built TINYINT(1) NOT NULL DEFAULT 0",
    "ALTER TABLE projects ADD COLUMN IF NOT EXISTS index_rebuild_pending TINYINT(1) NOT NULL DEFAULT 0",
    /* each record's current status, kept with it (counting live findings
     * per mention through their versions was too slow) */
    "ALTER TABLE records ADD COLUMN IF NOT EXISTS current_status VARCHAR(16) COLLATE utf8mb4_nopad_bin NULL",
    "UPDATE records r JOIN (SELECT record_id, MAX(ver) AS mv FROM record_versions GROUP BY record_id) x ON x.record_id = r.id"
    " JOIN record_versions v ON v.record_id = x.record_id AND v.ver = x.mv SET r.current_status = v.status",
    "ALTER TABLE records ADD KEY IF NOT EXISTS rec_status (project_id, kind, current_status)",
    NULL,
};

/* 007: chats the author told the observer not to read (user testing). */
static const char *const k_migration_007[] = {
    "ALTER TABLE conversations ADD COLUMN IF NOT EXISTS observer_ignore TINYINT(1) NOT NULL DEFAULT 0",
    NULL,
};

/* Historical stopped state was not stored and cannot be reconstructed. */
static const char *const k_migration_008[] = {
    "ALTER TABLE messages ADD COLUMN IF NOT EXISTS stopped TINYINT(1) NOT NULL DEFAULT 0",
    NULL,
};

/* 009: experimental opt-in chat memory. No scope is created by migration.
 * Composite keys prevent cross-project/source associations in SQL itself. */
static const char *const k_migration_009[] = {
    /* One-time linear backfill. Future source changes recompute once at write,
     * not once per bounded chunk / candidate read. */
    "ALTER TABLE messages ADD COLUMN IF NOT EXISTS source_hash CHAR(64) COLLATE utf8mb4_bin"
    " GENERATED ALWAYS AS (SHA2(content,256)) PERSISTENT",
    "ALTER TABLE messages ADD COLUMN IF NOT EXISTS memory_sources TEXT NULL",
    "ALTER TABLE messages ADD CONSTRAINT IF NOT EXISTS memory_sources_bounded"
    " CHECK (memory_sources IS NULL OR OCTET_LENGTH(memory_sources)<=16384)",
    "ALTER TABLE conversations ADD UNIQUE KEY IF NOT EXISTS memory_conversation (project_id,id)",
    "ALTER TABLE messages ADD UNIQUE KEY IF NOT EXISTS memory_message (conversation_id,id)",
    "CREATE TABLE IF NOT EXISTS memory_scopes ("
    " id VARCHAR(40) PRIMARY KEY, project_id VARCHAR(40) NOT NULL, conversation_id VARCHAR(40) NOT NULL,"
    " enabled TINYINT NOT NULL DEFAULT 0, history_epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,"
    " policy_epoch BIGINT UNSIGNED NOT NULL DEFAULT 1, generation BIGINT UNSIGNED NOT NULL DEFAULT 1,"
    " cursor_seq BIGINT NOT NULL DEFAULT -1, byte_offset BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " pending_seq BIGINT NOT NULL DEFAULT -1, lease_owner VARCHAR(63) NOT NULL DEFAULT '',"
    " tokenizer VARCHAR(191) NULL,"
    " lease_until DATETIME(6) NULL, fence BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " attempts INT NOT NULL DEFAULT 0, retry_after DATETIME(6) NULL,"
    " UNIQUE KEY scope_conversation (project_id,conversation_id), UNIQUE KEY scope_owner (id,project_id,conversation_id),"
    " FOREIGN KEY (project_id,conversation_id) REFERENCES conversations(project_id,id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS memory_receipts ("
    " scope_id VARCHAR(40) NOT NULL, project_id VARCHAR(40) NOT NULL, conversation_id VARCHAR(40) NOT NULL,"
    " message_id BIGINT NOT NULL, generation BIGINT UNSIGNED NOT NULL, sequence BIGINT NOT NULL,"
    " source_hash CHAR(64) NOT NULL, byte_offset BIGINT UNSIGNED NOT NULL, complete TINYINT NOT NULL, eligible TINYINT NOT NULL,"
    " PRIMARY KEY (scope_id,generation,message_id),"
    " FOREIGN KEY (scope_id,project_id,conversation_id) REFERENCES memory_scopes(id,project_id,conversation_id) ON DELETE CASCADE,"
    " FOREIGN KEY (conversation_id,message_id) REFERENCES messages(conversation_id,id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS memory_units ("
    " id CHAR(64) PRIMARY KEY, scope_id VARCHAR(40) NOT NULL, project_id VARCHAR(40) NOT NULL, conversation_id VARCHAR(40) NOT NULL,"
    " message_id BIGINT NOT NULL, sequence BIGINT NOT NULL, history_epoch BIGINT UNSIGNED NOT NULL,"
    " policy_epoch BIGINT UNSIGNED NOT NULL, generation BIGINT UNSIGNED NOT NULL, byte_start BIGINT UNSIGNED NOT NULL,"
    " source_hash CHAR(64) NOT NULL, text_hash CHAR(64) NOT NULL, text VARCHAR(2000) NOT NULL,"
    " tokens INT NOT NULL, tokenizer VARCHAR(191) NOT NULL, role ENUM('user','assistant') NOT NULL,"
    " CHECK (OCTET_LENGTH(text)<=2000 AND tokens>=0 AND tokens<=512),"
    " UNIQUE KEY unit_range (scope_id,generation,message_id,byte_start),"
    " UNIQUE KEY unit_scope (id,scope_id), KEY unit_order (scope_id,generation,sequence,byte_start), FULLTEXT KEY unit_text (text),"
    " FOREIGN KEY (scope_id,project_id,conversation_id) REFERENCES memory_scopes(id,project_id,conversation_id) ON DELETE CASCADE,"
    " FOREIGN KEY (conversation_id,message_id) REFERENCES messages(conversation_id,id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS memory_outbox ("
    " id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY, scope_id VARCHAR(40) NOT NULL, unit_id CHAR(64) NOT NULL,"
    " generation BIGINT UNSIGNED NOT NULL, state VARCHAR(12) NOT NULL DEFAULT 'pending', attempts INT NOT NULL DEFAULT 0,"
    " owner VARCHAR(63) NOT NULL DEFAULT '', fence BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " lease_until DATETIME(6) NULL, retry_after DATETIME(6) NULL,"
    " UNIQUE KEY delivery (unit_id,generation), KEY admission (scope_id,generation,state,id),"
    " FOREIGN KEY (unit_id,scope_id) REFERENCES memory_units(id,scope_id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

/* 010: explicit saved-write receipts, never inferred from historical origins.
 * The generated digest backfills old content linearly and is persisted at write. */
static const char *const k_migration_010[] = {
    "ALTER TABLE revisions ADD COLUMN IF NOT EXISTS source_hash CHAR(64) COLLATE utf8mb4_bin"
    " GENERATED ALWAYS AS (SHA2(content,256)) PERSISTENT",
    "ALTER TABLE revisions ADD UNIQUE KEY IF NOT EXISTS revision_document (document_id,id)",
    "ALTER TABLE documents ADD UNIQUE KEY IF NOT EXISTS document_project (project_id,id)",
    "ALTER TABLE documents ADD COLUMN IF NOT EXISTS saved_revision_id VARCHAR(40) COLLATE utf8mb4_bin NULL",
    /* RESTRICT deletion rather than clearing the document id in this composite key. */
    "ALTER TABLE documents ADD FOREIGN KEY IF NOT EXISTS document_saved_revision"
    " (id,saved_revision_id) REFERENCES revisions(document_id,id) ON DELETE RESTRICT",
    NULL,
};

/* 011: typed, scope-local saved-document memory. No source bodies are copied
 * into receipts; the saved pointer, not the editor's current pointer, owns it. */
static const char *const k_migration_011[] = {
    "ALTER TABLE memory_units ADD COLUMN IF NOT EXISTS source_kind ENUM('chat','document') NOT NULL DEFAULT 'chat',"
    " MODIFY message_id BIGINT NULL, MODIFY sequence BIGINT NULL,"
    " ADD COLUMN IF NOT EXISTS document_id VARCHAR(40) NULL, ADD COLUMN IF NOT EXISTS revision_id VARCHAR(40) NULL",
    "ALTER TABLE memory_units ADD CONSTRAINT IF NOT EXISTS memory_unit_shape CHECK"
    " ((source_kind='chat' AND message_id IS NOT NULL AND sequence IS NOT NULL AND document_id IS NULL AND revision_id IS NULL)"
    " OR (source_kind='document' AND message_id IS NULL AND sequence IS NULL AND document_id IS NOT NULL AND revision_id IS NOT NULL)),"
    " ADD UNIQUE KEY IF NOT EXISTS document_range(scope_id,generation,revision_id,byte_start),"
    " ADD FOREIGN KEY IF NOT EXISTS memory_unit_document(project_id,document_id) REFERENCES documents(project_id,id) ON DELETE CASCADE,"
    " ADD FOREIGN KEY IF NOT EXISTS memory_unit_revision(document_id,revision_id) REFERENCES revisions(document_id,id) ON DELETE CASCADE",
    "CREATE TABLE IF NOT EXISTS memory_document_receipts ("
    " scope_id VARCHAR(40) NOT NULL,project_id VARCHAR(40) NOT NULL,conversation_id VARCHAR(40) NOT NULL,"
    " generation BIGINT UNSIGNED NOT NULL,document_id VARCHAR(40) NOT NULL,revision_id VARCHAR(40) NOT NULL,"
    " source_hash CHAR(64) NOT NULL,source_bytes BIGINT UNSIGNED NOT NULL,byte_offset BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " complete TINYINT NOT NULL DEFAULT 0,lease_owner VARCHAR(63) NOT NULL DEFAULT '',lease_until DATETIME(6) NULL,"
    " fence BIGINT UNSIGNED NOT NULL DEFAULT 0,attempts INT NOT NULL DEFAULT 0,retry_after DATETIME(6) NULL,"
    " PRIMARY KEY(scope_id,generation,revision_id),"
    " CHECK(source_bytes<=16777216 AND byte_offset<=source_bytes),"
    " FOREIGN KEY(scope_id,project_id,conversation_id) REFERENCES memory_scopes(id,project_id,conversation_id) ON DELETE CASCADE,"
    " FOREIGN KEY(project_id,document_id) REFERENCES documents(project_id,id) ON DELETE CASCADE,"
    " FOREIGN KEY(document_id,revision_id) REFERENCES revisions(document_id,id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

/* 012: explicit, durable namespace retirements; never discover targets from a
 * vector server. Completed receipts remain for idempotent registration. */
static const char *const k_migration_012[] = {
    "CREATE TABLE IF NOT EXISTS memory_gc_scopes ("
    " scope_id VARCHAR(40) PRIMARY KEY,project_id VARCHAR(40) NOT NULL,conversation_id VARCHAR(40) NOT NULL,"
    " KEY gc_project(project_id,scope_id),"
    " FOREIGN KEY(scope_id,project_id,conversation_id)"
    " REFERENCES memory_scopes(id,project_id,conversation_id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    /* Metadata-only, one-time adoption of preexisting scopes. Generation
     * changes enqueue subsequent work in their own author/control transaction. */
    "INSERT IGNORE INTO memory_gc_scopes(scope_id,project_id,conversation_id)"
    " SELECT id,project_id,conversation_id FROM memory_scopes",
    "CREATE TABLE IF NOT EXISTS memory_namespace_floor ("
    " project_id VARCHAR(40) PRIMARY KEY,generation_floor BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE,"
    " CHECK(generation_floor<9223372036854775807)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS memory_retired_namespaces ("
    " id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,database_name VARCHAR(63) NOT NULL,"
    " project_id VARCHAR(40) NOT NULL,scope_id VARCHAR(40) NOT NULL,conversation_id VARCHAR(40) NOT NULL,"
    " generation BIGINT UNSIGNED NOT NULL,namespace_version INT NOT NULL,embedding_fingerprint CHAR(64) NOT NULL,"
    " state VARCHAR(12) NOT NULL DEFAULT 'pending',owner VARCHAR(63) NOT NULL DEFAULT '',"
    " fence BIGINT UNSIGNED NOT NULL DEFAULT 0,attempts INT NOT NULL DEFAULT 0,"
    " lease_until DATETIME(6) NULL,retry_after DATETIME(6) NULL,"
    " UNIQUE KEY retirement(database_name,project_id,generation,namespace_version,embedding_fingerprint),"
    " KEY admission(state,retry_after,id),CHECK(namespace_version IN (1,2) AND generation>0)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

#include "memory_brief_schema.h"

/* 014: case-insensitive lexical discovery. Under utf8mb4_bin, InnoDB's
 * full-text index on memory_units.text is case-sensitive and inconsistent:
 * incrementally inserted rows keep their case ("Tarn" matches, "tarn" does
 * not) while a rebuilt index lowercases tokens ("tarn" matches, "Tarn" does
 * not). Measured on MariaDB 11.4.13 in MEMORY-018. A case-insensitive
 * collation matches every case on every build path. Hashes and binary
 * comparisons of the text are unaffected; only MATCH and = change. */
static const char *const k_migration_014[] = {
    "ALTER TABLE memory_units MODIFY text VARCHAR(2000) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL",
    NULL,
};

/* journal/answer-cycle3_coref.md: an index text beside the original unit text,
 * searched, embedded and reranked in place of it under SCRIBODO_INDEX_TEXT_FILE
 * (then src/app/memory_index_text.h); NULL otherwise. Never shown; rebuilt with
 * the index. Retired by migration 016. */
static const char *const k_migration_015[] = {
    "ALTER TABLE memory_units ADD COLUMN IF NOT EXISTS index_text TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NULL,"
    " ADD FULLTEXT KEY IF NOT EXISTS unit_index_text (index_text)",
    NULL,
};

/* journal/answer-cycle4_retire_index.md (the author's decision, 2026-11-12): the
 * index-text experiment is retired into the harness (tests/memory_index_text.c).
 * The column never held author text (NULL, or a derived copy under the
 * experiment), so removing it deletes nothing of the record. Two statements, each
 * re-runnable, so a death between them recovers (GAP-004 matrix). */
static const char *const k_migration_016[] = {
    "ALTER TABLE memory_units DROP INDEX IF EXISTS unit_index_text",
    "ALTER TABLE memory_units DROP COLUMN IF EXISTS index_text",
    NULL,
};

/* journal/answer-cycle4_note_turns.md (the author's decision, 2026-11-14): a user turn the author marked as a note in the
 * composer. The answer gate does not arm on it however it is worded; the record keeps the author's declaration beside
 * the turn. One re-runnable statement; existing rows read as not notes. */
static const char *const k_migration_017[] = {
    "ALTER TABLE messages ADD COLUMN IF NOT EXISTS is_note TINYINT(1) NOT NULL DEFAULT 0",
    NULL,
};

static const char *const *const k_migrations[] = { k_migration_001, k_migration_002, k_migration_003, k_migration_004,
                                                   k_migration_005, k_migration_006, k_migration_007, k_migration_008,
                                                   k_migration_009, k_migration_010, k_migration_011, k_migration_012,
                                                   k_migration_013, k_migration_014, k_migration_015, k_migration_016,
                                                   k_migration_017 };

int db_schema_version(Db *db)
{
    DbRows r;
    if (!db_query(db, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations", NULL, 0, &r)) return -1;
    int64_t parsed;
    int v = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_i64(&r, 0, 0, &parsed) && parsed >= 0 && parsed <= INT_MAX
                ? (int)parsed
                : -1;
    db_rows_free(&r);
    return v;
}

bool db_migrate(Db *db, char *err, size_t errlen) { return db_migrate_upto(db, 1 << 30, err, errlen); }

bool db_migrate_upto(Db *db, int upto, char *err, size_t errlen)
{
    if (!db_run(db,
                "CREATE TABLE IF NOT EXISTS schema_migrations (version INT PRIMARY KEY, applied_at DOUBLE NOT NULL) "
                "ENGINE=InnoDB",
                NULL, 0, NULL)) {
        snprintf(err, errlen, "%s", db->err);
        return false;
    }
    int have = db_schema_version(db);
    if (have < 0) {
        snprintf(err, errlen, "%s", db->err);
        return false;
    }
    int latest = (int)(sizeof(k_migrations) / sizeof(k_migrations[0]));
    if (have > latest) {
        snprintf(err, errlen, "The project store is from a newer Scribodo (schema %d, this version knows %d)", have, latest);
        return false;
    }
    for (int v = have + 1; v <= latest && v <= upto; v++) {
        for (const char *const *stmt = k_migrations[v - 1]; *stmt; stmt++) {
            if (!db_run(db, *stmt, NULL, 0, NULL)) {
                snprintf(err, errlen, "Migration %d failed: %s", v, db->err);
                return false;
            }
            /* DDL commits itself: a process that dies here has applied this statement and holds no receipt for the
             * migration, so every statement must be re-runnable (GAP-004 matrix, migration rows). */
            crash_point("migration.statement");
        }
        DbArg a[2] = { DB_I(v), DB_D(0) };
        if (!db_run(db, "INSERT INTO schema_migrations (version, applied_at) VALUES (?, UNIX_TIMESTAMP(NOW(6)) + ?)", a, 2,
                    NULL)) {
            snprintf(err, errlen, "Recording migration %d failed: %s", v, db->err);
            return false;
        }
        crash_point("migration.receipt");
    }
    return true;
}
