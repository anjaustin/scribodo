#define _DARWIN_C_SOURCE
#include "memory_documents.h"
#include "app/memory_saved_file.h"
#include "app/store_backends.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include "core/deadline_clock.h"

bool memory_document_name_copy(const char *title, bool dirty, MemoryDocumentName *out)
{
    if (!out) return false;
    *out = (MemoryDocumentName){0};
    if (dirty || !title || !title[0] || strnlen(title, sizeof(out->name)) >= sizeof(out->name)) return false;
    memcpy(out->name, title, strlen(title) + 1);
    return true;
}

static bool identity_current(StoreManager *store, const char *path, const StoreProjectIdentity *expected)
{
    StoreProjectIdentity now;
    return expected && store_project_identity(store, path, &now) && now.generation == expected->generation &&
           now.device == expected->device && now.inode == expected->inode && !strcmp(now.project_id, expected->project_id);
}

static bool reference_same(const MemoryDocumentRef *a, const MemoryDocumentRef *b)
{
    return !strcmp(a->database, b->database) && !strcmp(a->project_id, b->project_id) &&
           !strcmp(a->document_id, b->document_id) && !strcmp(a->revision_id, b->revision_id) &&
           !strcmp(a->source_hash, b->source_hash) && !strcmp(a->name, b->name) && a->bytes == b->bytes;
}

static bool reference_valid(const MemoryDocumentRef *ref)
{
    const char *fields[] = { ref->database, ref->project_id, ref->document_id, ref->revision_id, ref->name };
    const size_t sizes[] = { sizeof(ref->database), sizeof(ref->project_id), sizeof(ref->document_id),
                            sizeof(ref->revision_id), sizeof(ref->name) };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
        if (!fields[i][0] || strnlen(fields[i], sizes[i]) == sizes[i]) return false;
    return strnlen(ref->source_hash, sizeof(ref->source_hash)) == 64 &&
           strspn(ref->source_hash, "0123456789abcdef") == 64 && ref->bytes <= MEMORY_SAVED_FILE_MAX_BYTES;
}

