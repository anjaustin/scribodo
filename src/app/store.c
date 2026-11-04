#define _DARWIN_C_SOURCE
#include "store.h"

#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void sha256_hex(const char *s, char out[65]);
static double now_s(void);

static DbConfig g_cfg;
static atomic_bool g_online;
static atomic_bool g_forced_offline;
static _Thread_local Db *t_db;

bool store_start(const char *database, char *why, size_t whylen)
{
    atomic_store(&g_online, false);
    if (atomic_load(&g_forced_offline)) {
        snprintf(why, whylen, "The project store is off");
        return false;
    }
    if (!db_config_default(&g_cfg, why, whylen)) return false;
    if (database) snprintf(g_cfg.database, sizeof(g_cfg.database), "%s", database);
    Db *db = db_open(&g_cfg, why, whylen);
    if (!db) return false;
    if (!db_migrate(db, why, whylen)) {
        db_close(db);
        return false;
    }
    if (t_db) db_close(t_db);
    t_db = db;
    atomic_store(&g_online, true);
    return true;
}

void store_stop(void)
{
    atomic_store(&g_online, false);
    store_thread_done();
}

bool store_online(void) { return atomic_load(&g_online) && !atomic_load(&g_forced_offline); }
const char *store_database(void) { return g_cfg.database[0] ? g_cfg.database : "scribodo"; }
void store_set_offline(bool offline) { atomic_store(&g_forced_offline, offline); }

Db *store_db(void)
{
    if (!store_online()) return NULL;
    /* A lost connection is dropped and reopened; if the database can't be
     * reached any more the store goes offline (the app retries and shows
     * the paused state) instead of failing every call while looking
     * online (red-team 008). */
    if (t_db && db_broken(t_db)) {
        db_close(t_db);
        t_db = NULL;
    }
    if (!t_db) {
        char err[256];
        t_db = db_open(&g_cfg, err, sizeof(err));
        if (!t_db) atomic_store(&g_online, false);
    }
    return t_db;
}

bool store_probe(void)
{
    if (atomic_load(&g_forced_offline)) return false;
    DbConfig c;
    char err[256];
    if (!db_config_default(&c, err, sizeof(err))) return false;
    if (g_cfg.database[0]) snprintf(c.database, sizeof(c.database), "%s", g_cfg.database);
    const char *dbn = getenv("SCRIBODO_DB_NAME");
    if (dbn) snprintf(c.database, sizeof(c.database), "%s", dbn);
    c.timeout_s = 2;
    Db *db = db_open(&c, err, sizeof(err));
    if (db) db_close(db);
    return db != NULL;
}

void store_thread_done(void)
{
    if (t_db) db_close(t_db);
    t_db = NULL;
}

/* The marker: {"project_id":"p-…","version":2,"dev":N,"ino":N}. The folder's
 * identity (device and inode) tells a moved folder (same identity, new path:
 * fine) from a copy or a replacement (another identity: it must not write
 * into the original's project). Parsed by hand to keep cJSON out of here. */
typedef struct {
    char id[64];
    unsigned long long dev, ino;
    bool ident;
} Marker;

static bool read_marker(const char *project_dir, Marker *m)
{
    memset(m, 0, sizeof(*m));
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.scribodo/store.json", project_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *k = strstr(buf, "\"project_id\"");
    const char *q = k ? strchr(k + 12, '"') : NULL;
    const char *e = q ? strchr(q + 1, '"') : NULL;
    if (!q || !e || e - q - 1 <= 0 || (size_t)(e - q - 1) >= sizeof(m->id)) return false;
    memcpy(m->id, q + 1, (size_t)(e - q - 1));
    m->id[e - q - 1] = '\0';
    for (char *c = m->id; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || *c == '-' || *c == 'p')) return false;
    if (m->id[0] != 'p') return false;
    const char *dv = strstr(buf, "\"dev\":"), *in = strstr(buf, "\"ino\":");
    if (dv && in) {
        m->dev = strtoull(dv + 6, NULL, 10);
        m->ino = strtoull(in + 6, NULL, 10);
        m->ident = true;
    }
    return true;
}

