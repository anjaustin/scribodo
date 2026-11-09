#define _DARWIN_C_SOURCE
#include "observer_store_internal.h"

#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>
#include <unistd.h>

#include "app/store.h"
#include "core/json.h"

static void state_path(const char *project, char *out, size_t outlen, bool create)
{
    char a[PATH_MAX + 32], b[PATH_MAX + 64];
    snprintf(a, sizeof(a), "%s/.scribodo", project);
    snprintf(b, sizeof(b), "%s/observer", a);
    if (create) {
        mkdir(a, 0755);
        mkdir(b, 0755);
    }
    snprintf(out, outlen, "%s/state.json", b);
}

cJSON *observer_state_load_files(const char *project)
{
    char path[PATH_MAX + 128];
    state_path(project, path, sizeof(path), false);
    FILE *f = fopen(path, "rb");
    cJSON *j = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = n > 0 && n < (64l << 20) ? malloc((size_t)n + 1) : NULL;
        if (buf) {
            buf[fread(buf, 1, (size_t)n, f)] = '\0';
            j = json_parse(buf);
            free(buf);
        }
        fclose(f);
    }
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        j = cJSON_CreateObject();
    }
    if (!j) return NULL;
    if (!cJSON_IsObject(cJSON_GetObjectItem(j, "docs"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(j, "docs");
        if (!cJSON_AddObjectToObject(j, "docs")) { cJSON_Delete(j); return NULL; }
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(j, "chats"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(j, "chats");
        if (!cJSON_AddObjectToObject(j, "chats")) { cJSON_Delete(j); return NULL; }
    }
    return j;
}

static bool valid_import_state(cJSON *state)
{
    if (!cJSON_IsObject(state)) return false;
    int ndocs = 0, nchats = 0, nversion = 0;
    cJSON *root;
    cJSON_ArrayForEach(root, state) {
        if (root->string && !strcmp(root->string, "docs")) ndocs++;
        if (root->string && !strcmp(root->string, "chats")) nchats++;
        if (root->string && !strcmp(root->string, "_version")) nversion++;
    }
    if (ndocs != 1 || nchats != 1 || nversion > 1) return false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(state, "_version");
    if (version && (!cJSON_IsNumber(version) || !isfinite(version->valuedouble) || version->valuedouble < 0 ||
                    version->valuedouble > 9007199254740990.0 || floor(version->valuedouble) != version->valuedouble))
        return false;
    cJSON *docs = cJSON_GetObjectItemCaseSensitive(state, "docs");
    cJSON *chats = cJSON_GetObjectItemCaseSensitive(state, "chats");
    if (!cJSON_IsObject(docs) || !cJSON_IsObject(chats)) return false;
    cJSON *doc, *hash, *chat;
    cJSON_ArrayForEach(doc, docs) {
        if (!doc->string || strchr(doc->string, '/') || !cJSON_IsArray(doc)) return false;
        cJSON_ArrayForEach(hash, doc) {
            if (!cJSON_IsString(hash) || strlen(hash->valuestring) != 16) return false;
            for (const char *p = hash->valuestring; *p; p++) if (!isxdigit((unsigned char)*p)) return false;
        }
    }
    cJSON_ArrayForEach(chat, chats) {
        if (!chat->string || strchr(chat->string, '/') || (!cJSON_IsNumber(chat) && !cJSON_IsObject(chat))) return false;
        if (cJSON_IsNumber(chat) && (!isfinite(chat->valuedouble) || chat->valuedouble < 0 ||
                                     chat->valuedouble > 9007199254740991.0 || floor(chat->valuedouble) != chat->valuedouble))
            return false;
        if (cJSON_IsObject(chat)) {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(chat, "n");
            cJSON *off = cJSON_GetObjectItemCaseSensitive(chat, "off");
            int nn = 0, noff = 0;
            cJSON *field;
            cJSON_ArrayForEach(field, chat) {
                if (field->string && !strcmp(field->string, "n")) nn++;
                if (field->string && !strcmp(field->string, "off")) noff++;
            }
            if (nn > 1 || noff > 1 ||
                (n && (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) || n->valuedouble < 0 ||
                       n->valuedouble > 9007199254740991.0 || floor(n->valuedouble) != n->valuedouble)) ||
                (off && (!cJSON_IsNumber(off) || !isfinite(off->valuedouble) || off->valuedouble < 0 ||
                         off->valuedouble > 9007199254740991.0 || floor(off->valuedouble) != off->valuedouble)))
                return false;
        }
    }
    return true;
}

bool observer_state_import_files(const char *project, cJSON **state)
{
    *state = NULL;
    char path[PATH_MAX + 128];
    state_path(project, path, sizeof(path), false);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) return false;
        cJSON *empty = cJSON_CreateObject();
        if (!empty || !cJSON_AddObjectToObject(empty, "docs") || !cJSON_AddObjectToObject(empty, "chats")) {
            cJSON_Delete(empty);
            return false;
        }
        *state = empty;
        return true;
    }
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long len = ok ? ftell(f) : -1;
    ok = len >= 0 && len < (64l << 20) && fseek(f, 0, SEEK_SET) == 0;
    char *buf = ok ? malloc((size_t)len + 1) : NULL;
    ok = ok && buf != NULL;
    if (ok) {
        size_t got = fread(buf, 1, (size_t)len, f);
        ok = got == (size_t)len && !ferror(f);
        buf[got] = '\0';
    }
    ok = fclose(f) == 0 && ok;
    cJSON *parsed = ok ? json_parse(buf) : NULL;
    free(buf);
    if (!valid_import_state(parsed)) {
        cJSON_Delete(parsed);
        return false;
    }
    *state = parsed;
    return true;
}

