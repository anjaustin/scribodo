#include "project_store.h"
#include "project_store_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/records_internal.h"
#include "app/revisions_internal.h"
#include "app/chat_store_internal.h"
#include "app/observer_store_internal.h"
#include "app/store_backends.h"

typedef struct ProjectStore ProjectStore;
typedef struct ProjectStoreOps ProjectStoreOps;

struct ProjectStore {
    StoreManager *manager;
    const ProjectStoreOps *ops;
    const char *project_dir;
    char project_id[64];
    Db *db;
};

struct ProjectStoreOps {
    bool (*records_list)(ProjectStore *, RecordKind, Record **, size_t *);
    bool (*records_append)(ProjectStore *, RecordKind, const char *, const char *, const char *, const SourceRef *, size_t,
                           const FindingInfo *, bool, int *);
    bool (*revisions_list)(ProjectStore *, const char *, Revision **, size_t *);
    bool (*revisions_record)(ProjectStore *, const char *, const char *, const char *, const char *, bool *);
    bool (*revisions_snapshot)(ProjectStore *, const char *, const char *, char *, size_t);
    char *(*revisions_load)(ProjectStore *, const char *, const Revision *);
    bool (*chat_append)(ProjectStore *, const char *, const char *, const ChatMessage *);
    bool (*chat_rename)(ProjectStore *, const char *, const char *);
    bool (*chat_set_observer)(ProjectStore *, const char *, bool);
    size_t (*chat_list)(ProjectStore *, ChatSummary **);
    bool (*chat_load)(ProjectStore *, const char *, ChatMessage **, size_t *);
    cJSON *(*observer_state_load)(ProjectStore *);
    bool (*observer_state_save)(ProjectStore *, cJSON *);
    bool (*observer_rules_get)(ProjectStore *, char[17]);
    bool (*observer_rules_set)(ProjectStore *, const char *);
};

static bool file_records_list(ProjectStore *store, RecordKind kind, Record **out, size_t *count)
{
    return records_list_files_checked(store->project_dir, kind, out, count);
}

static bool file_records_append(ProjectStore *store, RecordKind kind, const char *id, const char *status,
                                const char *content, const SourceRef *sources, size_t nsources,
                                const FindingInfo *finding, bool fresh, int *ver_out)
{
    return records_append_files(store->manager, store->project_dir, kind, id, status, content, sources, nsources, finding, fresh, ver_out);
}

static bool db_records_list(ProjectStore *store, RecordKind kind, Record **out, size_t *count)
{
    return sdb_record_list_checked(store->db, store->project_id, kind, out, count);
}

static bool db_records_append(ProjectStore *store, RecordKind kind, const char *id, const char *status,
                              const char *content, const SourceRef *sources, size_t nsources,
                              const FindingInfo *finding, bool fresh, int *ver_out)
{
    return sdb_record_append(store->db, store->project_id, kind, id, status, content, sources, nsources, finding, fresh,
                             ver_out);
}

static bool paused_records_list(ProjectStore *store, RecordKind kind, Record **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    return false;
}

static bool paused_records_append(ProjectStore *store, RecordKind kind, const char *id, const char *status,
                                  const char *content, const SourceRef *sources, size_t nsources,
                                  const FindingInfo *finding, bool fresh, int *ver_out)
{
    return false;
}

static bool file_revisions_list(ProjectStore *store, const char *doc_path, Revision **out, size_t *count)
{
    return revisions_list_files_checked(store->project_dir, doc_path, out, count);
}

static bool file_revisions_record(ProjectStore *store, const char *doc_path, const char *text, const char *origin,
                                  const char *note, bool *recorded)
{
    return revisions_record_files(store->manager, store->project_dir, doc_path, text, origin, note, recorded);
}

static bool file_revisions_snapshot(ProjectStore *store, const char *doc_path, const char *text,
                                    char *file_out, size_t file_len)
{
    return revisions_snapshot_files(store->manager, store->project_dir, doc_path, text, file_out, file_len);
}

static char *file_revisions_load(ProjectStore *store, const char *doc_path, const Revision *revision)
{
    return revisions_load_files(store->project_dir, doc_path, revision);
}

