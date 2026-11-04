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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    if (!t_db) {
        char err[256];
        t_db = db_open(&g_cfg, err, sizeof(err));
    }
    return t_db;
}

void store_thread_done(void)
{
    if (t_db) db_close(t_db);
    t_db = NULL;
}

static bool read_marker(const char *project_dir, char *id, size_t idlen)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.scribodo/store.json", project_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* {"project_id":"p-…"} — parsed by hand to keep this file free of cJSON. */
    const char *k = strstr(buf, "\"project_id\"");
    const char *q = k ? strchr(k + 12, '"') : NULL;
    const char *e = q ? strchr(q + 1, '"') : NULL;
    if (!q || !e || e - q - 1 <= 0 || (size_t)(e - q - 1) >= idlen) return false;
    memcpy(id, q + 1, (size_t)(e - q - 1));
    id[e - q - 1] = '\0';
    for (char *c = id; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || *c == '-' || *c == 'p')) return false;
    return id[0] == 'p';
}

StoreMode store_mode(const char *project_dir, char *project_id, size_t idlen, Db **db)
{
    if (db) *db = NULL;
    char id[64];
    if (!project_dir || !project_dir[0] || !read_marker(project_dir, id, sizeof(id))) return STORE_FILES;
    Db *d = store_db();
    if (!d) return STORE_PAUSED;
    if (project_id) snprintf(project_id, idlen, "%s", id);
    if (db) *db = d;
    return STORE_DB;
}

bool store_mark_imported(const char *project_dir, const char *project_id)
{
    char dir[PATH_MAX + 32], path[PATH_MAX + 64], tmp[PATH_MAX + 96];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/store.json", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "{\"project_id\":\"%s\",\"version\":1}\n", project_id);
    bool ok = fchmod(fd, 0644) == 0 && write(fd, buf, (size_t)n) == n && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

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
