/* Spawning helper tools with nothing of the app's but what is handed to them (AUDIT-004; the register's "spawned tools
 * inherit the app's descriptors"): the backup pipeline, the services script. */
#ifndef SCRIBODO_CHILD_PROCESS_H
#define SCRIBODO_CHILD_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* A pipe whose ends are both above the standard descriptors and close-on-exec in this process, so no other spawn in
 * the app inherits them. False (nothing left open) on failure. */
bool child_pipe(int fds[2]);

/* Runs `path` with `argv` (no shell, no PATH search). The child gets stdin from /dev/null, stdout on `out_fd` and
 * stderr on `err_fd` (-1: /dev/null), and no other descriptor: POSIX_SPAWN_CLOEXEC_DEFAULT closes everything the file
 * actions did not name. `own_group`: the child leads a new process group, so a cancel can signal it and its
 * descendants. Returns 0 or an errno value. */
int child_spawn(pid_t *pid, const char *path, char *const argv[], int out_fd, int err_fd, bool own_group);

/* The arguments of a running process, joined by spaces, read from the kernel (no `ps`, no child process). False if the
 * process is gone or not this user's. */
bool child_process_args(pid_t pid, char *out, size_t outlen);

#endif
