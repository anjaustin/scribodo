#define _DARWIN_C_SOURCE
#include "revisions.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"

#include <pthread.h>

/* The UI thread and the observer both record revisions (red-team 005). */
static pthread_mutex_t g_rev_mu = PTHREAD_MUTEX_INITIALIZER;

static void hash_text(const char *s, char out[17])
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 1099511628211ull;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* History folder name: the document's basename with unsafe bytes replaced. */
static bool doc_dir(const char *project_dir, const char *doc_path, bool create, char *out, size_t outlen)
{
    const char *base = strrchr(doc_path, '/');
    base = base ? base + 1 : doc_path;
    char safe[200];
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)base; *p && k < sizeof(safe) - 1; p++)
        safe[k++] = (*p == '/' || *p < 0x20 || *p == ':') ? '_' : (char)*p;
    safe[k] = '\0';
    if (!k || !strcmp(safe, ".") || !strcmp(safe, "..")) return false;
    char a[1200], b[1300];
    snprintf(a, sizeof(a), "%s/.scribodo", project_dir);
    snprintf(b, sizeof(b), "%s/revisions", a);
    snprintf(out, outlen, "%s/%s", b, safe);
    if (!create) return true;
    const char *dirs[] = { a, b, out };
    for (size_t i = 0; i < 3; i++)
        if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool write_atomic(const char *path, const char *data, size_t n)
{
    char tmp[1500];
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t off = 0; ok && off < n;) {
        ssize_t w = write(fd, data + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        ok = w > 0;
        if (ok) off += (size_t)w;
    }
    ok = ok && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (buf) buf[fread(buf, 1, (size_t)n, f)] = '\0';
    fclose(f);
    return buf;
}

/* Missing numbers read as NaN, which must never be cast to an integer. */
static double num(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    double d = cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    return d >= 0.0 && d < 1e15 ? d : 0.0;
}

size_t revisions_list(const char *project_dir, const char *doc_path, Revision **out)
{
    *out = NULL;
    char dir[1500], idx[1600];
    if (!doc_dir(project_dir, doc_path, false, dir, sizeof(dir))) return 0;
    snprintf(idx, sizeof(idx), "%s/index.jsonl", dir);
    char *text = slurp(idx);
    if (!text) return 0;
    size_t n = 0, cap = 0;
    Revision *list = NULL;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        cJSON *j = cJSON_Parse(line);
        if (!j) continue; /* torn line after a crash */
        cJSON *file = cJSON_GetObjectItem(j, "file"), *origin = cJSON_GetObjectItem(j, "origin");
        if (cJSON_IsString(file) && cJSON_IsString(origin) && !strchr(file->valuestring, '/')) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                Revision *l = realloc(list, cap * sizeof(Revision));
                if (!l) {
                    cJSON_Delete(j);
                    break;
                }
                list = l;
            }
            Revision *r = &list[n++];
            memset(r, 0, sizeof(*r));
            snprintf(r->file, sizeof(r->file), "%s", file->valuestring);
            snprintf(r->origin, sizeof(r->origin), "%s", origin->valuestring);
            cJSON *note = cJSON_GetObjectItem(j, "note"), *h = cJSON_GetObjectItem(j, "hash");
            if (cJSON_IsString(note)) snprintf(r->note, sizeof(r->note), "%s", note->valuestring);
            if (cJSON_IsString(h)) snprintf(r->hash, sizeof(r->hash), "%s", h->valuestring);
            r->ts = (time_t)num(j, "ts");
            r->bytes = (size_t)num(j, "bytes");
        }
        cJSON_Delete(j);
    }
    free(text);
    /* Newest first. */
    for (size_t i = 0; i < n / 2; i++) {
        Revision t = list[i];
        list[i] = list[n - 1 - i];
        list[n - 1 - i] = t;
    }
    *out = list;
    return n;
}

