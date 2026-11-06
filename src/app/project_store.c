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
#include "app/store_backends.h"

typedef struct ProjectStore ProjectStore;
typedef struct ProjectStoreOps ProjectStoreOps;

struct ProjectStore {
    const ProjectStoreOps *ops;
    const char *project_dir;
    char project_id[64];
    Db *db;
};

struct ProjectStoreOps {
    size_t (*records_list)(ProjectStore *, RecordKind, Record **);
    bool (*records_append)(ProjectStore *, RecordKind, const char *, const char *, const char *, const SourceRef *, size_t,
                           const FindingInfo *, bool, int *);
    size_t (*revisions_list)(ProjectStore *, const char *, Revision **);
    bool (*revisions_record)(ProjectStore *, const char *, const char *, const char *, const char *, bool *);
    bool (*revisions_snapshot)(ProjectStore *, const char *, const char *, char *, size_t);
    char *(*revisions_load)(ProjectStore *, const char *, const Revision *);
    bool (*chat_append)(ProjectStore *, const char *, const char *, const ChatMessage *);
    bool (*chat_rename)(ProjectStore *, const char *, const char *);
    bool (*chat_set_observer)(ProjectStore *, const char *, bool);
    size_t (*chat_list)(ProjectStore *, ChatSummary **);
    bool (*chat_load)(ProjectStore *, const char *, ChatMessage **, size_t *);
};

static size_t file_records_list(ProjectStore *store, RecordKind kind, Record **out)
{
    return records_list_files(store->project_dir, kind, out);
}

static bool file_records_append(ProjectStore *store, RecordKind kind, const char *id, const char *status,
                                const char *content, const SourceRef *sources, size_t nsources,
                                const FindingInfo *finding, bool fresh, int *ver_out)
{
    return records_append_files(store->project_dir, kind, id, status, content, sources, nsources, finding, fresh, ver_out);
}

static size_t db_records_list(ProjectStore *store, RecordKind kind, Record **out)
{
    return sdb_record_list(store->db, store->project_id, kind, out);
}

static bool db_records_append(ProjectStore *store, RecordKind kind, const char *id, const char *status,
                              const char *content, const SourceRef *sources, size_t nsources,
                              const FindingInfo *finding, bool fresh, int *ver_out)
{
    return sdb_record_append(store->db, store->project_id, kind, id, status, content, sources, nsources, finding, fresh,
                             ver_out);
}

static size_t paused_records_list(ProjectStore *store, RecordKind kind, Record **out)
{
    *out = NULL;
    return 0;
}

static bool paused_records_append(ProjectStore *store, RecordKind kind, const char *id, const char *status,
                                  const char *content, const SourceRef *sources, size_t nsources,
                                  const FindingInfo *finding, bool fresh, int *ver_out)
{
    return false;
}

static size_t file_revisions_list(ProjectStore *store, const char *doc_path, Revision **out)
{
    return revisions_list_files(store->project_dir, doc_path, out);
}

static bool file_revisions_record(ProjectStore *store, const char *doc_path, const char *text, const char *origin,
                                  const char *note, bool *recorded)
{
    return revisions_record_files(store->project_dir, doc_path, text, origin, note, recorded);
}

static bool file_revisions_snapshot(ProjectStore *store, const char *doc_path, const char *text,
                                    char *file_out, size_t file_len)
{
    return revisions_snapshot_files(store->project_dir, doc_path, text, file_out, file_len);
}

static char *file_revisions_load(ProjectStore *store, const char *doc_path, const Revision *revision)
{
    return revisions_load_files(store->project_dir, doc_path, revision);
}

