#define _DARWIN_C_SOURCE
#include "recent.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "core/json.h"

struct RecentStore {
    char dir[PATH_MAX];
    pthread_mutex_t mutex;
};

static int recent_lock(RecentStore *store, int operation);
static void recent_unlock(int fd);

RecentStore *recent_store_create(const char *dir)
{
    RecentStore *store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    snprintf(store->dir, sizeof(store->dir), "%s", dir ? dir : "");
    if (pthread_mutex_init(&store->mutex, NULL) != 0) {
        free(store);
        return NULL;
    }
    return store;
}

void recent_store_destroy(RecentStore *store)
{
    if (!store) return;
    pthread_mutex_destroy(&store->mutex);
    free(store);
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* *corrupt is set when the file exists but can't be read as a list. */
static size_t load_all(RecentStore *store, RecentProject *out, size_t max, bool existing_only, bool *corrupt)
{
    if (corrupt) *corrupt = false;
    if (!store || !store->dir[0]) return 0;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/recent.json", store->dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = json_parse(buf), *it;
    if (corrupt && n > 0 && !cJSON_IsArray(cJSON_GetObjectItem(j, "projects"))) *corrupt = true;
    size_t k = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(j, "projects"))
    {
        if (k == max) break;
        cJSON *p = cJSON_GetObjectItem(it, "path"), *d = cJSON_GetObjectItem(it, "doc");
        if (!cJSON_IsString(p) || p->valuestring[0] != '/') continue;
        if (existing_only && !is_dir(p->valuestring)) continue;
        snprintf(out[k].path, sizeof(out[k].path), "%s", p->valuestring);
        snprintf(out[k].doc, sizeof(out[k].doc), "%s", cJSON_IsString(d) && !strchr(d->valuestring, '/') ? d->valuestring : "");
        k++;
    }
    cJSON_Delete(j);
    return k;
}

size_t recent_store_load(RecentStore *store, RecentProject *out, size_t max)
{
    if (!store) return 0;
    pthread_mutex_lock(&store->mutex);
    int lock = recent_lock(store, LOCK_SH);
    size_t count = lock >= 0 ? load_all(store, out, max, true, NULL) : 0;
    recent_unlock(lock);
    pthread_mutex_unlock(&store->mutex);
    return count;
}

static void mkdir_p(const char *dir)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = '\0';
            mkdir(p, 0755);
            *c = '/';
        }
    mkdir(p, 0755);
}

static int recent_lock(RecentStore *store, int operation)
{
    mkdir_p(store->dir);
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/recent.lock", store->dir);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (flock(fd, operation) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void recent_unlock(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

static void save_list(RecentStore *store, RecentProject *list, size_t n);

void recent_store_touch(RecentStore *store, const char *project, const char *doc)
{
    if (!store || !store->dir[0] || !project || project[0] != '/') return;
    pthread_mutex_lock(&store->mutex);
    int lock = recent_lock(store, LOCK_EX);
    if (lock < 0) {
        pthread_mutex_unlock(&store->mutex);
        return;
    }
    RecentProject list[RECENT_MAX + 1];
    bool corrupt = false;
    /* Folders that are gone don't keep their places (red-team 006). */
    size_t n = load_all(store, list, RECENT_MAX, true, &corrupt);
    if (corrupt) { /* keep the unreadable file for inspection, never overwrite it silently */
        char a[PATH_MAX + 32], b[PATH_MAX + 64];
        snprintf(a, sizeof(a), "%s/recent.json", store->dir);
        snprintf(b, sizeof(b), "%s/recent.json.corrupt-%ld", store->dir, (long)time(NULL));
        rename(a, b);
    }
    RecentProject cur = { 0 };
    snprintf(cur.path, sizeof(cur.path), "%s", project);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(list[i].path, project)) {
            snprintf(cur.doc, sizeof(cur.doc), "%s", list[i].doc);
            memmove(&list[i], &list[i + 1], (n - i - 1) * sizeof(RecentProject));
            n--;
            break;
        }
    if (doc) snprintf(cur.doc, sizeof(cur.doc), "%s", doc);
    memmove(&list[1], &list[0], n * sizeof(RecentProject));
    list[0] = cur;
    n = n + 1 > RECENT_MAX ? RECENT_MAX : n + 1;
    save_list(store, list, n);
    recent_unlock(lock);
    pthread_mutex_unlock(&store->mutex);
}

void recent_store_rename_doc(RecentStore *store, const char *project, const char *from, const char *to)
{
    if (!store) return;
    pthread_mutex_lock(&store->mutex);
    int lock = recent_lock(store, LOCK_EX);
    if (lock < 0) {
        pthread_mutex_unlock(&store->mutex);
        return;
    }
    RecentProject list[RECENT_MAX + 1];
    size_t n = load_all(store, list, RECENT_MAX, true, NULL);
    bool changed = false;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(list[i].path, project) && !strcmp(list[i].doc, from)) {
            snprintf(list[i].doc, sizeof(list[i].doc), "%s", to);
            changed = true;
        }
    if (changed) save_list(store, list, n);
    recent_unlock(lock);
    pthread_mutex_unlock(&store->mutex);
}

static void save_list(RecentStore *store, RecentProject *list, size_t n)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "projects");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", list[i].path);
        cJSON_AddStringToObject(o, "doc", list[i].doc);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return;
    mkdir_p(store->dir);
    char path[PATH_MAX + 32], tmp[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/recent.json", store->dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd >= 0) {
        size_t len = strlen(s);
        bool ok = fchmod(fd, 0644) == 0 && write(fd, s, len) == (ssize_t)len && fsync(fd) == 0;
        ok = close(fd) == 0 && ok;
        if (!ok || rename(tmp, path) != 0) unlink(tmp);
    }
    free(s);
}
