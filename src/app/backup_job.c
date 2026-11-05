#include "backup_job.h"

#include <stdio.h>
#include <string.h>

#include "app/backup.h"
#include "app/store.h"

static void *backup_job_main(void *arg)
{
    BackupJob *job = arg;
    job->ok = backup_project(job->tools_dir, job->project_dir, job->dest, sizeof(job->dest), job->why, sizeof(job->why));
    store_thread_done(); /* the worker owns any store connection it opened */
    atomic_store(&job->state, 2); /* publish the completed result */
    return NULL;
}

bool backup_job_init(BackupJob *job)
{
    memset(job, 0, sizeof(*job));
    atomic_init(&job->state, 0);
    return pthread_mutex_init(&job->mu, NULL) == 0;
}

bool backup_job_start(BackupJob *job, const char *tools_dir, const char *project_dir)
{
    if (!tools_dir || !project_dir || strlen(tools_dir) >= sizeof(job->tools_dir) ||
        strlen(project_dir) >= sizeof(job->project_dir) || pthread_mutex_lock(&job->mu) != 0)
        return false;
    if (atomic_load(&job->state) != 0) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    memcpy(job->tools_dir, tools_dir, strlen(tools_dir) + 1);
    memcpy(job->project_dir, project_dir, strlen(project_dir) + 1);
    job->dest[0] = '\0';
    job->why[0] = '\0';
    job->ok = false;
    atomic_store(&job->state, 1);
    bool ok = pthread_create(&job->thread, NULL, backup_job_main, job) == 0;
    if (!ok) atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return ok;
}

bool backup_job_busy(const BackupJob *job) { return atomic_load(&job->state) != 0; }

bool backup_job_poll(BackupJob *job, bool *ok, char *dest, size_t destlen, char *why, size_t whylen)
{
    if (pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 2) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    if (pthread_join(job->thread, NULL) != 0) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    if (ok) *ok = job->ok;
    if (dest && destlen) snprintf(dest, destlen, "%s", job->dest);
    if (why && whylen) snprintf(why, whylen, "%s", job->why);
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool backup_job_destroy(BackupJob *job)
{
    if (pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 0 && pthread_join(job->thread, NULL) != 0) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return pthread_mutex_destroy(&job->mu) == 0;
}
