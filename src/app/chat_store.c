#define _DARWIN_C_SOURCE
#include "chat_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/json.h"
#include "core/file_text.h"
#include "core/utf8.h"
#include <float.h>
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
    if (!title || !title[0]) return false; /* a rename names something */
    return chat_store_title_line_files(project_dir, id, title, (double)time(NULL));
}

bool chat_store_title_line_files(const char *project_dir, const char *id, const char *title, double ts)
{
    char path[1200];
    /* An empty title is a title (an archived one, or a current one the export writes back); only a rename refuses it. */
    if (!path_for(project_dir, id, path, sizeof(path)) || !title) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false; /* only stored chats */
    int fd = open(path, O_RDWR | O_APPEND);
    if (fd < 0) return false;
    bool ok = ensure_trailing_newline(fd);
    cJSON *j = cJSON_CreateObject();
    ok = ok && j && cJSON_AddStringToObject(j, "type", "title") && cJSON_AddStringToObject(j, "title", title) &&
         cJSON_AddNumberToObject(j, "ts", ts);
    ok = ok && write_line(fd, j);
    cJSON_Delete(j);
    ok = ok && fsync(fd) == 0;
    close(fd);
    return ok;
}

/* A setting line appended to a stored chat's file ({"type":…,…,"ts":…}; the latest one counts). The file keeps its
 * modified time, which orders the list: a setting is not activity, as in the store, where only messages and renames
 * move a chat's updated_at (user request 2026-11-17: filing a chat must not move it to the top). Takes `j`. */
static bool append_setting_line(const char *project_dir, const char *id, cJSON *j)
{
    char path[1200];
    bool ok = j && path_for(project_dir, id, path, sizeof(path)) && cJSON_AddNumberToObject(j, "ts", (double)time(NULL));
    int fd = ok ? open(path, O_RDWR | O_APPEND) : -1; /* only stored chats: never created here */
    struct stat st;
    ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && ensure_trailing_newline(fd);
    off_t before = ok ? lseek(fd, 0, SEEK_END) : -1;
    ok = ok && write_line(fd, j) && fsync(fd) == 0;
    char *line = ok ? cJSON_PrintUnformatted(j) : NULL;
    struct stat now;
    /* Only when the file holds exactly what it did plus this line: a message another process appended meanwhile
     * keeps the time it gave the file (red team 2026-11-17). */
    bool alone = line && fstat(fd, &now) == 0 && before >= 0 && now.st_size == before + (off_t)strlen(line) + 1;
    free(line);
    if (ok && alone) {
        struct timespec times[2] = { st.st_atimespec, st.st_mtimespec };
        (void)futimens(fd, times); /* best effort: a failure only reorders the list */
    }
    if (fd >= 0) close(fd);
    cJSON_Delete(j);
    return ok;
}

static bool append_flag_line(const char *project_dir, const char *id, const char *type, const char *key, bool value)
{
    cJSON *j = cJSON_CreateObject();
    if (!j || !cJSON_AddStringToObject(j, "type", type) || !cJSON_AddBoolToObject(j, key, value)) {
        cJSON_Delete(j);
        return false;
    }
    return append_setting_line(project_dir, id, j);
}

bool chat_store_set_observer_files(const char *project_dir, const char *id, bool read)
{
    return append_flag_line(project_dir, id, "observer", "read", read);
}

bool chat_store_set_archived_files(const char *project_dir, const char *id, bool archived)
{
    return append_flag_line(project_dir, id, "archive", "archived", archived);
}

/* Characters a name may not hold: bidi overrides and isolates, the zero-width space and marks that only steer
 * direction, line separators, the BOM, the C1 controls (red team round 2: an override could make one heading read
 * as another). The joiners (U+200C, U+200D) and the soft hyphen stay: Persian and Indic names and emoji need them
 * (round 3). */
static bool invisible_control(uint32_t cp)
{
    return (cp >= 0x80 && cp <= 0x9F) || cp == 0x61C || cp == 0xE0001 ||
           cp == 0x200B || cp == 0x200E || cp == 0x200F || (cp >= 0x2028 && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB);
}