static double state_version(cJSON *state)
{
    cJSON *version = cJSON_GetObjectItemCaseSensitive(state, "_version");
    return cJSON_IsNumber(version) ? version->valuedouble : 0;
}

static bool state_write_files(const char *project, cJSON *st)
{
    char path[PATH_MAX + 128], tmp[PATH_MAX + 160];
    state_path(project, path, sizeof(path), true);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    char *s = cJSON_PrintUnformatted(st);
    if (!s) return false;
    int fd = mkstemp(tmp);
    bool ok = fd >= 0;
    size_t n = strlen(s);
    if (ok) {
        ok = fchmod(fd, 0644) == 0 && write(fd, s, n) == (ssize_t)n && fsync(fd) == 0;
        ok = close(fd) == 0 && ok;
        ok = ok && rename(tmp, path) == 0;
        if (!ok) unlink(tmp);
    }
    free(s);
    return ok;
}

bool observer_state_export_files(const char *project, cJSON *state)
{
    return state_write_files(project, state);
}

bool observer_state_save_files(const char *project, cJSON *state)
{
    char dir[PATH_MAX + 32], lock_path[PATH_MAX + 96];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    snprintf(lock_path, sizeof(lock_path), "%s/observer-state.lock", dir);
    int lock = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock < 0 || flock(lock, LOCK_EX) != 0) {
        if (lock >= 0) close(lock);
        return false;
    }
    cJSON *current = observer_state_load_files(project);
    bool ok = current && state_version(current) == state_version(state) && state_version(state) < 9007199254740991.0;
    cJSON_Delete(current);
    if (ok) {
        double next = state_version(state) + 1;
        cJSON_DeleteItemFromObjectCaseSensitive(state, "_version");
        ok = cJSON_AddNumberToObject(state, "_version", next) != NULL &&
             state_write_files(project, state);
    }
    flock(lock, LOCK_UN);
    close(lock);
    return ok;
}

