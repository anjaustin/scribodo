/* No services or production hooks: link store.c against this gated DB double.
 * cc -std=c11 -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -Isrc \
 *   src/app/store.c tests/test_store_connections.c -lpthread -o build/test_store_connections
 */
#define _DARWIN_C_SOURCE
#include "app/store.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct Db {
    pthread_t owner;
    char database[64];
    bool broken;
    bool in_tx;
};

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool entered, released, fail_open, fail_start;
static unsigned opened_count, closed_count;
static _Thread_local bool gated;
static _Thread_local bool budget_abort;
static _Thread_local atomic_bool *cancel_on_open;
static _Thread_local const char *query_path;
static _Thread_local const char *query_id;
static _Thread_local bool query_fail, query_break;
static _Thread_local StoreManager *query_transition;
static _Thread_local atomic_bool *query_cancel;
static _Thread_local unsigned query_count, run_count;

static double deadline_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

bool db_config_default(DbConfig *config, char *err, size_t len)
{
    (void)err; (void)len;
    memset(config, 0, sizeof(*config));
    snprintf(config->database, sizeof(config->database), "default");
    return true;
}

Db *db_open(const DbConfig *config, char *err, size_t len)
{
    (void)err; (void)len;
    if (gated) {
        pthread_mutex_lock(&gate_mutex);
        entered = true;
        pthread_cond_broadcast(&gate_cond);
        while (!released) pthread_cond_wait(&gate_cond, &gate_mutex);
        bool fail = fail_open;
        pthread_mutex_unlock(&gate_mutex);
        if (fail) return NULL;
    } else if (fail_start) return NULL;
    Db *db = calloc(1, sizeof(*db));
    assert(db);
    db->owner = pthread_self();
    snprintf(db->database, sizeof(db->database), "%s", config->database);
    pthread_mutex_lock(&gate_mutex);
    opened_count++;
    pthread_mutex_unlock(&gate_mutex);
    return db;
}

Db *db_open_until(const DbConfig *config, const atomic_bool *cancel, double deadline, char *err, size_t len)
{
    if (cancel_on_open) atomic_store(cancel_on_open, true);
    if (budget_abort) {
        struct timespec delay = { .tv_nsec = 20000000 };
        nanosleep(&delay, NULL);
    }
    if (!isfinite(deadline) || deadline <= deadline_now() || (cancel && atomic_load(cancel))) {
        snprintf(err, len, "budget exhausted");
        return NULL;
    }
    return db_open(config, err, len);
}

int db_socket(Db *db) { (void)db; return -1; }

void db_close(Db *db)
{
    assert(pthread_equal(db->owner, pthread_self()));
    pthread_mutex_lock(&gate_mutex);
    closed_count++;
    pthread_mutex_unlock(&gate_mutex);
    free(db);
}

bool db_broken(Db *db)
{
    assert(pthread_equal(db->owner, pthread_self()));
    return db->broken;
}

bool db_migrate(Db *db, char *err, size_t len)
{
    (void)db; (void)err; (void)len;
    return true;
}

bool db_in_transaction(Db *db)
{
    assert(pthread_equal(db->owner, pthread_self()));
    return db->in_tx;
}

bool db_query(Db *db, const char *sql, const DbArg *args, size_t n, DbRows *out)
{
    assert(pthread_equal(db->owner, pthread_self()));
    assert(query_path && query_id);
    assert(!strcmp(sql, "SELECT path FROM projects WHERE id = ?"));
    assert(n == 1 && args[0].type == 's' && !strcmp(args[0].s, query_id));
    query_count++;
    if (query_transition) {
        store_set_offline(query_transition, true);
        store_set_offline(query_transition, false);
    }
    if (query_cancel) atomic_store(query_cancel, true);
    if (query_break) db->broken = true;
    if (query_fail) return false;
    memset(out, 0, sizeof(*out));
    out->nrows = out->ncols = 1;
    out->cells = calloc(1, sizeof(*out->cells));
    assert(out->cells);
    out->cells[0] = strdup(query_path);
    assert(out->cells[0]);
    return true;
}
bool db_run(Db *db, const char *sql, const DbArg *args, size_t n, uint64_t *affected)
{
    (void)args; (void)n; (void)affected;
    assert(pthread_equal(db->owner, pthread_self()));
    assert(query_path && !strncmp(sql, "UPDATE projects SET ", 20));
    run_count++;
    return true;
}
void db_rows_free(DbRows *rows)
{
    for (size_t i = 0; i < rows->nrows * rows->ncols; i++) free(rows->cells[i]);
    free(rows->cells);
}
bool db_rows_has_columns(const DbRows *rows, size_t n) { return rows && rows->ncols == n; }
bool db_cell_text(const DbRows *rows, size_t r, size_t c, const char **out)
{
    if (!rows || r >= rows->nrows || c >= rows->ncols || !rows->cells[r * rows->ncols + c]) return false;
    *out = rows->cells[r * rows->ncols + c];
    return true;
}
bool db_begin(Db *db) { (void)db; abort(); }
bool db_commit(Db *db) { (void)db; abort(); }
void db_rollback(Db *db) { (void)db; abort(); }

