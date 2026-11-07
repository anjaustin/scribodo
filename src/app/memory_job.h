#ifndef SCRIBODO_MEMORY_JOB_H
#define SCRIBODO_MEMORY_JOB_H

#include "app/store.h"
#include "app/memory_store.h"
#include "llm/memory_vectors.h"
#include "llm/memory_tokens.h"

typedef struct MemoryJob MemoryJob;

typedef enum MemoryJobAction {
    MEMORY_JOB_RESUME,
    MEMORY_JOB_ENABLE,
    MEMORY_JOB_DISABLE,
    MEMORY_JOB_REBUILD
} MemoryJobAction;

typedef struct {
    bool active, enabled, controls_pending;
    MemoryScope scope;
    size_t ingested_steps, indexed;
    char why[256];
} MemoryJobStatus;

/* Borrowed dependencies must outlive destroy, which joins the sole worker.
 * Selection copies its inputs and captures only local project identity. */
MemoryJob *memory_job_create(StoreManager *manager, SemanticClient *vectors, LlmServer *tokenizer);
/* Destroy joins in-flight I/O, but discards any uncommitted RAM controls. The
 * borrower must guard close with controls_pending, or obtain explicit discard
 * consent first. It does not wait indefinitely for offline SQL to recover. */
void memory_job_destroy(MemoryJob *job);
/* RESUME replaces navigation only. Other actions enqueue FIFO consent work
 * AND select navigation; true means accepted, not yet committed. Check the
 * global controls_pending snapshot before sending or closing. */
bool memory_job_select(MemoryJob *job, const char *project_dir, const char *conversation,
                       MemoryJobAction action);
/* Clears navigation/status only, never accepted consent controls. */
void memory_job_clear(MemoryJob *job);
/* Cooperatively yields resource steps, not SQL consent controls; cannot
 * interrupt blocking I/O. Unchanged values do not wake the worker. */
void memory_job_pause(MemoryJob *job, bool paused);
/* Snapshot, not a consuming poll. False only for invalid arguments. */
bool memory_job_status(MemoryJob *job, MemoryJobStatus *out);

/* Capacity includes the in-flight head. Full queues reject controls without
 * changing navigation or previously accepted intent. RESUME is latest-only. */
#define MEMORY_JOB_CONTROL_CAPACITY 8u

/* Deterministic lifecycle seam. Called on the worker after identity checks.
 * RESUME: return progress (10 ms pacing), false means idle (1 second poll).
 * Other actions: execute ONLY the SQL control; true means committed, false
 * keeps the FIFO head for retry after 1 second. These run even while paused.
 * first identifies a navigation's first iteration, or a control's first try. */
typedef bool (*MemoryJobRunner)(StoreManager *manager, const char *project_dir,
                                const char *conversation, MemoryJobAction action,
                                bool first, MemoryJobStatus *status, void *context);
MemoryJob *memory_job_create_with_runner(StoreManager *manager, MemoryJobRunner runner, void *context);

#endif