cJSON *observer_state_load_db(Db *db, const char *pid)
{
    DbRows r, c;
    if (!db_query(db,
                  "SELECT d.name, p.piece_hash FROM observer_pieces p JOIN documents d ON d.id = p.document_id"
                  " WHERE d.project_id = ? ORDER BY d.name",
                  (DbArg[1]){ DB_S(pid) }, 1, &r))
        return NULL;
    if (!db_query(db,
                  "SELECT o.conversation_id, o.msg, o.off FROM observer_chats o JOIN conversations c ON c.id = o.conversation_id"
                  " WHERE c.project_id = ?",
                  (DbArg[1]){ DB_S(pid) }, 1, &c)) {
        db_rows_free(&r);
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        db_rows_free(&r);
        db_rows_free(&c);
        return NULL;
    }
    cJSON *docs = cJSON_AddObjectToObject(j, "docs"), *chats = cJSON_AddObjectToObject(j, "chats");
    if (!docs || !chats) {
        db_rows_free(&r);
        db_rows_free(&c);
        cJSON_Delete(j);
        return NULL;
    }
    bool ok = db_rows_has_columns(&r, 2) && db_rows_has_columns(&c, 3);
    for (size_t i = 0; ok && i < r.nrows; i++) {
        const char *name, *hash;
        ok = db_cell_text(&r, i, 0, &name) && db_cell_text(&r, i, 1, &hash) && strlen(hash) == 16;
        if (!ok) break;
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(docs, name);
        if (!arr) arr = cJSON_AddArrayToObject(docs, name);
        cJSON *value = arr ? cJSON_CreateString(hash) : NULL;
        ok = arr && value && cJSON_AddItemToArray(arr, value);
        if (!ok) cJSON_Delete(value);
    }
    for (size_t i = 0; ok && i < c.nrows; i++) {
        const char *id;
        int64_t message;
        uint64_t offset;
        ok = db_cell_text(&c, i, 0, &id) && db_cell_i64(&c, i, 1, &message) && message >= 0 && message < (1ll << 53) &&
             db_cell_u64(&c, i, 2, &offset) && offset < (1ull << 53);
        if (!ok) break;
        cJSON *o = cJSON_AddObjectToObject(chats, id);
        ok = o && cJSON_AddNumberToObject(o, "n", (double)message) && cJSON_AddNumberToObject(o, "off", (double)offset);
    }
    db_rows_free(&r);
    db_rows_free(&c);
    if (!ok) {
        cJSON_Delete(j);
        return NULL;
    }
    char id[80];
    snprintf(id, sizeof(id), "obs-%s", pid);
    DbRows version;
    if (!db_query(db, "SELECT state FROM jobs WHERE id = ?", (DbArg[1]){ DB_S(id) }, 1, &version)) {
        cJSON_Delete(j);
        return NULL;
    }
    uint64_t value = 0;
    ok = version.nrows <= 1 && db_rows_has_columns(&version, 1) &&
         (!version.nrows || (db_cell_u64(&version, 0, 0, &value) && value < (1ull << 53)));
    if (ok && version.nrows) ok = cJSON_AddNumberToObject(j, "_version", (double)value) != NULL;
    db_rows_free(&version);
    if (!ok) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

static bool pieces_batch(Db *db, bool insert, const char *did, const char *const *hashes, size_t n)
{
    bool ok = true;
    for (size_t at = 0; ok && at < n; at += 200) {
        size_t k = n - at < 200 ? n - at : 200;
        size_t sqlcap = 160 + k * 8;
        char *sql = malloc(sqlcap);
        DbArg *args = malloc((1 + 2 * k) * sizeof(DbArg));
        if (!sql || !args) {
            free(sql);
            free(args);
            return false;
        }
        size_t na = 0, len = 0;
        if (insert) {
            len = (size_t)snprintf(sql, sqlcap, "INSERT IGNORE INTO observer_pieces (document_id, piece_hash) VALUES ");
            for (size_t q = 0; q < k; q++) {
                len += (size_t)snprintf(sql + len, sqlcap - len, "%s(?,?)", q ? "," : "");
                args[na++] = DB_S(did);
                args[na++] = DB_S(hashes[at + q]);
            }
        } else {
            len = (size_t)snprintf(sql, sqlcap, "DELETE FROM observer_pieces WHERE document_id = ? AND piece_hash IN (");
            args[na++] = DB_S(did);
            for (size_t q = 0; q < k; q++) {
                len += (size_t)snprintf(sql + len, sqlcap - len, "%s?", q ? "," : "");
                args[na++] = DB_S(hashes[at + q]);
            }
            snprintf(sql + len, sqlcap - len, ")");
        }
        ok = db_run(db, sql, args, na, NULL);
        free(sql);
        free(args);
    }
    return ok;
}

static bool in_array(cJSON *arr, const char *s)
{
    cJSON *it;
    cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) return true;
    return false;
}

