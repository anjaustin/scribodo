/* Moving a project from its .scribodo/ files into the project store.
 *
 * Everything is imported in one transaction with its ids, version numbers
 * and times kept: revisions (text from the snapshot files, hash-checked),
 * chats, notes, canon, findings with their sources (resolved to document
 * and revision ids, following renames made after each source), and the
 * observer's read state. Revision ids are derived from the snapshot file,
 * so a rerun after a crash finds the same rows (INSERT IGNORE throughout).
 * Only after the commit is the project marked as moved, and only then are
 * the old files set aside in .scribodo/legacy-<time>/ (never deleted).
 */
#define _DARWIN_C_SOURCE
#include "store_backends.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app/renames.h"
#include "cJSON.h"

typedef struct {
    char file[160];
    char id[40];
} RevMap;

static void derived_id(const char *a, const char *b, char *out, size_t outlen)
{
    char key[600];
    snprintf(key, sizeof(key), "%s/%s", a, b);
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(key, (CC_LONG)strlen(key), d);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    snprintf(out, outlen, "r-i%s", hex);
}

static const char *rev_lookup(const RevMap *m, size_t n, const char *file)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(m[i].file, file)) return m[i].id;
    return NULL;
}

/* A snapshot name "YYYYMMDD-HHMMSS-mmm-…" as seconds, or 0. */
static double snapshot_time(const char *file)
{
    struct tm tmv = { 0 };
    const char *rest = strptime(file, "%Y%m%d-%H%M%S", &tmv);
    if (!rest) return 0;
    tmv.tm_isdst = -1;
    double t = (double)mktime(&tmv);
    if (rest[0] == '-' && rest[1] >= '0' && rest[1] <= '9') t += atoi(rest + 1) / 1000.0;
    return t;
}

static bool import_revisions(Db *db, const char *project_dir, const char *pid, RevMap **map, size_t *nmap)
{
    char dir[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.scribodo/revisions", project_dir);
    DIR *d = opendir(dir);
    if (!d) return true; /* no history yet */
    bool ok = true;
    struct dirent *ent;
    size_t cap = 0;
    while (ok && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char doc_path[PATH_MAX + 300];
        snprintf(doc_path, sizeof(doc_path), "%s/%s", project_dir, ent->d_name);
        char did[40];
        if (!store_document_id(pid, ent->d_name, true, did, sizeof(did))) continue;
        Revision *list = NULL;
        size_t n = revisions_list(project_dir, doc_path, &list); /* the file store: not imported yet */
        size_t order = 0; /* ties within a millisecond keep the original order */
        for (size_t i = n; ok && i-- > 0;) {                      /* oldest first */
            char *text = revisions_load(project_dir, doc_path, &list[i]);
            if (!text) continue; /* a damaged snapshot is not imported */
            char id[40];
            derived_id(ent->d_name, list[i].file, id, sizeof(id));
            DbArg a[7] = { DB_S(id), DB_S(did), DB_S(list[i].hash), DB_S(text), DB_S(list[i].origin), DB_S(list[i].note),
                           DB_D((snapshot_time(list[i].file) ? snapshot_time(list[i].file) : (double)list[i].ts) +
                                (double)(order++) * 1e-6) };
            ok = db_run(db,
                        "INSERT IGNORE INTO revisions (id, document_id, hash, content, origin, note, created_at)"
                        " VALUES (?,?,?,?,?,?,?)",
                        a, 7, NULL);
            free(text);
            if (ok && *nmap == cap) {
                cap = cap ? cap * 2 : 64;
                RevMap *m = realloc(*map, cap * sizeof(RevMap));
                if (!m) ok = false;
                else *map = m;
            }
            if (ok) {
                snprintf((*map)[*nmap].file, sizeof((*map)[0].file), "%s", list[i].file);
                snprintf((*map)[*nmap].id, sizeof((*map)[0].id), "%s", id);
                (*nmap)++;
            }
            if (ok && i == 0)
                ok = db_run(db, "UPDATE documents SET current_revision_id = ? WHERE id = ?", (DbArg[2]){ DB_S(id), DB_S(did) },
                            2, NULL);
        }
        free(list);
    }
    closedir(d);
    return ok;
}

static bool import_chats(Db *db, const char *project_dir, const char *pid)
{
    ChatSummary *list = NULL;
    size_t nc = chat_store_list_files(project_dir, &list);
    bool ok = true;
    for (size_t i = 0; ok && i < nc; i++) {
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load_files(project_dir, list[i].id, &msgs, &n)) continue;
        double t0 = n && msgs[0].ts ? msgs[0].ts : (double)list[i].updated;
        DbArg c[5] = { DB_S(list[i].id), DB_S(pid), DB_S(list[i].title), DB_D(t0), DB_D((double)list[i].updated) };
        ok = db_run(db, "INSERT IGNORE INTO conversations (id, project_id, title, created_at, updated_at) VALUES (?,?,?,?,?)", c,
                    5, NULL);
        for (size_t k = 0; ok && k < n; k++) {
            uint64_t added = 0;
            DbArg a[8] = { DB_S(list[i].id), DB_I(k), DB_S(msgs[k].role == CHAT_USER ? "user" : "assistant"),
                           DB_S(msgs[k].content ? msgs[k].content : ""), DB_I(msgs[k].error ? 1 : 0),
                           DB_I(msgs[k].is_edit ? 1 : 0), DB_I(msgs[k].truncated ? 1 : 0), DB_D(msgs[k].ts ? msgs[k].ts : t0) };
            ok = db_run(db,
                        "INSERT IGNORE INTO messages (conversation_id, seq, role, content, error, is_edit, truncated, created_at)"
                        " VALUES (?,?,?,?,?,?,?,?)",
                        a, 8, &added);
            if (!ok || !added || !msgs[k].docs || !msgs[k].docs[0]) continue;
            char *copy = strdup(msgs[k].docs);
            for (char *save = NULL, *nm = copy ? strtok_r(copy, "\n", &save) : NULL; ok && nm;
                 nm = strtok_r(NULL, "\n", &save)) {
                char cur[256], did[40];
                renames_resolve_since(project_dir, nm, msgs[k].ts, cur, sizeof(cur));
                if (!store_document_id(pid, cur, true, did, sizeof(did))) continue;
                DbArg m[3] = { DB_S(did), DB_S(list[i].id), DB_I(k) };
                ok = db_run(db,
                            "INSERT IGNORE INTO message_documents (message_id, document_id)"
                            " SELECT id, ? FROM messages WHERE conversation_id = ? AND seq = ?",
                            m, 3, NULL);
            }
            free(copy);
        }
        chat_store_free_messages(msgs, n);
    }
    free(list);
    return ok;
}

