#include "entity_backfill_job.h"

#include <stdlib.h>
#include <string.h>

#include "app/project_store.h"
#include "app/store.h"

static void *entity_backfill_main(void *arg)
{
    EntityBackfillJob *job = arg;
    job->succeeded = project_store_entities_backfill(job->project_dir);
    store_thread_done();
    atomic_store(&job->state, 2);
    return NULL;
}

bool entity_backfill_job_init(EntityBackfillJob *job)
{
    memset(job, 0, sizeof(*job));
    atomic_init(&job->state, 0);
    return pthread_mutex_init(&job->mu, NULL) == 0;
}

bool entity_backfill_job_start(EntityBackfillJob *job, const char *project_dir)
{
    if (!job || !project_dir || !project_dir[0] || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 0) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    job->project_dir = strdup(project_dir);
    if (!job->project_dir) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    job->succeeded = false;
    atomic_store(&job->state, 1);
    if (pthread_create(&job->thread, NULL, entity_backfill_main, job) != 0) {
        atomic_store(&job->state, 0);
        free(job->project_dir);
        job->project_dir = NULL;
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool entity_backfill_job_poll(EntityBackfillJob *job, bool *succeeded)
{
    if (!job || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 2) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_join(job->thread, NULL);
    if (succeeded) *succeeded = job->succeeded;
    free(job->project_dir);
    job->project_dir = NULL;
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool entity_backfill_job_busy(const EntityBackfillJob *job)
{
    return job && atomic_load(&job->state) != 0;
}

void entity_backfill_job_destroy(EntityBackfillJob *job)
{
    if (!job) return;
    pthread_mutex_lock(&job->mu);
    if (atomic_load(&job->state) != 0) pthread_join(job->thread, NULL);
    free(job->project_dir);
    job->project_dir = NULL;
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    pthread_mutex_destroy(&job->mu);
}
