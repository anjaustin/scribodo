/* Will the local model fit in memory? (user request 2026-11-04)
 *
 * Before the chat model starts (and before it restarts with a new context
 * size), its memory need is estimated and compared with the memory macOS
 * can hand out right now. If the author's context doesn't fit, the model
 * starts with the largest context that does, and the author is told;
 * their saved setting is kept, so a later retry (Apply in Model settings)
 * goes back up when memory allows.
 *
 * The estimate: the model file (memory-mapped weights) plus measured
 * per-token and fixed costs for the KV cache and compute buffers. See
 * llm_server_mem_need() and docs/MODEL-001 "Memory". */
#ifndef SCRIBODO_MEMFIT_H
#define SCRIBODO_MEMFIT_H

#include <stdbool.h>

#include "llm/server.h"

/* Memory that can be handed out now: all but wired, compressed and apps'
 * own memory (cached files count as available). $SCRIBODO_MEM_AVAILABLE_MB overrides it (tests). */
long long sysmem_available(void);
long long sysmem_total(void);

/* Kept free for the app itself (about 0.3 GB) and its growth; macOS's
 * own memory is already counted as used. */
#define MEMFIT_HEADROOM (512LL * 1024 * 1024)

typedef struct {
    int slot_ctx;        /* the context to start with */
    bool reduced;        /* smaller than wanted */
    bool short_even_min; /* not even the smallest fits (started anyway) */
    long long need;      /* bytes at slot_ctx */
    long long need_wanted; /* bytes at the wanted context */
    long long avail;     /* bytes free (as passed in) */
    long long room;      /* of that, what the model can have: less the other server and the headroom */
} MemFit;

/* The largest context per slot, from `wanted` down to `min` in `step`s,
 * for which the model plus `other` (another server about to start) fits
 * in `avail` minus the headroom. */
MemFit memfit_context(long long avail, long long other, const LlmServerConfig *cfg, int wanted, int min, int step);

/* "5.4 GB" */
void memfit_gb(long long bytes, char *out, int outlen);

#endif