static const char *kind_name(RecordKind k) { return k == REC_NOTE ? "note" : k == REC_CANON ? "canon" : "finding"; }
static const char *nz(const char *s) { return s && s[0] ? s : NULL; }

static bool import_records(Db *db, const char *project_dir, const char *pid, RecordKind kind, const RevMap *map, size_t nmap)
{
    Record *recs = NULL;
    size_t n = records_list_files(project_dir, kind, &recs);
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        const Record *r = &recs[i];
        DbArg rec[4] = { DB_S(r->id), DB_S(pid), DB_S(kind_name(kind)), DB_D((double)r->versions[0].ts) };
        ok = db_run(db, "INSERT IGNORE INTO records (id, project_id, kind, created_at) VALUES (?,?,?,?)", rec, 4, NULL);
        for (size_t v = 0; ok && v < r->nversions; v++) {
            const RecordVersion *ver = &r->versions[v];
            const FindingInfo *f = &ver->finding;
            uint64_t added = 0;
            /* Sub-second order within a record is kept by adding the version
             * number in microseconds (the file store has whole seconds). */
            DbArg a[11] = { DB_S(r->id), DB_I(ver->ver), DB_S(ver->status), DB_S(ver->content),
                            DB_S(nz(f->type)), DB_S(nz(f->subject)), DB_S(nz(f->relation)), DB_S(nz(f->object)),
                            DB_S(nz(f->related)), DB_S(f->related_text), DB_D((double)ver->ts + ver->ver * 1e-6) };
            ok = db_run(db,
                        "INSERT IGNORE INTO record_versions (record_id, ver, status, content, finding_type, subject, relation,"
                        " object, related_id, related_text, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                        a, 11, &added);
            for (size_t s = 0; ok && added && s < ver->nsources; s++) {
                const SourceRef *src = &ver->sources[s];
                char cur[256] = "", did[40] = "";
                const char *rev = NULL, *conv = NULL;
                if (src->kind == SRC_DOC) {
                    double made = snapshot_time(src->revision);
                    renames_resolve_since(project_dir, src->doc, made, cur, sizeof(cur));
                    if (cur[0]) store_document_id(pid, cur, true, did, sizeof(did));
                    rev = rev_lookup(map, nmap, src->revision);
                } else {
                    DbRows c;
                    if (db_query(db, "SELECT id FROM conversations WHERE id = ? AND project_id = ?",
                                 (DbArg[2]){ DB_S(src->chat), DB_S(pid) }, 2, &c)) {
                        if (c.nrows) conv = src->chat;
                        db_rows_free(&c);
                    }
                }
                DbArg sa[10] = { DB_S(r->id), DB_I(ver->ver), DB_S(src->kind == SRC_DOC ? "doc" : "chat"), DB_S(nz(did)),
                                 DB_S(rev), DB_S(conv), DB_I(src->kind == SRC_CHAT ? src->message : 0), DB_I(src->start),
                                 DB_I(src->len), DB_S(src->quote ? src->quote : "") };
                ok = db_run(db,
                            "INSERT INTO source_refs (record_id, ver, kind, document_id, revision_id, conversation_id,"
                            " message_seq, start, len, quote) VALUES (?,?,?,?,?,?,?,?,?,?)",
                            sa, 10, NULL);
            }
        }
    }
    records_free(recs, n);
    return ok;
}