static bool blank(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

/* Allowed but drawn as nothing on their own: joiners, the soft hyphen, variation selectors, fillers (round 4). */
static bool zero_width(uint32_t cp)
{
    return cp == 0xAD || cp == 0x34F || cp == 0x200C || cp == 0x200D || (cp >= 0x180B && cp <= 0x180F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x115F || cp == 0x1160 || cp == 0x3164 || cp == 0xFFA0 || cp == 0x2800 ||
           (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0xE0020 && cp <= 0xE007F); /* tags: the subdivision flags */
}

/* Something to see, and, its blanks and zero-width characters left out, not the Archived heading's name (ASCII
 * case; "Arch ived" is refused too). */
static bool name_shows(const char *s, size_t n, bool reserve)
{
    char bare[48];
    size_t b = 0;
    bool visible = false;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int used = utf8_decode_one((const uint8_t *)s + i, n - i, &cp);
        if (used <= 0) break;
        if (!zero_width(cp) && !blank(cp)) { /* compared without padding of any kind (round 5) */
            visible = true;
            if (b + (size_t)used < sizeof(bare)) { memcpy(bare + b, s + i, (size_t)used); b += (size_t)used; }
        }
        i += (size_t)used;
    }
    bare[b] = '\0';
    return visible && !(reserve && !strcasecmp(bare, "Archived"));
}

/* The name's own bytes as kept, or false: every character allowed (see invisible_control). */
static bool name_chars(const char *in, size_t n, char out[48], size_t *len)
{
    size_t o = 0, last_visible_end = 0;
    bool started = false;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int used = utf8_decode_one((const uint8_t *)in + i, n - i, &cp);
        /* The decoder answers an invalid sequence with U+FFFD; only the real one (EF BF BD) is a character here. */
        bool invalid = cp == 0xFFFD && !(used == 3 && !memcmp(in + i, "\xEF\xBF\xBD", 3));
        if (used <= 0 || invalid || (cp < 0x20 && cp != '\t') || cp == 0x7F || (cp >= 0xD800 && cp <= 0xDFFF) ||
            invisible_control(cp))
            return false;
        if (!started && blank(cp)) { i += (size_t)used; continue; } /* leading blanks of any kind */
        started = true;
        if (o + (size_t)used > CHAT_GROUP_MAX) break; /* cut between characters */
        memcpy(out + o, in + i, (size_t)used);
        o += (size_t)used;
        if (!blank(cp)) last_visible_end = o;
        i += (size_t)used;
    }
    *len = last_visible_end; /* trailing blanks of any kind go, as does a cut's */
    out[*len] = '\0';
    return !memchr(out, '\t', *len); /* a tab only at the ends */
}

bool chat_group_name_clean(const char *in, char out[48])
{
    out[0] = '\0';
    size_t o = 0;
    if (!in || !name_chars(in, strlen(in), out, &o) || !o || !name_shows(out, o, true)) {
        out[0] = '\0';
        return false; /* refused, nothing visible, or the Archived heading's name */
    }
    return true;
}

bool chat_group_name_stored(const char *in, char out[48])
{
    /* A group already kept is shown, even under a rule added since (round 3): only its characters are checked. */
    out[0] = '\0';
    size_t o = 0;
    if (chat_group_name_clean(in, out)) return true;
    if (!in || !name_chars(in, strlen(in), out, &o) || !o || !name_shows(out, o, false)) { out[0] = '\0'; return false; }
    return true;
}

bool chat_store_set_group_files(const char *project_dir, const char *id, const char *group)
{
    char clean[48];
    if (group && group[0] && !chat_group_name_stored(group, clean)) return false;
    if (!group || !group[0]) clean[0] = '\0';
    cJSON *j = cJSON_CreateObject();
    if (!j || !cJSON_AddStringToObject(j, "type", "group") || !cJSON_AddStringToObject(j, "group", clean)) {
        cJSON_Delete(j);
        return false;
    }
    return append_setting_line(project_dir, id, j);
}

bool chat_store_rename_group_files(const char *project_dir, const char *from, const char *to)
{
    ChatSummary *list = NULL;
    size_t n = 0;
    if (!chat_store_list_files_checked(project_dir, &list, &n)) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++)
        if (!strcmp(list[i].group, from)) ok = chat_store_set_group_files(project_dir, list[i].id, to);
    free(list);
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

/* Read a whole file (chat logs are small). An escaped NUL is refused line by line by the reader: in a message it would
 * truncate the author's text, so the message (and the chat) is refused; elsewhere it is marked (nul_escapes_marked). */
static char *slurp(const char *path)
{
    char *text = NULL;
    if (file_text_read(path, &text) != FILE_TEXT_OK) return NULL;
    return text;
}

/* Every escaped NUL ("\u0000") in a JSON line becomes U+FFFD ("\uFFFD", the same length): cJSON would end the string
 * at the NUL and drop the rest. Escapes are skipped whole, so an escaped backslash before "u0000" is not one. True if
 * any was marked. */
static bool nul_escapes_marked(char *line)
{
    bool marked = false;
    for (char *p = line; *p; p++) {
        if (*p != '\\' || !p[1]) continue;
        if (!strncmp(p + 1, "u0000", 5)) {
            memcpy(p + 2, "FFFD", 4);
            marked = true;
        }
        p++;
    }
    return marked;
}

