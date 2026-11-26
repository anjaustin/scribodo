#define _DARWIN_C_SOURCE
#include "revisions.h"

#include <errno.h>
#include <dirent.h>
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
#include "core/crash_point.h"
#include "app/project_store.h"
#include "app/revisions_internal.h"
#include "app/store.h"
#include "app/renames.h"

static void hash_text(const char *s, char out[17])
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 1099511628211ull;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

double revisions_file_time(const char *file, time_t index_ts)
{
    if (!file) return 0;
    struct tm tmv = {0};
    const char *rest = strptime(file, "%Y%m%d-%H%M%S", &tmv);
    if (!rest || strlen(rest) < 4 || rest[0] != '-' ||
        rest[1] < '0' || rest[1] > '9' || rest[2] < '0' || rest[2] > '9' ||
        rest[3] < '0' || rest[3] > '9') return index_ts > 0 ? (double)index_ts : 0;
    int ms = (rest[1] - '0') * 100 + (rest[2] - '0') * 10 + rest[3] - '0';
    if (strlen(rest) >= 22 && rest[4] == '-' && rest[5] == 't') {
        uint64_t micros = 0;
        for (size_t i = 6; i < 22; i++) {
            unsigned char c = (unsigned char)rest[i];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (v < 0) return 0;
            micros = (micros << 4) | (unsigned)v;
        }
        return rest[22] == '-' ? (double)micros / 1e6 : 0;
    }
    if (index_ts > 0) return (double)index_ts + ms / 1000.0;
    tmv.tm_isdst = -1;
    return (double)mktime(&tmv) + ms / 1000.0;
}

/* Ordinary paths stay compatible. Names the old writer would transform use a
 * separate namespace with the complete literal basename, on the project filesystem.
 * A format tag is created for a new/empty history, or an explicit author rename
 * into the new layout. It asserts path format, never historical semantic truth. */
static const char exact_format[] = "exact-v1\n";

static bool join_path(const char *a, const char *b, char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!a || !b) return false;
    int n = snprintf(out, cap, "%s/%s", a, b);
    if (n >= 0 && (size_t)n < cap) return true;
    out[0] = '\0';
    errno = ENAMETOOLONG;
    return false;
}

static const char *document_base(const char *path)
{
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t n = strnlen(base, 256);
    return n && n < 256 && base[0] != '.' ? base : NULL;
}

static void old_key(const char *base, char out[200])
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)base; *p && n < 199; p++)
        out[n++] = *p < 0x20 || *p == ':' ? '_' : (char)*p;
    out[n] = '\0';
}

static bool exceptional_name(const char *base)
{
    if (strlen(base) >= 199) return true;
    for (const unsigned char *p = (const unsigned char *)base; *p; p++)
        if (*p < 0x20 || *p == ':') return true;
    return false;
}

static bool directory(const char *path, bool create)
{
    if (create && mkdir(path, 0755) != 0 && errno != EEXIST) return false;
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Missing is not malformed. No allocation, and never follow a planted marker. */
static int format_tag(const char *dir)
{
    char path[1600], text[sizeof(exact_format)];
    if (!join_path(dir, ".format", path, sizeof(path))) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size == sizeof(exact_format) - 1 &&
              read(fd, text, sizeof(text)) == sizeof(exact_format) - 1 &&
              !memcmp(text, exact_format, sizeof(exact_format) - 1);
    if (close(fd)) ok = false;
    return ok ? 1 : -1;
}

static bool empty_history(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return false;
    int directory_fd = dirfd(dir);
    if (directory_fd < 0) { closedir(dir); return false; }
    bool empty = true;
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { if (errno) empty = false; break; }
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..") && strcmp(ent->d_name, ".lock")) {
            /* A crash before publishing a format tag may leave only its derived
             * temporary. Preserve it, but allow exact-prefix marker bytes to resume. */
            const char prefix[] = ".format.tmp-";
            bool temporary = strlen(ent->d_name) == sizeof(prefix) - 1 + 6 &&
                             !strncmp(ent->d_name, prefix, sizeof(prefix) - 1);
            for (size_t i = sizeof(prefix) - 1; temporary && ent->d_name[i]; i++) {
                unsigned char c = (unsigned char)ent->d_name[i];
                temporary = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            }
            int fd = temporary ? openat(directory_fd, ent->d_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC) : -1;
            struct stat st;
            char bytes[sizeof(exact_format)];
            temporary = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
                        st.st_size <= (off_t)(sizeof(exact_format) - 1) &&
                        read(fd, bytes, sizeof(bytes)) == st.st_size &&
                        !memcmp(bytes, exact_format, (size_t)st.st_size);
            if (fd >= 0 && close(fd)) temporary = false;
            if (!temporary) { empty = false; break; }
        }
    }
    if (closedir(dir)) empty = false;
    return empty;
}