static bool db_revisions_list(ProjectStore *store, const char *doc_path, Revision **out, size_t *count)
{
    return sdb_revision_list_checked(store->db, store->project_id, doc_path, out, count);
}

static bool db_revisions_record(ProjectStore *store, const char *doc_path, const char *text, const char *origin,
                                const char *note, bool *recorded)
{
    return sdb_revision_record(store->db, store->project_id, doc_path, text, origin, note, false, recorded, NULL, 0);
}

static bool db_revisions_snapshot(ProjectStore *store, const char *doc_path, const char *text,
                                  char *file_out, size_t file_len)
{
    return sdb_revision_record(store->db, store->project_id, doc_path, text, "source", NULL, true, NULL,
                               file_out, file_len) && file_out && file_out[0];
}

static char *db_revisions_load(ProjectStore *store, const char *doc_path, const Revision *revision)
{
    return sdb_revision_load(store->db, store->project_id, doc_path, revision);
}

static bool paused_revisions_list(ProjectStore *store, const char *doc_path, Revision **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    return false;
}

static bool paused_revisions_record(ProjectStore *store, const char *doc_path, const char *text, const char *origin,
                                    const char *note, bool *recorded)
{
    return false;
}

static bool paused_revisions_snapshot(ProjectStore *store, const char *doc_path, const char *text,
                                      char *file_out, size_t file_len)
{
    return false;
}

static char *paused_revisions_load(ProjectStore *store, const char *doc_path, const Revision *revision)
{
    return NULL;
}

static bool file_chat_append(ProjectStore *store, const char *id, const char *title, const ChatMessage *message)
{
    return chat_store_append_files(store->manager, store->project_dir, id, title, message);
}

static bool file_chat_rename(ProjectStore *store, const char *id, const char *title)
{
    return chat_store_rename_files(store->project_dir, id, title);
}

static bool file_chat_set_observer(ProjectStore *store, const char *id, bool read)
{
    return chat_store_set_observer_files(store->project_dir, id, read);
}

static size_t file_chat_list(ProjectStore *store, ChatSummary **out)
{
    return chat_store_list_files(store->project_dir, out);
}

static bool file_chat_load(ProjectStore *store, const char *id, ChatMessage **messages, size_t *count)
{
    return chat_store_load_files(store->project_dir, id, messages, count);
}

static bool db_chat_append(ProjectStore *store, const char *id, const char *title, const ChatMessage *message)
{
    return sdb_chat_append(store->db, store->project_id, id, title, message);
}

static bool db_chat_rename(ProjectStore *store, const char *id, const char *title)
{
    return sdb_chat_rename(store->db, store->project_id, id, title);
}

static bool db_chat_set_observer(ProjectStore *store, const char *id, bool read)
{
    return sdb_chat_set_observer(store->db, store->project_id, id, read);
}

static size_t db_chat_list(ProjectStore *store, ChatSummary **out)
{
    return sdb_chat_list(store->db, store->project_id, out);
}

static bool db_chat_load(ProjectStore *store, const char *id, ChatMessage **messages, size_t *count)
{
    return sdb_chat_load(store->db, store->project_id, id, messages, count);
}

static bool paused_chat_append(ProjectStore *store, const char *id, const char *title, const ChatMessage *message)
{
    return false;
}

static bool paused_chat_rename(ProjectStore *store, const char *id, const char *title) { return false; }
static bool paused_chat_set_observer(ProjectStore *store, const char *id, bool read) { return false; }

static size_t paused_chat_list(ProjectStore *store, ChatSummary **out)
{
    *out = NULL;
    return 0;
}

static bool paused_chat_load(ProjectStore *store, const char *id, ChatMessage **messages, size_t *count)
{
    *messages = NULL;
    *count = 0;
    return false;
}

static cJSON *file_observer_state_load(ProjectStore *store)
{
    return observer_state_load_files(store->project_dir);
}

static bool file_observer_state_save(ProjectStore *store, cJSON *state)
{
    return observer_state_save_files(store->project_dir, state);
}

