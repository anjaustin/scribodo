#define _DARWIN_C_SOURCE
#include "recent.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

static char g_dir[PATH_MAX];

void recent_set_dir(const char *dir) { snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : ""); }

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static size_t load_all(RecentProject *out, size_t max, bool existing_only)
{
    if (!g_dir[0]) return 0;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/recent.json", g_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf), *it;
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

size_t recent_load(RecentProject *out, size_t max) { return load_all(out, max, true); }

void recent_touch(const char *project, const char *doc)
{
    if (!g_dir[0] || !project || project[0] != '/') return;
    RecentProject list[RECENT_MAX + 1];
    size_t n = load_all(list, RECENT_MAX, false);
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
    mkdir(g_dir, 0755);
    char path[PATH_MAX + 32], tmp[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/recent.json", g_dir);
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