static bool tag_new_history(const char *dir)
{
    char path[1600], temporary[1600];
    if (!join_path(dir, ".format", path, sizeof(path)) ||
        !join_path(dir, ".format.tmp-XXXXXX", temporary, sizeof(temporary))) return false;
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    bool ok = fchmod(fd, 0644) == 0 &&
              write(fd, exact_format, sizeof(exact_format) - 1) == sizeof(exact_format) - 1 && fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok) crash_point("history_move.tag_written");
    if (ok) ok = renamex_np(temporary, path, RENAME_EXCL) == 0;
    if (!ok) { unlink(temporary); return false; } /* only this call's derived temporary */
    crash_point("history_move.tag_published");
    int parent = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return false;
    if (fsync(parent)) ok = false;
    if (close(parent)) ok = false;
    return ok;
}

/* The index never stored original ownership. Preserve ordinary literal legacy
 * labels, but refuse a known competing lossy name. Never infer ownership from text. */
static bool known_alias(const char *project, const char *base)
{
    DIR *dir = opendir(project);
    if (!dir) return true; /* cannot establish an ambiguity-free legacy read */
    bool alias = false;
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { if (errno) alias = true; break; }
        if (ent->d_name[0] == '.' || !strcmp(ent->d_name, base)) continue;
        char key[200];
        old_key(ent->d_name, key);
        if (!strcmp(key, base)) {
            char path[1600];
            struct stat st;
            if (!join_path(project, ent->d_name, path, sizeof(path)) ||
                (lstat(path, &st) == 0 && S_ISREG(st.st_mode))) { alias = true; break; }
        }
    }
    if (closedir(dir)) alias = true;
    return alias;
}

/* Caller holds the revisions-root lock. A lossy legacy lookup is a refusal,
 * not a fallback, even when a new-format path could be created beside it. */
static bool doc_dir(const char *project_dir, const char *doc_path, bool create, char *out, size_t outlen)
{
    if (outlen) out[0] = '\0';
    const char *base = document_base(doc_path);
    if (!base) return false;
    char root[1300], meta[1200], legacy[1600], key[200], canonical[1500];
    if (!join_path(project_dir, ".scribodo", meta, sizeof(meta)) ||
        !join_path(meta, "revisions", root, sizeof(root))) return false;
    old_key(base, key);
    if (!join_path(root, key, legacy, sizeof(legacy))) return false;
    bool exceptional = exceptional_name(base);
    char parent[1400];
    if (exceptional) {
        if (!join_path(root, ".exact-v1", parent, sizeof(parent))) return false;
        struct stat st;
        if (lstat(parent, &st) == 0) { if (!S_ISDIR(st.st_mode)) return false; }
        else if (errno != ENOENT) return false;
    } else snprintf(parent, sizeof(parent), "%s", root);
    if (!join_path(parent, base, canonical, sizeof(canonical))) return false;
    struct stat st;
    if (exceptional && lstat(legacy, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || format_tag(legacy) != 1) { errno = EILSEQ; return false; }
    } else if (exceptional && errno != ENOENT) return false;
    bool exists = lstat(canonical, &st) == 0;
    if (!exists && errno != ENOENT) return false;
    if (exists && !S_ISDIR(st.st_mode)) return false;
    int tag = exists ? format_tag(canonical) : 0;
    if (tag < 0) return false;
    bool empty = exists && create && !tag && empty_history(canonical);
    if (exists && !tag && !empty && (exceptional || known_alias(project_dir, base))) {
        errno = EILSEQ;
        return false;
    }
    if (create) {
        if (!directory(parent, true) || !directory(canonical, true)) return false;
        if (!tag && (!exists || empty) && !tag_new_history(canonical)) return false;
    }
    return join_path(parent, base, out, outlen);
}