static bool file_observer_rules_get(ProjectStore *store, char signature[17])
{
    return observer_rules_get_files(store->project_dir, signature);
}

static bool file_observer_rules_set(ProjectStore *store, const char *signature)
{
    return observer_rules_set_files(store->project_dir, signature);
}

static cJSON *db_observer_state_load(ProjectStore *store)
{
    return observer_state_load_db(store->db, store->project_id);
}

static bool db_observer_state_save(ProjectStore *store, cJSON *state)
{
    return observer_state_save_db(store->db, store->project_id, state);
}

static bool db_observer_rules_get(ProjectStore *store, char signature[17])
{
    return observer_rules_get_db(store->db, store->project_id, signature);
}

static bool db_observer_rules_set(ProjectStore *store, const char *signature)
{
    return observer_rules_set_db(store->db, store->project_id, signature);
}

static cJSON *paused_observer_state_load(ProjectStore *store) { return NULL; }
static bool paused_observer_state_save(ProjectStore *store, cJSON *state) { return false; }
static bool paused_observer_rules_get(ProjectStore *store, char signature[17])
{
    signature[0] = '\0';
    return false;
}
static bool paused_observer_rules_set(ProjectStore *store, const char *signature) { return false; }

static const ProjectStoreOps file_ops = {
    .records_list = file_records_list, .records_append = file_records_append,
    .revisions_list = file_revisions_list, .revisions_record = file_revisions_record,
    .revisions_snapshot = file_revisions_snapshot, .revisions_load = file_revisions_load,
    .chat_append = file_chat_append, .chat_rename = file_chat_rename,
    .chat_set_observer = file_chat_set_observer, .chat_list = file_chat_list, .chat_load = file_chat_load,
    .observer_state_load = file_observer_state_load, .observer_state_save = file_observer_state_save,
    .observer_rules_get = file_observer_rules_get, .observer_rules_set = file_observer_rules_set
};
static const ProjectStoreOps db_ops = {
    .records_list = db_records_list, .records_append = db_records_append,
    .revisions_list = db_revisions_list, .revisions_record = db_revisions_record,
    .revisions_snapshot = db_revisions_snapshot, .revisions_load = db_revisions_load,
    .chat_append = db_chat_append, .chat_rename = db_chat_rename,
    .chat_set_observer = db_chat_set_observer, .chat_list = db_chat_list, .chat_load = db_chat_load,
    .observer_state_load = db_observer_state_load, .observer_state_save = db_observer_state_save,
    .observer_rules_get = db_observer_rules_get, .observer_rules_set = db_observer_rules_set
};
static const ProjectStoreOps paused_ops = {
    .records_list = paused_records_list, .records_append = paused_records_append,
    .revisions_list = paused_revisions_list, .revisions_record = paused_revisions_record,
    .revisions_snapshot = paused_revisions_snapshot, .revisions_load = paused_revisions_load,
    .chat_append = paused_chat_append, .chat_rename = paused_chat_rename,
    .chat_set_observer = paused_chat_set_observer, .chat_list = paused_chat_list, .chat_load = paused_chat_load,
    .observer_state_load = paused_observer_state_load, .observer_state_save = paused_observer_state_save,
    .observer_rules_get = paused_observer_rules_get, .observer_rules_set = paused_observer_rules_set
};

static void resolve(StoreManager *manager, const char *project_dir, ProjectStore *out)
{
    memset(out, 0, sizeof(*out));
    out->manager = manager;
    out->project_dir = project_dir;
    StoreMode mode = store_mode(manager, project_dir, out->project_id, sizeof(out->project_id), &out->db);
    out->ops = mode == STORE_DB ? &db_ops : mode == STORE_PAUSED ? &paused_ops : &file_ops;
}