static bool write_marker(const char *project_dir, const char *project_id)
{
    struct stat st;
    if (stat(project_dir, &st) != 0) return false;
    char dir[PATH_MAX + 32], path[PATH_MAX + 64], tmp[PATH_MAX + 96];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/store.json", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "{\"project_id\":\"%s\",\"version\":2,\"dev\":%llu,\"ino\":%llu}\n", project_id,
                     (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
    bool ok = fchmod(fd, 0644) == 0 && write(fd, buf, (size_t)n) == n && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    if (ok) { /* make the rename durable too */
        int dfd = open(dir, O_RDONLY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return ok;
}

bool store_mark_imported(const char *project_dir, const char *project_id) { return write_marker(project_dir, project_id); }

/* Another process (or thread) is importing this folder right now. */
static bool import_in_progress(const char *project_dir)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.scribodo/import.lock", project_dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool busy = flock(fd, LOCK_SH | LOCK_NB) != 0;
    if (!busy) flock(fd, LOCK_UN);
    close(fd);
    return busy;
}

/* The thread running an import reads the files it is importing (everyone
 * else sees the project paused meanwhile). */
static _Thread_local const char *t_importing;

void store_set_importing(const char *project_dir) { t_importing = project_dir; }

/* Per-thread cache of the last folder verified (store_mode runs often). */
static _Thread_local struct {
    char dir[PATH_MAX];
    char id[64];
    int verdict; /* STORE_OK_VERDICT, PROBLEM_COPY, PROBLEM_MISSING */
    char original[PATH_MAX];
    double checked;
} t_verify;

enum { VERDICT_OK = 0 };

static int verify(Db *d, const char *project_dir, const Marker *m)
{
    double t = now_s();
    if (!strcmp(t_verify.dir, project_dir) && !strcmp(t_verify.id, m->id) && t - t_verify.checked < 30.0) return t_verify.verdict;
    snprintf(t_verify.dir, sizeof(t_verify.dir), "%s", project_dir);
    snprintf(t_verify.id, sizeof(t_verify.id), "%s", m->id);
    t_verify.checked = t;
    t_verify.original[0] = '\0';
    struct stat st;
    if (stat(project_dir, &st) != 0) return t_verify.verdict = STORE_PROBLEM_MISSING;
    DbRows r;
    if (!db_query(d, "SELECT path FROM projects WHERE id = ?", (DbArg[1]){ DB_S(m->id) }, 1, &r)) {
        t_verify.checked = 0; /* not a verdict: try again next time */
        return VERDICT_OK;
    }
    bool found = r.nrows > 0;
    if (found) snprintf(t_verify.original, sizeof(t_verify.original), "%s", db_cell(&r, 0, 0));
    db_rows_free(&r);
    if (!found) return t_verify.verdict = STORE_PROBLEM_MISSING;
    bool other_inode = m->ident && (m->dev != (unsigned long long)st.st_dev || m->ino != (unsigned long long)st.st_ino);
    if (other_inode) {
        /* Another folder with this project's marker. A copy only if the
         * original still is this project; otherwise the folder was moved
         * across volumes or restored from a backup, and is the project. */
        Marker om;
        struct stat ost;
        char oreal[PATH_MAX], here[PATH_MAX];
        bool original_alive = stat(t_verify.original, &ost) == 0 && read_marker(t_verify.original, &om) &&
                              !strcmp(om.id, m->id) && realpath(t_verify.original, oreal) &&
                              realpath(project_dir, here) && strcmp(oreal, here) != 0;
        if (original_alive) return t_verify.verdict = STORE_PROBLEM_COPY;
    }
    /* The same folder. Moved? Then the project follows it. */
    char real[PATH_MAX];
    if (!realpath(project_dir, real)) snprintf(real, sizeof(real), "%s", project_dir);
    if (strcmp(real, t_verify.original) != 0) {
        char hash[65];
        sha256_hex(real, hash);
        /* A row that claims the new path (a project registered there earlier)
         * keeps its data but gives up the path. */
        char moved[80];
        snprintf(moved, sizeof(moved), "moved-%s", m->id);
        db_run(d, "UPDATE projects SET path_hash = CONCAT('superseded-', id) WHERE path_hash = ? AND id <> ?",
               (DbArg[2]){ DB_S(hash), DB_S(m->id) }, 2, NULL);
        db_run(d, "UPDATE projects SET path = ?, path_hash = ? WHERE id = ?", (DbArg[3]){ DB_S(real), DB_S(hash), DB_S(m->id) }, 3,
               NULL);
    }
    if (!m->ident || other_inode) write_marker(project_dir, m->id); /* upgrade a version 1 marker / adopt the new folder */
    return t_verify.verdict = VERDICT_OK;
}

StoreMode store_mode(const char *project_dir, char *project_id, size_t idlen, Db **db)
{
    if (db) *db = NULL;
    Marker m;
    if (!project_dir || !project_dir[0]) return STORE_FILES;
    if (!read_marker(project_dir, &m)) {
        if (t_importing && !strcmp(t_importing, project_dir)) return STORE_FILES;
        return import_in_progress(project_dir) ? STORE_PAUSED : STORE_FILES;
    }
    Db *d = store_db();
    if (!d) return STORE_PAUSED;
    if (verify(d, project_dir, &m) != VERDICT_OK) return STORE_PAUSED; /* a copy, or its project is missing */
    if (project_id) snprintf(project_id, idlen, "%s", m.id);
    if (db) *db = d;
    return STORE_DB;
}

int store_problem(const char *project_dir, char *original, size_t len)
{
    Marker m;
    if (original && len) original[0] = '\0';
    if (!project_dir || !read_marker(project_dir, &m)) return 0;
    Db *d = store_db();
    if (!d) return 0;
    int v = verify(d, project_dir, &m);
    if (original && len) snprintf(original, len, "%s", t_verify.original);
    return v;
}

void store_forget_verification(void) { t_verify.checked = 0; }

void store_new_id(char prefix, char *out, size_t outlen)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(out, outlen, "%c-%s-%08x%08x", prefix, stamp, arc4random(), arc4random());
}

static void sha256_hex(const char *s, char out[65])
{
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(s, (CC_LONG)strlen(s), d);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static bool one_value(Db *db, const char *sql, const DbArg *args, size_t n, char *out, size_t outlen)
{
    DbRows r;
    if (!db_query(db, sql, args, n, &r)) return false;
    bool found = r.nrows > 0 && db_cell(&r, 0, 0);
    if (found) snprintf(out, outlen, "%s", db_cell(&r, 0, 0));
    db_rows_free(&r);
    return found;
}

bool store_project_id(const char *project_dir, bool create, char *out, size_t outlen)
{
    Db *db = store_db();
    if (!db) return false;
    char real[PATH_MAX], hash[65];
    if (!realpath(project_dir, real)) snprintf(real, sizeof(real), "%s", project_dir);
    sha256_hex(real, hash);
    DbArg h[1] = { DB_S(hash) };
    if (one_value(db, "SELECT id FROM projects WHERE path_hash = ?", h, 1, out, outlen)) return true;
    if (!create) return false;
    char id[40];
    store_new_id('p', id, sizeof(id));
    const char *name = strrchr(real, '/');
    name = name ? name + 1 : real;
    double t = now_s();
    DbArg a[6] = { DB_S(id), DB_S(name), DB_S(real), DB_S(hash), DB_D(t), DB_D(t) };
    /* Another connection may have created it meanwhile: insert-or-ignore,
     * then read back whichever row won. */
    db_run(db,
           "INSERT IGNORE INTO projects (id, name, path, path_hash, created_at, updated_at) VALUES (?,?,?,?,?,?)",
           a, 6, NULL);
    return one_value(db, "SELECT id FROM projects WHERE path_hash = ?", h, 1, out, outlen);
}

bool store_document_id(const char *project_id, const char *name, bool create, char *out, size_t outlen)
{
    Db *db = store_db();
    if (!db || strchr(name, '/')) return false;
    DbArg k[2] = { DB_S(project_id), DB_S(name) };
    if (one_value(db, "SELECT id FROM documents WHERE project_id = ? AND name = ?", k, 2, out, outlen)) return true;
    if (!create) return false;
    char id[40];
    store_new_id('d', id, sizeof(id));
    DbArg a[4] = { DB_S(id), DB_S(project_id), DB_S(name), DB_D(now_s()) };
    db_run(db, "INSERT IGNORE INTO documents (id, project_id, name, created_at) VALUES (?,?,?,?)", a, 4, NULL);
    return one_value(db, "SELECT id FROM documents WHERE project_id = ? AND name = ?", k, 2, out, outlen);
}

bool store_document_name(const char *document_id, char *out, size_t outlen)
{
    Db *db = store_db();
    if (!db) return false;
    DbArg k[1] = { DB_S(document_id) };
    return one_value(db, "SELECT name FROM documents WHERE id = ?", k, 1, out, outlen);
}

bool store_rename_document(const char *project_id, const char *from, const char *to)
{
    Db *db = store_db();
    if (!db) return false;
    if (!db_begin(db)) return false;
    /* A row already holding the new name belongs to an earlier, deleted
     * document: move it aside so the renamed document keeps its own id. */
    char aside[300];
    snprintf(aside, sizeof(aside), "%s (earlier %ld)", to, (long)time(NULL));
    DbArg a1[3] = { DB_S(aside), DB_S(project_id), DB_S(to) };
    DbArg a2[4] = { DB_S(to), DB_D(now_s()), DB_S(project_id), DB_S(from) };
    bool ok = db_run(db, "UPDATE documents SET name = ? WHERE project_id = ? AND name = ? COLLATE utf8mb4_bin", a1, 3, NULL) &&
              db_run(db, "UPDATE documents SET name = ?, renamed_at = ? WHERE project_id = ? AND name = ? COLLATE utf8mb4_bin",
                     a2, 4, NULL);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}
