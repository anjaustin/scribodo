/* Owned background import of a legacy project into the store (MEMORY-019
 * phase 1 remediation). store_import() already elects one importer with
 * file locks and makes every other thread see the project as paused while
 * it runs, so it is safe off the frame thread; this job only moves it there.
 * The borrowed StoreManager must outlive destroy. One import at a time. */
#ifndef SCRIBODO_IMPORT_JOB_H
#define SCRIBODO_IMPORT_JOB_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "app/store.h"

typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    StoreManager *store;
    char *project_dir;
    bool succeeded;
    char why[400];
} ImportJob;

bool import_job_init(ImportJob *job, StoreManager *store);
/* False while a run is in flight or awaiting poll. */
bool import_job_start(ImportJob *job, const char *project_dir);
/* True once per run; copies the project folder, success and reason out. */
bool import_job_poll(ImportJob *job, char *project_dir, size_t dirlen, bool *succeeded, char *why, size_t whylen);
bool import_job_busy(const ImportJob *job);
/* True while a run for exactly this folder is in flight or unpolled. */
bool import_job_running_for(ImportJob *job, const char *project_dir);
void import_job_destroy(ImportJob *job);

#endif