/* A title as the store keeps it (AUDIT-004 verification round 2): valid UTF-8, each invalid sequence or surrogate a
 * U+FFFD, at most 159 bytes, cut between characters. The current title (ChatSummary) and every archived one get the
 * same, so no title line can make an import or an export fail; the original stays on its line in the file. */
static void title_clean(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0, n = strlen(src);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int used = utf8_decode_one((const uint8_t *)src + i, n - i, &cp);
        if (used <= 0) break;
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        uint8_t enc[4];
        int len = utf8_encode_one(cp, enc);
        if (o + (size_t)len + 1 > dstlen) break;
        memcpy(dst + o, enc, (size_t)len);
        o += (size_t)len;
        i += (size_t)used;
    }
    dst[o] = '\0';
}

/* A title line's time, in the range the store can keep and read back: finite, 0 or normal (a subnormal is written but
 * not read back by db_cell_double), below 2^63. */
static bool title_time_ok(double ts) { return isfinite(ts) && ts >= 0 && ts < 0x1p63 && (ts == 0 || ts >= DBL_MIN); }

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
                    nul_escapes_marked(line);
                    cJSON *j = json_parse_checked(line, &allocation_error);
                    if (allocation_error) { cJSON_Delete(j); lines_ok = false; break; }
                    cJSON *t = j ? cJSON_GetObjectItem(j, "title") : NULL;
                    if (t && cJSON_IsString(t)) title_clean(c->title, sizeof(c->title), t->valuestring);
                    cJSON_Delete(j);
                } else if (strstr(line, "\"type\":\"observer\"")) { /* the latest one counts */
                    bool allocation_error;
                    cJSON *j = json_parse_checked(line, &allocation_error);
                    if (allocation_error) { cJSON_Delete(j); lines_ok = false; break; }
                    cJSON *rd = j ? cJSON_GetObjectItem(j, "read") : NULL;
                    if (cJSON_IsBool(rd)) c->observer_ignore = !cJSON_IsTrue(rd);
                    cJSON_Delete(j);
                } else if (strstr(line, "\"type\":\"archive\"")) { /* the latest one counts */
                    bool allocation_error;
                    cJSON *j = json_parse_checked(line, &allocation_error);
                    if (allocation_error) { cJSON_Delete(j); lines_ok = false; break; }
                    cJSON *ty = j ? cJSON_GetObjectItem(j, "type") : NULL, *ar = j ? cJSON_GetObjectItem(j, "archived") : NULL;
                    if (ty && cJSON_IsString(ty) && ty->valuestring && !strcmp(ty->valuestring, "archive") && cJSON_IsBool(ar)) c->archived = cJSON_IsTrue(ar);
                    cJSON_Delete(j);
                } else if (strstr(line, "\"type\":\"group\"")) { /* the latest one counts */
                    bool allocation_error;
                    cJSON *j = json_parse_checked(line, &allocation_error);
                    if (allocation_error) { cJSON_Delete(j); lines_ok = false; break; }
                    cJSON *ty = j ? cJSON_GetObjectItem(j, "type") : NULL, *g = j ? cJSON_GetObjectItem(j, "group") : NULL;
                    if (ty && cJSON_IsString(ty) && ty->valuestring && !strcmp(ty->valuestring, "group") && g && cJSON_IsString(g) &&
                        g->valuestring) {
                        char clean[48];
                        if (!g->valuestring[0]) c->group[0] = '\0';
                        else if (chat_group_name_stored(g->valuestring, clean)) memcpy(c->group, clean, sizeof(clean));
                    }
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

void chat_store_titles_free(ChatTitle *titles, size_t count)
{
    for (size_t i = 0; titles && i < count; i++) free(titles[i].title);
    free(titles);
}

/* The same lines chat_store_list_files_checked reads a title from: the first line's "title", then every title line;
 * a line that does not parse carries no title there either. */
bool chat_store_titles_files(const char *project_dir, const char *id, ChatTitle **titles, size_t *count)
{
    *titles = NULL;
    *count = 0;
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    return chat_store_titles_path(path, titles, count);
}

bool chat_store_titles_path(const char *path, ChatTitle **titles, size_t *count)
{
    *titles = NULL;
    *count = 0;
    struct stat st;
    FILE *f = open_regular(path, &st);
    if (!f) return false;
    ChatTitle *list = NULL;
    size_t n = 0, cap = 0;
    char *line = NULL;
    size_t cap_l = 0;
    bool first = true, ok = true;
    while (ok && getline(&line, &cap_l, f) > 0) {
        if (first || strstr(line, "\"type\":\"title\"")) {
            bool allocation_error;
            nul_escapes_marked(line);
            cJSON *j = json_parse_checked(line, &allocation_error);
            if (allocation_error) ok = false;
            cJSON *t = j ? cJSON_GetObjectItem(j, "title") : NULL, *ts = j ? cJSON_GetObjectItem(j, "ts") : NULL;
            if (ok && t && cJSON_IsString(t)) {
                if (n == cap) {
                    size_t next = cap ? cap * 2 : 4;
                    ChatTitle *m = next > SIZE_MAX / sizeof(ChatTitle) ? NULL : realloc(list, next * sizeof(ChatTitle));
                    if (!m) ok = false;
                    else {
                        list = m;
                        cap = next;
                    }
                }
                char *copy = ok ? malloc(sizeof(((ChatSummary *)0)->title)) : NULL;
                if (copy) title_clean(copy, sizeof(((ChatSummary *)0)->title), t->valuestring);
                if (!copy) ok = false;
                else {
                    /* The message lines' range (chat_store_load_files): a time outside it is refused, the title kept
                     * with no time, so an out-of-range stamp never reaches replaced_at, which every export, backup and
                     * fork decodes with that range (AUDIT-004 F11 red team). The line itself stays in the file. */
                    bool timed = !first && cJSON_IsNumber(ts) && title_time_ok(ts->valuedouble);
                    list[n++] = (ChatTitle){ .title = copy, .ts = timed ? ts->valuedouble : 0, .has_ts = timed };
                }
            }
            cJSON_Delete(j);
        }
        first = false;
    }
    ok = ok && feof(f) && !ferror(f);
    free(line);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        chat_store_titles_free(list, n);
        return false;
    }
    *titles = list;
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
    return chat_store_load_path(path, msgs, n);
}

