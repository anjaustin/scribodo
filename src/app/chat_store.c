#define _DARWIN_C_SOURCE
#include "chat_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/json.h"
#include "core/file_text.h"
#include <limits.h>
#include "app/chat_store_internal.h"
#include "app/project_store.h"
#include "app/store.h"

bool chat_store_dir(const char *project_dir, char *out, size_t outlen)
{
    char base[PATH_MAX + 32];
    if (snprintf(base, sizeof(base), "%s/.scribodo", project_dir) >= (int)sizeof(base)) return false; /* never a cut path */
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

bool chat_store_append_files(StoreManager *manager, const char *project_dir, const char *id, const char *title, const ChatMessage *m)
{
    if (m->memory_sources && strnlen(m->memory_sources, CHAT_MEMORY_SOURCES_BYTES + 1) > CHAT_MEMORY_SOURCES_BYTES)
        return false;
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    struct stat st;
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return false;
    bool ok = ensure_trailing_newline(fd);
    if (fresh) {
        cJSON *meta = cJSON_CreateObject();
        ok = ok && meta && cJSON_AddStringToObject(meta, "type", "meta") && cJSON_AddNumberToObject(meta, "version", 1) &&
             cJSON_AddStringToObject(meta, "title", title ? title : "Chat") &&
             cJSON_AddNumberToObject(meta, "created", store_write_time(manager)) && write_line(fd, meta);
        cJSON_Delete(meta);
    }
    cJSON *j = cJSON_CreateObject();
    ok = ok && j && cJSON_AddStringToObject(j, "type", "message") &&
         cJSON_AddStringToObject(j, "role", m->role == CHAT_USER ? "user" : "assistant") &&
         cJSON_AddStringToObject(j, "content", m->content ? m->content : "") &&
         cJSON_AddNumberToObject(j, "ts", store_write_time(manager));
    if (ok && m->error) ok = cJSON_AddBoolToObject(j, "error", true);
    if (ok && m->is_edit) ok = cJSON_AddBoolToObject(j, "edit", true);
    if (ok && m->is_note) ok = cJSON_AddBoolToObject(j, "note", true);
    if (ok && m->truncated) ok = cJSON_AddBoolToObject(j, "truncated", true);
    if (ok && m->stopped) ok = cJSON_AddBoolToObject(j, "stopped", true);
    if (ok && m->via && m->via[0]) ok = cJSON_AddStringToObject(j, "via", m->via);
    if (ok && m->memory_sources) ok = cJSON_AddStringToObject(j, "memory_sources", m->memory_sources);
    /* The documents a user message was sent with; an empty list is recorded
     * too, so "nothing attached" is never confused with "not recorded". */
    if (ok && m->role == CHAT_USER && m->docs) {
        cJSON *arr = cJSON_AddArrayToObject(j, "docs");
        char *copy = strdup(m->docs);
        ok = arr && copy;
        for (char *save = NULL, *t = copy ? strtok_r(copy, "\n", &save) : NULL; ok && t; t = strtok_r(NULL, "\n", &save)) {
            cJSON *name = cJSON_CreateString(t);
            ok = name && cJSON_AddItemToArray(arr, name);
            if (!ok) cJSON_Delete(name);
        }
        free(copy);
    }
    ok = ok && write_line(fd, j);
    cJSON_Delete(j);
    ok = ok && fsync(fd) == 0;
    close(fd);
    return ok;
}

bool chat_store_rename_files(const char *project_dir, const char *id, const char *title)
{
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path)) || !title || !title[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false; /* only stored chats */
    int fd = open(path, O_RDWR | O_APPEND);
    if (fd < 0) return false;
    bool ok = ensure_trailing_newline(fd);
    cJSON *j = cJSON_CreateObject();
    ok = ok && j && cJSON_AddStringToObject(j, "type", "title") && cJSON_AddStringToObject(j, "title", title) &&
         cJSON_AddNumberToObject(j, "ts", (double)time(NULL));
    ok = ok && write_line(fd, j);
    cJSON_Delete(j);
    ok = ok && fsync(fd) == 0;
    close(fd);
    return ok;
}