ProjectStoreInspection project_store_inspect(StoreManager *manager, const char *project_dir)
{
    ProjectStoreInspection inspection = { 0 };
    if (!project_dir || !project_dir[0]) return inspection;
    ProjectStore store;
    resolve(manager, project_dir, &store);
    if (store.ops == &file_ops) {
        inspection.capabilities = PROJECT_STORE_CAP_STRUCTURED_WRITE;
        return inspection;
    }
    if (store.ops == &db_ops) {
        inspection.capabilities = PROJECT_STORE_CAP_STRUCTURED_WRITE | PROJECT_STORE_CAP_LINKED_DOCUMENTS |
                                  PROJECT_STORE_CAP_ENTITIES | PROJECT_STORE_CAP_SEMANTIC |
                                  PROJECT_STORE_CAP_SLOW_REFRESH;
        return inspection;
    }
    if (!store_online(manager)) inspection.blocked_by = PROJECT_STORE_BLOCK_OFFLINE;
    else {
        int problem = store_problem(manager, project_dir, NULL, 0);
        inspection.blocked_by = problem == STORE_PROBLEM_COPY      ? PROJECT_STORE_BLOCK_COPY
                                : problem == STORE_PROBLEM_MISSING ? PROJECT_STORE_BLOCK_MISSING
                                                                   : PROJECT_STORE_BLOCK_TRANSITION;
    }
    return inspection;
}

static unsigned capabilities_for(const ProjectStore *store)
{
    if (store->ops == &file_ops) return PROJECT_STORE_CAP_STRUCTURED_WRITE;
    if (store->ops == &db_ops)
        return PROJECT_STORE_CAP_STRUCTURED_WRITE | PROJECT_STORE_CAP_LINKED_DOCUMENTS |
               PROJECT_STORE_CAP_ENTITIES | PROJECT_STORE_CAP_SEMANTIC | PROJECT_STORE_CAP_SLOW_REFRESH;
    return 0;
}

bool project_store_guard_begin(StoreManager *manager, const char *project_dir, ProjectStoreGuard *guard)
{
    if (!guard) return false;
    guard->file_lock = -1;
    guard->capabilities = 0;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    resolve(manager, project_dir, &store);
    if (store.ops == &file_ops) {
        guard->file_lock = project_store_file_lock_shared(project_dir);
        if (guard->file_lock < 0) return false;
        resolve(manager, project_dir, &store);
    }
    guard->capabilities = capabilities_for(&store);
    if (!(guard->capabilities & PROJECT_STORE_CAP_STRUCTURED_WRITE)) {
        project_store_file_unlock(guard->file_lock);
        guard->file_lock = -1;
        return false;
    }
    return true;
}

void project_store_guard_end(ProjectStoreGuard *guard)
{
    if (!guard) return;
    project_store_file_unlock(guard->file_lock);
    guard->file_lock = -1;
    guard->capabilities = 0;
}

bool project_store_feature_key(StoreManager *manager, const char *project_dir, unsigned capability, char *key, size_t keylen)
{
    if (key && keylen) key[0] = '\0';
    if (!project_dir || !project_dir[0] || !key || !keylen ||
        (capability != PROJECT_STORE_CAP_SEMANTIC && capability != PROJECT_STORE_CAP_ENTITIES))
        return false;
    ProjectStore store;
    resolve(manager, project_dir, &store);
    if (store.ops != &db_ops) return false;
    snprintf(key, keylen, "%s", store.project_id);
    return key[0] != '\0';
}

/* File operations share the import lock. An importer takes it exclusively,
 * so a writer that selected files cannot run after authority moves to the DB.
 * Resolve again while holding it: an import may have completed while we
 * waited. */
static int file_lock(const char *project_dir, int operation)
{
    char dir[PATH_MAX + 32], path[PATH_MAX + 64];
    int n = snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir ? project_dir : "");
    if (n < 0 || (size_t)n >= sizeof(dir) || (mkdir(dir, 0755) != 0 && errno != EEXIST)) return -1;
    n = snprintf(path, sizeof(path), "%s/import.lock", dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    int rc;
    do rc = flock(fd, operation); while (rc != 0 && errno == EINTR);
    if (rc == 0) return fd;
    close(fd);
    return -1;
}

int project_store_file_lock_shared(const char *project_dir) { return file_lock(project_dir, LOCK_SH); }
int project_store_file_lock_exclusive(const char *project_dir) { return file_lock(project_dir, LOCK_EX); }

