/* Retrieval-only local memory: bounded recall and SQL-only source hydration.
 * Build on a worker; the caller revalidates scope at dispatch admission. */
#ifndef SCRIBODO_CONTEXT_PLANNER_H
#define SCRIBODO_CONTEXT_PLANNER_H

#include <stdatomic.h>
#include "app/memory_store.h"
#include "app/memory_documents.h"
#include "llm/semantic.h"

#define MEMORY_PLAN_BYTES (16u * 1024u)
#define MEMORY_PLAN_TOKENS 4096u
#define MEMORY_BASE_REQUEST_BYTES (2u * 1024u * 1024u)

typedef enum {
    MEMORY_DISABLED, MEMORY_WARMING, MEMORY_PARTIAL, MEMORY_READY, MEMORY_UNAVAILABLE
} MemoryReadiness;

typedef struct {
    MemoryScope scope;
    MemoryDocumentSet documents; /* owned immutable refs only, never source bodies */
    MemoryUnit units[MEMORY_PLAN_UNITS];
    size_t count, text_bytes, tokens, candidates;
    bool lexical_ok, semantic_ok, limited;
    MemoryReadiness readiness;
    char *reference; /* owned, neutralized reference data, never instructions/canon */
} MemoryPlan;

/* Owned adapter seam also used to fault/interleave-test the finite protocol.
 * Candidate callbacks MUST respect limit; count overflow fails closed. */
typedef struct {
    void *context;
    bool (*scope)(void *, const char *, const char *, MemoryScope *);
    bool (*lexical)(void *, const MemoryScope *, const char *, size_t, MemoryCandidate *, size_t *);
    bool (*semantic)(void *, const MemoryScope *, const char *, size_t, MemoryCandidate *, size_t *);
    bool (*hydrate)(void *, const MemoryScope *, const MemoryCandidate *, MemoryUnit *, bool *);
    MemoryTokenCount count;
    const char *tokenizer; /* expected current generation's source-count protocol */
    const MemoryDocumentSet *documents;
    bool (*documents_current)(void *, const MemoryScope *, const MemoryDocumentSet *);
} MemoryRecallOps;

void memory_plan_free(MemoryPlan *plan);
bool memory_scope_same(const MemoryScope *a, const MemoryScope *b);
/* remote=true returns disabled BEFORE invoking any callback. Older-than excludes
 * the protected recent tail (use INT64_MAX if it contains no persisted row).
 * No caller transaction/tokenizer fallback/full-history scan is permitted. */
bool memory_plan_recall(const MemoryRecallOps *ops, const char *project, const char *conversation,
                         const char *query, int64_t older_than, bool remote, const atomic_bool *cancel,
                         MemoryPlan *out);
bool memory_plan_build(Db *db, SemanticClient *vectors, const char *project, const char *conversation,
                        const char *query, int64_t older_than, bool remote, MemoryTokenCount count,
                        void *count_context, const char *tokenizer, const atomic_bool *cancel, MemoryPlan *out);
bool memory_plan_build_with_documents(Db *db, SemanticClient *vectors, const char *project, const char *conversation,
                        const char *query, int64_t older_than, bool remote, MemoryTokenCount count,
                        void *count_context, const char *tokenizer, const atomic_bool *cancel, MemoryPlan *out,
                        const MemoryDocumentSet *documents);
/* Recheck immediately before dispatch. An immutable plan describes its admission
 * point, not permanent current eligibility. No network/model work in this call. */
bool memory_plan_admit(Db *db, const MemoryPlan *plan, bool remote);
bool memory_plan_admit_with_documents(Db *db, const MemoryPlan *plan, const MemoryDocumentSet *documents, bool remote);
/* Count the COMPLETE templated request, including role/generation framing.
 * Unknown counts fail closed; no estimate is accepted for memory admission. */
typedef long (*MemoryRequestTokens)(const char *request_json, void *context);
/* Prefix optional references to the newest author turn without changing that
 * turn's original bytes or explicit documents. Drops whole optional units until
 * the exact final request fits. On remote selection, only the untouched base
 * request is copied and the plan is cleared BEFORE counting/reference access.
 * The caller must re-admit the resulting plan after counting, before dispatch. */
bool memory_plan_apply(MemoryPlan *plan, const char *base_request, bool remote, size_t prompt_budget,
                        MemoryRequestTokens count, void *context, char **out);

#endif
