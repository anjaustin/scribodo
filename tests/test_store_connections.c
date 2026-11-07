/* No services or production hooks: link store.c against this gated DB double.
 * cc -std=c11 -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -Isrc \
 *   src/app/store.c tests/test_store_connections.c -lpthread -o build/test_store_connections
 */
#define _DARWIN_C_SOURCE
#include "app/store.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct Db {
    pthread_t owner;
    char database[64];
    bool broken;
};

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool entered, released, fail_open, fail_start;
static unsigned opened_count, closed_count;
static _Thread_local bool gated;

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

/* Unused project APIs still need their DB symbols when linking store.c. */
bool db_query(Db *db, const char *sql, const DbArg *args, size_t n, DbRows *out)
{ (void)db; (void)sql; (void)args; (void)n; (void)out; abort(); }
bool db_run(Db *db, const char *sql, const DbArg *args, size_t n, uint64_t *affected)
{ (void)db; (void)sql; (void)args; (void)n; (void)affected; abort(); }
void db_rows_free(DbRows *rows) { (void)rows; abort(); }
bool db_rows_has_columns(const DbRows *rows, size_t n) { (void)rows; (void)n; abort(); }
bool db_cell_text(const DbRows *rows, size_t r, size_t c, const char **out)
{ (void)rows; (void)r; (void)c; (void)out; abort(); }
bool db_begin(Db *db) { (void)db; abort(); }
bool db_commit(Db *db) { (void)db; abort(); }
void db_rollback(Db *db) { (void)db; abort(); }

typedef struct {
    StoreManager *manager;
    bool stale;
    const char *retry_database;
} Worker;

static void *connect_worker(void *arg)
{
    Worker *worker = arg;
    gated = true;
    Db *db = store_db(worker->manager);
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

static void run_case(enum Transition transition, bool fail)
{
    StoreManager *manager = store_manager_create();
    assert(manager);
    char why[256];
    assert(store_start(manager, "old", why, sizeof(why)));
    pthread_mutex_lock(&gate_mutex);
    entered = released = false;
    fail_open = fail;
    pthread_mutex_unlock(&gate_mutex);
    Worker worker = { .manager = manager, .stale = transition != NONE };
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

int main(void)
{
    alarm(15);
    for (int transition = NONE; transition <= STOP_RESTART; transition++) {
        run_case((enum Transition)transition, false);
        run_case((enum Transition)transition, true);
    }
    puts("store connection lifecycle: 14 gated cases passed");
    test_project_identity();
    alarm(0);
    return 0;
}
