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

#include "core/json.h"
#include "core/file_text.h"
#include "app/revisions_internal.h"

static bool path_for(const char *project_dir, char *out, size_t outlen)
{
    int n = snprintf(out, outlen, "%s/.scribodo/renames.jsonl", project_dir);
    if (n >= 0 && (size_t)n < outlen) return true;
    if (outlen) out[0] = '\0';
    return false;
}

static bool record_at(const char *project_dir, const char *from, const char *to, double when, const char *operation)
{
    if (strchr(from, '/') || strchr(to, '/')) return false;
    char dir[1500], path[1600];
    int dn = snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    if (dn < 0 || (size_t)dn >= sizeof(dir)) return false;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    if (!path_for(project_dir, path, sizeof(path))) return false;
    if (operation) {
        char *text = NULL;
        FileTextStatus read_status = file_text_read_no_follow(path, &text);
        if (read_status == FILE_TEXT_ERROR) return false;
        bool found = false, ok = true;
        for (char *save = NULL, *line = text ? strtok_r(text, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
            bool allocation_error = false;
            cJSON *j = json_parse_checked(line, &allocation_error);
            if (allocation_error) { cJSON_Delete(j); ok = false; break; }
            cJSON *id = cJSON_GetObjectItem(j, "operation");
            if (cJSON_IsString(id) && !strcmp(id->valuestring, operation)) {
                cJSON *a = cJSON_GetObjectItem(j, "from"), *b = cJSON_GetObjectItem(j, "to"), *t = cJSON_GetObjectItem(j, "ts");
                found = true;
                ok = cJSON_IsString(a) && cJSON_IsString(b) && cJSON_IsNumber(t) &&
                     !strcmp(a->valuestring, from) && !strcmp(b->valuestring, to) && t->valuedouble == when;
            }
            cJSON_Delete(j);
            if (!ok) break;
        }
        free(text);
        if (!ok) return false;
        if (found) {
            int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            bool durable = fd >= 0 && fsync(fd) == 0;
            if (fd >= 0 && close(fd)) durable = false;
            return durable;
        }
    }
    cJSON *j = cJSON_CreateObject();
    bool built = j && cJSON_AddStringToObject(j, "from", from) && cJSON_AddStringToObject(j, "to", to) &&
                 cJSON_AddNumberToObject(j, "ts", when) &&
                 (!operation || cJSON_AddStringToObject(j, "operation", operation));
    char *s = built ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!s) return false;
    size_t n = strlen(s);
    char *line = malloc(n + 2);
    bool ok = line != NULL;
    if (ok) {
        memcpy(line, s, n);
        line[n] = '\n';
        int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
        ok = fd >= 0;
        if (ok) {
            off_t end = lseek(fd, 0, SEEK_END);
            char last = '\n';
            if (end > 0 && pread(fd, &last, 1, end - 1) == 1 && last != '\n') ok = write(fd, "\n", 1) == 1;
            ok = ok && write(fd, line, n + 1) == (ssize_t)(n + 1) && fsync(fd) == 0;
            if (close(fd)) ok = false;
        }
    }
    free(line);
    free(s);
    return ok;
}

bool renames_record(const char *project_dir, const char *from, const char *to)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return record_at(project_dir, from, to, (double)tv.tv_sec + (double)tv.tv_usec / 1e6, NULL);
}

bool renames_record_once(const char *project_dir, const char *from, const char *to, double when, const char *operation)
{
    if (!operation || !operation[0] || !(when >= 0 && when < 0x1p63)) return false;
    return record_at(project_dir, from, to, when, operation);
}

void renames_resolve(const char *project_dir, const char *name, char *out, size_t outlen)
{
    renames_resolve_since(project_dir, name, 0, out, outlen);
}

void renames_resolve_since(const char *project_dir, const char *name, double since, char *out, size_t outlen)
{
    if (!outlen) return;
    out[0] = '\0';
    if (!revisions_recover_files(project_dir)) return;
    snprintf(out, outlen, "%s", name);
    char path[1600];
    if (!path_for(project_dir, path, sizeof(path))) return;
    char *text = NULL;
    FileTextStatus status = file_text_read_no_follow(path, &text);
    if (status == FILE_TEXT_MISSING) return;
    if (status != FILE_TEXT_OK) { out[0] = '\0'; return; }
    /* Each line moves a name forward; applying them in order follows a chain
     * (a -> b, then b -> c). A name reused later starts a new chain, which is
     * also what applying in order gives. */
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        bool allocation_error;
        cJSON *j = json_parse_checked(line, &allocation_error);
        if (allocation_error || json_has_nul_escape(line, strlen(line))) {
            cJSON_Delete(j); out[0] = '\0'; break;
        }
        cJSON *from = cJSON_GetObjectItem(j, "from"), *to = cJSON_GetObjectItem(j, "to"), *ts = cJSON_GetObjectItem(j, "ts");
        /* Only renames made after the source was recorded apply to it: a name
         * reused later by another document is not this one (red-team 006). */
        bool after = !cJSON_IsNumber(ts) || ts->valuedouble >= since;
        if (after && cJSON_IsString(from) && cJSON_IsString(to) && !strchr(to->valuestring, '/') && !strcmp(out, from->valuestring))
            snprintf(out, outlen, "%s", to->valuestring);
        cJSON_Delete(j);
    }
    free(text);
}