bool chat_store_load_path(const char *path, ChatMessage **msgs, size_t *n)
{
    *msgs = NULL;
    *n = 0;
    char *text = slurp(path);
    if (!text) return false;
    size_t cap = 0;
    bool ok = true;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line && ok; line = strtok_r(NULL, "\n", &save)) {
        bool allocation_error, nul = nul_escapes_marked(line);
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
        if (nul && cJSON_IsString(type) && !strcmp(type->valuestring, "message")) ok = false; /* the author's text: never cut */
        else if (cJSON_IsString(type) && !strcmp(type->valuestring, "message")) {
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

size_t chat_store_resave_failed(StoreManager *manager, ChatSession *chat, const char *project_dir, const char *id,
                                const char *title, bool *ok)
{
    if (ok) *ok = true;
    if (!chat || chat_busy(chat)) return 0;
    chat_lock(chat);
    size_t n = chat_count(chat), k = 0;
    ChatMessage *copy = calloc(n ? n : 1, sizeof(ChatMessage));
    size_t *at = calloc(n ? n : 1, sizeof(size_t));
    for (size_t i = 0; copy && at && i < n; i++) {
        const ChatMessage *m = chat_message(chat, i);
        if (!m->save_failed) continue;
        copy[k] = *m;
        copy[k].content = strdup(m->content ? m->content : "");
        at[k++] = i;
    }
    chat_unlock(chat);
    bool saved = true;
    for (size_t j = 0; saved && j < k; j++) {
        int64_t sequence = -1;
        saved = copy[j].content && chat_store_append_sequenced(manager, project_dir, id, title, &copy[j], &sequence);
        if (saved && sequence >= 0) chat_mark_saved(chat, at[j], sequence);
    }
    for (size_t j = 0; copy && j < k; j++) free(copy[j].content);
    free(copy);
    free(at);
    if (ok) *ok = saved;
    return k;
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

bool chat_store_set_archived(StoreManager *manager, const char *project_dir, const char *id, bool archived)
{
    return project_store_chat_set_archived(manager, project_dir, id, archived);
}

bool chat_store_delete_files(const char *project_dir, const char *id, ChatDeleteStats *out)
{
    if (out) memset(out, 0, sizeof(*out));
    char path[1200];
    if (!path_for(project_dir, id, path, sizeof(path))) return false;
    ChatMessage *msgs = NULL;
    size_t n = 0;
    if (chat_store_load_path(path, &msgs, &n)) chat_store_free_messages(msgs, n);
    if (unlink(path) != 0) return false;
    if (out) out->messages = n;
    return true;
}

bool chat_store_delete(StoreManager *manager, const char *project_dir, const char *id, ChatDeleteStats *out)
{
    return project_store_chat_delete(manager, project_dir, id, out);
}

bool chat_store_set_group(StoreManager *manager, const char *project_dir, const char *id, const char *group)
{
    char clean[48] = "";
    if (group && group[0] && !chat_group_name_stored(group, clean)) return false;
    return project_store_chat_set_group(manager, project_dir, id, clean);
}

bool chat_store_rename_group(StoreManager *manager, const char *project_dir, const char *from, const char *to)
{
    char f[48], t[48] = "";
    if (!chat_group_name_stored(from, f) || (to && to[0] && !chat_group_name_stored(to, t))) return false;
    return project_store_chat_rename_group(manager, project_dir, f, t);
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
