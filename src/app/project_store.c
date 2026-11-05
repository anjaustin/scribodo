#include "project_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/records_internal.h"
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

static const ProjectStoreOps file_ops = { file_records_list, file_records_append };
static const ProjectStoreOps db_ops = { db_records_list, db_records_append };
static const ProjectStoreOps paused_ops = { paused_records_list, paused_records_append };

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
static int lock_file_backend(const char *project_dir)
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

static void unlock_file_backend(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

size_t project_store_records_list(const char *project_dir, RecordKind kind, Record **out)
{
    if (!project_dir || !project_dir[0]) {
        *out = NULL;
        return 0;
    }
    ProjectStore store;
    resolve(project_dir, &store);
    int lock = -1;
    if (store.ops == &file_ops) {
        lock = lock_file_backend(project_dir);
        if (lock < 0) {
            *out = NULL;
            return 0;
        }
        resolve(project_dir, &store);
    }
    size_t n = store.ops->records_list(&store, kind, out);
    unlock_file_backend(lock);
    return n;
}

bool project_store_records_append(const char *project_dir, RecordKind kind, const char *id, const char *status,
                                   const char *content, const SourceRef *sources, size_t nsources,
                                   const FindingInfo *finding, bool fresh, int *ver_out)
{
    if (!project_dir || !project_dir[0]) return false;
    ProjectStore store;
    resolve(project_dir, &store);
    int lock = -1;
    if (store.ops == &file_ops) {
        lock = lock_file_backend(project_dir);
        if (lock < 0) return false;
        resolve(project_dir, &store);
    }
    bool ok = store.ops->records_append(&store, kind, id, status, content, sources, nsources, finding, fresh, ver_out);
    unlock_file_backend(lock);
    return ok;
}