static size_t db_revisions_list(ProjectStore *store, const char *doc_path, Revision **out)
{
    return sdb_revision_list(store->db, store->project_id, doc_path, out);
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

static size_t paused_revisions_list(ProjectStore *store, const char *doc_path, Revision **out)
{
    *out = NULL;
    return 0;
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
    return chat_store_append_files(store->project_dir, id, title, message);
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

static const ProjectStoreOps file_ops = {
    file_records_list, file_records_append, file_revisions_list, file_revisions_record,
    file_revisions_snapshot, file_revisions_load, file_chat_append, file_chat_rename,
    file_chat_set_observer, file_chat_list, file_chat_load
};
static const ProjectStoreOps db_ops = {
    db_records_list, db_records_append, db_revisions_list, db_revisions_record,
    db_revisions_snapshot, db_revisions_load, db_chat_append, db_chat_rename,
    db_chat_set_observer, db_chat_list, db_chat_load
};
static const ProjectStoreOps paused_ops = {
    paused_records_list, paused_records_append, paused_revisions_list, paused_revisions_record,
    paused_revisions_snapshot, paused_revisions_load, paused_chat_append, paused_chat_rename,
    paused_chat_set_observer, paused_chat_list, paused_chat_load
};

static void resolve(const char *project_dir, ProjectStore *out)
{
    memset(out, 0, sizeof(*out));
    out->project_dir = project_dir;
    StoreMode mode = store_mode(project_dir, out->project_id, sizeof(out->project_id), &out->db);
    out->ops = mode == STORE_DB ? &db_ops : mode == STORE_PAUSED ? &paused_ops : &file_ops;
}

/* File operations share the import lock. An importer takes it exclusively,
 * so a writer that selected files cannot run after authority moves to the DB.
 * Resolve again while holding it: an import may have completed while we
 * waited. */
int project_store_file_lock_shared(const char *project_dir)
{
    char dir[PATH_MAX + 32], path[PATH_MAX + 64];
    int n = snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir ? project_dir : "");
    if (n < 0 || (size_t)n >= sizeof(dir) || (mkdir(dir, 0755) != 0 && errno != EEXIST)) return -1;
    n = snprintf(path, sizeof(path), "%s/import.lock", dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    int rc;
    do rc = flock(fd, LOCK_SH); while (rc != 0 && errno == EINTR);
    if (rc == 0) return fd;
    close(fd);
    return -1;
}

void project_store_file_unlock(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

static int begin_operation(const char *project_dir, ProjectStore *store)
{
    resolve(project_dir, store);
    if (store->ops != &file_ops) return -1;
    int lock = project_store_file_lock_shared(project_dir);
    if (lock >= 0) resolve(project_dir, store);
    return lock;
}

size_t project_store_records_list(const char *project_dir, RecordKind kind, Record **out)
{
    if (!project_dir || !project_dir[0]) {
        *out = NULL;
        return 0;
    }
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops) {
        if (lock < 0) {
            *out = NULL;
            return 0;
        }
    }
    size_t n = store.ops->records_list(&store, kind, out);
    project_store_file_unlock(lock);
    return n;
}

bool project_store_records_append(const char *project_dir, RecordKind kind, const char *id, const char *status,
                                   const char *content, const SourceRef *sources, size_t nsources,
                                   const FindingInfo *finding, bool fresh, int *ver_out)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->records_append(&store, kind, id, status, content, sources, nsources, finding, fresh, ver_out);
    project_store_file_unlock(lock);
    return ok;
}


size_t project_store_revisions_list(const char *project_dir, const char *doc_path, Revision **out)
{
    *out = NULL;
    if (!project_dir || !project_dir[0]) return 0;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return 0;
    size_t n = store.ops->revisions_list(&store, doc_path, out);
    project_store_file_unlock(lock);
    return n;
}

bool project_store_revisions_record(const char *project_dir, const char *doc_path, const char *text,
                                    const char *origin, const char *note, bool *recorded)
{
    if (recorded) *recorded = false;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->revisions_record(&store, doc_path, text, origin, note, recorded);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_revisions_snapshot(const char *project_dir, const char *doc_path, const char *text,
                                      char *file_out, size_t file_len)
{
    if (file_out && file_len) file_out[0] = '\0';
    if (!project_dir || !project_dir[0] || !file_out || !file_len) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->revisions_snapshot(&store, doc_path, text, file_out, file_len);
    project_store_file_unlock(lock);
    return ok;
}

char *project_store_revisions_load(const char *project_dir, const char *doc_path, const Revision *revision)
{
    if (!project_dir || !project_dir[0]) return NULL;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return NULL;
    char *text = store.ops->revisions_load(&store, doc_path, revision);
    project_store_file_unlock(lock);
    return text;
}

bool project_store_chat_append(const char *project_dir, const char *id, const char *title, const ChatMessage *message)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_append(&store, id, title, message);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_rename(const char *project_dir, const char *id, const char *title)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_rename(&store, id, title);
    project_store_file_unlock(lock);
    return ok;
}

bool project_store_chat_set_observer(const char *project_dir, const char *id, bool read)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_set_observer(&store, id, read);
    project_store_file_unlock(lock);
    return ok;
}

size_t project_store_chat_list(const char *project_dir, ChatSummary **out)
{
    *out = NULL;
    if (!project_dir || !project_dir[0]) return 0;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return 0;
    size_t n = store.ops->chat_list(&store, out);
    project_store_file_unlock(lock);
    return n;
}

bool project_store_chat_load(const char *project_dir, const char *id, ChatMessage **messages, size_t *count)
{
    *messages = NULL;
    *count = 0;
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    int lock = begin_operation(project_dir, &store);
    if (store.ops == &file_ops && lock < 0) return false;
    bool ok = store.ops->chat_load(&store, id, messages, count);
    project_store_file_unlock(lock);
    return ok;
}