static bool write_atomic(const char *path, const char *data, size_t n) { return file_text_write_atomic(path, data, n, 0644); }

static char *slurp(const char *path)
{
    char *text = NULL;
    return file_text_read_no_follow(path, &text) == FILE_TEXT_OK ? text : NULL;
}

/* Missing numbers read as NaN, which must never be cast to an integer. */
static double num(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    double d = cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    return d >= 0.0 && d < 1e15 ? d : 0.0;
}

static int history_root_lock_raw(const char *project_dir, bool create, int operation)
{
    char meta[1200], root[1300];
    if (!join_path(project_dir, ".scribodo", meta, sizeof(meta)) ||
        !join_path(meta, "revisions", root, sizeof(root))) return -1;
    if (create && (!directory(meta, true) || !directory(root, true))) return -1;
    struct stat st;
    if (lstat(meta, &st) == 0) { if (!S_ISDIR(st.st_mode)) return -1; }
    else if (errno != ENOENT) return -1;
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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

static bool recover_move(const char *project, const char *from, const char *to, bool with_file,
                         bool *matched, bool *file_moved);

static int history_root_lock(const char *project, bool create, int operation)
{
    int fd = history_root_lock_raw(project, create, operation);
    if (fd < 0) return fd;
    char path[1500];
    int n = snprintf(path, sizeof(path), "%s/.scribodo/revisions/.pending-move.json", project);
    struct stat st;
    if (n < 0 || (size_t)n >= sizeof(path)) { revision_unlock(fd); return -1; }
    if (lstat(path, &st) == 0) {
        /* Release, reacquire exclusively, then reread the receipt. No resolved path/data
         * from the shared-lock interval is reused; never upgrade the project guard. */
        if (operation != LOCK_EX && (flock(fd, LOCK_UN) || flock(fd, LOCK_EX))) {
            revision_unlock(fd); return -1;
        }
        if (!recover_move(project, NULL, NULL, false, NULL, NULL) ||
            (operation != LOCK_EX && flock(fd, operation))) { revision_unlock(fd); return -1; }
    } else if (errno != ENOENT) { revision_unlock(fd); return -1; }
    return fd;
}

static int revision_lock(const char *project_dir, const char *doc_path, bool create, int operation,
                         char *dir, size_t dirlen)
{
    int fd = history_root_lock(project_dir, create, operation);
    if (fd == -1) return -1;
    if (!doc_dir(project_dir, doc_path, create, dir, dirlen)) {
        revision_unlock(fd);
        return -1;
    }
    return fd;
}

bool revisions_recover_files(const char *project)
{
    int lock = history_root_lock(project, false, LOCK_SH);
    if (lock == -1) return false;
    revision_unlock(lock);
    return true;
}

bool revisions_history_path_files(const char *project, const char *doc, char *out, size_t cap)
{
    int lock = history_root_lock(project, false, LOCK_SH);
    if (lock == -1) { if (cap) out[0] = '\0'; return false; }
    bool ok = doc_dir(project, doc, false, out, cap);
    revision_unlock(lock);
    return ok;
}

bool revisions_history_same_files(const char *project, const char *a, const char *b)
{
    int lock = history_root_lock(project, false, LOCK_SH);
    if (lock < 0) return false;
    char x[1500], y[1500];
    struct stat xs, ys;
    bool same = doc_dir(project, a, false, x, sizeof(x)) && doc_dir(project, b, false, y, sizeof(y)) &&
                lstat(x, &xs) == 0 && lstat(y, &ys) == 0 && S_ISDIR(xs.st_mode) && S_ISDIR(ys.st_mode) &&
                xs.st_dev == ys.st_dev && xs.st_ino == ys.st_ino;
    revision_unlock(lock);
    return same;
}

static bool history_name_add(const char *project, const char *name, const char *physical,
                             RevisionHistoryName **out, size_t *count, size_t *capacity)
{
    char resolved[1500];
    if (!doc_dir(project, name, false, resolved, sizeof(resolved)) || strcmp(resolved, physical)) return false;
    for (size_t i = 0; i < *count; i++) if (!strcmp((*out)[i].name, name)) return false;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 16;
        if (next < *capacity || next > SIZE_MAX / sizeof(**out)) return false;
        RevisionHistoryName *grown = realloc(*out, next * sizeof(**out));
        if (!grown) return false;
        *out = grown;
        *capacity = next;
    }
    snprintf((*out)[(*count)++].name, sizeof((*out)[0].name), "%s", name);
    return true;
}