bool observer_state_save_db(Db *db, const char *pid, cJSON *st)
{
    bool ok = db_begin(db);
    DbRows locked;
    if (ok) ok = db_query(db, "SELECT id FROM projects WHERE id = ? FOR UPDATE", (DbArg[1]){ DB_S(pid) }, 1, &locked);
    if (ok) db_rows_free(&locked);
    cJSON *old = ok ? observer_state_load_db(db, pid) : NULL;
    if (!old || state_version(old) != state_version(st) || state_version(st) >= 9007199254740991.0) ok = false;
    cJSON *docs = cJSON_GetObjectItem(st, "docs"), *odocs = cJSON_GetObjectItem(old, "docs");
    cJSON *doc;
    cJSON_ArrayForEach(doc, docs)
    {
        if (!ok) break;
        cJSON *prev = cJSON_GetObjectItem(odocs, doc->string);
        size_t n = (size_t)cJSON_GetArraySize(doc), m = prev ? (size_t)cJSON_GetArraySize(prev) : 0;
        const char **add = calloc(n ? n : 1, sizeof(char *)), **del = calloc(m ? m : 1, sizeof(char *));
        size_t na = 0, nd = 0;
        cJSON *h;
        cJSON_ArrayForEach(h, doc) if (add && cJSON_IsString(h) && !(prev && in_array(prev, h->valuestring))) add[na++] = h->valuestring;
        if (prev) cJSON_ArrayForEach(h, prev) if (del && cJSON_IsString(h) && !in_array(doc, h->valuestring)) del[nd++] = h->valuestring;
        if ((n && !add) || (m && !del)) ok = false;
        if (ok && (na || nd)) {
            char did[40];
            if (store_document_id_db(db, pid, doc->string, true, did, sizeof(did)))
                ok = pieces_batch(db, true, did, add, na) && pieces_batch(db, false, did, del, nd);
            else ok = false;
        }
        free(add);
        free(del);
    }
    cJSON_ArrayForEach(doc, odocs)
    {
        if (!ok) break;
        if (cJSON_GetObjectItem(docs, doc->string)) continue;
        char did[40];
        if (store_document_id_db(db, pid, doc->string, false, did, sizeof(did)))
            ok = db_run(db, "DELETE FROM observer_pieces WHERE document_id = ?", (DbArg[1]){ DB_S(did) }, 1, NULL);
    }
    cJSON *c, *ochats = cJSON_GetObjectItem(old, "chats");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(st, "chats"))
    {
        if (!ok) break;
        cJSON *n = cJSON_GetObjectItem(c, "n"), *off = cJSON_GetObjectItem(c, "off");
        double nv = cJSON_IsNumber(n) ? n->valuedouble : 0, ov = cJSON_IsNumber(off) ? off->valuedouble : 0;
        cJSON *was = cJSON_GetObjectItem(ochats, c->string);
        if (was && cJSON_GetNumberValue(cJSON_GetObjectItem(was, "n")) == nv &&
            cJSON_GetNumberValue(cJSON_GetObjectItem(was, "off")) == ov)
            continue;
        DbArg a[4] = { DB_I((int64_t)nv), DB_I((int64_t)ov), DB_S(c->string), DB_S(pid) };
        ok = db_run(db,
                    "INSERT INTO observer_chats (conversation_id, msg, off)"
                    " SELECT id, ?, ? FROM conversations WHERE id = ? AND project_id = ?"
                    " ON DUPLICATE KEY UPDATE msg = VALUES(msg), off = VALUES(off)",
                    a, 4, NULL);
    }
    cJSON_ArrayForEach(c, ochats)
    {
        if (!ok) break;
        if (cJSON_GetObjectItem(cJSON_GetObjectItem(st, "chats"), c->string)) continue;
        ok = db_run(db,
                    "DELETE o FROM observer_chats o JOIN conversations c ON c.id = o.conversation_id"
                    " WHERE o.conversation_id = ? AND c.project_id = ?",
                    (DbArg[2]){ DB_S(c->string), DB_S(pid) }, 2, NULL);
    }
    if (ok) {
        double next = state_version(st) + 1;
        char id[80], version[32];
        snprintf(id, sizeof(id), "obs-%s", pid);
        snprintf(version, sizeof(version), "%.0f", next);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ok = db_run(db,
                    "INSERT INTO jobs (id, project_id, kind, state, updated_at) VALUES (?,?,'observer_state',?,?)"
                    " ON DUPLICATE KEY UPDATE state = VALUES(state), updated_at = VALUES(updated_at)",
                    (DbArg[4]){ DB_S(id), DB_S(pid), DB_S(version), DB_D((double)tv.tv_sec) }, 4, NULL);
        if (ok) {
            cJSON_DeleteItemFromObjectCaseSensitive(st, "_version");
            ok = cJSON_AddNumberToObject(st, "_version", next) != NULL;
        }
    }
    cJSON_Delete(old);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}