bool memory_documents_same(const MemoryDocumentSet *a, const MemoryDocumentSet *b)
{
    if (!a || !b || a->count > MEMORY_DOCUMENTS_MAX || a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (!reference_valid(&a->refs[i]) || !reference_valid(&b->refs[i]) || !reference_same(&a->refs[i], &b->refs[i])) return false;
    return true;
}

static MemoryDocumentRef from_saved(const SavedRevisionIdentity *saved, const char *name)
{
    MemoryDocumentRef ref = { .bytes = saved->bytes };
    snprintf(ref.database, sizeof(ref.database), "%s", saved->database);
    snprintf(ref.project_id, sizeof(ref.project_id), "%s", saved->project_id);
    snprintf(ref.document_id, sizeof(ref.document_id), "%s", saved->document_id);
    snprintf(ref.revision_id, sizeof(ref.revision_id), "%s", saved->revision_id);
    snprintf(ref.source_hash, sizeof(ref.source_hash), "%s", saved->source_hash);
    snprintf(ref.name, sizeof(ref.name), "%s", name);
    return ref;
}

bool memory_documents_capture(StoreManager *store, const char *path,
                               const StoreProjectIdentity *project, const MemoryDocumentName *names,
                               size_t count, bool remote, const atomic_bool *cancel, MemoryDocumentSet *out)
{
    if (!out) return false;
    *out = (MemoryDocumentSet){0};
    if (remote) return true;
    if (!store || !path || !project || count > MEMORY_DOCUMENTS_MAX || (count && !names) ||
        (cancel && atomic_load(cancel)) || !identity_current(store, path, project)) return false;
    char id[64]; Db *db = NULL;
    if (store_mode(store, path, id, sizeof(id), &db) != STORE_DB || !db || strcmp(id, project->project_id)) return false;
    MemoryDocumentSet result = {0};
    for (size_t i = 0; i < count; i++) {
        if (cancel && atomic_load(cancel)) return false;
        if (names[i].dirty || !names[i].name[0] || strnlen(names[i].name, sizeof(names[i].name)) == sizeof(names[i].name)) continue;
        SavedRevisionIdentity saved;
        if (!sdb_saved_revision_identity(db, id, names[i].name, &saved)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < result.count; j++)
            if (!strcmp(saved.document_id, result.refs[j].document_id)) { duplicate = true; break; }
        if (duplicate) continue;
        if (memory_saved_file_matches(path, names[i].name, project, saved.source_hash, saved.bytes, false, false, cancel))
            result.refs[result.count++] = from_saved(&saved, names[i].name);
    }
    if ((cancel && atomic_load(cancel)) || !identity_current(store, path, project)) return false;
    *out = result;
    return true;
}

typedef struct {
    bool used, inflight;
    uint64_t serial;
    unsigned failures;
    uint64_t retry_at_ms;
    StoreManager *store; /* identity only; never dereferenced from an entry */
    StoreProjectIdentity project;
    MemoryDocumentRef ref;
    MemorySavedFileVerification file;
} VerificationEntry;

struct MemoryDocumentCache {
    pthread_mutex_t mutex;
    uint64_t serial;
    VerificationEntry entries[MEMORY_DOCUMENT_CACHE_CAPACITY];
};

MemoryDocumentCache *memory_document_cache_create(void)
{
    MemoryDocumentCache *cache = calloc(1, sizeof(*cache));
    if (cache && pthread_mutex_init(&cache->mutex, NULL)) { free(cache); return NULL; }
    return cache;
}

void memory_document_cache_destroy(MemoryDocumentCache *cache)
{
    if (!cache) return;
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}

static bool project_same(const StoreProjectIdentity *a, const StoreProjectIdentity *b)
{
    return a->generation == b->generation && a->device == b->device && a->inode == b->inode &&
           !strcmp(a->project_id, b->project_id);
}

void memory_document_cache_invalidate(MemoryDocumentCache *cache)
{
    if (!cache) return;
    pthread_mutex_lock(&cache->mutex);
    /* Clearing an in-flight slot fences its eventual publication by serial. */
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++) cache->entries[i].used = false;
    pthread_mutex_unlock(&cache->mutex);
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(SCRIBODO_DEADLINE_CLOCK, &now) || now.tv_sec < 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void entry_discard_proof(MemoryDocumentCache *cache, VerificationEntry *entry)
{
    entry->file = (MemorySavedFileVerification){0};
    entry->inflight = false;
    entry->serial = ++cache->serial;
}

/* Worker cancellation discards partial contexts without refunding failures or
 * flushing independent verified proofs. Publication of old claims is fenced. */
static void cache_discard_partials(MemoryDocumentCache *cache)
{
    if (!cache) return;
    pthread_mutex_lock(&cache->mutex);
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++)
        if (cache->entries[i].used && !cache->entries[i].file.ready) entry_discard_proof(cache, &cache->entries[i]);
    pthread_mutex_unlock(&cache->mutex);
}

typedef struct {
    Db *db;
    const atomic_bool *cancel;
    double deadline_ms;
    bool borrowed;
} DocumentLookup;