static bool scan_histories(const char *project, const char *root, bool exact,
                           RevisionHistoryName **out, size_t *count, size_t *capacity)
{
    DIR *dir = opendir(root);
    if (!dir) return errno == ENOENT;
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { ok = errno == 0; break; }
        if (!strcmp(ent->d_name, ".exact-v1") && !exact) {
            char child[1500];
            ok = join_path(root, ent->d_name, child, sizeof(child)) && directory(child, false) &&
                 scan_histories(project, child, true, out, count, capacity);
        } else if (ent->d_name[0] != '.') {
            char path[1500];
            ok = document_base(ent->d_name) && join_path(root, ent->d_name, path, sizeof(path)) &&
                 directory(path, false) && history_name_add(project, ent->d_name, path, out, count, capacity);
        }
    }
    if (closedir(dir)) ok = false;
    return ok;
}

bool revisions_history_names_files(const char *project, RevisionHistoryName **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    int lock = history_root_lock(project, false, LOCK_SH);
    if (lock == -1) return false;
    if (lock == -3) return true;
    char meta[1200], root[1300];
    size_t capacity = 0;
    bool ok = join_path(project, ".scribodo", meta, sizeof(meta)) &&
              join_path(meta, "revisions", root, sizeof(root)) &&
              scan_histories(project, root, false, out, count, &capacity);
    revision_unlock(lock);
    if (!ok) { free(*out); *out = NULL; *count = 0; }
    return ok;
}

typedef struct { bool enabled; uint64_t device, inode; } MoveIdentity;
typedef struct {
    char from[256], to[256], operation[33];
    double when;
    MoveIdentity file, history;
} HistoryMove;

static bool sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    return ok;
}

static bool sync_parent(const char *path)
{
    char parent[1600];
    if (strlen(path) >= sizeof(parent)) return false;
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = '\0';
    return sync_directory(parent);
}

static bool move_paths(const char *project, const char *name, bool history, char *out, size_t cap)
{
    if (!document_base(name) || strchr(name, '/')) return false;
    if (!history) return join_path(project, name, out, cap);
    char root[1400];
    int n = snprintf(root, sizeof(root), "%s/.scribodo/revisions%s", project,
                     exceptional_name(name) ? "/.exact-v1" : "");
    return n > 0 && (size_t)n < sizeof(root) && join_path(root, name, out, cap);
}

static bool matches_move(const struct stat *st, const MoveIdentity *id, bool history)
{
    return (history ? S_ISDIR(st->st_mode) : (S_ISREG(st->st_mode) && st->st_nlink == 1)) &&
           (uint64_t)st->st_dev == id->device && (uint64_t)st->st_ino == id->inode;
}