bool observer_state_import_version_db(Db *db, const char *pid, cJSON *state)
{
    double value = state_version(state);
    if (value <= 0) return true;
    char id[80], version[32];
    snprintf(id, sizeof(id), "obs-%s", pid);
    snprintf(version, sizeof(version), "%.0f", value);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return db_run(db,
                  "INSERT INTO jobs (id, project_id, kind, state, updated_at) VALUES (?,?,'observer_state',?,?)"
                  " ON DUPLICATE KEY UPDATE state = VALUES(state), updated_at = VALUES(updated_at)",
                  (DbArg[4]){ DB_S(id), DB_S(pid), DB_S(version), DB_D((double)tv.tv_sec) }, 4, NULL);
}

bool observer_rules_get_files(const char *project, char out[17])
{
    out[0] = '\0';
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.scribodo/observer-rules", project);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(out, 1, 16, f);
        out[n] = '\0';
        fclose(f);
    }
    return true;
}

bool observer_rules_import_files(const char *project, char out[17])
{
    out[0] = '\0';
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.scribodo/observer-rules", project);
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT;
    size_t n = fread(out, 1, 16, f);
    bool ok = !ferror(f);
    /* Read one more byte only after a full, successful read. A short read
     * already reached EOF or failed; a 17th byte is not a signature file. */
    if (ok && n == 16) ok = fgetc(f) == EOF && !ferror(f);
    ok = fclose(f) == 0 && ok;
    out[n] = '\0';
    if (!ok || (n != 0 && n != 16)) return false;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)out[i])) return false;
    return true;
}

bool observer_rules_set_files(const char *project, const char *sig)
{
    char dir[PATH_MAX + 32], path[PATH_MAX + 64], tmp[PATH_MAX + 96];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project);
    if (mkdir(dir, 0755) != 0 && access(dir, F_OK) != 0) return false;
    snprintf(path, sizeof(path), "%s/observer-rules", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    size_t n = strlen(sig);
    bool ok = fchmod(fd, 0644) == 0 && write(fd, sig, n) == (ssize_t)n && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

bool observer_rules_get_db(Db *db, const char *pid, char out[17])
{
    out[0] = '\0';
    char id[80];
    snprintf(id, sizeof(id), "rules-%s", pid);
    DbRows r;
    if (!db_query(db, "SELECT state FROM jobs WHERE id = ?", (DbArg[1]){ DB_S(id) }, 1, &r)) return false;
    const char *value;
    bool ok = r.nrows <= 1 && db_rows_has_columns(&r, 1) &&
              (!r.nrows || (db_cell_text(&r, 0, 0, &value) && strlen(value) == 16));
    if (ok && r.nrows) snprintf(out, 17, "%s", value);
    db_rows_free(&r);
    return ok;
}

bool observer_rules_set_db(Db *db, const char *pid, const char *sig)
{
    char id[80];
    snprintf(id, sizeof(id), "rules-%s", pid);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    DbArg a[4] = { DB_S(id), DB_S(pid), DB_S(sig), DB_D((double)tv.tv_sec) };
    return db_run(db,
                  "INSERT INTO jobs (id, project_id, kind, state, updated_at) VALUES (?,?,'observer_rules',?,?)"
                  " ON DUPLICATE KEY UPDATE state = VALUES(state), updated_at = VALUES(updated_at)",
                  a, 4, NULL);
}