bool chat_store_set_observer_files(const char *project_dir, const char *id, bool read)
{
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false; /* only stored chats */
    int fd = open(path, O_RDWR | O_APPEND);
    if (fd < 0) return false;
    bool ok = ensure_trailing_newline(fd);
    cJSON *j = cJSON_CreateObject();
    ok = ok && j && cJSON_AddStringToObject(j, "type", "observer") && cJSON_AddBoolToObject(j, "read", read) &&
         cJSON_AddNumberToObject(j, "ts", (double)time(NULL));
    ok = ok && write_line(fd, j);
    cJSON_Delete(j);
    ok = ok && fsync(fd) == 0;
    close(fd);
    return ok;
}

/* Validate the descriptor we actually read, not a racy stat-before-fopen.
 * Nonblocking open also prevents a replaced log/FIFO from hanging readers. */
static FILE *open_regular(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode) || st->st_size < 0 || (uint64_t)st->st_size >= SIZE_MAX) {
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return NULL; }
    return f;
}

/* Read a whole file (chat logs are small). */
static char *slurp(const char *path)
{
    char *text = NULL;
    if (file_text_read(path, &text) != FILE_TEXT_OK) return NULL;
    if (json_has_nul_escape(text, strlen(text))) { free(text); return NULL; }
    return text;
}

static int by_updated_desc(const void *a, const void *b)
{
    const ChatSummary *x = a, *y = b;
    if (y->updated != x->updated) return (y->updated > x->updated) - (y->updated < x->updated);
    return (y->updated_nsec > x->updated_nsec) - (y->updated_nsec < x->updated_nsec);
}

bool chat_store_list_files_checked(const char *project_dir, ChatSummary **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char dir[1100];
    if (!chat_store_dir(project_dir, dir, sizeof(dir))) return false;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT;
    size_t n = 0, cap = 0;
    ChatSummary *list = NULL;
    struct dirent *ent;
    bool read_ok = true;
    for (;;) {
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            read_ok = errno == 0;
            break;
        }
        size_t len = strlen(ent->d_name);
        if (len < 7 || strcmp(ent->d_name + len - 6, ".jsonl") != 0 || len - 6 >= sizeof(list->id)) continue;
        char path[1300];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        FILE *f = open_regular(path, &st);
        if (!f) { read_ok = false; break; }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            ChatSummary *l = realloc(list, cap * sizeof(ChatSummary));
            if (!l) {
                fclose(f);
                free(list);
                closedir(d);
                return false;
            }
            list = l;
        }
        ChatSummary *c = &list[n];
        memset(c, 0, sizeof(*c));
        memcpy(c->id, ent->d_name, len - 6);
        c->updated = st.st_mtimespec.tv_sec;
        c->updated_nsec = st.st_mtimespec.tv_nsec;
        snprintf(c->title, sizeof(c->title), "Chat %s", c->id);
        /* Title from the meta line, or the latest rename. */
        {
            char *line = NULL;
            size_t cap_l = 0;
            bool first = true, lines_ok = true;
            while (getline(&line, &cap_l, f) > 0) { /* lines of any length */
                if (first || strstr(line, "\"type\":\"title\"")) {
                    bool allocation_error;
                    cJSON *j = json_parse_checked(line, &allocation_error);
                    if (allocation_error) { cJSON_Delete(j); lines_ok = false; break; }
                    cJSON *t = j ? cJSON_GetObjectItem(j, "title") : NULL;
                    if (t && cJSON_IsString(t)) snprintf(c->title, sizeof(c->title), "%s", t->valuestring);
                    cJSON_Delete(j);
                } else if (strstr(line, "\"type\":\"observer\"")) { /* the latest one counts */
                    bool allocation_error;
                    cJSON *j = json_parse_checked(line, &allocation_error);
                    if (allocation_error) { cJSON_Delete(j); lines_ok = false; break; }
                    cJSON *rd = j ? cJSON_GetObjectItem(j, "read") : NULL;
                    if (cJSON_IsBool(rd)) c->observer_ignore = !cJSON_IsTrue(rd);
                    cJSON_Delete(j);
                }
                first = false;
            }
            lines_ok = lines_ok && feof(f) && !ferror(f);
            free(line);
            if (fclose(f) != 0) lines_ok = false;
            if (!lines_ok) { read_ok = false; break; }
        }
        n++;
    }
    bool ok = closedir(d) == 0 && read_ok;
    if (!ok) {
        free(list);
        return false;
    }
    qsort(list, n, sizeof(ChatSummary), by_updated_desc);
    *out = list;
    *count = n;
    return true;
}

