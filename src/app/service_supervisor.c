#include "service_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app/store.h"

extern char **environ;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool run_services(ServiceSupervisor *supervisor, const char *tool)
{
    char *argv[] = { (char *)tool, "up", NULL };
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    bool actions_ok = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
    bool attr_inited = posix_spawnattr_init(&attr) == 0;
    bool attr_ok = attr_inited;
    if (attr_inited) {
        attr_ok = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) == 0 &&
                  posix_spawnattr_setpgroup(&attr, 0) == 0;
    }
    pid_t pid = 0;
    bool spawned = actions_ok && attr_ok && posix_spawn(&pid, tool, &actions, &attr, argv, environ) == 0;
    if (attr_inited) posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (!spawned) return false;

    bool term_sent = false, kill_sent = false;
    double cancel_at = 0;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR) return false;
        if (atomic_load(&supervisor->cancel)) {
            double now = monotonic_seconds();
            if (!term_sent) {
                kill(-pid, SIGTERM);
                term_sent = true;
                cancel_at = now;
            } else if (!kill_sent && now - cancel_at >= 0.5) {
                kill(-pid, SIGKILL);
                kill_sent = true;
            }
        }
        struct timespec pause = { .tv_nsec = 20000000 };
        nanosleep(&pause, NULL);
    }
}

static void *service_supervisor_main(void *arg)
{
    ServiceSupervisor *supervisor = arg;
    supervisor->result = (ServiceSupervisorResult){ .command_attempted = supervisor->start_services };
    if (supervisor->start_services) {
        char tool[PATH_MAX + 64];
        int n = snprintf(tool, sizeof(tool), "%s/services.sh", supervisor->tools_dir);
        if (n > 0 && (size_t)n < sizeof(tool)) supervisor->result.command_succeeded = run_services(supervisor, tool);
    }
    if (!atomic_load(&supervisor->cancel)) supervisor->result.reachable = store_probe(supervisor->store);
    atomic_store(&supervisor->state, 2);
    return NULL;
}

bool service_supervisor_init(ServiceSupervisor *supervisor, StoreManager *store)
{
    if (!supervisor || !store) return false;
    memset(supervisor, 0, sizeof(*supervisor));
    supervisor->store = store;
    atomic_init(&supervisor->state, 0);
    atomic_init(&supervisor->cancel, false);
    return pthread_mutex_init(&supervisor->mu, NULL) == 0;
}

bool service_supervisor_start(ServiceSupervisor *supervisor, const char *tools_dir, bool start_services)
{
    if (!tools_dir || strlen(tools_dir) >= sizeof(supervisor->tools_dir) ||
        pthread_mutex_lock(&supervisor->mu) != 0)
        return false;
    if (atomic_load(&supervisor->state) != 0) {
        pthread_mutex_unlock(&supervisor->mu);
        return false;
    }
    memcpy(supervisor->tools_dir, tools_dir, strlen(tools_dir) + 1);
    supervisor->start_services = start_services;
    supervisor->result = (ServiceSupervisorResult){ 0 };
    atomic_store(&supervisor->cancel, false);
    atomic_store(&supervisor->state, 1);
    bool ok = pthread_create(&supervisor->thread, NULL, service_supervisor_main, supervisor) == 0;
    if (!ok) atomic_store(&supervisor->state, 0);
    pthread_mutex_unlock(&supervisor->mu);
    return ok;
}

bool service_supervisor_busy(const ServiceSupervisor *supervisor)
{
    return atomic_load(&supervisor->state) != 0;
}

bool service_supervisor_poll(ServiceSupervisor *supervisor, ServiceSupervisorResult *result)
{
    if (pthread_mutex_lock(&supervisor->mu) != 0) return false;
    if (atomic_load(&supervisor->state) != 2 || pthread_join(supervisor->thread, NULL) != 0) {
        pthread_mutex_unlock(&supervisor->mu);
        return false;
    }
    if (result) *result = supervisor->result;
    atomic_store(&supervisor->state, 0);
    pthread_mutex_unlock(&supervisor->mu);
    return true;
}

bool service_supervisor_destroy(ServiceSupervisor *supervisor)
{
    atomic_store(&supervisor->cancel, true);
    if (pthread_mutex_lock(&supervisor->mu) != 0) return false;
    if (atomic_load(&supervisor->state) != 0 && pthread_join(supervisor->thread, NULL) != 0) {
        pthread_mutex_unlock(&supervisor->mu);
        return false;
    }
    atomic_store(&supervisor->state, 0);
    pthread_mutex_unlock(&supervisor->mu);
    return pthread_mutex_destroy(&supervisor->mu) == 0;
}
