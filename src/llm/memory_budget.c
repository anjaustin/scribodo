#define _POSIX_C_SOURCE 200809L
#include "memory_budget.h"
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

double memory_budget_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

bool memory_budget_expired(const MemoryBudget *budget)
{
    double now = memory_budget_now_ms();
    return !budget || !budget->started || now < 0 || now >= budget->deadline_ms;
}

static void *watch_budget(void *context)
{
    MemoryBudget *budget = context;
    const struct timespec slice = { .tv_nsec = 1000000 };
    while (!atomic_load(&budget->done)) {
        if (memory_budget_expired(budget) || (budget->cancel && atomic_load(budget->cancel))) {
            atomic_store(&budget->stop, true);
            int fd = atomic_load(&budget->sql_socket);
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
            break;
        }
        nanosleep(&slice, NULL);
    }
    return NULL;
}

bool memory_budget_begin(MemoryBudget *budget, const atomic_bool *cancel, int milliseconds)
{
    if (!budget) return false;
    memset(budget, 0, sizeof(*budget));
    atomic_init(&budget->stop, false);
    atomic_init(&budget->done, false);
    atomic_init(&budget->sql_socket, -1);
    double now = memory_budget_now_ms();
    if (now < 0 || milliseconds < 1 || milliseconds > 30000 || (cancel && atomic_load(cancel))) return false;
    budget->cancel = cancel;
    budget->deadline_ms = now + milliseconds;
    budget->started = true;
    if (pthread_create(&budget->guard, NULL, watch_budget, budget) == 0) return true;
    budget->started = false;
    return false;
}

bool memory_budget_bind_db(MemoryBudget *budget, Db *db)
{
    if (!budget || !budget->started || !db || db_in_transaction(db) || atomic_load(&budget->sql_socket) >= 0) return false;
    int original = db_socket(db);
    if (original < 0) return false;
    int fd = fcntl(original, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return false;
    atomic_store(&budget->sql_socket, fd);
    if (atomic_load(&budget->stop) || memory_budget_expired(budget) || (budget->cancel && atomic_load(budget->cancel))) {
        shutdown(fd, SHUT_RDWR);
        atomic_store(&budget->stop, true);
        return false;
    }
    return true;
}

void memory_budget_end(MemoryBudget *budget)
{
    if (!budget || !budget->started) return;
    atomic_store(&budget->done, true);
    pthread_join(budget->guard, NULL);
    int fd = atomic_exchange(&budget->sql_socket, -1);
    if (fd >= 0) close(fd);
    budget->started = false;
}
