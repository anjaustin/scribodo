#ifndef SCRIBODO_ENTITY_BACKFILL_JOB_H
#define SCRIBODO_ENTITY_BACKFILL_JOB_H

#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include "app/store.h"

typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    char *project_dir;
    bool succeeded;
    StoreManager *store;
} EntityBackfillJob;

/* `store` is required and borrowed until entity_backfill_job_destroy(). */
bool entity_backfill_job_init(EntityBackfillJob *job, StoreManager *store);
bool entity_backfill_job_start(EntityBackfillJob *job, const char *project_dir);
bool entity_backfill_job_poll(EntityBackfillJob *job, bool *succeeded);
bool entity_backfill_job_busy(const EntityBackfillJob *job);
void entity_backfill_job_destroy(EntityBackfillJob *job);

#endif
