/* Owned background recall: one query at a time, off the frame thread. The
 * borrowed StoreManager must outlive destroy; inputs are copied and the
 * result is handed over exactly once by poll. */
#ifndef SCRIBODO_RECALL_JOB_H
#define SCRIBODO_RECALL_JOB_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "app/recall.h"

typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    StoreManager *store;
    char *project_dir, *query;
    RecallResult result;
    bool ok;
} RecallJob;

bool recall_job_init(RecallJob *job, StoreManager *store);
/* False while a run is in flight or awaiting poll, or when inputs are invalid. */
bool recall_job_start(RecallJob *job, const char *project_dir, const char *query);
/* True once per run: moves the result to the caller (who frees it) and the
 * run's success flag, then returns to idle. */
bool recall_job_poll(RecallJob *job, RecallResult *out, bool *ok);
bool recall_job_busy(const RecallJob *job);
void recall_job_destroy(RecallJob *job);

#endif