static bool move_object(const char *project, const HistoryMove *move, const MoveIdentity *id, bool history)
{
    if (!id->enabled) return true;
    char from[1500], to[1500], parent[1500];
    if (!move_paths(project, move->from, history, from, sizeof(from)) ||
        !move_paths(project, move->to, history, to, sizeof(to))) return false;
    if (history && (!sync_parent(from))) return false; /* reject symlinked exceptional parents */
    struct stat a, b;
    bool have_a = lstat(from, &a) == 0;
    if (!have_a && errno != ENOENT) return false;
    bool have_b = lstat(to, &b) == 0;
    if (!have_b && errno != ENOENT) return false;
    if (have_b && matches_move(&b, id, history) && (!have_a || !matches_move(&a, id, history)))
        return (!history || (format_tag(to) >= 0 && (!exceptional_name(move->to) || format_tag(to) == 1))) &&
               sync_parent(from) && sync_parent(to); /* the move completed before interruption */
    if (!have_a || !matches_move(&a, id, history) || (have_b && !matches_move(&b, id, history))) return false;
    snprintf(parent, sizeof(parent), "%s", to);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = '\0';
    if (!directory(parent, history)) return false;
    if (history) {
        int tag = format_tag(from);
        if (tag < 0 || (exceptional_name(move->to) && !tag && !tag_new_history(from)) || !sync_directory(from)) return false;
    }
    if (renamex_np(from, to, have_b ? 0 : RENAME_EXCL)) return false;
    return sync_parent(from) && sync_parent(to);
}

static bool identity_json(cJSON *j, const char *key, const MoveIdentity *id)
{
    char device[17], inode[17];
    snprintf(device, sizeof(device), "%016llx", (unsigned long long)id->device);
    snprintf(inode, sizeof(inode), "%016llx", (unsigned long long)id->inode);
    cJSON *value = cJSON_AddObjectToObject(j, key);
    return value && cJSON_AddBoolToObject(value, "enabled", id->enabled) &&
           cJSON_AddStringToObject(value, "device", device) && cJSON_AddStringToObject(value, "inode", inode);
}

static bool parse_hex(const char *s, size_t count, uint64_t *out)
{
    if (!s || strlen(s) != count) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned char c = (unsigned char)s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return false;
        value = (value << 4) | (unsigned)v;
    }
    if (out) *out = value;
    return true;
}

static bool identity_parse(cJSON *j, const char *key, MoveIdentity *out)
{
    cJSON *value = cJSON_GetObjectItem(j, key), *enabled = cJSON_GetObjectItem(value, "enabled");
    cJSON *device = cJSON_GetObjectItem(value, "device"), *inode = cJSON_GetObjectItem(value, "inode");
    if (!cJSON_IsBool(enabled) || !cJSON_IsString(device) || !cJSON_IsString(inode) ||
        !parse_hex(device->valuestring, 16, &out->device) || !parse_hex(inode->valuestring, 16, &out->inode)) return false;
    out->enabled = cJSON_IsTrue(enabled);
    return !out->enabled || out->inode != 0;
}

static bool receipt_path(const char *project, char *out, size_t cap)
{
    char root[1400];
    int n = snprintf(root, sizeof(root), "%s/.scribodo/revisions", project);
    return n > 0 && (size_t)n < sizeof(root) && join_path(root, ".pending-move.json", out, cap);
}

/* Root lock is exclusive. A valid receipt is never guessed from a filename:
 * exact inode/device guards, full names, operation id and timestamp survive replay. */