typedef struct {
    StoreManager *manager;
    bool stale;
    bool bounded;
    const char *retry_database;
} Worker;

static void *connect_worker(void *arg)
{
    Worker *worker = arg;
    gated = true;
    Db *db = worker->bounded ? store_db_until(worker->manager, NULL, deadline_now() + 5000) : store_db(worker->manager);
    if (worker->stale || fail_open) assert(!db);
    else {
        assert(db && !strcmp(db->database, "old"));
        assert(store_db(worker->manager) == db); /* TLS reuse */
        db->broken = true;
        Db *replacement = store_db(worker->manager);
        assert(replacement && !replacement->broken);
    }
    gated = false;
    if (worker->retry_database) {
        db = store_db(worker->manager);
        assert(db && !strcmp(db->database, worker->retry_database));
    } else if (worker->stale || fail_open) assert(!store_db(worker->manager));
    store_thread_done(worker->manager);
    store_thread_done(worker->manager); /* idempotent */
    return NULL;
}

enum Transition { NONE, STOP, RESTART, OFFLINE, OFFLINE_ROUNDTRIP, FAILED_RESTART, STOP_RESTART };

static void run_case(enum Transition transition, bool fail, bool bounded)
{
    StoreManager *manager = store_manager_create();
    assert(manager);
    char why[256];
    assert(store_start(manager, "old", why, sizeof(why)));
    pthread_mutex_lock(&gate_mutex);
    entered = released = false;
    fail_open = fail;
    pthread_mutex_unlock(&gate_mutex);
    Worker worker = { .manager = manager, .stale = transition != NONE, .bounded = bounded };
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, connect_worker, &worker));
    pthread_mutex_lock(&gate_mutex);
    while (!entered) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);

    /* These execute before releasing the network gate. The old implementation
     * deadlocks here; alarm/watchdog makes that regression bounded. */
    char database[64];
    store_database(manager, database, sizeof(database));
    assert(!strcmp(database, "old"));
    switch (transition) {
    case NONE: break;
    case STOP: store_stop(manager); break;
    case STOP_RESTART:
        store_stop(manager);
        /* fall through */
    case RESTART:
        assert(store_start(manager, "new", why, sizeof(why)));
        worker.retry_database = "new";
        break;
    case OFFLINE: store_set_offline(manager, true); break;
    case OFFLINE_ROUNDTRIP:
        store_set_offline(manager, true);
        store_set_offline(manager, false);
        worker.retry_database = "old";
        break;
    case FAILED_RESTART:
        fail_start = true;
        assert(!store_start(manager, "failed", why, sizeof(why)));
        fail_start = false;
        store_database(manager, database, sizeof(database));
        assert(!strcmp(database, "failed"));
        break;
    }
    bool expected_online = transition == RESTART || transition == STOP_RESTART ||
                           transition == OFFLINE_ROUNDTRIP || (transition == NONE && !fail);
    pthread_mutex_lock(&gate_mutex);
    released = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    assert(!pthread_join(thread, NULL));
    assert(store_online(manager) == expected_online);
    /* Destruction follows join, as required by the borrowed-manager contract. */
    store_manager_destroy(manager);
    assert(opened_count == closed_count);
}

static bool same_identity(const StoreProjectIdentity *a, const StoreProjectIdentity *b)
{
    return a->generation == b->generation && !strcmp(a->project_id, b->project_id) &&
           a->device == b->device && a->inode == b->inode;
}

