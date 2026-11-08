#define _DARWIN_C_SOURCE
#include "app/memory_documents.h"
#include "app/memory_saved_file.h"
#include "app/store_backends.h"
#include "test_util.h"
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* Standalone white-box test: compile this translation unit alone. Interpose
 * body reads, not wall-clock timing, to pin the foreground/worker I/O contract. */
static ssize_t cache_test_pread(int fd, void *buf, size_t bytes, off_t offset);
static ssize_t cache_test_read(int fd, void *buf, size_t bytes);
static int cache_test_openat(int fd, const char *name, int flags);
#define pread cache_test_pread
#define read cache_test_read
#define openat cache_test_openat
#include "../src/app/memory_saved_file.c"
#undef pread
#undef read
#undef openat
static int cache_test_clock_gettime(clockid_t clock, struct timespec *out);
#define clock_gettime cache_test_clock_gettime
#include "../src/app/memory_documents.c"
#include "core/deadline_clock.h"
#undef clock_gettime

struct StoreManager { int unused; };
struct Db { int unused; };
static StoreManager managers[2];
static Db databases[2];
static StoreProjectIdentity identity;
static SavedRevisionIdentity sources[24];
static MemoryDocumentName attachments[24];
static MemoryDocumentCache *active_cache;
static char directory[PATH_MAX];
static size_t body_bytes, requested_bytes, largest_read;
static unsigned body_calls, file_opens, sql_calls, action;
static unsigned ordinary_routes, borrowed_routes, closed_databases, sql_action;
static unsigned sql_skip;
static bool close_on_ordinary_route;
static double borrowed_deadline;
static atomic_bool cancelled;
static bool missing;
static uint64_t clock_ms;
static bool advance_clock;

static int cache_test_clock_gettime(clockid_t clock, struct timespec *out)
{
    CHECK(clock == SCRIBODO_DEADLINE_CLOCK);
    out->tv_sec = (time_t)(clock_ms / 1000u);
    out->tv_nsec = (long)(clock_ms % 1000u) * 1000000L;
    return 0;
}

static void unlocked(void)
{
    if (!active_cache) return;
    int result = pthread_mutex_trylock(&active_cache->mutex);
    CHECK(result == 0);
    if (!result) pthread_mutex_unlock(&active_cache->mutex);
}

bool db_in_transaction(Db *db) { (void)db; return false; }
bool store_project_identity(StoreManager *store, const char *path, StoreProjectIdentity *out)
{
    unlocked(); CHECK(store == &managers[0] || store == &managers[1]);
    (void)path; *out = identity; return true;
}
StoreMode store_mode(StoreManager *store, const char *path, char *id, size_t len, Db **db)
{
    unlocked(); (void)path; ordinary_routes++;
    if (close_on_ordinary_route) { closed_databases++; return STORE_PAUSED; }
    snprintf(id, len, "%s", identity.project_id);
    *db = &databases[store == &managers[1]];
    return STORE_DB;
}
StoreMode store_mode_borrowed(StoreManager *store, const char *path, const StoreProjectIdentity *expected,
                               Db *db, const atomic_bool *cancel, double deadline_ms, char *id, size_t len)
{
    unlocked(); (void)path; borrowed_routes++; borrowed_deadline = deadline_ms;
    CHECK(store == &managers[0] || store == &managers[1]);
    CHECK(db == &databases[store == &managers[1]]);
    if ((cancel && atomic_load(cancel)) || (deadline_ms && clock_ms >= deadline_ms) ||
        expected->generation != identity.generation || expected->device != identity.device ||
        expected->inode != identity.inode || strcmp(expected->project_id, identity.project_id)) return STORE_PAUSED;
    snprintf(id, len, "%s", identity.project_id);
    return STORE_DB;
}
bool sdb_saved_revision_identity(Db *db, const char *project, const char *name, SavedRevisionIdentity *out)
{
    unlocked(); sql_calls++;
    if (missing || strcmp(project, identity.project_id)) return false;
    for (size_t i = 0; i < 24; i++) {
        if (strcmp(name, attachments[i].name)) continue;
        *out = sources[i];
        if (db == &databases[1]) snprintf(out->database, sizeof(out->database), "other-database");
        unsigned next_action = 0;
        if (sql_skip) sql_skip--;
        else { next_action = sql_action; sql_action = 0; }
        if (next_action == 1) identity.generation++;
        if (next_action == 2) atomic_store(&cancelled, true);
        if (next_action == 3) clock_ms += 1000;
        return true;
    }
    return false;
}

