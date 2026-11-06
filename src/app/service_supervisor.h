/* Joinable background start/probe for the local project-store services. */
#ifndef SCRIBODO_SERVICE_SUPERVISOR_H
#define SCRIBODO_SERVICE_SUPERVISOR_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    bool command_attempted;
    bool command_succeeded;
    bool reachable;
} ServiceSupervisorResult;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    atomic_bool cancel;
    char tools_dir[PATH_MAX];
    bool start_services;
    ServiceSupervisorResult result;
} ServiceSupervisor;

bool service_supervisor_init(ServiceSupervisor *supervisor);
bool service_supervisor_start(ServiceSupervisor *supervisor, const char *tools_dir, bool start_services);
bool service_supervisor_busy(const ServiceSupervisor *supervisor);
bool service_supervisor_poll(ServiceSupervisor *supervisor, ServiceSupervisorResult *result);
/* Ends the object's lifetime; requires exclusive ownership with no concurrent
 * API entry. A running start/probe is allowed to finish and is joined. */
bool service_supervisor_destroy(ServiceSupervisor *supervisor);

#endif