static bool lookup_stopped(const DocumentLookup *lookup)
{
    if (lookup->cancel && atomic_load(lookup->cancel)) return true;
    if (!lookup->deadline_ms) return false;
    struct timespec now;
    return !isfinite(lookup->deadline_ms) || clock_gettime(SCRIBODO_DEADLINE_CLOCK, &now) != 0 ||
           (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0 >= lookup->deadline_ms;
}

static bool resolve_current(const DocumentLookup *lookup, StoreManager *store, const char *path, const StoreProjectIdentity *project,
                             const MemoryDocumentName *names, size_t count,
                             MemoryDocumentSet *out)
{
    *out = (MemoryDocumentSet){0};
    if (!store || !path || !project || count > MEMORY_DOCUMENTS_MAX || (count && !names) ||
        (lookup->borrowed && !lookup->db) || lookup_stopped(lookup) || !identity_current(store, path, project)) return false;
    char id[64]; Db *db = lookup->db;
    StoreMode mode = lookup->borrowed ? store_mode_borrowed(store, path, project, db, lookup->cancel,
                                                           lookup->deadline_ms, id, sizeof(id)) :
                                       store_mode(store, path, id, sizeof(id), &db);
    if (mode != STORE_DB || !db || strcmp(id, project->project_id) || lookup_stopped(lookup)) return false;
    for (size_t i = 0; i < count; i++) {
        if (lookup_stopped(lookup)) return false;
        if (names[i].dirty || !names[i].name[0] || strnlen(names[i].name, sizeof(names[i].name)) == sizeof(names[i].name)) continue;
        bool dirty_alias = false;
        for (size_t j = 0; j < count; j++)
            if (names[j].dirty && !strncmp(names[i].name, names[j].name, sizeof(names[j].name))) { dirty_alias = true; break; }
        if (dirty_alias) continue;
        SavedRevisionIdentity saved;
        if (!sdb_saved_revision_identity(db, id, names[i].name, &saved)) continue;
        if (lookup_stopped(lookup)) return false;
        MemoryDocumentRef ref = from_saved(&saved, names[i].name);
        if (!reference_valid(&ref) || strcmp(ref.project_id, project->project_id)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < out->count; j++)
            if (!strcmp(ref.document_id, out->refs[j].document_id)) { duplicate = true; break; }
        if (!duplicate) out->refs[out->count++] = ref;
    }
    return !lookup_stopped(lookup) && identity_current(store, path, project);
}

/* Revoke stale identities for these attachment names, including dirty/unsaved
 * names. A claim in progress can finish I/O, but can no longer publish. */
static void cache_reconcile(MemoryDocumentCache *cache, StoreManager *store, const StoreProjectIdentity *project,
                             const MemoryDocumentName *names, size_t count, const MemoryDocumentSet *current)
{
    pthread_mutex_lock(&cache->mutex);
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++) {
        VerificationEntry *e = &cache->entries[i];
        if (!e->used || e->store != store || e->project.device != project->device ||
            e->project.inode != project->inode || strcmp(e->project.project_id, project->project_id)) continue;
        bool named = false, live = false, changed = false;
        for (size_t j = 0; j < count; j++)
            if (!strncmp(e->ref.name, names[j].name, sizeof(names[j].name))) { named = true; break; }
        if (!named) continue;
        for (size_t j = 0; j < current->count; j++) {
            if (strcmp(e->ref.name, current->refs[j].name)) continue;
            if (project_same(&e->project, project) && reference_same(&e->ref, &current->refs[j])) live = true;
            else changed = true;
        }
        if (changed) e->used = false;
        else if (!live) entry_discard_proof(cache, e);
    }
    pthread_mutex_unlock(&cache->mutex);
}

/* Stack-owned claim: no borrowed entry/hash context escapes the mutex. FIFO
 * evicts only idle entries; revoked claims are fenced before slot reuse. */
static int cache_claim(MemoryDocumentCache *cache, StoreManager *store, const StoreProjectIdentity *project,
                        const MemoryDocumentRef *ref, bool create, VerificationEntry *copy)
{
    int slot = -1, oldest = -1;
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&cache->mutex);
    for (size_t i = 0; i < MEMORY_DOCUMENT_CACHE_CAPACITY; i++) {
        VerificationEntry *e = &cache->entries[i];
        if (e->used && e->store == store && project_same(&e->project, project) && reference_same(&e->ref, ref)) {
            bool retry = e->failures < 3 && (!e->retry_at_ms || (now && now >= e->retry_at_ms));
            if (!e->inflight && (e->file.ready || (create && retry))) slot = (int)i;
            goto found;
        }
        if (!e->used) oldest = (int)i;
        else if (!e->inflight && (oldest < 0 || (cache->entries[oldest].used && e->serial < cache->entries[oldest].serial))) oldest = (int)i;
    }
    if (create && oldest >= 0) {
        slot = oldest;
        cache->entries[slot] = (VerificationEntry){ .used = true, .serial = ++cache->serial,
            .store = store, .project = *project, .ref = *ref };
    }
found:
    if (slot >= 0) {
        cache->entries[slot].inflight = true;
        *copy = cache->entries[slot];
    }
    pthread_mutex_unlock(&cache->mutex);
    return slot;
}