static ssize_t cache_test_pread(int fd, void *buf, size_t bytes, off_t offset)
{
    unlocked();
    ssize_t n = pread(fd, buf, bytes, offset);
    body_calls++; requested_bytes += bytes;
    if (n > 0) body_bytes += (size_t)n;
    if (bytes > largest_read) largest_read = bytes;
    unsigned next_action = action; action = 0;
    if (next_action == 1) atomic_store(&cancelled, true);
    if (next_action == 2) {
        /* Explicit invalidation fences this claim without taking an I/O lock. */
        memory_document_cache_invalidate(active_cache);
    }
    if (next_action == 3) strcpy(sources[0].revision_id, "changed-during-io");
    if (next_action == 4) memory_document_cache_invalidate(active_cache);
    return n;
}
static ssize_t cache_test_read(int fd, void *buf, size_t bytes)
{
    body_calls++; requested_bytes += bytes;
    ssize_t n = read(fd, buf, bytes);
    if (n > 0) body_bytes += (size_t)n;
    return n;
}
static int cache_test_openat(int fd, const char *name, int flags)
{
    unlocked(); file_opens++;
    return openat(fd, name, flags);
}

static void reset_io(void)
{
    body_bytes = requested_bytes = largest_read = 0;
    body_calls = file_opens = sql_calls = 0;
}

static void hash_zeros(uint64_t bytes, char hash[65])
{
    unsigned char zero[16384] = {0}, sum[32];
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    while (bytes) {
        size_t n = bytes < sizeof(zero) ? (size_t)bytes : sizeof(zero);
        CC_SHA256_Update(&ctx, zero, (CC_LONG)n); bytes -= n;
    }
    CC_SHA256_Final(sum, &ctx);
    for (size_t i = 0; i < sizeof(sum); i++) snprintf(hash + i * 2, 3, "%02x", sum[i]);
}