size_t chat_store_list_files(const char *project_dir, ChatSummary **out)
{
    size_t count = 0;
    (void)chat_store_list_files_checked(project_dir, out, &count);
    return count;
}

bool chat_store_load_files(const char *project_dir, const char *id, ChatMessage **msgs, size_t *n)
{
    *msgs = NULL;
    *n = 0;
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    char *text = slurp(path);
    if (!text) return false;
    size_t cap = 0;
    bool ok = true;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line && ok; line = strtok_r(NULL, "\n", &save)) {
        bool allocation_error;
        cJSON *j = json_parse_checked(line, &allocation_error);
        if (allocation_error) { cJSON_Delete(j); ok = false; break; }
        if (!j) {
            /* A torn line from a crash, possibly glued to a later record by an
             * older version: recover the last complete record in it. */
            char *rec = strstr(line, "{\"type\"");
            char *last = NULL;
            while (rec) {
                last = rec;
                rec = strstr(rec + 1, "{\"type\"");
            }
            if (last && last != line) j = json_parse_checked(last, &allocation_error);
            if (allocation_error) { cJSON_Delete(j); ok = false; break; }
            if (!j) continue;
        }
        cJSON *type = cJSON_GetObjectItem(j, "type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "message")) {
            cJSON *role = cJSON_GetObjectItem(j, "role"), *content = cJSON_GetObjectItem(j, "content");
            if (cJSON_IsString(role) && cJSON_IsString(content) &&
                (!strcmp(role->valuestring, "user") || !strcmp(role->valuestring, "assistant"))) {
                if (*n == cap) {
                    size_t next = cap ? cap * 2 : 16;
                    if (next < cap || next > SIZE_MAX / sizeof(ChatMessage)) ok = false;
                    ChatMessage *m = ok ? realloc(*msgs, next * sizeof(ChatMessage)) : NULL;
                    if (!m) {
                        ok = false;
                        cJSON_Delete(j);
                        break;
                    }
                    *msgs = m;
                    cap = next;
                }
                ChatMessage *message = &(*msgs)[(*n)++];
                *message = (ChatMessage){ .role = strcmp(role->valuestring, "user") ? CHAT_ASSISTANT : CHAT_USER,
                                          .content = strdup(content->valuestring),
                                           .error = cJSON_IsTrue(cJSON_GetObjectItem(j, "error")),
                                          .is_edit = cJSON_IsTrue(cJSON_GetObjectItem(j, "edit")),
                                          .is_note = cJSON_IsTrue(cJSON_GetObjectItem(j, "note")),
                                          .truncated = cJSON_IsTrue(cJSON_GetObjectItem(j, "truncated")),
                                          .stopped = cJSON_IsTrue(cJSON_GetObjectItem(j, "stopped")) };
                ok = message->content != NULL;
                cJSON *docs = cJSON_GetObjectItem(j, "docs"), *it;
                size_t len = 1;
                cJSON_ArrayForEach(it, docs) if (cJSON_IsString(it)) {
                    size_t item_len = strlen(it->valuestring);
                    if (item_len > SIZE_MAX - len - 1) {
                        ok = false;
                        break;
                    }
                    len += item_len + 1;
                }
                char *joined = ok && len > 1 ? malloc(len) : NULL;
                if (joined) {
                    joined[0] = '\0';
                    cJSON_ArrayForEach(it, docs)
                    if (cJSON_IsString(it) && !strchr(it->valuestring, '\n')) {
                        if (joined[0]) strcat(joined, "\n");
                        strcat(joined, it->valuestring);
                    }
                }
                if (ok && len > 1 && !joined) ok = false;
                if (!joined && ok && cJSON_IsArray(docs)) joined = strdup(""); /* recorded: none */
                if (ok && cJSON_IsArray(docs) && !joined) ok = false;
                message->docs = joined;
                cJSON *via = cJSON_GetObjectItem(j, "via");
                message->via = cJSON_IsString(via) && via->valuestring[0] ? strdup(via->valuestring) : NULL;
                if (ok && cJSON_IsString(via) && via->valuestring[0] && !message->via) ok = false;
                cJSON *sources = cJSON_GetObjectItem(j, "memory_sources");
                if (sources && !cJSON_IsNull(sources)) {
                    if (!cJSON_IsString(sources) || strlen(sources->valuestring) > CHAT_MEMORY_SOURCES_BYTES) ok = false;
                    else if (ok) {
                        message->memory_sources = strdup(sources->valuestring);
                        if (!message->memory_sources) ok = false;
                    }
                }
                cJSON *ts = cJSON_GetObjectItem(j, "ts");
                message->ts = ts ? cJSON_GetNumberValue(ts) : 0; /* legacy logs may omit it */
                if (!isfinite(message->ts) || message->ts < 0 || message->ts >= 0x1p63) ok = false;
            } else ok = false; /* never hide a recognized but invalid message */
        }
        cJSON_Delete(j);
    }
    free(text);
    if (!ok) {
        chat_store_free_messages(*msgs, *n);
        *msgs = NULL;
        *n = 0;
    }
    return ok;
}