void project_store_file_unlock(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

static int begin_operation(StoreManager *manager, const char *project_dir, ProjectStore *store)
{
    resolve(manager, project_dir, store);
    if (store->ops != &file_ops) return -1;
    int lock = project_store_file_lock_shared(project_dir);
    if (lock >= 0) resolve(manager, project_dir, store);
    return lock;
}

bool project_store_records_list_checked(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out,
                                        size_t *count)
{
    *out = NULL;
    *count = 0;
    if (!project_dir || !project_dir[0]) {
        return false;
    }
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops) {
        if (lock < 0) {
            *out = NULL;
            return false;
        }
    }
    bool ok = store.ops->records_list(&store, kind, out, count);
    project_store_file_unlock(lock);
    return ok;
}

size_t project_store_records_list(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out)
{
    size_t count = 0;
    (void)project_store_records_list_checked(manager, project_dir, kind, out, &count);
    return count;
}

bool project_store_records_append(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status,
                                   const char *content, const SourceRef *sources, size_t nsources,
                                   const FindingInfo *finding, bool fresh, int *ver_out)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->records_append(&store, kind, id, status, content, sources, nsources, finding, fresh, ver_out);
    project_store_file_unlock(lock);
    return ok;
}


bool project_store_revisions_list_checked(StoreManager *manager, const char *project_dir, const char *doc_path,
                                          Revision **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->revisions_list(&store, doc_path, out, count);
    project_store_file_unlock(lock);
    return ok;
}

size_t project_store_revisions_list(StoreManager *manager, const char *project_dir, const char *doc_path, Revision **out)
{
    size_t count = 0;
    (void)project_store_revisions_list_checked(manager, project_dir, doc_path, out, &count);
    return count;
}

