#define _DARWIN_C_SOURCE
#include "child_process.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

extern char **environ;

static int above_stdio(int fd)
{
    if (fd < 0 || fd > STDERR_FILENO) return fd;
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    close(fd);
    return moved;
}

bool child_pipe(int fds[2])
{
    int p[2];
    if (pipe(p) != 0) return false;
    p[0] = above_stdio(p[0]);
    p[1] = above_stdio(p[1]);
    if (p[0] < 0 || p[1] < 0 || fcntl(p[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) != 0) {
        if (p[0] >= 0) close(p[0]);
        if (p[1] >= 0) close(p[1]);
        return false;
    }
    fds[0] = p[0];
    fds[1] = p[1];
    return true;
}

/* `fd` onto `target` in the child; -1 means /dev/null. A descriptor already in place is inherited as it is. */
static int place(posix_spawn_file_actions_t *fa, int fd, int target, int flags)
{
    if (fd < 0) return posix_spawn_file_actions_addopen(fa, target, "/dev/null", flags, 0);
#ifdef __APPLE__
    if (fd == target) return posix_spawn_file_actions_addinherit_np(fa, target);
#endif
    return posix_spawn_file_actions_adddup2(fa, fd, target);
}

int child_spawn(pid_t *pid, const char *path, char *const argv[], int out_fd, int err_fd, bool own_group)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc) return rc;
    rc = posix_spawnattr_init(&attr);
    if (rc) {
        posix_spawn_file_actions_destroy(&fa);
        return rc;
    }
    rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = place(&fa, out_fd, STDOUT_FILENO, O_WRONLY);
    if (!rc) rc = place(&fa, err_fd, STDERR_FILENO, O_WRONLY);
    short flags = 0;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    if (own_group) flags |= POSIX_SPAWN_SETPGROUP;
    if (!rc) rc = posix_spawnattr_setflags(&attr, flags);
    if (!rc && own_group) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (!rc) rc = posix_spawn(pid, path, &fa, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    return rc;
}

bool child_process_args(pid_t pid, char *out, size_t outlen)
{
    if (!out || !outlen) return false;
    out[0] = '\0';
#ifdef __APPLE__
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, (int)pid };
    size_t size = 0;
    int argmax_mib[2] = { CTL_KERN, KERN_ARGMAX };
    int argmax = 0;
    size_t len = sizeof(argmax);
    if (sysctl(argmax_mib, 2, &argmax, &len, NULL, 0) != 0 || argmax <= 0) return false;
    char *buf = malloc((size_t)argmax);
    if (!buf) return false;
    size = (size_t)argmax;
    if (sysctl(mib, 3, buf, &size, NULL, 0) != 0 || size < sizeof(int)) {
        free(buf);
        return false;
    }
    /* argc, the executable's path, padding NULs, then argc NUL-terminated arguments. */
    int argc = 0;
    memcpy(&argc, buf, sizeof(argc));
    size_t at = sizeof(int);
    while (at < size && buf[at]) at++;
    while (at < size && !buf[at]) at++;
    size_t w = 0;
    for (int i = 0; i < argc && at < size; i++) {
        size_t n = strnlen(buf + at, size - at);
        if (i && w + 1 < outlen) out[w++] = ' ';
        size_t copy = n < outlen - 1 - w ? n : outlen - 1 - w;
        memcpy(out + w, buf + at, copy);
        w += copy;
        at += n + 1;
    }
    out[w] = '\0';
    free(buf);
    return argc > 0;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *f = fopen(path, "rbe");
    if (!f) return false;
    size_t n = fread(out, 1, outlen - 1, f);
    fclose(f);
    for (size_t i = 0; i + 1 < n; i++)
        if (!out[i]) out[i] = ' ';
    out[n] = '\0';
    return n > 0;
#endif
}