bool chat_store_append(StoreManager *manager, const char *project_dir, const char *id, const char *title, const ChatMessage *message)
{
    return project_store_chat_append(manager, project_dir, id, title, message);
}

bool chat_store_append_sequenced(StoreManager *manager, const char *project_dir, const char *id, const char *title,
                                const ChatMessage *message, int64_t *sequence)
{
    return project_store_chat_append_sequenced(manager, project_dir, id, title, message, sequence);
}

bool chat_store_window(StoreManager *manager, const char *project_dir, const char *id,
                        const ChatWindowRequest *request, ChatWindowPage *out)
{
    return project_store_chat_window(manager, project_dir, id, request, out);
}

bool chat_store_rename(StoreManager *manager, const char *project_dir, const char *id, const char *title)
{
    return project_store_chat_rename(manager, project_dir, id, title);
}

bool chat_store_set_observer(StoreManager *manager, const char *project_dir, const char *id, bool read)
{
    return project_store_chat_set_observer(manager, project_dir, id, read);
}

size_t chat_store_list(StoreManager *manager, const char *project_dir, ChatSummary **out)
{
    return project_store_chat_list(manager, project_dir, out);
}

bool chat_store_list_checked(StoreManager *manager, const char *project_dir, ChatSummary **out, size_t *count)
{
    return project_store_chat_list_checked(manager, project_dir, out, count);
}

bool chat_store_load(StoreManager *manager, const char *project_dir, const char *id, ChatMessage **messages, size_t *count)
{
    return project_store_chat_load(manager, project_dir, id, messages, count);
}

bool chat_store_source_page(StoreManager *manager, const char *project_dir, const char *id,
                            const ChatSourceCursor *cursor, ChatSourcePage *out)
{
    return project_store_chat_source_page(manager, project_dir, id, cursor, out);
}

void chat_store_free_messages(ChatMessage *msgs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(msgs[i].content);
        free(msgs[i].docs);
        free(msgs[i].via);
        free(msgs[i].memory_sources);
    }
    free(msgs);
}
