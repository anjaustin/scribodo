#ifndef SCRIBODO_LLM_MEMORY_BRIEF_H
#define SCRIBODO_LLM_MEMORY_BRIEF_H

#include "app/memory_brief_store.h"
#include "generative_gate.h"
#include "memory_tokens.h"

typedef struct {
    char json[MEMORY_BRIEF_JSON_BYTES + 1];
    size_t tokens;
    char model_fingerprint[65];
    uint64_t gate_fence;
    bool canceled; /* refund claim on refusal/preemption/external cancellation;
                     * a timeout/model/decoding failure is a genuine error */
    bool usage_known;
    size_t prompt_tokens, completion_tokens, tokenizer_http_calls;
    double generation_ms;
} MemoryBriefResult;

/* Structural/provenance validation, NOT a factuality/quality guarantee. */
bool memory_brief_validate_json(const char *json, size_t source_count);
bool memory_brief_model_fingerprint(MemoryTokenizer *tokenizer, char out[65]);
/* Only the pinned local Unix-socket model is allowed. NULL server explicitly
 * selects the deterministic headless fixture with a distinct model identity.
 * Caller rechecks gate fence + cancellation via publish admission callback.
 * A failed result retains gate_fence/canceled, but no JSON or model output.
 * No SQL, remote provider, fallback, or source deletion occurs here. */
bool memory_brief_generate(LlmServer *server, GenerativeGate *gate,
                           const MemoryBriefLease *lease, const atomic_bool *external_cancel,
                           MemoryBriefResult *out);

#endif
