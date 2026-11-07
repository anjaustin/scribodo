#ifndef SCRIBODO_MEMORY_BUDGET_H
#define SCRIBODO_MEMORY_BUDGET_H

#include "app/db.h"
#include <pthread.h>
#include <stdatomic.h>

#define MEMORY_PREPARE_BUDGET_MS 500

typedef struct {
    double deadline_ms;
    const atomic_bool *cancel;
    atomic_bool stop, done;
    atomic_int sql_socket;
    pthread_t guard;
    bool started;
} MemoryBudget;

double memory_budget_now_ms(void);
bool memory_budget_begin(MemoryBudget *budget, const atomic_bool *cancel, int milliseconds);
bool memory_budget_expired(const MemoryBudget *budget);
/* Duplicates the owned TLS connection's socket before any bounded SQL work.
 * Shutdown on timeout/cancel aborts that connection only. Join this guard before
 * store_thread_done/db_close; discard the connection after bounded preparation.
 * A duplicate prevents a library-close/descriptor-reuse race from aborting an
 * unrelated socket. This must never wrap an author's write transaction. */
bool memory_budget_bind_db(MemoryBudget *budget, Db *db);
void memory_budget_end(MemoryBudget *budget);

#endif
