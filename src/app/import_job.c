#include "import_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/store_backends.h"

static void *import_main(void *arg)
{
    ImportJob *job = arg;
    job->succeeded = store_import(job->store, job->project_dir, job->why, sizeof(job->why));
    store_thread_done(job->store);
    atomic_store(&job->state, 2);
    return NULL;
}

bool import_job_init(ImportJob *job, StoreManager *store)
{
    if (!job || !store) return false;
    memset(job, 0, sizeof(*job));
    job->store = store;
    atomic_init(&job->state, 0);
    return pthread_mutex_init(&job->mu, NULL) == 0;
}

bool import_job_start(ImportJob *job, const char *project_dir)
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
    job->why[0] = '\0';
    atomic_store(&job->state, 1);
    if (pthread_create(&job->thread, NULL, import_main, job) != 0) {
        atomic_store(&job->state, 0);
        free(job->project_dir);
        job->project_dir = NULL;
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool import_job_poll(ImportJob *job, char *project_dir, size_t dirlen, bool *succeeded, char *why, size_t whylen)
{
    if (!job || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 2) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_join(job->thread, NULL);
    if (project_dir && dirlen) snprintf(project_dir, dirlen, "%s", job->project_dir ? job->project_dir : "");
    if (succeeded) *succeeded = job->succeeded;
    if (why && whylen) snprintf(why, whylen, "%s", job->why);
    free(job->project_dir);
    job->project_dir = NULL;
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool import_job_busy(const ImportJob *job)
{
    return job && atomic_load(&job->state) != 0;
}

bool import_job_running_for(ImportJob *job, const char *project_dir)
{
    if (!job || !project_dir || pthread_mutex_lock(&job->mu) != 0) return false;
    bool running = atomic_load(&job->state) != 0 && job->project_dir && !strcmp(job->project_dir, project_dir);
    pthread_mutex_unlock(&job->mu);
    return running;
}

void import_job_destroy(ImportJob *job)
{
    if (!job) return;
    if (atomic_load(&job->state) != 0) pthread_join(job->thread, NULL);
    free(job->project_dir);
    job->project_dir = NULL;
    pthread_mutex_destroy(&job->mu);
    atomic_store(&job->state, 0);
}