static void assert_zero_identity(const StoreProjectIdentity *identity)
{
    StoreProjectIdentity zero = {0};
    assert(!memcmp(identity, &zero, sizeof(zero)));
}

static void test_project_identity(void)
{
    char root[PATH_MAX], project[PATH_MAX + 32], moved[PATH_MAX + 32];
    char marker_dir[PATH_MAX + 64], marker[PATH_MAX + 96];
    const char *temporary = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/store-identity-XXXXXX", temporary ? temporary : "/tmp");
    assert(mkdtemp(root));
    snprintf(project, sizeof(project), "%s/project", root);
    snprintf(moved, sizeof(moved), "%s/moved", root);
    snprintf(marker_dir, sizeof(marker_dir), "%s/.scribodo", project);
    snprintf(marker, sizeof(marker), "%s/store.json", marker_dir);
    StoreManager *manager = store_manager_create();
    assert(manager);
    unsigned opens_before = opened_count;
    StoreProjectIdentity original, next;
    memset(&next, 0xff, sizeof(next));
    assert(!store_project_identity(manager, project, &next));
    assert_zero_identity(&next);
    assert(!store_project_identity(NULL, project, &next));
    assert_zero_identity(&next);
    assert(!store_project_identity(manager, NULL, &next));
    assert_zero_identity(&next);
    assert(!store_project_identity(manager, project, NULL));
    assert(!mkdir(project, 0700));
    assert(!store_project_identity(manager, project, &next));
    assert_zero_identity(&next);
    assert(store_mark_imported(project, "p-a"));
    assert(store_project_identity(manager, project, &original));
    assert(!strcmp(original.project_id, "p-a"));
    struct stat st;
    assert(!stat(project, &st));
    assert(original.device == (unsigned long long)st.st_dev);
    assert(original.inode == (unsigned long long)st.st_ino);
    assert(store_project_identity(manager, project, &next));
    assert(same_identity(&original, &next));
    assert(!store_project_identity(manager, marker, &next));
    assert_zero_identity(&next); /* a regular file is not a project directory */

    /* Old/copied marker metadata must not substitute for the actual inode. */
    FILE *file = fopen(marker, "w");
    assert(file);
    assert(fputs("{\"project_id\":\"p-a\",\"dev\":0,\"ino\":0}", file) >= 0);
    assert(!fclose(file));
    assert(store_project_identity(manager, project, &next));
    assert(same_identity(&original, &next));

    /* Rebinding keeps the directory inode but changes its immutable project ID. */
    assert(store_mark_imported(project, "p-b"));
    assert(store_project_identity(manager, project, &next));
    assert(!same_identity(&original, &next));
    assert(next.inode == original.inode && !strcmp(next.project_id, "p-b"));
    assert(store_mark_imported(project, "p-a"));

    /* Replacement at the same path, with the same marker ID, is still stale. */
    assert(!rename(project, moved));
    assert(!mkdir(project, 0700));
    assert(store_mark_imported(project, "p-a"));
    assert(store_project_identity(manager, project, &next));
    assert(!same_identity(&original, &next));
    assert(next.inode != original.inode);
    original = next;

    store_set_offline(manager, true);
    assert(store_project_identity(manager, project, &next));
    assert(next.generation != original.generation);
    store_set_offline(manager, false);
    assert(store_project_identity(manager, project, &next));
    assert(next.generation != original.generation);
    original = next;
    store_stop(manager);
    assert(store_project_identity(manager, project, &next));
    assert(next.generation != original.generation);
    assert(opened_count == opens_before); /* all snapshots are connection-free */
    original = next;
    char why[256];
    assert(store_start(manager, "identity", why, sizeof(why)));
    opens_before = opened_count;
    assert(store_project_identity(manager, project, &next));
    assert(next.generation != original.generation);
    assert(opened_count == opens_before);

    assert(!unlink(marker));
    file = fopen(marker, "w");
    assert(file);
    assert(fputs("{\"project_id\":\"invalid\"}", file) >= 0);
    assert(!fclose(file));
    assert(!store_project_identity(manager, project, &next));
    assert_zero_identity(&next);

    /* A local identity probe must not wait for a writer on a malformed marker. */
    assert(store_mode(manager, project, NULL, 0, NULL) == STORE_PAUSED);
    assert(!unlink(marker));
    assert(!mkfifo(marker, 0600));
    assert(!store_project_identity(manager, project, &next));
    assert_zero_identity(&next);
    assert(store_mode(manager, project, NULL, 0, NULL) == STORE_PAUSED);
    assert(opened_count == opens_before);
    store_manager_destroy(manager);
    assert(opened_count == closed_count);

    assert(!unlink(marker));
    assert(!rmdir(marker_dir));
    assert(!rmdir(project));
    snprintf(marker_dir, sizeof(marker_dir), "%s/.scribodo", moved);
    snprintf(marker, sizeof(marker), "%s/store.json", marker_dir);
    assert(!unlink(marker));
    assert(!rmdir(marker_dir));
    assert(!rmdir(moved));
    assert(!rmdir(root));
    puts("store project identity: local snapshots and nonregular-marker guard passed");
}

