#include "name_detection_job.h"

#include <stdlib.h>
#include <string.h>

static void clear_inputs(NameDetectionJob *job)
{
    free(job->assistant_prev);
    free(job->author_msg);
    job->assistant_prev = NULL;
    job->author_msg = NULL;
}

static void *name_detection_main(void *arg)
{
    NameDetectionJob *job = arg;
    job->succeeded = names_detect_cancel(job->server, job->assistant_prev, job->author_msg, &job->result, &job->cancel);
    atomic_store(&job->state, 2);
    return NULL;
}

bool name_detection_job_init(NameDetectionJob *job, LlmServer *server)
{
    memset(job, 0, sizeof(*job));
    atomic_init(&job->state, 0);
    atomic_init(&job->cancel, false);
    job->server = server;
    return pthread_mutex_init(&job->mu, NULL) == 0;
}

void name_detection_job_set_server(NameDetectionJob *job, LlmServer *server)
{
    pthread_mutex_lock(&job->mu);
    if (atomic_load(&job->state) == 0) job->server = server;
    pthread_mutex_unlock(&job->mu);
}

bool name_detection_job_start(NameDetectionJob *job, const char *assistant_prev, const char *author_msg)
{
    if (!job || !author_msg || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 0) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    job->author_msg = strdup(author_msg);
    job->assistant_prev = assistant_prev ? strdup(assistant_prev) : NULL;
    if (!job->author_msg || (assistant_prev && !job->assistant_prev)) {
        clear_inputs(job);
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    memset(&job->result, 0, sizeof(job->result));
    job->succeeded = false;
    atomic_store(&job->cancel, false);
    atomic_store(&job->state, 1);
    if (pthread_create(&job->thread, NULL, name_detection_main, job) != 0) {
        atomic_store(&job->state, 0);
        clear_inputs(job);
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool name_detection_job_poll(NameDetectionJob *job, NameGuess *result, bool *succeeded)
{
    if (!job || pthread_mutex_lock(&job->mu) != 0) return false;
    if (atomic_load(&job->state) != 2) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    pthread_join(job->thread, NULL);
    if (result) *result = job->result;
    if (succeeded) *succeeded = job->succeeded;
    clear_inputs(job);
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool name_detection_job_busy(const NameDetectionJob *job)
{
    return job && atomic_load(&job->state) != 0;
}

void name_detection_job_destroy(NameDetectionJob *job)
{
    if (!job) return;
    atomic_store(&job->cancel, true);
    pthread_mutex_lock(&job->mu);
    atomic_store(&job->cancel, true);
    if (atomic_load(&job->state) != 0) pthread_join(job->thread, NULL);
    clear_inputs(job);
    atomic_store(&job->state, 0);
    pthread_mutex_unlock(&job->mu);
    pthread_mutex_destroy(&job->mu);
}
