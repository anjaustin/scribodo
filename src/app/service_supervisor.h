/* Joinable background start/probe for the local project-store services. */
#ifndef SCRIBODO_SERVICE_SUPERVISOR_H
#define SCRIBODO_SERVICE_SUPERVISOR_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "app/store.h"

/* The exit codes of `tools/services.sh up`: the contract between the script and the app, defined here and in the
 * script's header comment, pinned by tests/test_services.c. The script's last line on stderr says the rest. */
enum {
    SERVICES_EXIT_OK = 0,
    SERVICES_EXIT_FAILED = 1,  /* any other failure */
    SERVICES_EXIT_REFUSED = 2, /* the author decides: the credentials' store is still in the containers
                                * (migrate-from-docker moves it), a purge or migration guard, a usage error */
    SERVICES_EXIT_MARIADB = 3, /* MariaDB is not installed, or not the version the store is pinned to */
    SERVICES_EXIT_QDRANT = 4,  /* Qdrant is not built (make qdrant) */
    SERVICES_EXIT_PORT = 5,    /* a service's port is taken by another program */
    SERVICES_EXIT_DOCKER = 6,  /* docker mode, and Docker is not running */
    SERVICES_EXIT_BUSY = 75,   /* another services.sh held the store's lock for five minutes (lockf's EX_TEMPFAIL) */
};

enum { SERVICES_REASON_MAX = 240 };

typedef struct {
    bool command_attempted;
    bool command_succeeded;
    bool reachable;
    int exit_code;                    /* the script's exit status; -1 if it could not run or was ended by a signal */
    char reason[SERVICES_REASON_MAX]; /* its last non-empty line on stderr, control characters made spaces */
} ServiceSupervisorResult;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    atomic_int state; /* 0 idle, 1 running, 2 completed and awaiting poll */
    atomic_bool cancel;
    char tools_dir[PATH_MAX];
    bool start_services;
    ServiceSupervisorResult result;
    StoreManager *store;
} ServiceSupervisor;

/* `store` is required and borrowed until service_supervisor_destroy(). */
bool service_supervisor_init(ServiceSupervisor *supervisor, StoreManager *store);
bool service_supervisor_start(ServiceSupervisor *supervisor, const char *tools_dir, bool start_services);
bool service_supervisor_busy(const ServiceSupervisor *supervisor);
bool service_supervisor_poll(ServiceSupervisor *supervisor, ServiceSupervisorResult *result);
/* For the author, after a start that did not succeed: the code's sentence and the script's own line. The sentence
 * alone is what service_supervisor_headline() returns (a static string; NULL for success or no attempt). */
const char *service_supervisor_headline(const ServiceSupervisorResult *result);
void service_supervisor_describe(const ServiceSupervisorResult *result, char *out, size_t outlen);
/* After a start that succeeded: the script's `warning: ` line without its prefix (a newer MariaDB patch than the pin),
 * for the author to see once; NULL when there is none. Points into `result`. */
const char *service_supervisor_warning(const ServiceSupervisorResult *result);
/* Ends the object's lifetime; requires exclusive ownership with no concurrent
 * API entry. A running start/probe is allowed to finish and is joined. */
bool service_supervisor_destroy(ServiceSupervisor *supervisor);

#endif