static void fixture(uint64_t bytes)
{
    snprintf(directory, sizeof(directory), "%s/memory-cache-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(directory) != NULL);
    struct stat st; CHECK(stat(directory, &st) == 0);
    identity = (StoreProjectIdentity){ .generation = 1, .device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino };
    strcpy(identity.project_id, "project");
    char hash[65]; hash_zeros(bytes, hash);
    for (size_t i = 0; i < 24; i++) {
        attachments[i] = (MemoryDocumentName){0};
        snprintf(attachments[i].name, sizeof(attachments[i].name), "file-%zu.md", i);
        sources[i] = (SavedRevisionIdentity){ .bytes = bytes };
        strcpy(sources[i].database, "database"); strcpy(sources[i].project_id, "project");
        snprintf(sources[i].document_id, sizeof(sources[i].document_id), "doc-%zu", i);
        strcpy(sources[i].revision_id, "revision"); strcpy(sources[i].source_hash, hash);
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", directory, attachments[i].name);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600); CHECK(fd >= 0);
        if (fd >= 0) { CHECK(ftruncate(fd, (off_t)bytes) == 0); close(fd); }
    }
    missing = false; action = 0; atomic_init(&cancelled, false);
    sql_action = ordinary_routes = borrowed_routes = closed_databases = 0;
    sql_skip = 0;
    close_on_ordinary_route = false;
    clock_ms = 100000; advance_clock = true;
    active_cache = memory_document_cache_create(); CHECK(active_cache != NULL);
    reset_io();
}

static void cleanup(void)
{
    memory_document_cache_destroy(active_cache); active_cache = NULL;
    for (size_t i = 0; i < 24; i++) {
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", directory, attachments[i].name);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(directory) == 0);
}

static MemoryDocumentSet cached(StoreManager *store, MemoryDocumentName *names, size_t count)
{
    MemoryDocumentSet out;
    reset_io();
    CHECK(memory_documents_capture_cached(active_cache, store, directory, &identity, names, count, false, &cancelled, &out));
    CHECK(body_calls == 0 && body_bytes == 0 && requested_bytes == 0);
    CHECK(file_opens <= MEMORY_DOCUMENTS_MAX);
    return out;
}

static void step(MemoryDocumentName *names, size_t count, bool *progress, bool *pending)
{
    if (advance_clock) clock_ms += 10000;
    reset_io();
    CHECK(memory_documents_verify_step(active_cache, &managers[0], directory, &identity, names, count, false,
                                       &cancelled, progress, pending));
    CHECK(requested_bytes <= MEMORY_SAVED_FILE_STEP_BYTES && largest_read <= 16384);
    CHECK(file_opens <= MEMORY_DOCUMENTS_MAX);
}

static void warm(MemoryDocumentName *names, size_t count)
{
    bool progress, pending;
    for (unsigned i = 0; i < 1033; i++) {
        step(names, count, &progress, &pending);
        if (!pending) return;
        CHECK(progress);
    }
    CHECK(false);
}

static void bounded_verification(void)
{
    fixture(MEMORY_SAVED_FILE_MAX_BYTES);
    MemoryDocumentSet out = cached(&managers[0], attachments, 1);
    CHECK(!out.count && file_opens == 0); /* stat-only cannot authorize. */
    bool progress, pending = true;
    unsigned steps = 0;
    while (pending && steps < 130) {
        step(attachments, 1, &progress, &pending); steps++;
        CHECK(progress);
        out = cached(&managers[0], attachments, 1);
        CHECK(out.count == (pending ? 0u : 1u));
    }
    CHECK(steps >= 128 && !pending);
    CHECK(out.count == 1 && file_opens == 1);
    /* The budget is shared across all attachments, not 128 KiB per file. */
    step(&attachments[1], 8, &progress, &pending);
    CHECK(progress && pending && requested_bytes == MEMORY_SAVED_FILE_STEP_BYTES);
    out = cached(&managers[0], &attachments[1], 8); CHECK(!out.count);
    /* New source hash has identical metadata, but can never become ready. */
    strcpy(sources[0].source_hash, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    out = cached(&managers[0], attachments, 1); CHECK(out.count == 0);
    for (unsigned i = 0; i < 129; i++) step(attachments, 1, &progress, &pending);
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    cleanup();
}

static void isolation_and_foreground(void)
{
    fixture(64);
    warm(attachments, 8);
    MemoryDocumentSet out = cached(&managers[0], attachments, 8);
    CHECK(out.count == 8 && file_opens == 8);
    out = cached(&managers[1], attachments, 8); CHECK(!out.count && !file_opens);
    out = cached(&managers[0], attachments, 8); CHECK(out.count == 8);
    identity.generation++;
    out = cached(&managers[0], attachments, 8); CHECK(!out.count);
    warm(attachments, 8);
    strcpy(sources[0].database, "new-database");
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    warm(attachments, 1);
    strcpy(sources[0].revision_id, "new-saved-id");
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    warm(attachments, 1);
    strcpy(sources[0].document_id, "new-document-id");
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    warm(attachments, 1);
    sources[0].bytes--;
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    sources[0].bytes++; warm(attachments, 1);
    attachments[0].dirty = true;
    out = cached(&managers[0], attachments, 1); CHECK(!out.count && !file_opens);
    attachments[0].dirty = false;
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    warm(attachments, 1);
    MemoryDocumentName aliases[2] = { attachments[0], attachments[0] }; aliases[1].dirty = true;
    out = cached(&managers[0], aliases, 2); CHECK(!out.count);
    warm(attachments, 1);
    missing = true;
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    missing = false;
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    warm(attachments, 1);
    reset_io();
    CHECK(memory_documents_capture_cached(active_cache, NULL, NULL, NULL, NULL, SIZE_MAX, true, NULL, &out));
    CHECK(!out.count && !sql_calls && !body_calls && !file_opens);
    out = cached(&managers[0], attachments, 1); CHECK(out.count == 1);
    cleanup();
}

static void cancellation_mutation_and_fencing(void)
{
    fixture(2 * MEMORY_SAVED_FILE_STEP_BYTES);
    bool progress, pending;
    action = 1; reset_io();
    CHECK(!memory_documents_verify_step(active_cache, &managers[0], directory, &identity, attachments, 1,
                                        false, &cancelled, &progress, &pending));
    CHECK(body_calls == 1 && body_bytes == 16384);
    atomic_store(&cancelled, false);
    MemoryDocumentSet out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    step(attachments, 1, &progress, &pending); CHECK(progress && pending);
    char path[PATH_MAX], replacement[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", directory, attachments[0].name);
    snprintf(replacement, sizeof(replacement), "%s/replacement", directory);
    int fd = open(replacement, O_CREAT | O_WRONLY, 0600); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(ftruncate(fd, 2 * MEMORY_SAVED_FILE_STEP_BYTES) == 0); close(fd); }
    CHECK(rename(replacement, path) == 0);
    step(attachments, 1, &progress, &pending);
    CHECK(!progress && pending && body_calls == 0); /* Resume must reject replacement. */
    step(attachments, 1, &progress, &pending); CHECK(progress && pending);
    fd = open(path, O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(pwrite(fd, "!", 1, 0) == 1); close(fd); }
    step(attachments, 1, &progress, &pending); CHECK(!progress && pending && !body_calls);
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    fd = open(path, O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(pwrite(fd, "\0", 1, 0) == 1); close(fd); }
    action = 2;
    step(attachments, 1, &progress, &pending); CHECK(!progress && pending);
    out = cached(&managers[0], attachments, 1); CHECK(!out.count); /* Fenced publication. */
    action = 3;
    step(attachments, 1, &progress, &pending); CHECK(!progress && pending);
    out = cached(&managers[0], attachments, 1); CHECK(!out.count); /* SQL changed during I/O. */
    warm(attachments, 1);
    /* A verified file still needs a cheap identity recheck on every capture. */
    CHECK(unlink(path) == 0); CHECK(symlink(attachments[1].name, path) == 0);
    out = cached(&managers[0], attachments, 1); CHECK(!out.count && file_opens == 1);
    cleanup();
}

static void fixed_capacity_fifo(void)
{
    fixture(2 * MEMORY_SAVED_FILE_STEP_BYTES);
    bool progress, pending;
    for (size_t i = 0; i < 24; i++) {
        step(&attachments[i], 1, &progress, &pending); CHECK(progress && pending);
        size_t used = 0;
        for (size_t j = 0; j < MEMORY_DOCUMENT_CACHE_CAPACITY; j++)
            if (active_cache->entries[j].used) used++;
        CHECK(used <= 16);
    }
    bool first_present = false, last_present = false;
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++) {
        VerificationEntry *e = &active_cache->entries[i];
        if (e->used && !strcmp(e->ref.name, attachments[0].name)) first_present = true;
        if (e->used && !strcmp(e->ref.name, attachments[23].name)) last_present = true;
    }
    CHECK(!first_present && last_present);
    /* No body reads even with sixteen pending hash contexts. */
    MemoryDocumentSet out = cached(&managers[0], attachments, 8); CHECK(!out.count);
    cleanup();
}

static VerificationEntry *first_entry(void)
{
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++)
        if (active_cache->entries[i].used && !strcmp(active_cache->entries[i].ref.name, attachments[0].name))
            return &active_cache->entries[i];
    return NULL;
}

static void bounded_failures_and_backoff(void)
{
    fixture(MEMORY_SAVED_FILE_MAX_BYTES);
    advance_clock = false;
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", directory, attachments[0].name);
    int fd = open(path, O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(pwrite(fd, "!", 1, 0) == 1); close(fd); }
    bool progress, pending;
    size_t total = 0;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        for (size_t chunk = 0; chunk < sources[0].bytes / MEMORY_SAVED_FILE_STEP_BYTES; chunk++) {
            step(attachments, 1, &progress, &pending); total += body_bytes; CHECK(progress && pending);
        }
        step(attachments, 1, &progress, &pending); total += body_bytes; CHECK(!progress && pending);
        VerificationEntry *entry = first_entry(); CHECK(entry && entry->failures == attempt + 1 && !entry->file.ready);
        uint64_t delay = 1000u << attempt;
        CHECK(entry && entry->retry_at_ms == clock_ms + delay);
        clock_ms += delay - 1;
        for (unsigned poll = 0; poll < 25; poll++) {
            step(attachments, 1, &progress, &pending);
            CHECK(!progress && pending && !body_calls && !file_opens);
            MemoryDocumentSet out = cached(&managers[0], attachments, 1); CHECK(!out.count);
        }
        clock_ms++;
    }
    CHECK(total == 3 * sources[0].bytes);
    clock_ms += 1000000;
    for (unsigned poll = 0; poll < 100; poll++) {
        step(attachments, 1, &progress, &pending); CHECK(!progress && pending && !body_calls && !file_opens);
    }
    /* Cancellation and remote policy changes do not refund terminal failure. */
    atomic_store(&cancelled, true);
    MemoryDocumentSet out;
    CHECK(!memory_documents_capture_cached(active_cache, &managers[0], directory, &identity, attachments, 1, false, &cancelled, &out));
    atomic_store(&cancelled, false);
    CHECK(memory_documents_capture_cached(active_cache, NULL, NULL, NULL, NULL, 0, true, NULL, &out));
    step(attachments, 1, &progress, &pending); CHECK(!progress && pending && !body_calls);
    /* Restoring the bytes alone must not silently restart a terminal key. */
    fd = open(path, O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(pwrite(fd, "\0", 1, 0) == 1); close(fd); }
    step(attachments, 1, &progress, &pending); CHECK(!progress && pending && !body_calls);
    memory_document_cache_invalidate(active_cache);
    warm(attachments, 1);
    out = cached(&managers[0], attachments, 1); CHECK(out.count == 1);
    /* A new SQL revision is a fresh key without requiring invalidate. */
    strcpy(sources[0].revision_id, "fresh-revision");
    step(attachments, 1, &progress, &pending); CHECK(progress && pending && body_bytes == MEMORY_SAVED_FILE_STEP_BYTES);
    CHECK(first_entry() && !first_entry()->failures);
    /* Explicit reset also drops a live partial context for this same SQL key. */
    memory_document_cache_invalidate(active_cache);
    warm(attachments, 1);
    out = cached(&managers[0], attachments, 1); CHECK(out.count == 1);
    cleanup();
}

