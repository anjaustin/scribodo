/* Canon-aware chat (MEMORY-014 P1, experimental): an exact, labelled, bounded
 * block of the author's committed canon entries, supplied to the model as a
 * reference class of its own, separate from retrieved evidence and never
 * ranked with it. The block is host-built from the current committed version
 * of each entry; nothing generated enters it, and the author's current request
 * is never clipped to make room for it. Off unless a session opts in. */
#ifndef SCRIBODO_CANON_REFERENCE_H
#define SCRIBODO_CANON_REFERENCE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/store.h"

#define CANON_REFERENCE_ENTRIES 8u
#define CANON_REFERENCE_BYTES 4096u

/* The committed canon entries whose current wording contains at least one
 * query term, best match first then newest, at most `max_entries` and
 * `max_bytes` of rendered block. Returns an owned block, or NULL when nothing
 * matched or the canon could not be read; *count receives the entries used. */
char *canon_reference_build(StoreManager *store, const char *project_dir, const char *query,
                            size_t max_entries, size_t max_bytes, size_t *count);

/* Prepends the block to the last user message of a chat-completions request
 * (the same place the planner puts retrieved evidence), with a boundary that
 * names what follows. Returns the owned new request, or NULL on failure. */
char *canon_reference_apply(const char *block, const char *request_json);

#endif
