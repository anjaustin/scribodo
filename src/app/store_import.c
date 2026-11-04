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
#include "store_entities.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include "app/renames.h"
#include "cJSON.h"

typedef struct {
    char file[160];
    char id[40];
    double t; /* when it was made (epoch seconds) */
} RevMap;

/* Ids that already belonged to another project get fresh ones (a copied
 * folder imported after the original): never merge into someone else's
 * rows (red-team 008). */
typedef struct {
    char from[40], to[40];
} IdMap;

typedef struct {
    IdMap *m;
    size_t n, cap;
} IdMaps;

static const char *mapped(const IdMaps *maps, const char *id)
{
    for (size_t i = 0; maps && i < maps->n; i++)
        if (!strcmp(maps->m[i].from, id)) return maps->m[i].to;
    return id;
}

static void map_add(IdMaps *maps, const char *from, const char *to)
{
    if (maps->n == maps->cap) {
        size_t cap = maps->cap ? maps->cap * 2 : 16;
        IdMap *m = realloc(maps->m, cap * sizeof(IdMap));
        if (!m) return;
        maps->m = m;
        maps->cap = cap;
    }
    snprintf(maps->m[maps->n].from, sizeof(maps->m[0].from), "%s", from);
    snprintf(maps->m[maps->n].to, sizeof(maps->m[0].to), "%s", to);
    maps->n++;
}

/* Does `table`.id belong to this project (and kind)? Row must exist. */
static bool owned(Db *db, const char *sql, const char *id, const char *pid, const char *kind)
{
    DbRows r;
    if (!db_query(db, sql, (DbArg[1]){ DB_S(id) }, 1, &r)) return false;
    bool ok = r.nrows == 1 && db_cell(&r, 0, 0) && !strcmp(db_cell(&r, 0, 0), pid) &&
              (!kind || (r.ncols > 1 && db_cell(&r, 0, 1) && !strcmp(db_cell(&r, 0, 1), kind)));
    db_rows_free(&r);
    return ok;
}

/* An id for this project in place of `orig` (another project's): the same
 * each time this import runs (so a re-run is idempotent), in the original's
 * format (its kind and time kept, the random tail replaced). `salt` > 0 if
 * the first choice is taken too. */
static void rebased_id(const char *pid, const char *orig, int salt, char *out, size_t outlen)
{
    char key[200];
    snprintf(key, sizeof(key), "%s/%s/%d", pid, orig, salt);
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(key, (CC_LONG)strlen(key), d);
    const char *dash = strrchr(orig, '-');
    int keep = dash && dash - orig <= 28 ? (int)(dash - orig) : 0;
    snprintf(out, outlen, "%.*s%s%02x%02x%02x%02x", keep, orig, keep ? "-" : "x-", d[0], d[1], d[2], d[3]);
}

static void derived_id(const char *pid, const char *a, const char *b, char *out, size_t outlen)
{
    /* The project is part of the key: a copied folder's snapshots must not
     * collide with the original's (red-team 008). */
    char key[700];
    snprintf(key, sizeof(key), "%s/%s/%s", pid, a, b);
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
static double snapshot_time(const char *file);

/* A snapshot's time: the index's epoch seconds plus the milliseconds in its
 * file name (both from one clock reading). Only without an index time is
 * the name's local time parsed (ambiguous across a DST change). */
static double revision_time(const char *file, time_t ts)
{
    const char *p = file;
    for (int dashes = 0; *p && dashes < 2; p++)
        if (*p == '-') dashes++;
    double ms = (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] == '-')
                    ? atoi(p) / 1000.0
                    : 0;
    if (ts > 0) return (double)ts + ms;
    return snapshot_time(file);
}

static double map_time(const RevMap *m, size_t n, const char *file)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(m[i].file, file)) return m[i].t;
    return snapshot_time(file);
}

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
            derived_id(pid, ent->d_name, list[i].file, id, sizeof(id));
            DbArg a[7] = { DB_S(id), DB_S(did), DB_S(list[i].hash), DB_S(text), DB_S(list[i].origin), DB_S(list[i].note),
                           DB_D(revision_time(list[i].file, list[i].ts) + (double)(order++) * 1e-6) };
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
                (*map)[*nmap].t = revision_time(list[i].file, list[i].ts);
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