static void cancellation_refunds_and_explicit_fence(void)
{
    fixture(64);
    bool progress, pending;
    for (unsigned i = 0; i < 5; i++) {
        action = 1; reset_io();
        CHECK(!memory_documents_verify_step(active_cache, &managers[0], directory, &identity, attachments, 1,
                                            false, &cancelled, &progress, &pending));
        CHECK(body_calls == 1);
        atomic_store(&cancelled, false);
        VerificationEntry *entry = first_entry();
        CHECK(entry && !entry->failures && !entry->file.started && !entry->file.ready);
    }
    action = 4;
    step(attachments, 1, &progress, &pending); CHECK(!progress && pending);
    CHECK(first_entry() == NULL); /* Held stack hash context cannot resurrect the slot. */
    warm(attachments, 1);
    MemoryDocumentSet out = cached(&managers[0], attachments, 1); CHECK(out.count == 1);
    /* Foreground recheck failure removes proof immediately and adds a bounded
     * failure, never making a blocked entry eligible from stat alone. */
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", directory, attachments[0].name);
    int fd = open(path, O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(pwrite(fd, "!", 1, 0) == 1); close(fd); }
    out = cached(&managers[0], attachments, 1); CHECK(!out.count);
    CHECK(first_entry() && first_entry()->failures == 1 && !first_entry()->file.ready);
    out = cached(&managers[0], attachments, 1); CHECK(!out.count && !file_opens);
    cleanup();
}