static bool recover_move(const char *project, const char *from, const char *to, bool with_file,
                         bool *matched, bool *file_moved)
{
    if (matched) *matched = false;
    char path[1500], *text = NULL;
    if (!receipt_path(project, path, sizeof(path))) return false;
    FileTextStatus status = file_text_read_no_follow(path, &text);
    if (status != FILE_TEXT_OK) return status == FILE_TEXT_MISSING;
    HistoryMove move = {0};
    cJSON *j = json_parse_complete(text, strlen(text));
    cJSON *version = cJSON_GetObjectItem(j, "version"), *a = cJSON_GetObjectItem(j, "from");
    cJSON *b = cJSON_GetObjectItem(j, "to"), *id = cJSON_GetObjectItem(j, "operation"), *when = cJSON_GetObjectItem(j, "ts");
    bool ok = cJSON_IsNumber(version) && version->valuedouble == 1 &&
              cJSON_IsString(a) && cJSON_IsString(b) && cJSON_IsString(id) && cJSON_IsNumber(when) &&
              when->valuedouble >= 0 && when->valuedouble < 0x1p63 &&
              document_base(a->valuestring) && !strchr(a->valuestring, '/') &&
              document_base(b->valuestring) && !strchr(b->valuestring, '/') &&
              parse_hex(id->valuestring, 32, NULL) &&
              identity_parse(j, "file", &move.file) && identity_parse(j, "history", &move.history);
    if (ok) {
        snprintf(move.from, sizeof(move.from), "%s", a->valuestring);
        snprintf(move.to, sizeof(move.to), "%s", b->valuestring);
        snprintf(move.operation, sizeof(move.operation), "%s", id->valuestring);
        move.when = when->valuedouble;
    }
    cJSON_Delete(j);
    free(text);
    if (!ok || !sync_parent(path)) return false;
    bool requested = from && to && move.file.enabled == with_file &&
                     !strcmp(from, move.from) && !strcmp(to, move.to);
    bool file_ok = move_object(project, &move, &move.file, false);
    if (requested && with_file && file_moved) {
        /* A retry may move the file but fail later, including after an uncertain
         * directory sync. Its editor still has to follow the validated inode. */
        char target[1500];
        struct stat st;
        *file_moved = move_paths(project, move.to, false, target, sizeof(target)) &&
                      lstat(target, &st) == 0 && matches_move(&st, &move.file, false);
    }
    if (!file_ok) return false;
    crash_point("history_move.file");
    if (!move_object(project, &move, &move.history, true)) return false;
    crash_point("history_move.history");
    if (!renames_record_once(project, move.from, move.to, move.when, move.operation)) return false;
    char log[1500];
    int n = snprintf(log, sizeof(log), "%s/.scribodo/renames.jsonl", project);
    if (n < 0 || (size_t)n >= sizeof(log) || !sync_parent(log)) return false;
    crash_point("history_move.log");
    if (unlink(path) || !sync_parent(path)) return false;
    crash_point("history_move.done");
    if (matched) *matched = requested;
    return true;
}

static bool prepare_move(const char *project, const char *from, const char *to, bool with_file, bool *file_moved)
{
    if (file_moved) *file_moved = false;
    HistoryMove move = {0};
    if (!document_base(from) || strchr(from, '/') || !document_base(to) || strchr(to, '/')) return false;
    snprintf(move.from, sizeof(move.from), "%s", from);
    snprintf(move.to, sizeof(move.to), "%s", to);
    unsigned char random[16];
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) snprintf(move.operation + i * 2, 3, "%02x", random[i]);
    struct timeval tv;
    if (gettimeofday(&tv, NULL)) return false;
    move.when = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    char source[1500], target[1500], receipt[1500];
    if (!doc_dir(project, from, false, source, sizeof(source)) ||
        !doc_dir(project, to, false, target, sizeof(target)) ||
        !receipt_path(project, receipt, sizeof(receipt))) return false;
    struct stat st;
    if (lstat(source, &st) == 0) move.history = (MoveIdentity){true, (uint64_t)st.st_dev, (uint64_t)st.st_ino};
    else if (errno != ENOENT) return false;
    if (with_file) {
        if (!join_path(project, from, source, sizeof(source)) || lstat(source, &st) ||
            !S_ISREG(st.st_mode) || st.st_nlink != 1) return false;
        move.file = (MoveIdentity){true, (uint64_t)st.st_dev, (uint64_t)st.st_ino};
        if (!join_path(project, to, target, sizeof(target))) return false;
        if (lstat(target, &st) == 0) { if (!matches_move(&st, &move.file, false)) return false; }
        else if (errno != ENOENT) return false;
    }
    cJSON *j = cJSON_CreateObject();
    bool built = j && cJSON_AddNumberToObject(j, "version", 1) && cJSON_AddStringToObject(j, "from", from) &&
                 cJSON_AddStringToObject(j, "to", to) && cJSON_AddStringToObject(j, "operation", move.operation) &&
                 cJSON_AddNumberToObject(j, "ts", move.when) &&
                 identity_json(j, "file", &move.file) && identity_json(j, "history", &move.history);
    char *text = built ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    bool published = text && file_text_write_atomic(receipt, text, strlen(text), 0600);
    bool ok = published && sync_parent(receipt);
    free(text);
    if (ok) {
        crash_point("history_move.prepared");
        ok = recover_move(project, from, to, with_file, NULL, NULL);
    }
    if (published && with_file && file_moved && join_path(project, to, target, sizeof(target)) && lstat(target, &st) == 0)
        *file_moved = matches_move(&st, &move.file, false);
    return ok;
}

