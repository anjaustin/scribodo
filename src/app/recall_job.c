#include "recall_job.h"

#include <stdlib.h>
#include <string.h>

#include "app/store.h"

static void *recall_main(void *arg)
{
    RecallJob *job = arg;
    job->ok = recall_run(job->store, job->project_dir, job->query, &job->result);
    store_thread_done(job->store);
    atomic_store(&job->state, 2);
    return NULL;
}

bool recall_job_init(RecallJob *job, StoreManager *store)
{
    if (!job || !store) return false;
    memset(job, 0, sizeof(*job));
    job->store = store;
    atomic_init(&job->state, 0);
    return pthread_mutex_init(&job->mu, NULL) == 0;
}

bool recall_job_start(RecallJob *job, const char *project_dir, const char *query)
{
    if (!job || !project_dir || !project_dir[0] || !query || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 0) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    job->project_dir = strdup(project_dir);
    job->query = strdup(query);
    if (!job->project_dir || !job->query) {
        free(job->project_dir);
        free(job->query);
        job->project_dir = job->query = NULL;
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    memset(&job->result, 0, sizeof(job->result));
    job->ok = false;
    atomic_store(&job->state, 1);
    if (pthread_create(&job->thread, NULL, recall_main, job) != 0) {
        atomic_store(&job->state, 0);
        free(job->project_dir);
        free(job->query);
        job->project_dir = job->query = NULL;
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool recall_job_poll(RecallJob *job, RecallResult *out, bool *ok)
{
    if (!job || !out || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 2) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_join(job->thread, NULL);
    *out = job->result; /* ownership moves */
    memset(&job->result, 0, sizeof(job->result));
    if (ok) *ok = job->ok;
    free(job->project_dir);
    free(job->query);
    job->project_dir = job->query = NULL;
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool recall_job_busy(const RecallJob *job)
{
    return job && atomic_load(&job->state) != 0;
}

void recall_job_destroy(RecallJob *job)
{
    if (!job) return;
    if (atomic_load(&job->state) != 0) pthread_join(job->thread, NULL);
    recall_result_free(&job->result);
    free(job->project_dir);
    free(job->query);
    job->project_dir = job->query = NULL;
    pthread_mutex_destroy(&job->mu);
    atomic_store(&job->state, 0);
}
