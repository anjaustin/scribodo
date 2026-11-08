/* Experimental PARTIAL local conversation memory (no generated briefs).
 * SQL owns eligibility and source text;
 * vectors are only untrusted locators. No function falls back to file history. */
#ifndef SCRIBODO_MEMORY_STORE_H
#define SCRIBODO_MEMORY_STORE_H

#include "app/db.h"
#include "llm/chat.h"

#define MEMORY_UNIT_BYTES 2000u /* never silently clipped by the pinned embedder */
#define MEMORY_UNIT_TOKENS 512u
#define MEMORY_CANDIDATES 32u
#define MEMORY_PLAN_UNITS 8u
/* Lexical candidate score: InnoDB's natural-language MATCH score divided by
 * CHAR_LENGTH(text) to this power. The raw score sums term frequency without
 * length normalization, so a long paragraph that mentions the query's common
 * words several times outranks the short author turn that states the fact
 * once; on the frozen reach fixtures the production query reached none of
 * the stated, superseded or rejected sources (MEMORY-018). Measured over
 * exponents 0 (raw), log, 0.5, 0.6, 0.75, 0.9 and 1.0 on both fixtures,
 * 0.75 reaches every lexically reachable source with the best worst-case
 * rank (4 and 7 of 16). It is a string because it lives inside SQL. */
#define MEMORY_LEXICAL_LENGTH_NORM "0.75"

typedef struct {
    char id[40], project_id[64], conversation_id[40];
    uint64_t history_epoch, policy_epoch, generation;
    int64_t ingested_sequence;
    bool enabled;
    char database[64]; /* owned originating SQL namespace; never manager-borrowed */
    int64_t pending_sequence; /* durable append/backfill high-water, not completion */
    bool index_pending; /* active-generation pending/leased/failed outbox exists */
    char tokenizer[192]; /* generation-pinned fingerprint; empty until first claim */
} MemoryScope;

typedef enum { MEMORY_SOURCE_CHAT = 0, MEMORY_SOURCE_DOCUMENT = 1 } MemorySourceKind;

typedef struct {
    char id[65], text_hash[65], source_hash[65];
    MemorySourceKind kind;
    char document_id[40], revision_id[40], document_name[256];
    int64_t message_id, sequence;
    uint64_t byte_start;
    size_t text_bytes, tokens;
    ChatRole role;
    char *text; /* owned, canonical SQL text, <= MEMORY_UNIT_BYTES */
} MemoryUnit;

typedef struct {
    char unit_id[65], text_hash[65];
    uint64_t history_epoch, generation;
    double score;
} MemoryCandidate;

typedef struct {
    uint64_t id, fence;
    char owner[64];
    MemoryScope scope;
    MemoryUnit unit;
    bool maintenance; /* committed stale/terminal retirement; may be true without a lease */
} MemoryIndexLease;

typedef long (*MemoryTokenCount)(const char *text, void *context);

void memory_unit_free(MemoryUnit *unit);
void memory_index_lease_free(MemoryIndexLease *lease);
/* Absent scope: false and a zeroed/disabled output (explicit opt-in required). */
bool memory_scope_get(Db *db, const char *project, const char *conversation, MemoryScope *out);
/* Explicit consent only. Enabling seeds bounded backfill intent, not a full
 * archive read. Policy changes fence old work. Must not join caller transactions. */
bool memory_scope_set(Db *db, const char *project, const char *conversation, bool enabled, MemoryScope *out);
/* Called INSIDE the author's message transaction; durable intent or rollback. */
bool memory_message_intent(Db *db, const char *project, const char *conversation, int64_t sequence);
/* One bounded, leased ingestion step. Tokenizer I/O is outside transactions.
 * Unknown/failed counts fail closed. progress=false may mean caught up/leased. */
bool memory_ingest_step(Db *db, const char *scope, const char *owner, MemoryTokenCount count,
                        void *context, const char *tokenizer, bool *progress, char *why, size_t whylen);
/* At most 64 jobs are retired/claimed per call. With claimed=false, out owns
 * nothing; maintenance alone reports committed retirement work to the worker. */
bool memory_outbox_claim(Db *db, const char *scope, const char *owner, MemoryIndexLease *out, bool *claimed);
/* Acknowledgment is conditional on the current live lease, fence and scope
 * epochs/generation. A failed delivery backs off; it never advances ingestion. */
bool memory_outbox_ack(Db *db, const MemoryIndexLease *lease, bool delivered);
/* Bump generation and enqueue/reseed bounded rebuild work; never trust marks
 * from a lost collection as proof of completeness. Disabled/absent scopes are
 * acknowledged without enabling consent or changing generation. */
bool memory_scope_rebuild(Db *db, const char *project, const char *conversation, MemoryScope *out);
bool memory_lexical_candidates(Db *db, const MemoryScope *scope, const char *query,
                                size_t limit, MemoryCandidate out[MEMORY_CANDIDATES], size_t *count);
/* Validates scope consent/epochs, source eligibility/version/hash and locator.
 * Wrong/stale locators return found=false, never Qdrant payload text. */
bool memory_candidate_hydrate(Db *db, const MemoryScope *scope, const MemoryCandidate *candidate,
                              MemoryUnit *out, bool *found);

#endif