static size_t ready_proofs(void)
{
    size_t count = 0;
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++)
        if (active_cache->entries[i].used && active_cache->entries[i].file.ready) count++;
    return count;
}

static void borrowed_database_and_optional_deadlines(void)
{
    fixture(64);
    warm(attachments, 2);
    CHECK(ready_proofs() == 2);
    StoreProjectIdentity expected = identity;
    close_on_ordinary_route = true; /* Model legacy refresh closing a borrowed Db. */
    ordinary_routes = borrowed_routes = closed_databases = 0;
    MemoryDocumentSet out;
    bool progress, pending;
    double deadline = (double)clock_ms + 10000;
    reset_io();
    CHECK(memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                 attachments, 2, false, &cancelled, deadline, &out));
    CHECK(out.count == 2 && !body_calls && file_opens == 2);
    CHECK(borrowed_routes >= 3 && borrowed_deadline == deadline && !ordinary_routes && !closed_databases);
    CHECK(memory_documents_verify_step_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                              attachments, 2, false, &cancelled, deadline, &progress, &pending));
    CHECK(!progress && !pending && !ordinary_routes && !closed_databases);

    /* Expiry before lookup performs no routing, SQL or file I/O and keeps both
     * independent proofs available for a future fresh SQL/stat admission. */
    unsigned borrowed_before = borrowed_routes;
    reset_io();
    CHECK(!memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                  attachments, 1, false, &cancelled, (double)clock_ms, &out));
    CHECK(!out.count && !body_calls && !sql_calls && !file_opens && borrowed_routes == borrowed_before);
    CHECK(ready_proofs() == 2);
    CHECK(!memory_documents_verify_step_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                               attachments, 1, false, &cancelled, (double)clock_ms, &progress, &pending));
    CHECK(!progress && !body_calls && !sql_calls && !file_opens && ready_proofs() == 2);
    atomic_store(&cancelled, true);
    CHECK(!memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                  attachments, 1, false, &cancelled, deadline, &out));
    CHECK(ready_proofs() == 2 && !file_opens && !sql_calls);
    atomic_store(&cancelled, false);

    /* Abort during the final per-entry SQL recheck, after claiming a verified
     * entry. Its claim is released without wiping that or independent proof. */
    sql_action = 2; sql_skip = 1; reset_io();
    CHECK(!memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                  attachments, 1, false, &cancelled, deadline, &out));
    CHECK(!out.count && !body_calls && file_opens == 1 && ready_proofs() == 2);
    atomic_store(&cancelled, false);
    sql_action = 3; sql_skip = 1; reset_io();
    CHECK(!memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                  attachments, 1, false, &cancelled, (double)clock_ms + 100, &out));
    CHECK(!out.count && !body_calls && file_opens == 1 && ready_proofs() == 2);
    CHECK(memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                 attachments, 2, false, &cancelled, 0, &out));
    CHECK(out.count == 2 && ready_proofs() == 2);

    /* Borrowed worker cancellation drops only partial hashing, never charging
     * a permanent failure or discarding the two independent ready proofs. */
    action = 1; reset_io();
    CHECK(!memory_documents_verify_step_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                               &attachments[2], 1, false, &cancelled, 0, &progress, &pending));
    CHECK(!progress && body_calls == 1 && ready_proofs() == 2);
    atomic_store(&cancelled, false);
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++) {
        VerificationEntry *entry = &active_cache->entries[i];
        if (entry->used && !strcmp(entry->ref.name, attachments[2].name))
            CHECK(!entry->failures && !entry->file.started && !entry->file.ready);
    }

    /* Remote bypass does not even inspect a cache/Db/cancel pointer. */
    reset_io(); borrowed_before = borrowed_routes;
    CHECK(memory_documents_capture_cached_on_db((MemoryDocumentCache *)(uintptr_t)1, (Db *)(uintptr_t)1,
                                                 NULL, NULL, NULL, NULL, SIZE_MAX, true,
                                                 (const atomic_bool *)(uintptr_t)1, -1, &out));
    CHECK(!out.count && !sql_calls && !file_opens && !body_calls && borrowed_routes == borrowed_before);
    CHECK(memory_documents_verify_step_on_db((MemoryDocumentCache *)(uintptr_t)1, (Db *)(uintptr_t)1,
                                              NULL, NULL, NULL, NULL, SIZE_MAX, true,
                                              (const atomic_bool *)(uintptr_t)1, -1, &progress, &pending));
    CHECK(!progress && !pending && !sql_calls && !file_opens && !body_calls && borrowed_routes == borrowed_before);

    /* Generation changes during SQL fail without any legacy refresh/close. */
    sql_action = 1; reset_io();
    CHECK(!memory_documents_capture_cached_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                                  attachments, 1, false, &cancelled, 0, &out));
    CHECK(!out.count && !body_calls && !ordinary_routes && !closed_databases);
    expected = identity;
    memory_document_cache_invalidate(active_cache);
    sql_action = 1; sql_skip = 1; reset_io();
    CHECK(!memory_documents_verify_step_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                               attachments, 1, false, &cancelled, 0, &progress, &pending));
    CHECK(!progress && !ready_proofs() && !ordinary_routes && !closed_databases);
    expected = identity;
    CHECK(memory_documents_verify_step_on_db(active_cache, &databases[0], &managers[0], directory, &expected,
                                              attachments, 1, false, &cancelled, 0, &progress, &pending));
    CHECK(progress && !pending && ready_proofs() == 1 && !ordinary_routes && !closed_databases);

    reset_io(); borrowed_before = borrowed_routes;
    CHECK(!memory_documents_capture_cached_on_db(active_cache, NULL, &managers[0], directory, &expected,
                                                  attachments, 1, false, &cancelled, 0, &out));
    CHECK(!memory_documents_verify_step_on_db(active_cache, NULL, &managers[0], directory, &expected,
                                               attachments, 1, false, &cancelled, 0, &progress, &pending));
    CHECK(!out.count && !body_calls && !sql_calls && !file_opens && borrowed_routes == borrowed_before && !ordinary_routes && !closed_databases);
    cleanup();
}

int main(void)
{
    TestCase tests[] = {
        { "bounded full-hash verification", bounded_verification },
        { "cache isolation and zero-body foreground", isolation_and_foreground },
        { "cancellation, mutation and publication fencing", cancellation_mutation_and_fencing },
        { "fixed capacity FIFO pending state", fixed_capacity_fifo },
        { "bounded failures and monotonic backoff", bounded_failures_and_backoff },
        { "cancellation refunds and explicit invalidation fence", cancellation_refunds_and_explicit_fence },
        { "borrowed Db routing and optional deadline isolation", borrowed_database_and_optional_deadlines },
    };
    return run_tests(tests, sizeof(tests) / sizeof(*tests));
}