static bool record_impl(const char *project_dir, const char *doc_path, const char *text, const char *origin,
                        const char *note, bool *recorded, char *file_out, size_t file_len)
{
    if (recorded) *recorded = false;
    char dir[1500];
    if (!doc_dir(project_dir, doc_path, true, dir, sizeof(dir))) return false;
    char hash[17];
    hash_text(text, hash);

    Revision *list = NULL;
    size_t n = revisions_list(project_dir, doc_path, &list);
    bool same = n > 0 && !strcmp(list[0].hash, hash);
    if (same && file_out) snprintf(file_out, file_len, "%s", list[0].file);
    /* A snapshot for a source may reuse any revision with exactly this text:
     * an older text must never become the newest History entry. */
    for (size_t i = 1; !same && file_out && i < n; i++)
        if (!strcmp(list[i].hash, hash)) {
            snprintf(file_out, file_len, "%s", list[i].file);
            same = true;
        }
    free(list);
    if (same) return true; /* identical to the newest revision */

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    time_t now = tv.tv_sec;
    localtime_r(&now, &tmv);
    char stamp[40], file[160], path[1700], idx[1600];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(file, sizeof(file), "%s-%03d-%s.md", stamp, (int)(tv.tv_usec / 1000), origin);
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    size_t len = strlen(text);
    if (!write_atomic(path, text, len)) return false;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "file", file);
    cJSON_AddStringToObject(j, "origin", origin);
    cJSON_AddStringToObject(j, "note", note ? note : "");
    cJSON_AddStringToObject(j, "hash", hash);
    cJSON_AddNumberToObject(j, "ts", (double)now);
    cJSON_AddNumberToObject(j, "bytes", (double)len);
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!line) return false;
    snprintf(idx, sizeof(idx), "%s/index.jsonl", dir);
    int fd = open(idx, O_RDWR | O_CREAT | O_APPEND, 0644);
    bool ok = fd >= 0;
    if (ok) {
        /* Terminate a torn line from a crash, then one write per record. */
        off_t end = lseek(fd, 0, SEEK_END);
        char last = '\n';
        if (end > 0 && pread(fd, &last, 1, end - 1) == 1 && last != '\n') ok = write(fd, "\n", 1) == 1;
        size_t ln = strlen(line);
        line[ln] = '\n';
        ok = ok && write(fd, line, ln + 1) == (ssize_t)(ln + 1) && fsync(fd) == 0;
        close(fd);
    }
    free(line);
    if (ok && recorded) *recorded = true;
    if (ok && file_out) snprintf(file_out, file_len, "%s", file);
    return ok;
}

bool revisions_record(const char *project_dir, const char *doc_path, const char *text, const char *origin,
                      const char *note, bool *recorded)
{
    pthread_mutex_lock(&g_rev_mu);
    bool ok = record_impl(project_dir, doc_path, text, origin, note, recorded, NULL, 0);
    pthread_mutex_unlock(&g_rev_mu);
    return ok;
}

bool revisions_snapshot(const char *project_dir, const char *doc_path, const char *text, char *file_out, size_t file_len)
{
    if (file_len) file_out[0] = '\0';
    pthread_mutex_lock(&g_rev_mu);
    bool ok = record_impl(project_dir, doc_path, text, "source", NULL, NULL, file_out, file_len) && file_out[0];
    pthread_mutex_unlock(&g_rev_mu);
    return ok;
}

char *revisions_load(const char *project_dir, const char *doc_path, const Revision *r)
{
    char dir[1500], path[1700];
    if (strchr(r->file, '/') || !doc_dir(project_dir, doc_path, false, dir, sizeof(dir))) return NULL;
    snprintf(path, sizeof(path), "%s/%s", dir, r->file);
    char *text = slurp(path);
    if (!text) return NULL;
    char hash[17];
    hash_text(text, hash);
    if (r->hash[0] && strcmp(hash, r->hash) != 0) { /* snapshot damaged */
        free(text);
        return NULL;
    }
    return text;
}
