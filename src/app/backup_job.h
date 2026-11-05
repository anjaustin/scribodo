/* One asynchronous in-app project backup.
 *
 * The job owns every value touched by its worker. UI policy and presentation
 * remain with the caller; no worker receives or retains application state.
 */
#ifndef SCRIBODO_BACKUP_JOB_H
#define SCRIBODO_BACKUP_JOB_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    pthread_t thread;
    pthread_mutex_t mu; /* serializes start and result delivery */
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    char tools_dir[PATH_MAX];
    char project_dir[PATH_MAX];
    char dest[PATH_MAX + 64];
    char why[400];
    bool ok;
} BackupJob;

bool backup_job_init(BackupJob *job);
bool backup_job_start(BackupJob *job, const char *tools_dir, const char *project_dir);
bool backup_job_busy(const BackupJob *job);
/* Deliver a completed result exactly once, after joining its worker. */
bool backup_job_poll(BackupJob *job, bool *ok, char *dest, size_t destlen, char *why, size_t whylen);
/* A running backup is allowed to finish and is then joined. Destruction ends
 * the object's lifetime: the caller must have exclusive ownership and ensure
 * that no thread is entering or executing any BackupJob API for this object. */
bool backup_job_destroy(BackupJob *job);

#endif
