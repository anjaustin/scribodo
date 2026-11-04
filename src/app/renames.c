#define _DARWIN_C_SOURCE
#include "renames.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

static void path_for(const char *project_dir, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s/.scribodo/renames.jsonl", project_dir);
}

bool renames_record(const char *project_dir, const char *from, const char *to)
{
    if (strchr(from, '/') || strchr(to, '/')) return false;
    char dir[1100], path[1200];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    path_for(project_dir, path, sizeof(path));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "from", from);
    cJSON_AddStringToObject(j, "to", to);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* Milliseconds: a rename, a new file with the old name and a note on it
     * can all happen within one second. */
    cJSON_AddNumberToObject(j, "ts", (double)tv.tv_sec + (double)(tv.tv_usec / 1000) / 1000.0);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return false;
    size_t n = strlen(s);
    char *line = malloc(n + 2);
    bool ok = line != NULL;
    if (ok) {
        memcpy(line, s, n);
        line[n] = '\n';
        int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
        ok = fd >= 0;
        if (ok) {
            off_t end = lseek(fd, 0, SEEK_END);
            char last = '\n';
            if (end > 0 && pread(fd, &last, 1, end - 1) == 1 && last != '\n') ok = write(fd, "\n", 1) == 1;
            ok = ok && write(fd, line, n + 1) == (ssize_t)(n + 1) && fsync(fd) == 0;
            close(fd);
        }
    }
    free(line);
    free(s);
    return ok;
}

void renames_resolve(const char *project_dir, const char *name, char *out, size_t outlen)
{
    renames_resolve_since(project_dir, name, 0, out, outlen);
}

void renames_resolve_since(const char *project_dir, const char *name, double since, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s", name);
    char path[1200];
    path_for(project_dir, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    /* Each line moves a name forward; applying them in order follows a chain
     * (a -> b, then b -> c). A name reused later starts a new chain, which is
     * also what applying in order gives. */
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        cJSON *j = cJSON_Parse(line);
        cJSON *from = cJSON_GetObjectItem(j, "from"), *to = cJSON_GetObjectItem(j, "to"), *ts = cJSON_GetObjectItem(j, "ts");
        /* Only renames made after the source was recorded apply to it: a name
         * reused later by another document is not this one (red-team 006). */
        bool after = !cJSON_IsNumber(ts) || ts->valuedouble >= since;
        if (after && cJSON_IsString(from) && cJSON_IsString(to) && !strchr(to->valuestring, '/') && !strcmp(out, from->valuestring))
            snprintf(out, outlen, "%s", to->valuestring);
        cJSON_Delete(j);
    }
    fclose(f);
}