typedef enum { CACHE_ACCEPT, CACHE_FILE_FAILED, CACHE_DISCARD, CACHE_RETAIN_PROOF } CachePublication;

static bool cache_publish(MemoryDocumentCache *cache, int slot, const VerificationEntry *copy, CachePublication outcome)
{
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&cache->mutex);
    VerificationEntry *e = &cache->entries[slot];
    bool current = e->used && e->inflight && e->serial == copy->serial && e->store == copy->store &&
                   project_same(&e->project, &copy->project) && reference_same(&e->ref, &copy->ref);
    if (current) {
        e->inflight = false;
        if (outcome == CACHE_ACCEPT) e->file = copy->file;
        else if (outcome != CACHE_RETAIN_PROOF) {
            e->file = (MemorySavedFileVerification){0};
            if (outcome == CACHE_FILE_FAILED && e->failures < 3) {
                e->failures++;
                e->retry_at_ms = now + (1000u << (e->failures - 1));
            }
        }
    }
    pthread_mutex_unlock(&cache->mutex);
    return current && outcome == CACHE_ACCEPT;
}

/* SQL is resolved again after I/O; a source/origin/store change cannot publish
 * a verification using the earlier SQL receipt. No cache lock crosses I/O. */
static bool ref_current(const DocumentLookup *lookup, StoreManager *store, const char *path,
                         const StoreProjectIdentity *project, const MemoryDocumentRef *ref)
{
    MemoryDocumentName name = {0};
    snprintf(name.name, sizeof(name.name), "%s", ref->name);
    MemoryDocumentSet now;
    return resolve_current(lookup, store, path, project, &name, 1, &now) && now.count == 1 && reference_same(ref, &now.refs[0]);
}

static bool cached_operation(MemoryDocumentCache *cache, const DocumentLookup *lookup, StoreManager *store, const char *path,
                              const StoreProjectIdentity *project, const MemoryDocumentName *names,
                              size_t count, bool remote, MemoryDocumentSet *out,
                              bool verify, bool *progress, bool *pending)
{
    *out = (MemoryDocumentSet){0};
    if (remote) return true;
    if (!cache) return false;
    MemoryDocumentSet current;
    if (!resolve_current(lookup, store, path, project, names, count, &current)) {
        if (verify) cache_discard_partials(cache);
        if (!lookup_stopped(lookup) && store && project && count <= MEMORY_DOCUMENTS_MAX && (!count || names)) {
            MemoryDocumentSet empty = {0};
            cache_reconcile(cache, store, project, names, count, &empty);
        }
        return false;
    }
    cache_reconcile(cache, store, project, names, count, &current);
    size_t budget = MEMORY_SAVED_FILE_STEP_BYTES;
    MemoryDocumentSet result = {0};
    for (size_t i = 0; i < current.count; i++) {
        if (lookup_stopped(lookup)) { if (verify) cache_discard_partials(cache); return false; }
        VerificationEntry work;
        const MemoryDocumentRef *ref = &current.refs[i];
        int slot = cache_claim(cache, store, project, ref, verify, &work);
        if (slot < 0) { if (pending) *pending = true; continue; }
        uint64_t before = work.file.offset;
        bool was_ready = work.file.ready;
        bool ok;
        if (work.file.ready) ok = memory_saved_file_recheck(path, ref->name, project, ref->bytes, lookup->cancel, &work.file);
        else if (verify && budget) ok = memory_saved_file_verify_step(path, ref->name, project, ref->source_hash,
                                                                      ref->bytes, lookup->cancel, &budget, &work.file);
        else ok = true; /* Keep bounded pending state, but no extra file lookup. */
        CachePublication outcome = ok ? CACHE_ACCEPT : CACHE_FILE_FAILED;
        if (ok && !ref_current(lookup, store, path, project, ref)) outcome = CACHE_DISCARD;
        if (lookup_stopped(lookup)) outcome = verify ? CACHE_DISCARD : CACHE_RETAIN_PROOF;
        bool published = cache_publish(cache, slot, &work, outcome);
        if (published && work.file.ready) result.refs[result.count++] = *ref;
        else if (pending) *pending = true;
        if (progress && published && (work.file.offset > before || (!was_ready && work.file.ready))) *progress = true;
    }
    if (lookup_stopped(lookup)) { if (verify) cache_discard_partials(cache); return false; }
    if (!identity_current(store, path, project)) {
        MemoryDocumentSet empty = {0};
        cache_reconcile(cache, store, project, names, count, &empty);
        return false;
    }
    if (lookup_stopped(lookup)) { if (verify) cache_discard_partials(cache); return false; }
    *out = result;
    return true;
}

