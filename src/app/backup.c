/* In-app backup and restore. See backup.h. */
#define _DARWIN_C_SOURCE
#include "backup.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app/store.h"
#include "app/store_backends.h"
#include "cJSON.h"

extern char **environ;

void backup_default_dir(char *out, size_t outlen)
{
    const char *env = getenv("SCRIBODO_BACKUPS"); /* tests */
    if (env && env[0]) snprintf(out, outlen, "%s", env);
    else snprintf(out, outlen, "%s/Library/Application Support/Scribodo/backups", getenv("HOME") ? getenv("HOME") : "");
}

/* Run argv (no shell: paths are never interpreted). The last line of its
 * standard output goes to `last`, its last error line to `err`. */
static int run(char *const argv[], char *last, size_t lastlen, char *err, size_t errlen)
{
    int out[2], er[2];
    if (pipe(out) != 0) return -1;
    if (pipe(er) != 0) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, er[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawn_file_actions_addclose(&fa, er[0]);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    close(er[1]);
    if (rc != 0) {
        close(out[0]);
        close(er[0]);
        return -1;
    }
    /* Both pipes are small in practice (a path, a message); read the output
     * then the errors. */
    char buf[8192];
    size_t n = 0;
    ssize_t r;
    while ((r = read(out[0], buf + n, sizeof(buf) - 1 - n)) > 0 && n < sizeof(buf) - 1) n += (size_t)r;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    const char *l = strrchr(buf, '\n');
    if (last && lastlen) snprintf(last, lastlen, "%s", l ? l + 1 : buf);
    n = 0;
    while ((r = read(er[0], buf + n, sizeof(buf) - 1 - n)) > 0 && n < sizeof(buf) - 1) n += (size_t)r;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    l = strrchr(buf, '\n');
    if (err && errlen) snprintf(err, errlen, "%s", l ? l + 1 : buf);
    close(out[0]);
    close(er[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void remove_tree(const char *path)
{
    char *argv[] = { "/bin/rm", "-rf", (char *)path, NULL };
    run(argv, NULL, 0, NULL, 0);
}

bool backup_project(const char *tools_dir, const char *project_dir, char *dest, size_t destlen, char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    char pid[64];
    StoreMode mode = store_mode(project_dir, pid, sizeof(pid), NULL);
    if (mode == STORE_PAUSED) {
        snprintf(why, whylen, "Backups wait for the project store (it's paused for this project)");
        return false;
    }
    char script[PATH_MAX + 32], dir[PATH_MAX], exp[PATH_MAX + 80], name[300] = "";
    snprintf(script, sizeof(script), "%s/backup.sh", tools_dir);
    backup_default_dir(dir, sizeof(dir));
    mkdir(dir, 0700);
    const char *base = strrchr(project_dir, '/');
    snprintf(name, sizeof(name), "%s", base ? base + 1 : project_dir);
    exp[0] = '\0';
    bool ok = true;
    if (mode == STORE_DB) {
        /* The project's own data as files, beside the dump. */
        snprintf(exp, sizeof(exp), "%s/.export-%s-%ld/%s", dir, pid, (long)time(NULL), name);
        char parent[PATH_MAX + 80];
        snprintf(parent, sizeof(parent), "%.*s", (int)(strrchr(exp, '/') - exp), exp);
        mkdir(parent, 0700);
        ok = store_export_project(pid, exp, why, whylen);
    }
    char out[PATH_MAX + 64] = "", err[400] = "", target[PATH_MAX + 400];
    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(target, sizeof(target), "%s/%s-%s", dir, name, stamp);
    if (ok) {
        char *argv_db[] = { script, "--db", (char *)store_database(), "--with", exp, (char *)project_dir, target, NULL };
        char *argv_files[] = { script, "--no-db", (char *)project_dir, target, NULL };
        int rc = run(mode == STORE_DB ? argv_db : argv_files, out, sizeof(out), err, sizeof(err));
        ok = rc == 0 && out[0] == '/';
        if (!ok) snprintf(why, whylen, "The backup failed%s%s", err[0] ? ": " : "", err);
    }
    if (exp[0]) {
        char parent[PATH_MAX + 80];
        snprintf(parent, sizeof(parent), "%.*s", (int)(strrchr(exp, '/') - exp), exp);
        remove_tree(parent);
    }
    if (ok) snprintf(dest, destlen, "%s", out);
    return ok;
}

static bool exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0;
}

bool backup_restore_copy(const char *tools_dir, const char *backup_dir, const char *parent_dir, char *restored,
                         size_t restoredlen, char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    char script[PATH_MAX + 32], err[400] = "";
    snprintf(script, sizeof(script), "%s/backup.sh", tools_dir);
    char *verify[] = { script, "verify", (char *)backup_dir, NULL };
    if (run(verify, NULL, 0, err, sizeof(err)) != 0) {
        snprintf(why, whylen, "That isn't a backup, or it's damaged (its checksums don't match); nothing was restored");
        return false;
    }
    /* The manifest names the project folder inside the backup; it must be a
     * plain name (a crafted manifest can't point outside the backup). */
    char mpath[PATH_MAX + 32];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", backup_dir);
    FILE *f = fopen(mpath, "rb");
    char buf[16384];
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = '\0';
    cJSON *m = cJSON_Parse(buf);
    const cJSON *nm = cJSON_GetObjectItem(m, "project_name");
    char name[256] = "";
    if (cJSON_IsString(nm)) snprintf(name, sizeof(name), "%s", nm->valuestring);
    cJSON_Delete(m);
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '\n')) {
        snprintf(why, whylen, "The backup's manifest is damaged; nothing was restored");
        return false;
    }
    char files[PATH_MAX + 300], exported[PATH_MAX + 300];
    snprintf(files, sizeof(files), "%s/files/%s", backup_dir, name);
    snprintf(exported, sizeof(exported), "%s/export/%s/.scribodo", backup_dir, name);
    if (!exists(files)) {
        snprintf(why, whylen, "The backup has no project folder; nothing was restored");
        return false;
    }
    /* A store project's data is in the export; a backup without one (made
     * with tools/backup.sh) can only be restored with tools/restore.sh. */
    char marker[PATH_MAX + 320];
    snprintf(marker, sizeof(marker), "%s/.scribodo/store.json", files);
    if (!exists(exported) && exists(marker)) {
        snprintf(why, whylen, "This backup's project data is in its database dump only; restore it with tools/restore.sh");
        return false;
    }
    /* A new folder, never an existing one. */
    char stamp[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M", &tmv);
    char dest[PATH_MAX + 400];
    snprintf(dest, sizeof(dest), "%s/%s (restored %s)", parent_dir, name, stamp);
    for (int k = 2; exists(dest) && k < 100; k++) snprintf(dest, sizeof(dest), "%s/%s (restored %s) %d", parent_dir, name, stamp, k);
    if (exists(dest)) {
        snprintf(why, whylen, "Couldn't find a free folder name to restore into");
        return false;
    }
    char *copy[] = { "/usr/bin/ditto", files, dest, NULL };
    if (run(copy, NULL, 0, err, sizeof(err)) != 0) {
        snprintf(why, whylen, "Copying the backup failed: %s", err);
        remove_tree(dest);
        return false;
    }
    /* Its project data: from the export (a store project) or as copied (a
     * project on files). The store marker and import state are the old
     * project's, not this new one's. */
    char sd[PATH_MAX + 420];
    snprintf(sd, sizeof(sd), "%s/.scribodo", dest);
    if (exists(exported)) {
        remove_tree(sd);
        char *cp[] = { "/usr/bin/ditto", exported, sd, NULL };
        if (run(cp, NULL, 0, err, sizeof(err)) != 0) {
            snprintf(why, whylen, "Copying the backup's project data failed: %s", err);
            remove_tree(dest);
            return false;
        }
    } else {
        static const char *const stale[] = { "store.json", "store.json.set-aside", "import-pending", "import.lock" };
        for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); i++) {
            char p[PATH_MAX + 500];
            snprintf(p, sizeof(p), "%s/%s", sd, stale[i]);
            unlink(p);
        }
    }
    snprintf(restored, restoredlen, "%s", dest);
    return true;
}