static bool import_observer(Db *db, const char *project_dir, const char *pid)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.scribodo/observer/state.json", project_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return true;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = len > 0 && len < (64l << 20) ? malloc((size_t)len + 1) : NULL;
    if (buf) buf[fread(buf, 1, (size_t)len, f)] = '\0';
    fclose(f);
    cJSON *st = buf ? cJSON_Parse(buf) : NULL;
    free(buf);
    bool ok = true;
    cJSON *doc, *h, *c;
    cJSON_ArrayForEach(doc, cJSON_GetObjectItem(st, "docs"))
    {
        char did[40];
        if (!ok || strchr(doc->string, '/') || !store_document_id(pid, doc->string, true, did, sizeof(did))) continue;
        cJSON_ArrayForEach(h, doc)
        if (ok && cJSON_IsString(h))
            ok = db_run(db, "INSERT IGNORE INTO observer_pieces (document_id, piece_hash) VALUES (?, ?)",
                        (DbArg[2]){ DB_S(did), DB_S(h->valuestring) }, 2, NULL);
    }
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(st, "chats"))
    {
        if (!ok) break;
        double n = 0, off = 0;
        if (cJSON_IsNumber(c)) n = c->valuedouble;
        else {
            cJSON *a = cJSON_GetObjectItem(c, "n"), *b = cJSON_GetObjectItem(c, "off");
            n = cJSON_IsNumber(a) ? a->valuedouble : 0;
            off = cJSON_IsNumber(b) ? b->valuedouble : 0;
        }
        DbArg a[4] = { DB_I((int64_t)n), DB_I((int64_t)off), DB_S(c->string), DB_S(pid) };
        ok = db_run(db,
                    "INSERT IGNORE INTO observer_chats (conversation_id, msg, off)"
                    " SELECT id, ?, ? FROM conversations WHERE id = ? AND project_id = ?",
                    a, 4, NULL);
    }
    cJSON_Delete(st);
    return ok;
}

/* The old files, set aside (never deleted). */
static void set_aside(const char *project_dir)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32], legacy[PATH_MAX + 64];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(legacy, sizeof(legacy), "%s/.scribodo/legacy-%s", project_dir, stamp);
    mkdir(legacy, 0755);
    static const char *const items[] = { "chats", "revisions", "notes.jsonl", "canon.jsonl", "findings.jsonl",
                                         "observer", "renames.jsonl" };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        char from[PATH_MAX + 64], to[PATH_MAX + 128];
        snprintf(from, sizeof(from), "%s/.scribodo/%s", project_dir, items[i]);
        snprintf(to, sizeof(to), "%s/%s", legacy, items[i]);
        rename(from, to);
    }
}

bool store_import(const char *project_dir, char *why, size_t whylen)
{
    if (store_mode(project_dir, NULL, 0, NULL) != STORE_FILES) return true; /* already moved */
    Db *db = store_db();
    if (!db) {
        snprintf(why, whylen, "The project store is offline");
        return false;
    }
    char pid[40];
    if (!store_project_id(project_dir, true, pid, sizeof(pid))) {
        snprintf(why, whylen, "Couldn't register the project: %s", db_error(db));
        return false;
    }
    /* Documents present now. */
    DIR *d = opendir(project_dir);
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if ((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")) ||
            (n > 9 && !strcasecmp(ent->d_name + n - 9, ".markdown"))) {
            char did[40];
            store_document_id(pid, ent->d_name, true, did, sizeof(did));
        }
    }
    if (d) closedir(d);
    RevMap *map = NULL;
    size_t nmap = 0;
    bool ok = db_begin(db);
    ok = ok && import_revisions(db, project_dir, pid, &map, &nmap);
    ok = ok && import_chats(db, project_dir, pid);
    ok = ok && import_records(db, project_dir, pid, REC_NOTE, map, nmap);
    ok = ok && import_records(db, project_dir, pid, REC_CANON, map, nmap);
    ok = ok && import_records(db, project_dir, pid, REC_FINDING, map, nmap);
    ok = ok && import_observer(db, project_dir, pid);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    free(map);
    if (!ok) {
        snprintf(why, whylen, "Moving the project into the store failed: %s", db_error(db));
        return false;
    }
    if (!store_mark_imported(project_dir, pid)) {
        snprintf(why, whylen, "Couldn't write .scribodo/store.json; the project stays on its files for now");
        return false;
    }
    set_aside(project_dir);
    return true;
}
