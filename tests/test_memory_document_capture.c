#define _DARWIN_C_SOURCE
#include "app/memory_documents.h"
#include "app/store_backends.h"
#include "test_util.h"
#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct StoreManager { int unused; };
struct Db { int unused; };
static StoreManager manager;
static Db database;
static StoreProjectIdentity project;
static SavedRevisionIdentity saved;
static unsigned reads, routes, identities;
static unsigned borrowed_routes;
static bool offline, unsaved, transaction;

bool db_in_transaction(Db *db) { (void)db; return transaction; }
bool store_project_identity(StoreManager *store, const char *path, StoreProjectIdentity *out)
{
    (void)path; CHECK(store == &manager); identities++; *out = project; return true;
}
StoreMode store_mode(StoreManager *store, const char *path, char *id, size_t idlen, Db **db)
{
    (void)path; CHECK(store == &manager); routes++;
    snprintf(id, idlen, "%s", project.project_id); *db = &database;
    return offline ? STORE_PAUSED : STORE_DB;
}
StoreMode store_mode_borrowed(StoreManager *store, const char *path, const StoreProjectIdentity *expected,
                               Db *db, const atomic_bool *cancel, double deadline_ms, char *id, size_t idlen)
{
    (void)path; (void)deadline_ms; borrowed_routes++;
    CHECK(store == &manager && db == &database);
    if (offline || (cancel && atomic_load(cancel)) || expected->generation != project.generation) return STORE_PAUSED;
    snprintf(id, idlen, "%s", project.project_id);
    return STORE_DB;
}
bool sdb_saved_revision_identity(Db *db, const char *owner, const char *name, SavedRevisionIdentity *out)
{
    CHECK(db == &database); reads++;
    if (unsaved || strcmp(owner, project.project_id) || strcmp(name, "one.md")) return false;
    *out = saved; return true;
}

static void capture(void)
{
    MemoryDocumentName boundary;
    char title[257]; memset(title, 'x', sizeof(title)); title[256] = 0;
    CHECK(!memory_document_name_copy(NULL, false, &boundary) && !boundary.name[0]);
    CHECK(!memory_document_name_copy(title, false, &boundary) && !boundary.name[0]);
    title[255] = 0;
    CHECK(memory_document_name_copy(title, false, &boundary) && strlen(boundary.name) == 255);
    CHECK(!memory_document_name_copy(title, true, &boundary) && !boundary.name[0]);
    char dir[PATH_MAX], path[PATH_MAX], hash[65];
    snprintf(dir, sizeof(dir), "%s/memory-capture-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    struct stat st; CHECK(stat(dir, &st) == 0);
    project = (StoreProjectIdentity){ .generation = 1, .device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino };
    strcpy(project.project_id, "project");
    const char *text = "Saved amber key. 東京。\n";
    unsigned char bytes[32]; CC_SHA256(text, (CC_LONG)strlen(text), bytes);
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(hash + 2 * i, 3, "%02x", bytes[i]);
    saved = (SavedRevisionIdentity){ .bytes = strlen(text) };
    strcpy(saved.database, "database"); strcpy(saved.project_id, "project");
    strcpy(saved.document_id, "doc"); strcpy(saved.revision_id, "revision"); strcpy(saved.source_hash, hash);
    snprintf(path, sizeof(path), "%s/one.md", dir);
    FILE *f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs(text, f); fclose(f); }
    MemoryDocumentName names[2] = { { .name = "one.md" }, { .name = "one.md" } };
    MemoryDocumentSet set, copy;
    CHECK(memory_documents_capture(&manager, dir, &project, names, 2, false, NULL, &set));
    CHECK(set.count == 1 && !strcmp(set.refs[0].revision_id, "revision") && !strcmp(set.refs[0].source_hash, hash));
    copy = set; CHECK(memory_documents_same(&set, &copy));
    strcpy(copy.refs[0].revision_id, "new-revision"); CHECK(!memory_documents_same(&set, &copy));
    copy = set; memset(copy.refs[0].name, 'x', sizeof(copy.refs[0].name)); CHECK(!memory_documents_same(&set, &copy));
    MemoryScope scope = { .enabled = true }; strcpy(scope.project_id, "project"); strcpy(scope.database, "database");
    CHECK(memory_documents_admit(&database, &scope, &set));
    strcpy(saved.revision_id, "new-revision"); CHECK(!memory_documents_admit(&database, &scope, &set));
    strcpy(saved.revision_id, "revision"); transaction = true;
    CHECK(!memory_documents_admit(&database, &scope, &set)); transaction = false;
    unsigned old_reads = reads, old_routes = routes, old_identities = identities;
    CHECK(memory_documents_capture(NULL, NULL, NULL, NULL, SIZE_MAX, true, NULL, &copy) && !copy.count);
    CHECK(reads == old_reads && routes == old_routes && identities == old_identities);
    names[0].dirty = true;
    CHECK(memory_documents_capture(&manager, dir, &project, names, 1, false, NULL, &copy) && !copy.count);
    CHECK(reads == old_reads);
    names[0].dirty = false; unsaved = true;
    CHECK(memory_documents_capture(&manager, dir, &project, names, 1, false, NULL, &copy) && !copy.count);
    unsaved = false; offline = true;
    CHECK(!memory_documents_capture(&manager, dir, &project, names, 1, false, NULL, &copy) && !copy.count);
    offline = false;
    atomic_bool cancel; atomic_init(&cancel, true);
    CHECK(!memory_documents_capture(&manager, dir, &project, names, 1, false, &cancel, &copy) && !copy.count);
    StoreProjectIdentity wrong = project; wrong.inode++;
    CHECK(!memory_documents_capture(&manager, dir, &wrong, names, 1, false, NULL, &copy) && !copy.count);
    CHECK(!memory_documents_capture(&manager, dir, &project, names, MEMORY_DOCUMENTS_MAX + 1, false, NULL, &copy));
    MemoryDocumentCache *cache = memory_document_cache_create(); CHECK(cache != NULL);
    CHECK(memory_documents_capture_cached(cache, &manager, dir, &project, names, 1, false, NULL, &copy) && !copy.count);
    bool progress = false, pending = true;
    CHECK(memory_documents_verify_step(cache, &manager, dir, &project, names, 1, false, NULL, &progress, &pending));
    CHECK(progress && !pending);
    CHECK(memory_documents_capture_cached(cache, &manager, dir, &project, names, 1, false, NULL, &copy));
    CHECK(memory_documents_same(&set, &copy));
    old_routes = routes;
    CHECK(memory_documents_capture_cached_on_db(cache, &database, &manager, dir, &project, names, 1, false, NULL, 0, &copy));
    CHECK(memory_documents_same(&set, &copy) && routes == old_routes && borrowed_routes > 0);
    CHECK(memory_documents_verify_step_on_db(cache, &database, &manager, dir, &project, names, 1, false, NULL, 0, &progress, &pending));
    CHECK(!pending && routes == old_routes);
    strcpy(saved.source_hash, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    CHECK(memory_documents_capture_cached(cache, &manager, dir, &project, names, 1, false, NULL, &copy) && !copy.count);
    strcpy(saved.source_hash, hash);
    memory_document_cache_destroy(cache);
    CHECK(unlink(path) == 0);
    CHECK(memory_documents_capture(&manager, dir, &project, names, 1, false, NULL, &copy) && !copy.count);
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    TestCase tests[] = { { "current saved attachment capture", capture } };
    return run_tests(tests, sizeof(tests) / sizeof(*tests));
}