static bool history_aside_locked(const char *project, const char *name)
{
    char from[1500];
    if (!doc_dir(project, name, false, from, sizeof(from))) return false;
    struct stat st;
    if (lstat(from, &st)) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return false;
    /* A bounded virtual name, not a truncated document identity. The complete original
     * name stays in the dated rename record and all snapshot/index bytes are retained. */
    unsigned char random[16];
    arc4random_buf(random, sizeof(random));
    char alias[80] = "history-", hex[33];
    for (size_t i = 0; i < sizeof(random); i++) snprintf(hex + i * 2, 3, "%02x", random[i]);
    snprintf(alias, sizeof(alias), "history-%s.earlier", hex);
    return prepare_move(project, name, alias, false, NULL);
}

bool revisions_history_set_aside_files(const char *project, const char *name)
{
    int lock = history_root_lock(project, true, LOCK_EX);
    if (lock < 0) return false;
    bool ok = history_aside_locked(project, name);
    revision_unlock(lock);
    return ok;
}

bool revisions_history_move_files(const char *project, const char *from, const char *to)
{
    int lock = history_root_lock_raw(project, true, LOCK_EX);
    if (lock < 0) return false;
    bool matched = false;
    bool ok = recover_move(project, from, to, false, &matched, NULL);
    if (ok && matched) { revision_unlock(lock); return true; }
    char source[1500], target[1500];
    ok = ok && doc_dir(project, from, false, source, sizeof(source)) &&
         doc_dir(project, to, false, target, sizeof(target));
    struct stat a, b;
    bool same = ok && lstat(source, &a) == 0 && lstat(target, &b) == 0 &&
                a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    if (ok && !same) ok = history_aside_locked(project, to);
    if (ok) ok = prepare_move(project, from, to, false, NULL);
    revision_unlock(lock);
    return ok;
}

bool revisions_document_rename_files(const char *project, const char *from, const char *to, bool *file_moved)
{
    if (file_moved) *file_moved = false;
    int lock = history_root_lock_raw(project, true, LOCK_EX);
    if (lock < 0) return false;
    bool matched = false;
    bool ok = recover_move(project, from, to, true, &matched, file_moved);
    if (ok && matched) {
        if (file_moved) *file_moved = true;
        revision_unlock(lock);
        return true;
    }
    char source[1500], target[1500], file[1500];
    struct stat a, b;
    /* Check the manuscript before archiving destination metadata: a refused rename
     * must not gratuitously change an unrelated destination's history. */
    ok = ok && join_path(project, from, file, sizeof(file)) && lstat(file, &a) == 0 &&
         S_ISREG(a.st_mode) && a.st_nlink == 1;
    if (ok) {
        ok = join_path(project, to, file, sizeof(file));
        if (ok && lstat(file, &b) == 0) ok = S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
        else if (ok && errno != ENOENT) ok = false;
    }
    ok = ok && doc_dir(project, from, false, source, sizeof(source)) && doc_dir(project, to, false, target, sizeof(target));
    bool same = ok && lstat(source, &a) == 0 && lstat(target, &b) == 0 &&
                a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    if (ok && !same) ok = history_aside_locked(project, to);
    if (ok) ok = prepare_move(project, from, to, true, file_moved);
    revision_unlock(lock);
    return ok;
}

static bool list_impl(const char *dir, Revision **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char idx[1600];
    if (!join_path(dir, "index.jsonl", idx, sizeof(idx))) return false;
    char *text = NULL;
    FileTextStatus read_status = file_text_read_no_follow(idx, &text);
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
            if (!file->valuestring[0] || file->valuestring[0] == '.' ||
                strlen(file->valuestring) >= sizeof(((Revision *)0)->file) ||
                strlen(origin->valuestring) >= sizeof(((Revision *)0)->origin)) {
                cJSON_Delete(j);
                ok = false;
                break;
            }
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
    char dir[1500];
    int lock = revision_lock(project_dir, doc_path, false, LOCK_SH, dir, sizeof(dir));
    if (lock == -1) return false;
    bool ok = list_impl(dir, out, count);
    revision_unlock(lock);
    return ok;
}