static void *deadline_worker(void *arg)
{
    StoreManager *manager = arg;
    atomic_bool cancel = false;
    unsigned before = opened_count;
    assert(!store_db_until(manager, NULL, NAN));
    assert(!store_db_until(manager, NULL, INFINITY));
    assert(!store_db_until(manager, NULL, 0));
    assert(!store_db_until(manager, NULL, deadline_now() - 1));
    assert(opened_count == before && store_online(manager));
    budget_abort = true;
    assert(!store_db_until(manager, NULL, deadline_now() + 5));
    budget_abort = false;
    assert(store_online(manager) && opened_count == before);
    cancel_on_open = &cancel;
    assert(!store_db_until(manager, &cancel, deadline_now() + 5000));
    cancel_on_open = NULL;
    assert(store_online(manager) && opened_count == before);
    atomic_store(&cancel, false);
    Db *db = store_db_until(manager, &cancel, deadline_now() + 5000);
    assert(db && store_db(manager) == db);
    unsigned closed = closed_count;
    assert(!store_db_until(manager, &cancel, deadline_now() - 1));
    assert(closed_count == closed && store_db(manager) == db);
    atomic_store(&cancel, true);
    assert(!store_db_until(manager, &cancel, deadline_now() + 5000));
    assert(closed_count == closed && store_db(manager) == db);
    store_thread_done(manager);
    return NULL;
}

static void test_deadline_connections(void)
{
    StoreManager *manager = store_manager_create();
    char why[256];
    assert(manager && store_start(manager, "budget", why, sizeof(why)));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, deadline_worker, manager));
    assert(!pthread_join(worker, NULL));
    assert(store_online(manager));
    store_manager_destroy(manager);
    assert(opened_count == closed_count);
    puts("store deadline connections: budget abort preserves online state and TLS ownership");
}

static void *gated_start(void *arg)
{
    gated = true;
    char why[256];
    assert(store_start(arg, "restarted", why, sizeof(why)));
    store_thread_done(arg);
    return NULL;
}

static void test_deadline_mutex(void)
{
    StoreManager *manager = store_manager_create();
    char why[256];
    assert(manager && store_start(manager, "mutex", why, sizeof(why)));
    pthread_mutex_lock(&gate_mutex);
    entered = released = fail_open = false;
    pthread_mutex_unlock(&gate_mutex);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, gated_start, manager));
    pthread_mutex_lock(&gate_mutex);
    while (!entered) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    /* store_start intentionally holds the manager mutex while connecting.
     * A budgeted foreground read must still be able to abandon that wait. */
    assert(!store_db_until(manager, NULL, deadline_now() + 20));
    atomic_bool cancel = true;
    assert(!store_db_until(manager, &cancel, deadline_now() + 5000));
    pthread_mutex_lock(&gate_mutex);
    released = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    assert(!pthread_join(worker, NULL));
    assert(store_online(manager));
    store_manager_destroy(manager);
    assert(opened_count == closed_count);
    puts("store deadline mutex: lifecycle contention respects budget/cancellation");
}

static void pinned_reject(StoreManager *manager, const char *path, const StoreProjectIdentity *identity,
                          Db *borrowed, const atomic_bool *cancel, double deadline)
{
    char id[64] = "must clear";
    unsigned opens = opened_count, closes = closed_count, writes = run_count;
    assert(store_mode_borrowed(manager, path, identity, borrowed, cancel, deadline, id, sizeof(id)) == STORE_PAUSED);
    assert(!id[0]);
    assert(opened_count == opens && closed_count == closes && run_count == writes);
}