bool memory_documents_capture_cached(MemoryDocumentCache *cache, StoreManager *store, const char *path,
                                      const StoreProjectIdentity *project, const MemoryDocumentName *names,
                                      size_t count, bool remote, const atomic_bool *cancel, MemoryDocumentSet *out)
{
    if (!out) return false;
    DocumentLookup lookup = { .cancel = cancel };
    return cached_operation(cache, &lookup, store, path, project, names, count, remote, out, false, NULL, NULL);
}

bool memory_documents_capture_cached_on_db(MemoryDocumentCache *cache, Db *db, StoreManager *store, const char *path,
                                            const StoreProjectIdentity *project, const MemoryDocumentName *names,
                                            size_t count, bool remote, const atomic_bool *cancel,
                                            double deadline_ms, MemoryDocumentSet *out)
{
    if (!out) return false;
    DocumentLookup lookup = { .db = db, .cancel = cancel, .deadline_ms = deadline_ms, .borrowed = true };
    return cached_operation(cache, &lookup, store, path, project, names, count, remote, out, false, NULL, NULL);
}

bool memory_documents_verify_step(MemoryDocumentCache *cache, StoreManager *store, const char *path,
                                   const StoreProjectIdentity *project, const MemoryDocumentName *names,
                                   size_t count, bool remote, const atomic_bool *cancel, bool *progress, bool *pending)
{
    if (progress) *progress = false;
    if (pending) *pending = false;
    MemoryDocumentSet ignored;
    DocumentLookup lookup = { .cancel = cancel };
    return cached_operation(cache, &lookup, store, path, project, names, count, remote, &ignored, true, progress, pending);
}

bool memory_documents_verify_step_on_db(MemoryDocumentCache *cache, Db *db, StoreManager *store, const char *path,
                                         const StoreProjectIdentity *project, const MemoryDocumentName *names,
                                         size_t count, bool remote, const atomic_bool *cancel, double deadline_ms,
                                         bool *progress, bool *pending)
{
    if (progress) *progress = false;
    if (pending) *pending = false;
    MemoryDocumentSet ignored;
    DocumentLookup lookup = { .db = db, .cancel = cancel, .deadline_ms = deadline_ms, .borrowed = true };
    return cached_operation(cache, &lookup, store, path, project, names, count, remote, &ignored, true, progress, pending);
}

bool memory_documents_admit(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents)
{
    if (!db || db_in_transaction(db) || !scope || !scope->enabled || !documents || documents->count > MEMORY_DOCUMENTS_MAX) return false;
    for (size_t i = 0; i < documents->count; i++) {
        const MemoryDocumentRef *ref = &documents->refs[i];
        if (!reference_valid(ref) || strnlen(ref->name, sizeof(ref->name)) == sizeof(ref->name) ||
            strnlen(ref->database, sizeof(ref->database)) == sizeof(ref->database) ||
            strnlen(ref->project_id, sizeof(ref->project_id)) == sizeof(ref->project_id) ||
            strnlen(ref->document_id, sizeof(ref->document_id)) == sizeof(ref->document_id) ||
            strnlen(ref->revision_id, sizeof(ref->revision_id)) == sizeof(ref->revision_id) ||
            strnlen(ref->source_hash, sizeof(ref->source_hash)) == sizeof(ref->source_hash) ||
            strcmp(ref->project_id, scope->project_id) || strcmp(ref->database, scope->database)) return false;
        SavedRevisionIdentity saved;
        if (!sdb_saved_revision_identity(db, scope->project_id, ref->name, &saved)) return false;
        MemoryDocumentRef now = from_saved(&saved, ref->name);
        if (!reference_same(ref, &now)) return false;
    }
    return true;
}
