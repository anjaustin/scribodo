#ifndef SCRIBODO_NAME_DETECTION_JOB_H
#define SCRIBODO_NAME_DETECTION_JOB_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "llm/names.h"

/* The borrowed server must outlive destroy; message inputs are copied. */
typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    atomic_bool cancel;
    LlmServer *server;
    char *assistant_prev;
    char *author_msg;
    NameGuess result;
    bool succeeded;
} NameDetectionJob;

bool name_detection_job_init(NameDetectionJob *job, LlmServer *server);
void name_detection_job_set_server(NameDetectionJob *job, LlmServer *server);
bool name_detection_job_start(NameDetectionJob *job, const char *assistant_prev, const char *author_msg);
bool name_detection_job_poll(NameDetectionJob *job, NameGuess *result, bool *succeeded);
bool name_detection_job_busy(const NameDetectionJob *job);
void name_detection_job_destroy(NameDetectionJob *job);

#endif