static bool import_chats(Db *db, const char *project_dir, const char *pid, IdMaps *chats_map)
{
    ChatSummary *list = NULL;
    size_t nc = chat_store_list_files(project_dir, &list);
    bool ok = true;
    for (size_t i = 0; ok && i < nc; i++) {
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load_files(project_dir, list[i].id, &msgs, &n)) continue;
        double t0 = n && msgs[0].ts ? msgs[0].ts : (double)list[i].updated;
        char cid[40];
        snprintf(cid, sizeof(cid), "%s", list[i].id);
        DbArg c[5] = { DB_S(cid), DB_S(pid), DB_S(list[i].title), DB_D(t0), DB_D((double)list[i].updated) };
        ok = db_run(db, "INSERT IGNORE INTO conversations (id, project_id, title, created_at, updated_at) VALUES (?,?,?,?,?)", c,
                    5, NULL);
        /* another project's chat id: this one gets its own */
        for (int salt = 0; ok && !owned(db, "SELECT project_id FROM conversations WHERE id = ?", cid, pid, NULL); salt++) {
            if (salt == 8) {
                ok = false;
                break;
            }
            rebased_id(pid, list[i].id, salt, cid, sizeof(cid));
            DbArg c2[5] = { DB_S(cid), DB_S(pid), DB_S(list[i].title), DB_D(t0), DB_D((double)list[i].updated) };
            ok = db_run(db, "INSERT IGNORE INTO conversations (id, project_id, title, created_at, updated_at) VALUES (?,?,?,?,?)",
                        c2, 5, NULL);
        }
        if (ok && strcmp(cid, list[i].id)) map_add(chats_map, list[i].id, cid);
        for (size_t k = 0; ok && k < n; k++) {
            uint64_t added = 0;
            DbArg a[9] = { DB_S(cid), DB_I(k), DB_S(msgs[k].role == CHAT_USER ? "user" : "assistant"),
                           DB_S(msgs[k].content ? msgs[k].content : ""), DB_I(msgs[k].error ? 1 : 0),
                           DB_I(msgs[k].is_edit ? 1 : 0), DB_I(msgs[k].truncated ? 1 : 0), DB_D(msgs[k].ts ? msgs[k].ts : t0),
                           DB_S(msgs[k].via && msgs[k].via[0] ? msgs[k].via : NULL) };
            ok = db_run(db,
                        "INSERT IGNORE INTO messages (conversation_id, seq, role, content, error, is_edit, truncated, created_at,"
                        " via) VALUES (?,?,?,?,?,?,?,?,?)",
                        a, 9, &added);
            if (!ok || !added || !msgs[k].docs || !msgs[k].docs[0]) continue;
            char *copy = strdup(msgs[k].docs);
            for (char *save = NULL, *nm = copy ? strtok_r(copy, "\n", &save) : NULL; ok && nm;
                 nm = strtok_r(NULL, "\n", &save)) {
                char cur[256], did[40];
                renames_resolve_since(project_dir, nm, msgs[k].ts, cur, sizeof(cur));
                if (!store_document_id(pid, cur, true, did, sizeof(did))) continue;
                DbArg m[3] = { DB_S(did), DB_S(cid), DB_I(k) };
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

static bool import_records(Db *db, const char *project_dir, const char *pid, RecordKind kind, const RevMap *map, size_t nmap,
                           IdMaps *rec_map, const IdMaps *chats_map)
{
    Record *recs = NULL;
    size_t n = records_list_files(project_dir, kind, &recs);
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        const Record *r = &recs[i];
        char rid[40];
        snprintf(rid, sizeof(rid), "%s", r->id);
        DbArg rec[4] = { DB_S(rid), DB_S(pid), DB_S(kind_name(kind)), DB_D((double)r->versions[0].ts) };
        ok = db_run(db, "INSERT IGNORE INTO records (id, project_id, kind, created_at) VALUES (?,?,?,?)", rec, 4, NULL);
        /* belongs elsewhere: an id of its own */
        for (int salt = 0; ok && !owned(db, "SELECT project_id, kind FROM records WHERE id = ?", rid, pid, kind_name(kind));
             salt++) {
            if (salt == 8) {
                ok = false;
                break;
            }
            rebased_id(pid, r->id, salt, rid, sizeof(rid));
            DbArg rec2[4] = { DB_S(rid), DB_S(pid), DB_S(kind_name(kind)), DB_D((double)r->versions[0].ts) };
            ok = db_run(db, "INSERT IGNORE INTO records (id, project_id, kind, created_at) VALUES (?,?,?,?)", rec2, 4, NULL);
        }
        if (ok && strcmp(rid, r->id)) map_add(rec_map, r->id, rid);
        for (size_t v = 0; ok && v < r->nversions; v++) {
            const RecordVersion *ver = &r->versions[v];
            const FindingInfo *f = &ver->finding;
            uint64_t added = 0;
            /* Sub-second order within a record is kept by adding the version
             * number in microseconds (the file store has whole seconds). */
            DbArg a[11] = { DB_S(rid), DB_I(ver->ver), DB_S(ver->status), DB_S(ver->content),
                            DB_S(nz(f->type)), DB_S(nz(f->subject)), DB_S(nz(f->relation)), DB_S(nz(f->object)),
                            DB_S(f->related[0] ? mapped(rec_map, f->related) : NULL), DB_S(f->related_text),
                            DB_D((double)ver->ts + ver->ver * 1e-6) };
            ok = db_run(db,
                        "INSERT IGNORE INTO record_versions (record_id, ver, status, content, finding_type, subject, relation,"
                        " object, related_id, related_text, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                        a, 11, &added);
            for (size_t s = 0; ok && added && s < ver->nsources; s++) {
                const SourceRef *src = &ver->sources[s];
                char cur[256] = "", did[40] = "";
                const char *rev = NULL, *conv = NULL;
                if (src->kind == SRC_DOC) {
                    double made = map_time(map, nmap, src->revision);
                    renames_resolve_since(project_dir, src->doc, made, cur, sizeof(cur));
                    if (cur[0]) store_document_id(pid, cur, true, did, sizeof(did));
                    rev = rev_lookup(map, nmap, src->revision);
                } else {
                    DbRows c;
                    const char *chat = mapped(chats_map, src->chat);
                    if (db_query(db, "SELECT id FROM conversations WHERE id = ? AND project_id = ?",
                                 (DbArg[2]){ DB_S(chat), DB_S(pid) }, 2, &c)) {
                        if (c.nrows) conv = chat;
                        db_rows_free(&c);
                    }
                }
                DbArg sa[10] = { DB_S(rid), DB_I(ver->ver), DB_S(src->kind == SRC_DOC ? "doc" : "chat"), DB_S(nz(did)),
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

/* A fingerprint (sizes and times) of the files an import reads, to notice
 * anything written meanwhile by another process. */
static void fingerprint_add(const char *path, uint64_t *h)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    /* Files by size and time; directories only through what they hold
     * (reading one may create it empty, which changes nothing). */
    if (!S_ISDIR(st.st_mode)) {
        char line[PATH_MAX + 64];
        int n = snprintf(line, sizeof(line), "%s|%lld|%ld|%ld\n", path, (long long)st.st_size, (long)st.st_mtimespec.tv_sec,
                         (long)st.st_mtimespec.tv_nsec);
        for (int k = 0; k < n; k++) *h = (*h ^ (unsigned char)line[k]) * 1099511628211ull;
    } else {
        DIR *d = opendir(path);
        struct dirent *ent;
        while (d && (ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char sub[PATH_MAX + 300];
            snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
            fingerprint_add(sub, h);
        }
        if (d) closedir(d);
    }
}

static uint64_t fingerprint(const char *project_dir)
{
    uint64_t h = 1469598103934665603ull;
    static const char *const items[] = { "chats", "revisions", "notes.jsonl", "canon.jsonl", "findings.jsonl",
                                         "observer", "renames.jsonl" };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        char p[PATH_MAX + 64];
        snprintf(p, sizeof(p), "%s/.scribodo/%s", project_dir, items[i]);
        fingerprint_add(p, &h);
    }
    return h;
}

static bool import_once(Db *db, const char *project_dir, const char *pid, char *why, size_t whylen)
{
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
    IdMaps chats = { 0 }, recs = { 0 };
    bool ok = db_begin(db);
    ok = ok && import_revisions(db, project_dir, pid, &map, &nmap);
    ok = ok && import_chats(db, project_dir, pid, &chats);
    /* notes and canon before findings: findings name them as related */
    ok = ok && import_records(db, project_dir, pid, REC_NOTE, map, nmap, &recs, &chats);
    ok = ok && import_records(db, project_dir, pid, REC_CANON, map, nmap, &recs, &chats);
    ok = ok && import_records(db, project_dir, pid, REC_FINDING, map, nmap, &recs, &chats);
    ok = ok && import_observer(db, project_dir, pid);
    /* The graph, from the imported findings: derived data, so a failure
     * here never fails the import (red-team 009); it's retried later. */
    if (ok)
        ok = db_run(db,
                    "UPDATE records r JOIN (SELECT v.record_id, MAX(v.ver) AS mv FROM record_versions v JOIN records q ON q.id = v.record_id"
                    " WHERE q.project_id = ? GROUP BY v.record_id) x ON x.record_id = r.id"
                    " JOIN record_versions v ON v.record_id = x.record_id AND v.ver = x.mv SET r.current_status = v.status",
                    (DbArg[1]){ DB_S(pid) }, 1, NULL);
    if (ok) sdb_entities_backfill(db, pid);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    free(map);
    free(chats.m);
    free(recs.m);
    if (!ok) snprintf(why, whylen, "Moving the project into the store failed: %s", db_error(db));
    return ok;
}

/* The project row this folder imports into. A crashed import's id is
 * resumed (import-pending); otherwise a fresh row: a row that already has
 * this path belongs to a folder that moved away (or to this one, marker
 * lost), and must never receive this folder's data (red-team 008). */
static bool import_project(Db *db, const char *project_dir, char *pid, size_t pidlen, char *why, size_t whylen)
{
    char pending[PATH_MAX + 64];
    snprintf(pending, sizeof(pending), "%s/.scribodo/import-pending", project_dir);
    FILE *f = fopen(pending, "rb");
    if (f) {
        char id[64] = "";
        size_t n = fread(id, 1, sizeof(id) - 1, f);
        fclose(f);
        id[n] = '\0';
        id[strcspn(id, "\r\n")] = '\0';
        DbRows r;
        if (id[0] == 'p' && db_query(db, "SELECT id FROM projects WHERE id = ?", (DbArg[1]){ DB_S(id) }, 1, &r)) {
            bool found = r.nrows > 0;
            db_rows_free(&r);
            if (found) {
                snprintf(pid, pidlen, "%s", id);
                return true;
            }
        }
    }
    char old[40];
    if (store_project_id(project_dir, false, old, sizeof(old)))
        db_run(db, "UPDATE projects SET path_hash = CONCAT('superseded-', id) WHERE id = ?", (DbArg[1]){ DB_S(old) }, 1, NULL);
    if (!store_project_id(project_dir, true, pid, pidlen)) {
        snprintf(why, whylen, "Couldn't register the project: %s", db_error(db));
        return false;
    }
    f = fopen(pending, "wb");
    if (f) {
        fprintf(f, "%s\n", pid);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    return true;
}

bool store_import(const char *project_dir, char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    if (store_mode(project_dir, NULL, 0, NULL) == STORE_DB) return true; /* already moved */
    Db *db = store_db();
    if (!db) {
        snprintf(why, whylen, "The project store is offline");
        return false;
    }
    /* One import at a time, across windows: while it runs, the project
     * counts as paused for every writer (store_mode), so nothing is written
     * to files that are about to be set aside (red-team 008). */
    char dir[PATH_MAX + 32], lockp[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    mkdir(dir, 0755);
    snprintf(lockp, sizeof(lockp), "%s/import.lock", dir);
    int lfd = open(lockp, O_RDWR | O_CREAT, 0644);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) != 0) {
        if (lfd >= 0) close(lfd);
        snprintf(why, whylen, "This project is being moved into the store by another window");
        return false;
    }
    store_set_importing(project_dir);
    bool ok = store_mode(project_dir, NULL, 0, NULL) == STORE_FILES; /* someone may have finished meanwhile */
    char pid[40] = "";
    if (ok) ok = import_project(db, project_dir, pid, sizeof(pid), why, whylen);
    /* Import, then check nothing changed underneath (another process still
     * writing files); if it did, import again (it's idempotent). */
    for (int attempt = 0; ok && attempt < 4; attempt++) {
        uint64_t before = fingerprint(project_dir);
        ok = import_once(db, project_dir, pid, why, whylen);
        if (ok && fingerprint(project_dir) == before) break;
        if (ok && attempt == 3) {
            snprintf(why, whylen, "The project's files kept changing; it stays on its files for now");
            ok = false;
        }
    }
    if (ok && !store_mark_imported(project_dir, pid)) {
        snprintf(why, whylen, "Couldn't write .scribodo/store.json; the project stays on its files for now");
        ok = false;
    }
    if (ok) {
        set_aside(project_dir);
        char pending[PATH_MAX + 64];
        snprintf(pending, sizeof(pending), "%s/import-pending", dir);
        unlink(pending);
        store_forget_verification();
    }
    store_set_importing(NULL);
    flock(lfd, LOCK_UN);
    close(lfd);
    return ok;
}

/* ---- copies and missing projects ------------------------------------------------ */

static void set_marker_aside(const char *project_dir, bool aside)
{
    char a[PATH_MAX + 64], b[PATH_MAX + 64];
    snprintf(a, sizeof(a), "%s/.scribodo/store.json", project_dir);
    snprintf(b, sizeof(b), "%s/.scribodo/store.json.set-aside", project_dir);
    if (aside) rename(a, b);
    else unlink(b);
    store_forget_verification();
}

/* Write the project's data from the store as files in `project_dir` (so a
 * normal import can take them in as a new project). Times of notes and
 * canon versions become the export time; sources keep their passages. */
typedef struct {
    char id[40];    /* the store's revision id */
    char file[160]; /* the snapshot it became */
} ExportRev;

/* Write the project's data from the store as files in `project_dir` (so a
 * normal import can take them in as a project). Everything keeps its
 * original time; sources keep their passages and point at the exported
 * snapshots. False if anything couldn't be written. */
static bool export_files(Db *db, const char *pid, const char *project_dir)
{
    DbRows docs;
    if (!db_query(db, "SELECT id, name FROM documents WHERE project_id = ?", (DbArg[1]){ DB_S(pid) }, 1, &docs)) return false;
    bool ok = true;
    ExportRev *revs = NULL;
    size_t nrevs = 0, cap = 0;
    for (size_t i = 0; ok && i < docs.nrows; i++) {
        DbRows r;
        if (!db_query(db, "SELECT id, content, origin, note, created_at FROM revisions WHERE document_id = ? ORDER BY seq",
                      (DbArg[1]){ DB_S(db_cell(&docs, i, 0)) }, 1, &r)) {
            ok = false;
            break;
        }
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", project_dir, db_cell(&docs, i, 1));
        for (size_t k = 0; ok && k < r.nrows; k++) {
            store_set_write_time(atof(db_cell(&r, k, 4) ? db_cell(&r, k, 4) : "0"));
            bool rec = false;
            ok = revisions_record(project_dir, path, db_cell(&r, k, 1), db_cell(&r, k, 2), db_cell(&r, k, 3), &rec);
            Revision *list = NULL;
            size_t n = ok ? revisions_list(project_dir, path, &list) : 0;
            if (n && nrevs == cap) {
                cap = cap ? cap * 2 : 64;
                ExportRev *m = realloc(revs, cap * sizeof(ExportRev));
                if (!m) ok = false;
                else revs = m;
            }
            if (ok && n) { /* the newest snapshot is this revision (or the identical one before it) */
                snprintf(revs[nrevs].id, sizeof(revs[0].id), "%s", db_cell(&r, k, 0));
                snprintf(revs[nrevs].file, sizeof(revs[0].file), "%s", list[0].file);
                nrevs++;
            }
            free(list);
        }
        db_rows_free(&r);
    }
    db_rows_free(&docs);
    ChatSummary *cs = NULL;
    size_t nc = ok ? sdb_chat_list(db, pid, &cs) : 0;
    for (size_t i = 0; ok && i < nc; i++) {
        ChatMessage *m = NULL;
        size_t n = 0;
        if (!sdb_chat_load(db, pid, cs[i].id, &m, &n)) {
            ok = false;
            break;
        }
        for (size_t k = 0; ok && k < n; k++) {
            store_set_write_time(m[k].ts);
            ok = chat_store_append(project_dir, cs[i].id, cs[i].title, &m[k]);
        }
        chat_store_free_messages(m, n);
    }
    free(cs);
    static const RecordKind kinds[] = { REC_NOTE, REC_CANON, REC_FINDING };
    for (size_t k = 0; ok && k < 3; k++) {
        Record *recs = NULL;
        size_t n = sdb_record_list(db, pid, kinds[k], &recs);
        for (size_t i = 0; ok && i < n; i++)
            for (size_t v = 0; ok && v < recs[i].nversions; v++) {
                RecordVersion *ver = &recs[i].versions[v];
                for (size_t q = 0; q < ver->nsources; q++) {
                    SourceRef *src = &ver->sources[q];
                    if (src->kind != SRC_DOC) continue;
                    const char *file = "";
                    for (size_t e = 0; e < nrevs && !file[0]; e++)
                        if (!strcmp(revs[e].id, src->revision)) file = revs[e].file;
                    snprintf(src->revision, sizeof(src->revision), "%s", file); /* store ids mean nothing in files */
                }
                store_set_write_time((double)ver->ts);
                ok = records_append_ex(project_dir, kinds[k], recs[i].id, ver->status, ver->content, ver->sources, ver->nsources,
                                       kinds[k] == REC_FINDING ? &ver->finding : NULL, NULL);
            }
        records_free(recs, n);
    }
    store_set_write_time(0);
    free(revs);
    return ok;
}

bool store_export_project(const char *project_id, const char *dir, char *why, size_t whylen)
{
    Db *db = store_db();
    if (!db) {
        snprintf(why, whylen, "The project store is offline");
        return false;
    }
    char sd[PATH_MAX + 32];
    snprintf(sd, sizeof(sd), "%s/.scribodo", dir);
    mkdir(dir, 0755);
    mkdir(sd, 0755);
    bool ok = export_files(db, project_id, dir);
    if (!ok) snprintf(why, whylen, "Couldn't export the project: %s", db_error(db)[0] ? db_error(db) : "a file couldn't be written");
    return ok;
}

bool store_fork(const char *project_dir, char *why, size_t whylen)
{
    char original[PATH_MAX];
    if (store_problem(project_dir, original, sizeof(original)) != STORE_PROBLEM_COPY) {
        snprintf(why, whylen, "This folder isn't a copy");
        return false;
    }
    Db *db = store_db();
    char path[PATH_MAX + 64], id[64] = "";
    snprintf(path, sizeof(path), "%s/.scribodo/store.json", project_dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        const char *k = strstr(buf, "\"project_id\":\"");
        if (k) snprintf(id, sizeof(id), "%.*s", (int)strcspn(k + 14, "\""), k + 14);
    }
    if (!db || id[0] != 'p') {
        snprintf(why, whylen, "The project store is offline");
        return false;
    }
    /* The copy gets the original's data as files, then imports them as its
     * own project: ids that are the original's get fresh ones. */
    set_marker_aside(project_dir, true);
    bool ok = export_files(db, id, project_dir) && store_import(project_dir, why, whylen);
    if (ok) set_marker_aside(project_dir, false);
    else {
        char a[PATH_MAX + 64], b[PATH_MAX + 64];
        snprintf(a, sizeof(a), "%s/.scribodo/store.json.set-aside", project_dir);
        snprintf(b, sizeof(b), "%s/.scribodo/store.json", project_dir);
        rename(a, b); /* back as it was */
        store_forget_verification();
    }
    return ok;
}

bool store_restore(const char *project_dir, char *why, size_t whylen)
{
    if (store_problem(project_dir, NULL, 0) != STORE_PROBLEM_MISSING) {
        snprintf(why, whylen, "This project isn't missing from the store");
        return false;
    }
    /* The newest set-aside files go back, then a normal import. */
    char dir[PATH_MAX + 32], newest[300] = "";
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    DIR *d = opendir(dir);
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL)
        if (!strncmp(ent->d_name, "legacy-", 7) && strcmp(ent->d_name, newest) > 0) snprintf(newest, sizeof(newest), "%s", ent->d_name);
    if (d) closedir(d);
    if (newest[0]) {
        static const char *const items[] = { "chats", "revisions", "notes.jsonl", "canon.jsonl", "findings.jsonl",
                                             "observer", "renames.jsonl" };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
            char from[PATH_MAX + 400], to[PATH_MAX + 64];
            snprintf(from, sizeof(from), "%s/%s/%s", dir, newest, items[i]);
            snprintf(to, sizeof(to), "%s/%s", dir, items[i]);
            struct stat st;
            if (stat(to, &st) != 0) rename(from, to);
        }
    }
    char marker[PATH_MAX + 64];
    snprintf(marker, sizeof(marker), "%s/store.json", dir);
    unlink(marker);
    store_forget_verification();
    if (!newest[0]) {
        snprintf(why, whylen, "No saved files to restore from: the project now starts again on its documents");
        return true;
    }
    return store_import(project_dir, why, whylen);
}
