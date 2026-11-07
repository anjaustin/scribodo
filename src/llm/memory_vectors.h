/* Rebuildable, untrusted locator projection. SQL alone owns source text. */
#ifndef SCRIBODO_MEMORY_VECTORS_H
#define SCRIBODO_MEMORY_VECTORS_H

#include "llm/semantic.h"
#include "app/memory_store.h"
#include "app/memory_documents.h"
#include "app/memory_gc.h"

/* 64 lowercase hex characters plus NUL; includes model and embedding protocol.
 * NULL embedding servers use a distinct deterministic-test namespace. */
bool memory_vectors_fingerprint(SemanticClient *client, char out[65]);
/* Every operation requires scope.database (the originating SQL connection's
 * database) to match the current manager at admission. Endpoints are pinned to
 * that immutable scope database across network I/O and concurrent switches.
 * Missing is success with *exists=false; network/schema errors are failures.
 * Never creates: SQL must bump generation/reseed BEFORE replacing a lost index. */
bool memory_vectors_exists(SemanticClient *client, const MemoryScope *scope, bool *exists, char *why, size_t whylen);
bool memory_vectors_create(SemanticClient *client, const MemoryScope *scope, char *why, size_t whylen);
bool memory_vectors_create_on_db(SemanticClient *client, Db *db, const MemoryScope *scope, char *why, size_t whylen);
bool memory_vectors_upsert(SemanticClient *client, const MemoryIndexLease *lease, char *why, size_t whylen);
/* limit must be 1..MEMORY_CANDIDATES. All-or-nothing: any malformed/wrong-scope locator fails with count=0.
 * Conflicting hashes for one unit reject the whole response; identical repeats
 * are deduplicated, retaining the first rank/score. Extra payload fields
 * (including injected text) are ignored, never hydrated. */
bool memory_vectors_search(SemanticClient *client, const MemoryScope *scope, const char *query, size_t limit,
                           MemoryCandidate out[MEMORY_CANDIDATES], size_t *count, char *why, size_t whylen);
bool memory_vectors_search_with_documents(SemanticClient *client, const MemoryScope *scope,
                           const MemoryDocumentSet *documents, const char *query, size_t limit,
                           MemoryCandidate out[MEMORY_CANDIDATES], size_t *count, char *why, size_t whylen);
/* Deletes only this exact scope/epochs/generation, NOT other conversations
 * sharing the project collection. Missing collections are already clean. */
bool memory_vectors_drop(SemanticClient *client, const MemoryScope *scope, char *why, size_t whylen);
/* Deletes exactly the durably registered retired collection (v1 or v2), not a
 * payload-discovered/legacy namespace. Fresh SQL guard commits a monotonic
 * retirement floor BEFORE HTTP; no SQL transaction/lock crosses network I/O.
 * Never acknowledges the lease: worker must ack on its originating Db. */
bool memory_vectors_retire_namespace(SemanticClient *, const MemoryNamespaceLease *, char *why, size_t whylen);
/* Borrowed connection variants never refresh/close a caller's TLS connection. */
bool memory_vectors_retire_namespace_on_db(SemanticClient *, Db *, const MemoryNamespaceLease *, char *why, size_t whylen);

#endif