bool project_store_revisions_record(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                                    const char *origin, const char *note, bool *recorded)
{
    if (recorded) *recorded = false;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->revisions_record(&store, doc_path, text, origin, note, recorded);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_revisions_snapshot(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                                      char *file_out, size_t file_len)
{
    if (file_out && file_len) file_out[0] = '\0';
    if (!project_dir || !project_dir[0] || !file_out || !file_len) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->revisions_snapshot(&store, doc_path, text, file_out, file_len);
    project_store_file_unlock(lock);
    return ok;
}

char *project_store_revisions_load(StoreManager *manager, const char *project_dir, const char *doc_path, const Revision *revision)
{
    if (!project_dir || !project_dir[0]) return NULL;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return NULL;
    char *text = store.ops->revisions_load(&store, doc_path, revision);
    project_store_file_unlock(lock);
    return text;
}

bool project_store_chat_append(StoreManager *manager, const char *project_dir, const char *id, const char *title, const ChatMessage *message)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_append(&store, id, title, message);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_append_sequenced(StoreManager *manager, const char *project_dir, const char *id, const char *title,
                                        const ChatMessage *message, int64_t *sequence)
{
    if (sequence) *sequence = -1;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops == &db_ops ? sdb_chat_append_sequenced(store.db, store.project_id, id, title, message, sequence)
                                    : store.ops->chat_append(&store, id, title, message);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_window(StoreManager *manager, const char *project_dir, const char *id,
                                const ChatWindowRequest *request, ChatWindowPage *out)
{
    if (!out) return false;
    *out = (ChatWindowPage){0};
    if (!project_dir || !project_dir[0] || !id || !request) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    bool ok = store.ops == &db_ops && sdb_chat_window(store.db, store.project_id, id, request, out);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_rename(StoreManager *manager, const char *project_dir, const char *id, const char *title)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_rename(&store, id, title);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_set_observer(StoreManager *manager, const char *project_dir, const char *id, bool read)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_set_observer(&store, id, read);
    project_store_file_unlock(lock);
    return ok;
}

size_t project_store_chat_list(StoreManager *manager, const char *project_dir, ChatSummary **out)
{
    size_t count = 0;
    (void)project_store_chat_list_checked(manager, project_dir, out, &count);
    return count;
}

bool project_store_chat_list_checked(StoreManager *manager, const char *project_dir, ChatSummary **out, size_t *count)
{
    if (!out || !count) return false;
    *out = NULL;
    *count = 0;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops == &file_ops ? chat_store_list_files_checked(store.project_dir, out, count)
              : store.ops == &db_ops ? sdb_chat_list_checked(store.db, store.project_id, out, count)
                                     : true;
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_load(StoreManager *manager, const char *project_dir, const char *id, ChatMessage **messages, size_t *count)
{
    *messages = NULL;
    *count = 0;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_load(&store, id, messages, count);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_source_page(StoreManager *manager, const char *project_dir, const char *id,
                                    const ChatSourceCursor *cursor, ChatSourcePage *out)
{
    if (!out) return false;
    *out = (ChatSourcePage){0};
    if (!project_dir || !project_dir[0] || !id || !cursor) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    /* The memory MVP is DB-only. Never slurp a file or trust cached data if
     * resolution pauses or the store becomes unavailable. */
    bool ok = store.ops == &db_ops && sdb_chat_source_page(store.db, store.project_id, id, cursor, out);
    project_store_file_unlock(lock);
    return ok;
}

cJSON *project_store_observer_state_load(StoreManager *manager, const char *project_dir)
{
    if (!project_dir || !project_dir[0]) return NULL;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return NULL;
    cJSON *state = store.ops->observer_state_load(&store);
    project_store_file_unlock(lock);
    return state;
}

bool project_store_observer_state_save(StoreManager *manager, const char *project_dir, cJSON *state)
{
    if (!project_dir || !project_dir[0] || !state) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->observer_state_save(&store, state);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_observer_rules_get(StoreManager *manager, const char *project_dir, char signature[17])
{
    if (signature) signature[0] = '\0';
    if (!project_dir || !project_dir[0] || !signature) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->observer_rules_get(&store, signature);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_observer_rules_set(StoreManager *manager, const char *project_dir, const char *signature)
{
    if (!project_dir || !project_dir[0] || !signature) return false;
    ProjectStore store;
    int lock = begin_operation(manager, project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->observer_rules_set(&store, signature);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_document_set_aside(StoreManager *manager, const char *project_dir, const char *name, const char *aside)
{
    ProjectStore store;
    resolve(manager, project_dir, &store);
    if (store.ops != &db_ops) return false;
    char id[40];
    if (!store_document_id(manager, store.project_id, name, false, id, sizeof(id))) return true;
    return store_rename_document(manager, store.project_id, name, aside);
}

bool project_store_document_rename(StoreManager *manager, const char *project_dir, const char *from, const char *to)
{
    ProjectStore store;
    resolve(manager, project_dir, &store);
    return store.ops == &db_ops && store_rename_document(manager, store.project_id, from, to);
}

bool project_store_entities_available(StoreManager *manager, const char *project_dir)
{
    return (project_store_inspect(manager, project_dir).capabilities & PROJECT_STORE_CAP_ENTITIES) != 0;
}

bool project_store_entities_built(StoreManager *manager, const char *project_dir)
{
    ProjectStore store;
    resolve(manager, project_dir, &store);
    return store.ops == &db_ops && sdb_entities_built(store.db, store.project_id);
}

bool project_store_entities_backfill(StoreManager *manager, const char *project_dir)
{
    ProjectStore store;
    resolve(manager, project_dir, &store);
    return store.ops == &db_ops && sdb_entities_backfill(store.db, store.project_id);
}

size_t project_store_entity_list(StoreManager *manager, const char *project_dir, EntityRow **out)
{
    *out = NULL;
    ProjectStore store;
    resolve(manager, project_dir, &store);
    return store.ops == &db_ops ? sdb_entity_list(store.db, store.project_id, out) : 0;
}

bool project_store_entity_detail(StoreManager *manager, const char *project_dir, const char *entity_id, EntityDetail *out)
{
    ProjectStore store;
    resolve(manager, project_dir, &store);
    return store.ops == &db_ops && sdb_entity_detail(store.db, store.project_id, entity_id, out);
}
