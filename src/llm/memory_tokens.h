#ifndef SCRIBODO_MEMORY_TOKENS_H
#define SCRIBODO_MEMORY_TOKENS_H

#include <stdatomic.h>
#include "server.h"

#define MEMORY_TOKENS_MAX_SOURCE_BYTES 2000u
#define MEMORY_TOKENS_MAX_REQUEST_BYTES (2u * 1024u * 1024u)

typedef struct {
    LlmServer *server;
    const atomic_bool *cancel;
    int timeout_ms; /* total operation deadline; <= 0 uses 10000 ms */
    double deadline_ms; /* optional enclosing absolute CLOCK_MONOTONIC deadline */
    size_t *http_calls; /* optional owner-thread diagnostic counter */
} MemoryTokenizer;

/* Callback context is a non-NULL MemoryTokenizer. Failures return -1.
 * Real counting uses only the pinned local chat server's Unix socket.
 * NULL server explicitly selects the deterministic test-only byte counter:
 * neutralized source bytes, or complete request JSON bytes (not real tokens). */
long memory_source_tokens(const char *source, void *tokenizer);
long memory_reference_tokens(const char *reference, void *tokenizer);
long memory_request_tokens(const char *complete_json, void *tokenizer);

/* SHA-256 identity includes model, neutralization and counting protocol.
 * Unknown real model identities are rejected; fake counts have their own ID. */
bool memory_tokenizer_fingerprint(MemoryTokenizer *tokenizer, char out[65]);

#endif
