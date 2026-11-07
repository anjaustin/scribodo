#ifndef SCRIBODO_MEMORY_BRIEF_STORE_H
#define SCRIBODO_MEMORY_BRIEF_STORE_H

#include "memory_store.h"

#define MEMORY_BRIEF_INPUTS 4u
#define MEMORY_BRIEF_JSON_BYTES 1536u
#define MEMORY_BRIEF_SUMMARY_TOKENS 256u
#define MEMORY_BRIEF_PROTOCOL "chat-brief-v1/strict-facts-v1/neutralize-zwnj-v1"

typedef struct {
    MemoryScope scope;
    char owner[64], input_digest[65];
    uint64_t fence;
    int64_t previous_sequence;
    uint64_t previous_byte_end;
    size_t count;
    MemoryUnit inputs[MEMORY_BRIEF_INPUTS]; /* owned originals, never summaries */
} MemoryBriefLease;

typedef struct {
    char id[65], json[MEMORY_BRIEF_JSON_BYTES + 1];
    char model_fingerprint[65];
    char summary_hash[65], created_at[32]; /* immutable SQL snapshot metadata */
    size_t tokens, count;
    MemoryUnit inputs[MEMORY_BRIEF_INPUTS]; /* original SQL provenance */
} MemoryBrief;

/* Pure, nonblocking admission check; invoked again inside publication TX.
 * Parent must also enforce SCRIBODO_MEMORY_BRIEFS=1 and remote bypass. */
typedef bool (*MemoryBriefAdmission)(void *context);

void memory_brief_lease_free(MemoryBriefLease *lease);
void memory_brief_free(MemoryBrief *brief);
bool memory_brief_claim(Db *db, const MemoryScope *scope, const char *owner,
                        MemoryBriefLease *out, bool *claimed);
bool memory_brief_publish(Db *db, const MemoryBriefLease *lease, const char *json,
                          size_t measured_tokens, const char *model_fingerprint,
                          const char *prompt_schema, MemoryBriefAdmission admitted,
                          void *context, bool *published);
/* canceled refunds this admitted attempt; genuine errors consume at most 3.
 * retryable=false exhausts the current frontier. No failure advances it. */
bool memory_brief_fail(Db *db, const MemoryBriefLease *lease, bool retryable, bool canceled);
bool memory_brief_recall(Db *db, const MemoryScope *scope, int64_t older_than,
                         const char *expected_model_fingerprint,
                         MemoryBrief out[MEMORY_BRIEF_INPUTS], size_t *count);
/* Direct-ID dispatch check, never a latest-four search. Revalidates exact
 * summary/model/protocol and EVERY original against current SQL eligibility.
 * Owns its short transaction; no token/model callbacks run while it is held. */
bool memory_brief_admit(Db *db, const MemoryScope *scope, const MemoryBrief *brief);

#endif
