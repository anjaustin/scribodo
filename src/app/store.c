#define _DARWIN_C_SOURCE
#include "store.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
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

struct StoreManager {
    DbConfig cfg;
    atomic_bool online;
    atomic_bool forced_offline;
    atomic_uint_fast64_t generation;
    pthread_mutex_t mutex;
};

typedef struct {
    char dir[PATH_MAX];
    char id[64];
    int verdict;
    char original[PATH_MAX];
    double checked;
    uint64_t generation;
    unsigned long long dev, ino;
    unsigned long long marker_dev, marker_ino;
    bool marker_ident;
} StoreVerification;

typedef struct StoreThreadContext {
    StoreManager *manager;
    uint64_t generation;
    Db *db;
    char importing[PATH_MAX];
    double write_time;
    StoreVerification verify;
    struct StoreThreadContext *next;
} StoreThreadContext;

static _Thread_local StoreThreadContext *thread_contexts;

StoreManager *store_manager_create(void)
{
    StoreManager *manager = calloc(1, sizeof(*manager));
    if (!manager) return NULL;
    atomic_init(&manager->generation, 1);
    atomic_init(&manager->online, false);
    atomic_init(&manager->forced_offline, false);
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        free(manager);
        return NULL;
    }
    return manager;
}

static StoreThreadContext *thread_context(StoreManager *manager, bool create)
{
    if (!manager) return NULL;
    StoreThreadContext **link = &thread_contexts;
    while (*link) {
        StoreThreadContext *context = *link;
        if (context->manager == manager) return context;
        link = &context->next;
    }
    if (!create) return NULL;
    StoreThreadContext *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->manager = manager;
    context->next = thread_contexts;
    thread_contexts = context;
    return context;
}

