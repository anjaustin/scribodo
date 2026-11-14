#define _DARWIN_C_SOURCE
#include "revisions.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/json.h"
#include "core/file_text.h"
#include "app/project_store.h"
#include "app/revisions_internal.h"
#include "app/store.h"

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

static bool write_atomic(const char *path, const char *data, size_t n) { return file_text_write_atomic(path, data, n, 0644); }

static char *slurp(const char *path)
{
    char *text = NULL;
    return file_text_read(path, &text) == FILE_TEXT_OK ? text : NULL;
}

/* Missing numbers read as NaN, which must never be cast to an integer. */
static double num(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    double d = cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    return d >= 0.0 && d < 1e15 ? d : 0.0;
}

static int revision_lock(const char *project_dir, const char *doc_path, bool create, int operation)
{
    char dir[1500], path[1600];
    if (!doc_dir(project_dir, doc_path, create, dir, sizeof(dir))) return -1;
    snprintf(path, sizeof(path), "%s/.lock", dir);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    /* Pre-upgrade read-only histories have no lock file. Since this process
     * cannot write that directory either, reading without a lock preserves
     * access without racing one of our writers. */
    if (fd < 0 && !create && (errno == EACCES || errno == EROFS)) return -2;
    if (fd < 0 && !create && errno == ENOENT) return -3;
    if (fd < 0) return -1;
    if (flock(fd, operation) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void revision_unlock(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

static bool list_impl(const char *project_dir, const char *doc_path, Revision **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char dir[1500], idx[1600];
    if (!doc_dir(project_dir, doc_path, false, dir, sizeof(dir))) return false;
    snprintf(idx, sizeof(idx), "%s/index.jsonl", dir);
    char *text = NULL;
    FileTextStatus read_status = file_text_read(idx, &text);
    if (read_status != FILE_TEXT_OK) return read_status == FILE_TEXT_MISSING;
    if (json_has_nul_escape(text, strlen(text))) { free(text); return false; }
    size_t n = 0, cap = 0;
    Revision *list = NULL;
    char *save = NULL;
    bool ok = true;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        bool allocation_error;
        cJSON *j = json_parse_checked(line, &allocation_error);
        if (allocation_error) { cJSON_Delete(j); ok = false; break; }
        if (!j) continue; /* torn line after a crash */
        cJSON *file = cJSON_GetObjectItem(j, "file"), *origin = cJSON_GetObjectItem(j, "origin");
        if (cJSON_IsString(file) && cJSON_IsString(origin) && !strchr(file->valuestring, '/')) {
            if (n == cap) {
                if (cap > SIZE_MAX / 2 / sizeof(Revision)) { cJSON_Delete(j); ok = false; break; }
                cap = cap ? cap * 2 : 16;
                Revision *l = realloc(list, cap * sizeof(Revision));
                if (!l) {
                    cJSON_Delete(j);
                    ok = false;
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
    if (!ok) { free(list); return false; }
    /* Newest first. */
    for (size_t i = 0; i < n / 2; i++) {
        Revision t = list[i];
        list[i] = list[n - 1 - i];
        list[n - 1 - i] = t;
    }
    *out = list;
    *count = n;
    return true;
}

bool revisions_list_files_checked(const char *project_dir, const char *doc_path, Revision **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    int lock = revision_lock(project_dir, doc_path, false, LOCK_SH);
    if (lock == -1) return false;
    bool ok = list_impl(project_dir, doc_path, out, count);
    revision_unlock(lock);
    return ok;
}

size_t revisions_list_files(const char *project_dir, const char *doc_path, Revision **out)
{
    size_t count = 0;
    (void)revisions_list_files_checked(project_dir, doc_path, out, &count);
    return count;
}

static bool record_impl(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, const char *origin,
                        const char *note, bool *recorded, char *file_out, size_t file_len)
{
    if (recorded) *recorded = false;
    char dir[1500];
    if (!doc_dir(project_dir, doc_path, true, dir, sizeof(dir))) return false;
    char hash[17];
    hash_text(text, hash);

    Revision *list = NULL;
    size_t n = 0;
    if (!list_impl(project_dir, doc_path, &list, &n)) return false;
    bool same = n > 0 && !strcmp(list[0].hash, hash);
    size_t reuse = 0;
    /* A snapshot for a source may reuse any revision with exactly this text:
     * an older text must never become the newest History entry. */
    for (size_t i = 1; !same && file_out && i < n; i++)
        if (!strcmp(list[i].hash, hash)) {
            reuse = i;
            same = true;
        }
    if (same) {
        char existing[1700];
        snprintf(existing, sizeof(existing), "%s/%s", dir, list[reuse].file);
        char *saved = slurp(existing);
        bool intact = saved && !strcmp(saved, text);
        free(saved);
        if (intact && file_out) snprintf(file_out, file_len, "%s", list[reuse].file);
        free(list);
        /* An index hash alone cannot establish a usable snapshot: a damaged entry is reported, not papered over
         * with a fresh copy (test_llm snapshot_reuse_integrity pins it; REDTEAM-022 R30 considered and kept). */
        return intact;
    }
    free(list);

    double wt = store_write_time(manager); /* now, or an export's original time */
    struct timeval tv = { .tv_sec = (time_t)wt, .tv_usec = (suseconds_t)((wt - (double)(time_t)wt) * 1e6) };
    struct tm tmv;
    time_t now = tv.tv_sec;
    localtime_r(&now, &tmv);
    char stamp[40], file[160], path[1700], idx[1600];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    /* Never replace a snapshot: two in one millisecond (an export of tied
     * times, a fast double save) take the next free name. */
    int ms = (int)(tv.tv_usec / 1000);
    for (int tries = 0;; tries++) {
        snprintf(file, sizeof(file), "%s-%03d-%s.md", stamp, ms, origin);
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        struct stat existing;
        if (lstat(path, &existing) != 0) {
            if (errno == ENOENT) break;
            return false;
        }
        if (tries == 999) return false; /* exhaustion must never replace history */
        if (++ms == 1000) { /* into the next second */
            ms = 0;
            now++;
            localtime_r(&now, &tmv);
            strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        }
    }
    size_t len = strlen(text);
    if (!write_atomic(path, text, len)) return false;

    cJSON *j = cJSON_CreateObject();
    bool built = j && cJSON_AddStringToObject(j, "file", file) && cJSON_AddStringToObject(j, "origin", origin) &&
                 cJSON_AddStringToObject(j, "note", note ? note : "") && cJSON_AddStringToObject(j, "hash", hash) &&
                 cJSON_AddNumberToObject(j, "ts", (double)now) && cJSON_AddNumberToObject(j, "bytes", (double)len);
    char *line = built ? cJSON_PrintUnformatted(j) : NULL;
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

bool revisions_record_files(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, const char *origin,
                            const char *note, bool *recorded)
{
    int lock = revision_lock(project_dir, doc_path, true, LOCK_EX);
    if (lock < 0) return false;
    bool ok = record_impl(manager, project_dir, doc_path, text, origin, note, recorded, NULL, 0);
    revision_unlock(lock);
    return ok;
}

bool revisions_snapshot_files(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                              char *file_out, size_t file_len)
{
    if (file_len) file_out[0] = '\0';
    int lock = revision_lock(project_dir, doc_path, true, LOCK_EX);
    if (lock < 0) return false;
    bool ok = record_impl(manager, project_dir, doc_path, text, "source", NULL, NULL, file_out, file_len) && file_out[0];
    revision_unlock(lock);
    return ok;
}

char *revisions_load_files(const char *project_dir, const char *doc_path, const Revision *r)
{
    int lock = revision_lock(project_dir, doc_path, false, LOCK_SH);
    if (lock == -1) return NULL;
    char dir[1500], path[1700];
    if (strchr(r->file, '/') || !doc_dir(project_dir, doc_path, false, dir, sizeof(dir))) {
        revision_unlock(lock);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, r->file);
    char *text = slurp(path);
    if (!text) {
        revision_unlock(lock);
        return NULL;
    }
    char hash[17];
    hash_text(text, hash);
    if (r->hash[0] && strcmp(hash, r->hash) != 0) { /* snapshot damaged */
        free(text);
        revision_unlock(lock);
        return NULL;
    }
    revision_unlock(lock);
    return text;
}

size_t revisions_list(StoreManager *manager, const char *project_dir, const char *doc_path, Revision **out)
{
    return project_store_revisions_list(manager, project_dir, doc_path, out);
}

bool revisions_list_checked(StoreManager *manager, const char *project_dir, const char *doc_path, Revision **out,
                            size_t *count)
{
    return project_store_revisions_list_checked(manager, project_dir, doc_path, out, count);
}

bool revisions_record(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, const char *origin,
                      const char *note, bool *recorded)
{
    if (recorded) *recorded = false;
    return project_store_revisions_record(manager, project_dir, doc_path, text, origin, note, recorded);
}

bool revisions_snapshot(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, char *file_out, size_t file_len)
{
    if (file_out && file_len) file_out[0] = '\0';
    return project_store_revisions_snapshot(manager, project_dir, doc_path, text, file_out, file_len);
}

char *revisions_load(StoreManager *manager, const char *project_dir, const char *doc_path, const Revision *r)
{
    return project_store_revisions_load(manager, project_dir, doc_path, r);
}