size_t revisions_list_files(const char *project_dir, const char *doc_path, Revision **out)
{
    size_t count = 0;
    (void)revisions_list_files_checked(project_dir, doc_path, out, &count);
    return count;
}

static bool record_impl(StoreManager *manager, const char *dir, const char *text, const char *origin,
                        const char *note, bool *recorded, char *file_out, size_t file_len)
{
    if (recorded) *recorded = false;
    if (!text || !origin || !origin[0] || strlen(origin) >= sizeof(((Revision *)0)->origin) ||
        strchr(origin, '/') || strchr(origin, '\\')) return false;
    char hash[17];
    hash_text(text, hash);

    Revision *list = NULL;
    size_t n = 0;
    if (!list_impl(dir, &list, &n)) return false;
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
        if (intact && file_out) {
            intact = file_len > strlen(list[reuse].file);
            if (intact) snprintf(file_out, file_len, "%s", list[reuse].file);
        }
        free(list);
        /* An index hash alone cannot establish a usable snapshot: a damaged entry is reported, not papered over
         * with a fresh copy (test_llm snapshot_reuse_integrity pins it; REDTEAM-022 R30 considered and kept). */
        return intact;
    }
    free(list);

    double wt = store_write_time(manager); /* now, or an export's original time */
    if (!(wt >= 0 && wt < (double)UINT64_MAX / 1e6)) return false;
    struct timeval tv = { .tv_sec = (time_t)wt, .tv_usec = (suseconds_t)((wt - (double)(time_t)wt) * 1e6) };
    struct tm tmv;
    time_t now = tv.tv_sec;
    localtime_r(&now, &tmv);
    char stamp[40], file[160], path[1700], idx[1600];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    /* Never replace a snapshot: two in one millisecond (an export of tied
     * times, a fast double save) take the next free name. */
    int ms = (int)(tv.tv_usec / 1000);
    unsigned char random[16];
    char identity[33];
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) snprintf(identity + i * 2, 3, "%02x", random[i]);
    for (int tries = 0;; tries++) {
        int named = snprintf(file, sizeof(file), "%s-%03d-t%016llx-g%s-%s.md", stamp, ms,
                             (unsigned long long)(uint64_t)(wt * 1e6), identity, origin);
        if (named < 0 || (size_t)named >= sizeof(file) || (file_out && file_len <= (size_t)named)) return false;
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
    if (recorded) *recorded = false;
    char dir[1500];
    int lock = revision_lock(project_dir, doc_path, true, LOCK_EX, dir, sizeof(dir));
    if (lock < 0) return false;
    bool ok = record_impl(manager, dir, text, origin, note, recorded, NULL, 0);
    revision_unlock(lock);
    return ok;
}

bool revisions_snapshot_files(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                              char *file_out, size_t file_len)
{
    if (!file_out || !file_len) return false;
    file_out[0] = '\0';
    char dir[1500];
    int lock = revision_lock(project_dir, doc_path, true, LOCK_EX, dir, sizeof(dir));
    if (lock < 0) return false;
    bool ok = record_impl(manager, dir, text, "source", NULL, NULL, file_out, file_len) && file_out[0];
    revision_unlock(lock);
    return ok;
}

char *revisions_load_files(const char *project_dir, const char *doc_path, const Revision *r)
{
    char dir[1500], path[1700];
    int lock = revision_lock(project_dir, doc_path, false, LOCK_SH, dir, sizeof(dir));
    if (lock == -1) return NULL;
    if (!r->file[0] || r->file[0] == '.' || strnlen(r->file, sizeof(r->file)) >= sizeof(r->file) ||
        strchr(r->file, '/') || !join_path(dir, r->file, path, sizeof(path))) {
        revision_unlock(lock);
        return NULL;
    }
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