static void *offline_roundtrip(void *arg)
{
    store_set_offline(arg, true);
    store_set_offline(arg, false);
    return NULL;
}

static void test_borrowed_mode(void)
{
    char root[PATH_MAX], project[PATH_MAX + 32], copy[PATH_MAX + 32];
    char metadata[PATH_MAX + 64], marker[PATH_MAX + 96];
    const char *temporary = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/store-borrowed-XXXXXX", temporary ? temporary : "/tmp");
    assert(mkdtemp(root));
    char resolved[PATH_MAX];
    assert(realpath(root, resolved));
    snprintf(root, sizeof(root), "%s", resolved);
    snprintf(project, sizeof(project), "%s/project", root);
    snprintf(copy, sizeof(copy), "%s/copy", root);
    assert(!mkdir(project, 0700));
    assert(!mkdir(copy, 0700));
    StoreManager *manager = store_manager_create();
    char why[256], id[64], database[64];
    assert(manager && store_start(manager, "pinned", why, sizeof(why)));
    Db *borrowed = store_db(manager);
    assert(borrowed && store_mark_imported(project, "p-a"));
    StoreProjectIdentity expected;
    assert(store_project_identity(manager, project, &expected));
    query_path = project;
    query_id = "p-a";
    unsigned opens = opened_count, closes = closed_count;
    assert(store_mode_borrowed(manager, project, &expected, borrowed, NULL, 0, id, sizeof(id)) == STORE_DB);
    assert(!strcmp(id, "p-a") && query_count == 1 && !run_count);
    assert(store_mode_borrowed(manager, project, &expected, borrowed, NULL, deadline_now() + 5000, NULL, 0) == STORE_DB);
    assert(opened_count == opens && closed_count == closes);
    assert(store_database_until(manager, database, sizeof(database), NULL, deadline_now() + 5000));
    assert(!strcmp(database, "pinned"));
    strcpy(database, "clear");
    assert(!store_database_until(manager, database, 2, NULL, deadline_now() + 5000) && !database[0]);
    strcpy(database, "clear");
    assert(!store_database_until(manager, database, sizeof(database), NULL, deadline_now() - 1) && !database[0]);

    pinned_reject(NULL, project, &expected, borrowed, NULL, 0);
    pinned_reject(manager, NULL, &expected, borrowed, NULL, 0);
    pinned_reject(manager, project, NULL, borrowed, NULL, 0);
    Db wrong = { .owner = pthread_self() };
    pinned_reject(manager, project, &expected, &wrong, NULL, 0);
    StoreProjectIdentity wrong_scope = expected;
    wrong_scope.generation++;
    pinned_reject(manager, project, &wrong_scope, borrowed, NULL, 0);
    wrong_scope = expected;
    wrong_scope.inode++;
    pinned_reject(manager, project, &wrong_scope, borrowed, NULL, 0);
    wrong_scope = expected;
    strcpy(wrong_scope.project_id, "p-other");
    pinned_reject(manager, project, &wrong_scope, borrowed, NULL, 0);
    atomic_bool cancel = true;
    pinned_reject(manager, project, &expected, borrowed, &cancel, 0);
    atomic_store(&cancel, false);
    double invalid[] = { NAN, INFINITY, -1, deadline_now() - 1 };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        pinned_reject(manager, project, &expected, borrowed, NULL, invalid[i]);
    borrowed->in_tx = true;
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    borrowed->in_tx = false;
    borrowed->broken = true;
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    borrowed->broken = false;

    /* A copied marker cannot authorize writing into its original project. */
    snprintf(metadata, sizeof(metadata), "%s/.scribodo", project);
    snprintf(marker, sizeof(marker), "%s/store.json", metadata);
    FILE *file = fopen(marker, "r");
    assert(file);
    char contents[512];
    size_t bytes = fread(contents, 1, sizeof(contents), file);
    assert(!fclose(file));
    snprintf(metadata, sizeof(metadata), "%s/.scribodo", copy);
    assert(!mkdir(metadata, 0700));
    snprintf(marker, sizeof(marker), "%s/store.json", metadata);
    file = fopen(marker, "w");
    assert(file && fwrite(contents, 1, bytes, file) == bytes && !fclose(file));
    StoreProjectIdentity copied;
    assert(store_project_identity(manager, copy, &copied));
    pinned_reject(manager, copy, &copied, borrowed, NULL, 0);
    file = fopen(marker, "r");
    assert(file);
    char after[512];
    assert(fread(after, 1, sizeof(after), file) == bytes && !memcmp(after, contents, bytes));
    assert(!fclose(file));
    assert(!unlink(marker) && !rmdir(metadata) && !rmdir(copy));

    /* Even a non-broken query error cannot authorize a borrowed route. */
    store_forget_verification(manager);
    query_fail = true;
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    query_fail = false;
    query_break = true;
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    query_break = false;
    borrowed->broken = false;
    query_cancel = &cancel;
    pinned_reject(manager, project, &expected, borrowed, &cancel, deadline_now() + 5000);
    query_cancel = NULL;
    atomic_store(&cancel, false);

    /* No metadata upgrade or SQL path adoption after a wrong-scope request. */
    snprintf(metadata, sizeof(metadata), "%s/.scribodo", project);
    snprintf(marker, sizeof(marker), "%s/store.json", metadata);
    file = fopen(marker, "w");
    assert(file && fputs("{\"project_id\":\"p-a\"}", file) >= 0 && !fclose(file));
    unsigned queries = query_count;
    pinned_reject(manager, project, &wrong_scope, borrowed, NULL, 0);
    assert(query_count == queries);
    file = fopen(marker, "r");
    assert(file);
    bytes = fread(contents, 1, sizeof(contents) - 1, file);
    contents[bytes] = 0;
    assert(!fclose(file) && !strcmp(contents, "{\"project_id\":\"p-a\"}"));
    assert(store_mark_imported(project, "p-a"));

    /* A lifecycle change in the verification query invalidates publication,
     * without closing the connection whose socket a caller's guard owns. */
    store_forget_verification(manager);
    query_transition = manager;
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    query_transition = NULL;
    assert(!db_broken(borrowed) && closed_count == closes);
    assert(store_project_identity(manager, project, &expected));
    pinned_reject(manager, project, &expected, borrowed, NULL, 0); /* stale TLS generation */
    borrowed = store_db(manager); /* explicit owner-controlled rebinding */
    assert(borrowed && store_project_identity(manager, project, &expected));

    pthread_t worker;
    assert(!pthread_create(&worker, NULL, offline_roundtrip, manager));
    assert(!pthread_join(worker, NULL));
    closes = closed_count;
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    assert(!db_broken(borrowed) && closed_count == closes);
    borrowed = store_db(manager);
    assert(borrowed && store_project_identity(manager, project, &expected));

    pthread_mutex_lock(&gate_mutex);
    entered = released = fail_open = false;
    pthread_mutex_unlock(&gate_mutex);
    assert(!pthread_create(&worker, NULL, gated_start, manager));
    pthread_mutex_lock(&gate_mutex);
    while (!entered) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    /* The worker holds the lifecycle mutex until after these calls. No exact
     * wall-time assertion is needed: an accidental lock causes watchdog exit. */
    pinned_reject(manager, project, &expected, borrowed, NULL, 0);
    strcpy(database, "clear");
    assert(!store_database_until(manager, database, sizeof(database), NULL, deadline_now() + 20) && !database[0]);
    assert(!db_broken(borrowed));
    pthread_mutex_lock(&gate_mutex);
    released = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    assert(!pthread_join(worker, NULL));
    store_manager_destroy(manager);
    assert(opened_count == closed_count);
    assert(!unlink(marker) && !rmdir(metadata) && !rmdir(project) && !rmdir(root));
    query_path = query_id = NULL;
    puts("borrowed store routing: pinned ownership, scope, SQL failure and lifecycle guards passed");
}

int main(void)
{
    alarm(15);
    for (int transition = NONE; transition <= STOP_RESTART; transition++) {
        run_case((enum Transition)transition, false, false);
        run_case((enum Transition)transition, true, false);
        run_case((enum Transition)transition, false, true);
        run_case((enum Transition)transition, true, true);
    }
    puts("store connection lifecycle: 14 legacy + 14 deadline gated cases passed");
    test_project_identity();
    test_deadline_connections();
    test_deadline_mutex();
    test_borrowed_mode();
    alarm(0);
    return 0;
}
