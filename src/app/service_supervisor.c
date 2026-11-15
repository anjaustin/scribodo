#include "service_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app/store.h"
#include "core/deadline_clock.h"

extern char **environ;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(SCRIBODO_DEADLINE_CLOCK, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The tail of the script's stderr, bounded: only its last line is kept, and a verbose script must not cost memory. */
typedef struct {
    char buf[1024];
    size_t len;
} StderrTail;

static void tail_add(StderrTail *t, const char *data, size_t n)
{
    if (n >= sizeof(t->buf)) {
        memcpy(t->buf, data + n - sizeof(t->buf), sizeof(t->buf));
        t->len = sizeof(t->buf);
        return;
    }
    if (t->len + n > sizeof(t->buf)) {
        size_t drop = t->len + n - sizeof(t->buf);
        memmove(t->buf, t->buf + drop, t->len - drop);
        t->len -= drop;
    }
    memcpy(t->buf + t->len, data, n);
    t->len += n;
}

static void tail_last_line(const StderrTail *t, char *out, size_t outlen)
{
    size_t n = t->len;
    while (n > 0 && (t->buf[n - 1] == '\n' || t->buf[n - 1] == '\r' || t->buf[n - 1] == ' ')) n--;
    size_t at = n;
    while (at > 0 && t->buf[at - 1] != '\n') at--;
    size_t w = 0;
    for (size_t i = at; i < n && w + 1 < outlen; i++) {
        unsigned char c = (unsigned char)t->buf[i];
        out[w++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    out[w] = '\0';
}

static void drain(int fd, StderrTail *t, bool *open)
{
    char chunk[512];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            tail_add(t, chunk, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) *open = false;
        return;
    }
}

/* A pipe end above the standard descriptors, close-on-exec in this process: a spawn elsewhere in the app never
 * inherits it, and in the child the file actions place the write end on 2 explicitly. */
static int above_stdio(int fd)
{
    if (fd > STDERR_FILENO) return fd;
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    close(fd);
    return moved;
}

/* `services.sh up`: its exit status, and its last stderr line in `result->reason`. */
static int run_services(ServiceSupervisor *supervisor, const char *tool, ServiceSupervisorResult *result)
{
    int er[2];
    if (pipe(er) != 0) {
        snprintf(result->reason, sizeof(result->reason), "couldn't run %s: %s", tool, strerror(errno));
        return -1;
    }
    er[0] = above_stdio(er[0]);
    er[1] = above_stdio(er[1]);
    if (er[0] < 0 || er[1] < 0 || fcntl(er[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(er[1], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(er[0], F_SETFL, O_NONBLOCK) != 0) {
        if (er[0] >= 0) close(er[0]);
        if (er[1] >= 0) close(er[1]);
        snprintf(result->reason, sizeof(result->reason), "couldn't run %s: no pipe", tool);
        return -1;
    }
    char *argv[] = { (char *)tool, "up", NULL };
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(er[0]);
        close(er[1]);
        return -1;
    }
    bool actions_ok = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
                      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                      posix_spawn_file_actions_adddup2(&actions, er[1], STDERR_FILENO) == 0;
    bool attr_inited = posix_spawnattr_init(&attr) == 0;
    bool attr_ok = attr_inited;
    if (attr_inited) {
        /* Only the three descriptors above reach the script: the app's model and store sockets must not outlive it in
         * the daemons it starts (REDTEAM-023; the script also starts them with nothing inherited, so no daemon holds
         * the stderr pipe either). */
        attr_ok = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT) == 0 &&
                  posix_spawnattr_setpgroup(&attr, 0) == 0;
    }
    pid_t pid = 0;
    int spawn_rc = actions_ok && attr_ok ? posix_spawn(&pid, tool, &actions, &attr, argv, environ) : EINVAL;
    if (attr_inited) posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(er[1]);
    if (spawn_rc != 0) {
        close(er[0]);
        snprintf(result->reason, sizeof(result->reason), "couldn't run %s: %s", tool, strerror(spawn_rc));
        return -1;
    }

    StderrTail tail = { .len = 0 };
    bool pipe_open = true, term_sent = false, kill_sent = false;
    double cancel_at = 0;
    int status = 0, code = -1;
    for (;;) {
        if (pipe_open) drain(er[0], &tail, &pipe_open);
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            /* What the script wrote before it exited; a descendant still holding the pipe is not waited for. */
            if (pipe_open) drain(er[0], &tail, &pipe_open);
            code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (done < 0 && errno != EINTR) break;
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
        if (pipe_open) {
            struct pollfd p = { .fd = er[0], .events = POLLIN };
            (void)poll(&p, 1, 20);
        } else {
            struct timespec pause = { .tv_nsec = 20000000 };
            nanosleep(&pause, NULL);
        }
    }
    close(er[0]);
    tail_last_line(&tail, result->reason, sizeof(result->reason));
    return code;
}

static void *service_supervisor_main(void *arg)
{
    ServiceSupervisor *supervisor = arg;
    supervisor->result = (ServiceSupervisorResult){ .command_attempted = supervisor->start_services, .exit_code = -1 };
    if (supervisor->start_services) {
        char tool[PATH_MAX + 64];
        int n = snprintf(tool, sizeof(tool), "%s/services.sh", supervisor->tools_dir);
        if (n > 0 && (size_t)n < sizeof(tool)) {
            supervisor->result.exit_code = run_services(supervisor, tool, &supervisor->result);
            supervisor->result.command_succeeded = supervisor->result.exit_code == SERVICES_EXIT_OK;
        }
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

const char *service_supervisor_headline(const ServiceSupervisorResult *result)
{
    if (!result || !result->command_attempted || result->command_succeeded) return NULL;
    switch (result->exit_code) {
    case SERVICES_EXIT_REFUSED: return "The project store refused to start";
    case SERVICES_EXIT_MARIADB: return "MariaDB is missing or not the pinned version";
    case SERVICES_EXIT_QDRANT: return "Qdrant isn't built (make qdrant)";
    case SERVICES_EXIT_PORT: return "A project store port is taken";
    case SERVICES_EXIT_DOCKER: return "Docker isn't running";
    case SERVICES_EXIT_BUSY: return "Another services.sh run holds the store";
    case -1: return "The services script couldn't run";
    default: return "The project store didn't start";
    }
}

void service_supervisor_describe(const ServiceSupervisorResult *result, char *out, size_t outlen)
{
    if (!out || !outlen) return;
    out[0] = '\0';
    const char *headline = service_supervisor_headline(result);
    if (!headline) return;
    if (result->reason[0]) snprintf(out, outlen, "%s: %s", headline, result->reason);
    else snprintf(out, outlen, "%s (tools/services.sh exit %d)", headline, result->exit_code);
}