void store_manager_destroy(StoreManager *manager)
{
    if (!manager) return;
    store_stop(manager);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

bool store_start(StoreManager *manager, const char *database, char *why, size_t whylen)
{
    if (!manager) return false;
    DbConfig config;
    if (!db_config_default(&config, why, whylen)) return false;
    if (database) snprintf(config.database, sizeof(config.database), "%s", database);

    pthread_mutex_lock(&manager->mutex);
    atomic_store(&manager->online, false);
    manager->cfg = config; /* probes keep targeting a requested database even if opening it fails */
    uint64_t generation = atomic_fetch_add(&manager->generation, 1) + 1;
    if (atomic_load(&manager->forced_offline)) {
        pthread_mutex_unlock(&manager->mutex);
        snprintf(why, whylen, "The project store is off");
        return false;
    }
    Db *db = db_open(&config, why, whylen);
    if (!db) {
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    if (!db_migrate(db, why, whylen)) {
        db_close(db);
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    StoreThreadContext *context = thread_context(manager, true);
    if (!context) {
        db_close(db);
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    if (context->db) db_close(context->db);
    context->db = db;
    context->generation = generation;
    context->importing[0] = '\0';
    context->write_time = 0;
    context->verify.checked = 0;
    context->verify.generation = generation;
    atomic_store(&manager->online, true);
    pthread_mutex_unlock(&manager->mutex);
    return true;
}

void store_stop(StoreManager *manager)
{
    if (!manager) return;
    pthread_mutex_lock(&manager->mutex);
    atomic_store(&manager->online, false);
    atomic_fetch_add(&manager->generation, 1);
    pthread_mutex_unlock(&manager->mutex);
    store_thread_done(manager);
}

bool store_online(const StoreManager *manager)
{
    return manager && atomic_load(&manager->online) && !atomic_load(&manager->forced_offline);
}
void store_database(StoreManager *manager, char *out, size_t outlen)
{
    if (!out || !outlen) return;
    if (!manager) {
        snprintf(out, outlen, "scribodo");
        return;
    }
    pthread_mutex_lock(&manager->mutex);
    snprintf(out, outlen, "%s", manager->cfg.database[0] ? manager->cfg.database : "scribodo");
    pthread_mutex_unlock(&manager->mutex);
}
void store_set_offline(StoreManager *manager, bool offline)
{
    if (!manager) return;
    pthread_mutex_lock(&manager->mutex);
    bool changed = atomic_load(&manager->forced_offline) != offline;
    atomic_store(&manager->forced_offline, offline);
    if (changed) atomic_fetch_add(&manager->generation, 1);
    pthread_mutex_unlock(&manager->mutex);
}

Db *store_db(StoreManager *manager)
{
    if (!manager) return NULL;
    pthread_mutex_lock(&manager->mutex);
    if (!atomic_load(&manager->online) || atomic_load(&manager->forced_offline)) {
        pthread_mutex_unlock(&manager->mutex);
        return NULL;
    }
    StoreThreadContext *context = thread_context(manager, true);
    if (!context) {
        pthread_mutex_unlock(&manager->mutex);
        return NULL;
    }
    uint64_t generation = atomic_load(&manager->generation);
    DbConfig config = manager->cfg;
    pthread_mutex_unlock(&manager->mutex);

    /* Contexts and connections belong exclusively to this thread. Neither
     * closing an old connection nor opening a new one may hold the manager
     * mutex: a worker's network wait must not stall UI lifecycle calls. The
     * owner must join borrowed workers before destroying the manager. */
    if (context->generation != generation) {
        if (context->db) db_close(context->db);
        context->db = NULL;
        context->generation = generation;
        context->verify.checked = 0;
    }
    /* A lost connection is dropped and reopened; if the database can't be
     * reached any more the store goes offline (the app retries and shows
     * the paused state) instead of failing every call while looking
     * online (red-team 008). */
    if (context->db && db_broken(context->db)) {
        db_close(context->db);
        context->db = NULL;
    }
    if (!context->db) {
        char err[256];
        Db *opened = db_open(&config, err, sizeof(err));
        pthread_mutex_lock(&manager->mutex);
        /* Every configuration/lifecycle change advances the generation. A
         * stale success must not escape into TLS, and a stale failure must
         * not take a newer store offline (even after an offline/on roundtrip). */
        bool current = generation == atomic_load(&manager->generation) &&
                       atomic_load(&manager->online) && !atomic_load(&manager->forced_offline);
        if (current) {
            context->db = opened;
            if (!opened) atomic_store(&manager->online, false);
        }
        pthread_mutex_unlock(&manager->mutex);
        if (!current) {
            if (opened) db_close(opened);
            return NULL;
        }
    }
    return context->db;
}

bool store_probe(StoreManager *manager)
{
    if (!manager || atomic_load(&manager->forced_offline)) return false;
    DbConfig c;
    char err[256];
    if (!db_config_default(&c, err, sizeof(err))) return false;
    pthread_mutex_lock(&manager->mutex);
    bool configured = manager->cfg.database[0] != '\0';
    if (configured) snprintf(c.database, sizeof(c.database), "%s", manager->cfg.database);
    pthread_mutex_unlock(&manager->mutex);
    const char *dbn = getenv("SCRIBODO_DB_NAME");
    if (!configured && dbn) snprintf(c.database, sizeof(c.database), "%s", dbn);
    c.timeout_s = 2;
    Db *db = db_open(&c, err, sizeof(err));
    if (db) db_close(db);
    return db != NULL;
}

void store_thread_done(StoreManager *manager)
{
    StoreThreadContext **link = &thread_contexts;
    while (*link) {
        StoreThreadContext *context = *link;
        if (context->manager == manager) {
            *link = context->next;
            if (context->db) db_close(context->db);
            free(context);
            return;
        }
        link = &context->next;
    }
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
    /* UI identity snapshots may inspect malformed markers. Opening a FIFO must
     * not wait for a writer; validate the opened object, not a racy path stat. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat marker_stat;
    if (fstat(fd, &marker_stat) != 0 || !S_ISREG(marker_stat.st_mode)) { close(fd); return false; }
    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return false; }
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

bool store_project_identity(StoreManager *manager, const char *project_dir, StoreProjectIdentity *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!manager || !project_dir || !project_dir[0]) return false;
    uint64_t generation = atomic_load(&manager->generation);
    struct stat before, after;
    Marker marker;
    if (stat(project_dir, &before) != 0 || !S_ISDIR(before.st_mode) ||
        !read_marker(project_dir, &marker) ||
        stat(project_dir, &after) != 0 || !S_ISDIR(after.st_mode) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        generation != atomic_load(&manager->generation)) return false;
    out->generation = generation;
    snprintf(out->project_id, sizeof(out->project_id), "%s", marker.id);
    out->device = (unsigned long long)after.st_dev;
    out->inode = (unsigned long long)after.st_ino;
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
void store_set_importing(StoreManager *manager, const char *project_dir)
{
    StoreThreadContext *context = thread_context(manager, true);
    if (context) snprintf(context->importing, sizeof(context->importing), "%s", project_dir ? project_dir : "");
}

void store_set_write_time(StoreManager *manager, double t)
{
    StoreThreadContext *context = thread_context(manager, true);
    if (context) context->write_time = t;
}
double store_write_time(StoreManager *manager)
{
    StoreThreadContext *context = thread_context(manager, false);
    if (context && context->write_time > 0) return context->write_time;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

enum { VERDICT_OK = 0 };

static int verify(StoreManager *manager, Db *d, const char *project_dir, const Marker *m)
{
    StoreThreadContext *context = thread_context(manager, true);
    if (!context) return VERDICT_OK;
    StoreVerification *verification = &context->verify;
    /* Tag the verdict with the generation of the connection that produced it,
     * not the manager's possibly newer generation. store_db() invalidates this
     * thread's context before the next operation after a restart. */
    uint64_t generation = context->generation;
    if (verification->generation != generation) {
        memset(verification, 0, sizeof(*verification));
        verification->generation = generation;
    }
    /* A persistent worker may see a different folder at a previously verified
     * path before the TTL expires. Check the actual identity before any hit,
     * and include the marker's claimed identity (read anew by the caller).
     * External DB-only path edits with unchanged identities still use the TTL. */
    struct stat st;
    if (stat(project_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        verification->checked = 0;
        verification->original[0] = '\0';
        return verification->verdict = STORE_PROBLEM_MISSING;
    }
    double t = now_s();
    if (!strcmp(verification->dir, project_dir) && !strcmp(verification->id, m->id) &&
        verification->dev == (unsigned long long)st.st_dev && verification->ino == (unsigned long long)st.st_ino &&
        verification->marker_ident == m->ident && verification->marker_dev == m->dev && verification->marker_ino == m->ino &&
        t - verification->checked < 30.0)
        return verification->verdict;
    snprintf(verification->dir, sizeof(verification->dir), "%s", project_dir);
    snprintf(verification->id, sizeof(verification->id), "%s", m->id);
    verification->dev = (unsigned long long)st.st_dev;
    verification->ino = (unsigned long long)st.st_ino;
    verification->marker_ident = m->ident;
    verification->marker_dev = m->dev;
    verification->marker_ino = m->ino;
    verification->checked = t;
    verification->original[0] = '\0';
    DbRows r;
    if (!db_query(d, "SELECT path FROM projects WHERE id = ?", (DbArg[1]){ DB_S(m->id) }, 1, &r)) {
        verification->checked = 0; /* not a verdict: try again next time */
        return VERDICT_OK;
    }
    const char *stored_path;
    bool found = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_text(&r, 0, 0, &stored_path) &&
                 strlen(stored_path) < sizeof(verification->original);
    if (found) snprintf(verification->original, sizeof(verification->original), "%s", stored_path);
    db_rows_free(&r);
    if (!found) return verification->verdict = STORE_PROBLEM_MISSING;
    bool other_inode = m->ident && (m->dev != (unsigned long long)st.st_dev || m->ino != (unsigned long long)st.st_ino);
    if (other_inode) {
        /* Another folder with this project's marker. A copy only if the
         * original still is this project; otherwise the folder was moved
         * across volumes or restored from a backup, and is the project. */
        Marker om;
        struct stat ost;
        char oreal[PATH_MAX], here[PATH_MAX];
        bool original_alive = stat(verification->original, &ost) == 0 && read_marker(verification->original, &om) &&
                               !strcmp(om.id, m->id) && realpath(verification->original, oreal) &&
                               realpath(project_dir, here) && strcmp(oreal, here) != 0;
        if (original_alive) return verification->verdict = STORE_PROBLEM_COPY;
    }
    /* The same folder. Moved? Then the project follows it. */
    char real[PATH_MAX];
    if (!realpath(project_dir, real)) snprintf(real, sizeof(real), "%s", project_dir);
    if (strcmp(real, verification->original) != 0) {
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
    return verification->verdict = VERDICT_OK;
}

StoreMode store_mode(StoreManager *manager, const char *project_dir, char *project_id, size_t idlen, Db **db)
{
    if (db) *db = NULL;
    Marker m;
    if (!project_dir || !project_dir[0]) return STORE_FILES;
    if (!read_marker(project_dir, &m)) {
        /* A malformed/unreadable marker is not consent to create a parallel
         * file history. Only absence permits the legacy file-store route. */
        char path[PATH_MAX + 64];
        struct stat st;
        snprintf(path, sizeof(path), "%s/.scribodo/store.json", project_dir);
        if (lstat(path, &st) == 0 || errno != ENOENT) return STORE_PAUSED;
        StoreThreadContext *context = thread_context(manager, false);
        if (context && !strcmp(context->importing, project_dir)) return STORE_FILES;
        return import_in_progress(project_dir) ? STORE_PAUSED : STORE_FILES;
    }
    Db *d = store_db(manager);
    if (!d) return STORE_PAUSED;
    if (verify(manager, d, project_dir, &m) != VERDICT_OK) return STORE_PAUSED; /* a copy, or its project is missing */
    if (project_id) snprintf(project_id, idlen, "%s", m.id);
    if (db) *db = d;
    return STORE_DB;
}

int store_problem(StoreManager *manager, const char *project_dir, char *original, size_t len)
{
    Marker m;
    if (original && len) original[0] = '\0';
    if (!project_dir || !read_marker(project_dir, &m)) return 0;
    Db *d = store_db(manager);
    if (!d) return 0;
    int v = verify(manager, d, project_dir, &m);
    StoreThreadContext *context = thread_context(manager, false);
    if (original && len && context) snprintf(original, len, "%s", context->verify.original);
    return v;
}

void store_forget_verification(StoreManager *manager)
{
    StoreThreadContext *context = thread_context(manager, false);
    if (context) context->verify.checked = 0;
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
    const char *value;
    bool found = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_text(&r, 0, 0, &value) &&
                 strlen(value) < outlen;
    if (found) snprintf(out, outlen, "%s", value);
    db_rows_free(&r);
    return found;
}

bool store_project_id(StoreManager *manager, const char *project_dir, bool create, char *out, size_t outlen)
{
    Db *db = store_db(manager);
    return store_project_id_db(db, project_dir, create, out, outlen);
}

bool store_project_id_db(Db *db, const char *project_dir, bool create, char *out, size_t outlen)
{
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

bool store_document_id(StoreManager *manager, const char *project_id, const char *name, bool create, char *out, size_t outlen)
{
    Db *db = store_db(manager);
    return store_document_id_db(db, project_id, name, create, out, outlen);
}

bool store_document_id_db(Db *db, const char *project_id, const char *name, bool create, char *out, size_t outlen)
{
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

bool store_document_name(StoreManager *manager, const char *document_id, char *out, size_t outlen)
{
    Db *db = store_db(manager);
    if (!db) return false;
    DbArg k[1] = { DB_S(document_id) };
    return one_value(db, "SELECT name FROM documents WHERE id = ?", k, 1, out, outlen);
}

bool store_rename_document(StoreManager *manager, const char *project_id, const char *from, const char *to)
{
    Db *db = store_db(manager);
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
