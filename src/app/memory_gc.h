/* Experimental derived-data maintenance only. No originals or provenance
 * snapshots are mutated. Call from the opt-in background worker, not UI paths. */
#ifndef SCRIBODO_MEMORY_GC_H
#define SCRIBODO_MEMORY_GC_H
#include "app/memory_store.h"

typedef struct {
    size_t outbox, receipts, document_receipts, units, total;
    size_t brief_inputs, briefs, brief_states;
    uint64_t current_generation;
} MemoryGcStats;

typedef struct {
    uint64_t id, fence;
    char owner[64];
    MemoryScope scope; /* exact originating database/project and retired generation */
    char embedding_fingerprint[65];
    int namespace_version;
} MemoryNamespaceLease;

/* Own transaction. Snapshot generation/consent ignored: cutoff read FOR UPDATE.
 * limit is 1..64 TOTAL rows across all seven derived tables, including brief
 * inputs (no uncounted cascades). Completed dirty intent is dequeued under the
 * same scope lock/transaction; a later generation bump atomically requeues it. */
bool memory_gc_step(Db *, const MemoryScope *, size_t limit, MemoryGcStats *);
/* Maintain at most ONE queued scope belonging to this exact SQL database and
 * project, regardless of consent/currently selected conversation. Never enables
 * or ingests. Same 1..64 total derived-row budget as memory_gc_step; queue
 * bookkeeping is metadata, not part of that row budget. Own transactions.
 * maintained_scope is fresh after cleanup, or a bounded project-scope fallback
 * when the dirty queue is empty (for independent namespace maintenance).
 * Success with an empty output id means the project has no scopes. */
bool memory_gc_project_step(Db *, const char *database, const char *project, size_t limit,
                            MemoryGcStats *, MemoryScope *maintained_scope);
/* Explicit known previous namespace only. May join caller's pre-bump transaction.
 * The target must equal SQL's current generation or be older at registration;
 * claiming/deletion requires no enabled project scope at/below the target.
 * Preparing deletion commits a project floor that all future enables honor. */
bool memory_gc_namespace_enqueue(Db *, const MemoryScope *old, const char *embedding_fingerprint, int version);
/* Own transactions, DB-clock 30s leases, monotonically fenced, three delivery
 * attempts with backoff. Blocked shared namespaces consume no attempts. */
bool memory_gc_namespace_claim(Db *, const MemoryScope *owner_scope, const char *owner,
                               MemoryNamespaceLease *, bool *claimed);
bool memory_gc_namespace_ack(Db *, const MemoryNamespaceLease *, bool delivered);
/* Internal vector deletion guard. Locks project/scopes, verifies live lease and
 * active writers, raises the monotonic floor, then COMMITS BEFORE returning.
 * No transaction/locks survive into network I/O. */
bool memory_gc_namespace_prepare(Db *, const MemoryNamespaceLease *);
/* Fresh autocommit create admission/postcheck against the exact SQL origin,
 * current enabled scope and retirement floor. retired is independent of scope
 * eligibility: only a proven floor authorizes cleanup of a late creator. */
bool memory_gc_namespace_admit(Db *, const MemoryScope *, bool *allowed, bool *retired);
#endif
