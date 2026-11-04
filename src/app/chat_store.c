#define _DARWIN_C_SOURCE
#include "chat_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

bool chat_store_dir(const char *project_dir, char *out, size_t outlen)
{
    char base[1024];
    snprintf(base, sizeof(base), "%s/.scribodo", project_dir);
    if (mkdir(base, 0755) != 0 && errno != EEXIST) return false;
    snprintf(out, outlen, "%s/chats", base);
    if (mkdir(out, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

void chat_store_new_id(char *out, size_t outlen)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(out, outlen, "%s-%04x", stamp, arc4random_uniform(0x10000));
}

static bool path_for(const char *project_dir, const char *id, char *out, size_t outlen)
{
    for (const char *p = id; *p; p++) /* ids are generated, but never trust paths */
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-')) return false;
    char dir[1100];
    if (!chat_store_dir(project_dir, dir, sizeof(dir))) return false;
    snprintf(out, outlen, "%s/%s.jsonl", dir, id);
    return true;
}

/* One write() per line (JSON plus newline), so a crash can tear at most
 * that line, never merge it with the next. */
static bool write_line(int fd, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    if (!s) return false;
    size_t n = strlen(s);
    char *line = malloc(n + 2);
    bool ok = line != NULL;
    if (ok) {
        memcpy(line, s, n);
        line[n] = '\n';
        line[n + 1] = '\0';
        ok = write(fd, line, n + 1) == (ssize_t)(n + 1);
    }
    free(line);
    free(s);
    return ok;
}

/* If a previous crash left the file without a trailing newline, terminate
 * that torn line so the next record starts on its own line. */
static bool ensure_trailing_newline(int fd)
{
    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) return true;
    char last;
    if (pread(fd, &last, 1, end - 1) != 1) return false;
    return last == '\n' || write(fd, "\n", 1) == 1;
}

bool chat_store_append(const char *project_dir, const char *id, const char *title, const ChatMessage *m)
{
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return false;
    bool ok = ensure_trailing_newline(fd);
    if (fresh) {
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "type", "meta");
        cJSON_AddNumberToObject(meta, "version", 1);
        cJSON_AddStringToObject(meta, "title", title ? title : "Chat");
        cJSON_AddNumberToObject(meta, "created", (double)time(NULL));
        ok = write_line(fd, meta);
        cJSON_Delete(meta);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "message");
    cJSON_AddStringToObject(j, "role", m->role == CHAT_USER ? "user" : "assistant");
    cJSON_AddStringToObject(j, "content", m->content ? m->content : "");
    cJSON_AddNumberToObject(j, "ts", (double)time(NULL));
    if (m->error) cJSON_AddBoolToObject(j, "error", true);
    if (m->is_edit) cJSON_AddBoolToObject(j, "edit", true);
    if (m->docs && m->docs[0]) { /* the documents this message was sent with */
        cJSON *arr = cJSON_AddArrayToObject(j, "docs");
        char *copy = strdup(m->docs);
        for (char *save = NULL, *t = copy ? strtok_r(copy, "\n", &save) : NULL; t; t = strtok_r(NULL, "\n", &save))
            cJSON_AddItemToArray(arr, cJSON_CreateString(t));
        free(copy);
    }
    ok = ok && write_line(fd, j);
    cJSON_Delete(j);
    ok = ok && fsync(fd) == 0;
    close(fd);
    return ok;
}

bool chat_store_rename(const char *project_dir, const char *id, const char *title)
{
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path)) || !title || !title[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false; /* only stored chats */
    int fd = open(path, O_RDWR | O_APPEND);
    if (fd < 0) return false;
    bool ok = ensure_trailing_newline(fd);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "title");
    cJSON_AddStringToObject(j, "title", title);
    cJSON_AddNumberToObject(j, "ts", (double)time(NULL));
    ok = ok && write_line(fd, j);
    cJSON_Delete(j);
    ok = ok && fsync(fd) == 0;
    close(fd);
    return ok;
}

