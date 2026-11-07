/* Current explicit saved attachment identities, never historical filename links. */
#ifndef SCRIBODO_MEMORY_DOCUMENTS_H
#define SCRIBODO_MEMORY_DOCUMENTS_H

#include "app/memory_store.h"
#include "app/store.h"
#include <stdatomic.h>

#define MEMORY_DOCUMENTS_MAX 8u

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
bool memory_candidate_hydrate_with_documents(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                             const MemoryCandidate *candidate, MemoryUnit *out, bool *found);

#endif
