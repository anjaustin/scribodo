/* Current explicit saved attachment identities, never historical filename links. */
#ifndef SCRIBODO_MEMORY_DOCUMENTS_H
#define SCRIBODO_MEMORY_DOCUMENTS_H

#include "app/memory_store.h"
#include "app/store.h"
#include <stdatomic.h>

#define MEMORY_DOCUMENTS_MAX 8u
#define MEMORY_DOCUMENT_SMALL_UNIT_TOKENS 128u /* gated experiment; normal/legacy cap remains 512 */

typedef struct {
    char database[64], project_id[40], document_id[40], revision_id[40];
    char source_hash[65], name[256];
    uint64_t bytes;
} MemoryDocumentRef;

typedef struct {
    MemoryDocumentRef refs[MEMORY_DOCUMENTS_MAX];
    size_t count;
} MemoryDocumentSet;

typedef struct {
    char name[256];
    bool dirty;
} MemoryDocumentName;

#define MEMORY_DOCUMENT_CACHE_CAPACITY 16u
typedef struct MemoryDocumentCache MemoryDocumentCache;

/* Owned by the app, never process-global. Owner must join all chat/worker
 * borrowers before destroy. Concurrent capture/verify calls are supported;
 * cache locks are never held across SQL/filesystem I/O. Fixed-capacity FIFO. */
MemoryDocumentCache *memory_document_cache_create(void);
void memory_document_cache_destroy(MemoryDocumentCache *cache);
/* Explicit Rebuild/scope-generation reset: discard proofs, failure/backoff
 * state and fence in-flight work. Safe with concurrent borrowers. */
void memory_document_cache_invalidate(MemoryDocumentCache *cache);
/* Metadata + previously full-hash-verified hits only; zero file-body reads.
 * Uncached/warming sources are omitted, not admitted. Call again for final
 * admission to re-resolve SQL identities and cheaply recheck file stamps.
 * Compatibility wrapper uses ordinary store routing; callers holding a Db
 * must use _on_db instead, to avoid connection refresh/close on reentry. */
bool memory_documents_capture_cached(MemoryDocumentCache *cache, StoreManager *store, const char *project_dir,
                                      const StoreProjectIdentity *project, const MemoryDocumentName *names,
                                      size_t count, bool remote, const atomic_bool *cancel, MemoryDocumentSet *out);
/* Borrowed-connection variants never refresh/close db or call store_mode /
 * store_db. Caller owns db's lifetime and SQL cancellation guard throughout.
 * deadline_ms is absolute milliseconds on the deadline clock (core/deadline_clock.h) (0: no deadline).
 * Missing db fails locally; remote returns empty without inspecting db/cache.
 * Foreground timeout/cancellation retains proofs, which still require fresh
 * SQL and descriptor/name/stat checks before any subsequent admission. */
bool memory_documents_capture_cached_on_db(MemoryDocumentCache *cache, Db *db, StoreManager *store,
                                            const char *project_dir, const StoreProjectIdentity *project,
                                            const MemoryDocumentName *names, size_t count, bool remote,
                                            const atomic_bool *cancel, double deadline_ms, MemoryDocumentSet *out);
/* Resolve current SQL sources and advance <=128 KiB total (16 KiB reads),
 * <=8 file lookups. No fd persists. pending means warming, NOT authorized;
 * progress is useful work and should schedule another bounded worker step.
 * Failed resident keys have at most three filesystem/hash failures, with
 * monotonic 1/2/4-second backoff; terminal keys remain pending, not authorized.
 * A new SQL/source key or explicit invalidate permits retry. Cancellation
 * discards partial work without consuming failures. No OS I/O deadline. */
bool memory_documents_verify_step(MemoryDocumentCache *cache, StoreManager *store, const char *project_dir,
                                   const StoreProjectIdentity *project, const MemoryDocumentName *names,
                                   size_t count, bool remote, const atomic_bool *cancel,
                                   bool *progress, bool *pending);
/* Same bounded worker step using only the caller's borrowed Db. The legacy
 * verify_step wrapper above uses ordinary routing and must not nest in a Db
 * borrow. Caller supplies the SQL cancellation guard as for capture_on_db. */
bool memory_documents_verify_step_on_db(MemoryDocumentCache *cache, Db *db, StoreManager *store,
                                         const char *project_dir, const StoreProjectIdentity *project,
                                         const MemoryDocumentName *names, size_t count, bool remote,
                                         const atomic_bool *cancel, double deadline_ms, bool *progress, bool *pending);

/* Never truncate an attachment title into a different filesystem name. */
bool memory_document_name_copy(const char *title, bool dirty, MemoryDocumentName *out);

/* Worker-only: SQL receipt plus actual local-file hash/path identity. A bad,
 * missing, dirty or unsaved attachment is omitted, never silently authorized.
 * remote=true returns empty before reading names/store/filesystem. */
bool memory_documents_capture(StoreManager *store, const char *project_dir,
                               const StoreProjectIdentity *project, const MemoryDocumentName *names,
                               size_t count, bool remote, const atomic_bool *cancel, MemoryDocumentSet *out);
bool memory_documents_same(const MemoryDocumentSet *a, const MemoryDocumentSet *b);
bool memory_documents_admit(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents);

/* Bounded durable scope-local revision ingestion. Seed only explicit captured
 * revisions. Detached receipts may be retained but are not admitted/continued
 * through another attachment set. Tokenizer callbacks run outside transactions. */
bool memory_documents_seed(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents);
bool memory_documents_complete(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents, bool *complete);
bool memory_document_ingest_step(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                  const char *owner, MemoryTokenCount count, void *context,
                                  const char *tokenizer, bool *progress, char *why, size_t whylen);
/* Select/hydrate the bounded union of eligible chats and these exact revisions.
 * No document set means chat-only. Admission never trusts Qdrant payload text. */
bool memory_lexical_candidates_with_documents(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                               const char *query, size_t limit, MemoryCandidate *out, size_t *count);
bool memory_lexical_candidates_with_documents_before(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                               const char *query, int64_t before, size_t limit, MemoryCandidate *out, size_t *count);
bool memory_candidate_hydrate_with_documents(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                             const MemoryCandidate *candidate, MemoryUnit *out, bool *found);

#endif