/* Read a whole file (chat logs are small). */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (buf) {
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static int by_updated_desc(const void *a, const void *b)
{
    const ChatSummary *x = a, *y = b;
    return (y->updated > x->updated) - (y->updated < x->updated);
}

size_t chat_store_list(const char *project_dir, ChatSummary **out)
{
    *out = NULL;
    char dir[1100];
    if (!chat_store_dir(project_dir, dir, sizeof(dir))) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0, cap = 0;
    ChatSummary *list = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 7 || strcmp(ent->d_name + len - 6, ".jsonl") != 0 || len - 6 >= sizeof(list->id)) continue;
        char path[1300];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            ChatSummary *l = realloc(list, cap * sizeof(ChatSummary));
            if (!l) break;
            list = l;
        }
        ChatSummary *c = &list[n];
        memset(c, 0, sizeof(*c));
        memcpy(c->id, ent->d_name, len - 6);
        c->updated = st.st_mtimespec.tv_sec;
        snprintf(c->title, sizeof(c->title), "Chat %s", c->id);
        /* Title from the meta line, or the latest rename. */
        FILE *f = fopen(path, "rb");
        if (f) {
            char line[4096];
            bool first = true;
            while (fgets(line, sizeof(line), f)) {
                if (first || strstr(line, "\"type\":\"title\"")) {
                    cJSON *j = cJSON_Parse(line);
                    cJSON *t = j ? cJSON_GetObjectItem(j, "title") : NULL;
                    if (cJSON_IsString(t)) snprintf(c->title, sizeof(c->title), "%s", t->valuestring);
                    cJSON_Delete(j);
                }
                first = false;
                /* long lines: skip the rest of this line */
                while (!strchr(line, '\n') && fgets(line, sizeof(line), f)) {}
            }
            fclose(f);
        }
        n++;
    }
    closedir(d);
    qsort(list, n, sizeof(ChatSummary), by_updated_desc);
    *out = list;
    return n;
}

bool chat_store_load(const char *project_dir, const char *id, ChatMessage **msgs, size_t *n)
{
    *msgs = NULL;
    *n = 0;
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    char *text = slurp(path);
    if (!text) return false;
    size_t cap = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        cJSON *j = cJSON_Parse(line);
        if (!j) {
            /* A torn line from a crash, possibly glued to a later record by an
             * older version: recover the last complete record in it. */
            char *rec = strstr(line, "{\"type\"");
            char *last = NULL;
            while (rec) {
                last = rec;
                rec = strstr(rec + 1, "{\"type\"");
            }
            if (last && last != line) j = cJSON_Parse(last);
            if (!j) continue;
        }
        cJSON *type = cJSON_GetObjectItem(j, "type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "message")) {
            cJSON *role = cJSON_GetObjectItem(j, "role"), *content = cJSON_GetObjectItem(j, "content");
            if (cJSON_IsString(role) && cJSON_IsString(content)) {
                if (*n == cap) {
                    cap = cap ? cap * 2 : 16;
                    ChatMessage *m = realloc(*msgs, cap * sizeof(ChatMessage));
                    if (!m) {
                        cJSON_Delete(j);
                        break;
                    }
                    *msgs = m;
                }
                (*msgs)[*n] = (ChatMessage){ .role = strcmp(role->valuestring, "user") ? CHAT_ASSISTANT : CHAT_USER,
                                             .content = strdup(content->valuestring),
                                             .error = cJSON_IsTrue(cJSON_GetObjectItem(j, "error")),
                                             .is_edit = cJSON_IsTrue(cJSON_GetObjectItem(j, "edit")) };
                cJSON *docs = cJSON_GetObjectItem(j, "docs"), *it;
                size_t len = 1;
                cJSON_ArrayForEach(it, docs) if (cJSON_IsString(it)) len += strlen(it->valuestring) + 1;
                char *joined = len > 1 ? malloc(len) : NULL;
                if (joined) {
                    joined[0] = '\0';
                    cJSON_ArrayForEach(it, docs)
                    if (cJSON_IsString(it) && !strchr(it->valuestring, '\n')) {
                        if (joined[0]) strcat(joined, "\n");
                        strcat(joined, it->valuestring);
                    }
                }
                (*msgs)[*n].docs = joined;
                (*n)++;
            }
        }
        cJSON_Delete(j);
    }
    free(text);
    return true;
}

void chat_store_free_messages(ChatMessage *msgs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(msgs[i].content);
        free(msgs[i].docs);
    }
    free(msgs);
}
