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
#include "core/crash_point.h"
#include "memory_store.h"
#include "store_entities.h"
#include "revisions_internal.h"
#include "chat_store_internal.h"
#include "records_internal.h"
#include "project_store_internal.h"
#include "observer_store_internal.h"
#include "core/file_text.h"

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
#include <sys/time.h>
#include <removefile.h>

#include "app/renames.h"
#include "core/json.h"
#include "core/file_text.h"

typedef struct {
    char doc[256];
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

static bool map_add(IdMaps *maps, const char *from, const char *to)
{
    if (maps->n == maps->cap) {
        size_t cap = maps->cap ? maps->cap * 2 : 16;
        IdMap *m = realloc(maps->m, cap * sizeof(IdMap));
        if (!m) return false;
        maps->m = m;
        maps->cap = cap;
    }
    snprintf(maps->m[maps->n].from, sizeof(maps->m[0].from), "%s", from);
    snprintf(maps->m[maps->n].to, sizeof(maps->m[0].to), "%s", to);
    maps->n++;
    return true;
}

/* Does `table`.id belong to this project (and kind)? Row must exist. */
static bool owned(Db *db, const char *sql, const char *id, const char *pid, const char *kind)
{
    DbRows r;
    if (!db_query(db, sql, (DbArg[1]){ DB_S(id) }, 1, &r)) return false;
    const char *owner, *stored_kind = NULL;
    bool ok = r.nrows == 1 && db_rows_has_columns(&r, kind ? 2 : 1) && db_cell_text(&r, 0, 0, &owner) &&
              !strcmp(owner, pid) && (!kind || (db_cell_text(&r, 0, 1, &stored_kind) && !strcmp(stored_kind, kind)));
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

static const RevMap *source_revision(const char *project, const RevMap *m, size_t n, const SourceRef *source,
                                     char *current, size_t currentlen)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].file, source->revision)) continue;
        renames_resolve_since(project, source->doc, m[i].t, current, currentlen);
        if (!strcmp(current, m[i].doc)) return &m[i];
    }
    renames_resolve_since(project, source->doc, snapshot_time(source->revision), current, currentlen);
    return NULL;
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
    if (!d) return errno == ENOENT; /* missing history is not unreadable history */
    bool ok = true;
    struct dirent *ent;
    size_t cap = 0;
    while (ok) {
        errno = 0;
        ent = readdir(d);
        if (!ent) { ok = errno == 0; break; }
        if (ent->d_name[0] == '.') continue;
        char doc_path[PATH_MAX + 300];
        snprintf(doc_path, sizeof(doc_path), "%s/%s", project_dir, ent->d_name);
        char did[40];
        if (!store_document_id_db(db, pid, ent->d_name, true, did, sizeof(did))) { ok = false; break; }
        Revision *list = NULL;
        size_t n = 0;
        ok = revisions_list_files_checked(project_dir, doc_path, &list, &n);
        size_t order = 0; /* ties within a millisecond keep the original order */
        for (size_t i = n; ok && i-- > 0;) {                      /* oldest first */
            char *text = revisions_load_files(project_dir, doc_path, &list[i]);
            if (!text) { ok = false; break; } /* no incomplete or damaged migration */
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
                snprintf((*map)[*nmap].doc, sizeof((*map)[0].doc), "%s", ent->d_name);
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
    if (closedir(d) != 0) ok = false;
    return ok;
}

/* A document file name as the explorer, the observer and the import see it:
 * a plain basename with one of the document extensions. */
static bool document_file_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.' || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) return false;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
        if (*c < 0x20 || *c == 127) return false;
    size_t n = strlen(name);
    return n < 256 && ((n > 3 && !strcasecmp(name + n - 3, ".md")) || (n > 4 && !strcasecmp(name + n - 4, ".txt")) ||
                       (n > 9 && !strcasecmp(name + n - 9, ".markdown")));
}

/* The document file as it is on disk is the author's saved state. It gets a
 * saved revision in the store: the newest revision whose content is the
 * file's, otherwise a new "import" revision dated by the file's modification
 * time. Without this, memory could not cover an imported manuscript until
 * every file was opened and saved again (MEMORY-021: eight imported parts,
 * none capturable).
 *
 * A name that is not a document file, a file that is missing, a directory or
 * anything else the app cannot read as text is skipped (true, not adopted):
 * such a file is simply not covered; it must not hold the project's move
 * into the store hostage (red-team 016). In an import (inside its
 * transaction) both pointers follow the file. Outside one, the lazy repair
 * of a project moved before this existed, the saved pointer is set only
 * when none exists and the current pointer only when none exists, so an
 * in-app save receipt or a newer recorded revision is never overridden. */
bool sdb_saved_file_adopt(Db *db, const char *pid, const char *project_dir, const char *name, bool import, bool *adopted)
{
    if (adopted) *adopted = false;
    if (!db || !pid || !pid[0] || !project_dir || !project_dir[0] || !document_file_name(name)) return false;
    if (import != db_in_transaction(db)) return false;
    char path[PATH_MAX + 300];
    snprintf(path, sizeof(path), "%s/%s", project_dir, name);
    char *text = NULL;
    FileTextStatus st = file_text_read(path, &text);
    if (st != FILE_TEXT_OK || !text) { free(text); return true; } /* missing, special, unreadable: not adopted */
    struct stat sb;
    double saved_at = stat(path, &sb) == 0 ? (double)sb.st_mtime : 0;
    bool own = !import;
    bool ok = !own || db_begin(db);
    char did[40];
    if (ok) ok = store_document_id_db(db, pid, name, true, did, sizeof(did));
    DbRows r = {0};
    const char *existing = NULL;
    if (ok) {
        ok = db_query(db, "SELECT id FROM revisions WHERE document_id = ? AND source_hash = SHA2(?,256) ORDER BY seq DESC LIMIT 1",
                      (DbArg[2]){ DB_S(did), DB_S(text) }, 2, &r) && r.nrows <= 1;
        if (ok && r.nrows == 1 && !db_cell_text(&r, 0, 0, &existing)) ok = false;
    }
    char id[40];
    if (ok && existing) {
        snprintf(id, sizeof(id), "%s", existing);
    } else if (ok) {
        /* The store's own id shape (r-YYYYMMDD-HHMMSS-16 hex), which the
         * index validates, with the file's time as the stamp and a suffix
         * derived from the project, the file and its content, so the same
         * text gets the same id and different text never shares one. */
        time_t stamp_t = saved_at > 0 ? (time_t)saved_at : time(NULL);
        struct tm tmv;
        localtime_r(&stamp_t, &tmv);
        char stamp[32], key[800], content_hex[65];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        unsigned char dg[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(text, (CC_LONG)strlen(text), dg);
        for (size_t i = 0; i < sizeof(dg); i++) snprintf(content_hex + 2 * i, 3, "%02x", dg[i]);
        snprintf(key, sizeof(key), "%s/%s/saved-file/%s", pid, name, content_hex);
        CC_SHA256(key, (CC_LONG)strlen(key), dg);
        snprintf(id, sizeof(id), "r-%s-%02x%02x%02x%02x%02x%02x%02x%02x", stamp, dg[0], dg[1], dg[2], dg[3], dg[4], dg[5], dg[6], dg[7]);
        char hash[17];
        sdb_content_hash16(text, hash);
        DbArg a[7] = { DB_S(id), DB_S(did), DB_S(hash), DB_S(text), DB_S("import"), DB_S(""), DB_D(saved_at) };
        ok = db_run(db, "INSERT IGNORE INTO revisions (id, document_id, hash, content, origin, note, created_at) VALUES (?,?,?,?,?,?,?)",
                    a, 7, NULL);
    }
    db_rows_free(&r);
    uint64_t changed = 0;
    if (ok)
        ok = import ? db_run(db, "UPDATE documents SET saved_revision_id = ?, current_revision_id = ? WHERE id = ?",
                             (DbArg[3]){ DB_S(id), DB_S(id), DB_S(did) }, 3, &changed)
                    : db_run(db, "UPDATE documents SET saved_revision_id = ?, current_revision_id = COALESCE(current_revision_id, ?)"
                                 " WHERE id = ? AND saved_revision_id IS NULL",
                             (DbArg[3]){ DB_S(id), DB_S(id), DB_S(did) }, 3, &changed);
    if (own) {
        if (ok) ok = db_commit(db);
        if (!ok) db_rollback(db);
    }
    free(text);
    if (ok && adopted) *adopted = changed == 1;
    return ok;
}

static bool import_saved_files(Db *db, const char *project_dir, const char *pid)
{
    DIR *d = opendir(project_dir);
    if (!d) return false;
    bool ok = true;
    struct dirent *ent;
    while (ok) {
        errno = 0;
        ent = readdir(d);
        if (!ent) { ok = errno == 0; break; }
        if (!document_file_name(ent->d_name)) continue;
        ok = sdb_saved_file_adopt(db, pid, project_dir, ent->d_name, true, NULL);
    }
    if (closedir(d) != 0) ok = false;
    return ok;
}

static bool import_chats(Db *db, const char *project_dir, const char *pid, IdMaps *chats_map)
{
    ChatSummary *list = NULL;
    size_t nc = 0;
    bool ok = chat_store_list_files_checked(project_dir, &list, &nc);
    for (size_t i = 0; ok && i < nc; i++) {
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load_files(project_dir, list[i].id, &msgs, &n)) { ok = false; break; }
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
        if (ok && strcmp(cid, list[i].id)) ok = map_add(chats_map, list[i].id, cid);
        if (ok && list[i].observer_ignore) ok = sdb_chat_set_observer(db, pid, cid, false);
        for (size_t k = 0; ok && k < n; k++) {
            uint64_t added = 0;
            if (msgs[k].memory_sources &&
                strnlen(msgs[k].memory_sources, CHAT_MEMORY_SOURCES_BYTES + 1) > CHAT_MEMORY_SOURCES_BYTES) {
                ok = false;
                break;
            }
            DbArg a[11] = { DB_S(cid), DB_I(k), DB_S(msgs[k].role == CHAT_USER ? "user" : "assistant"),
                           DB_S(msgs[k].content ? msgs[k].content : ""), DB_I(msgs[k].error ? 1 : 0),
                           DB_I(msgs[k].is_edit ? 1 : 0), DB_I(msgs[k].truncated ? 1 : 0), DB_D(msgs[k].ts ? msgs[k].ts : t0),
                           DB_S(msgs[k].via && msgs[k].via[0] ? msgs[k].via : NULL), DB_I(msgs[k].stopped ? 1 : 0),
                           DB_S(msgs[k].memory_sources) };
            ok = db_run(db,
                        "INSERT IGNORE INTO messages (conversation_id, seq, role, content, error, is_edit, truncated, created_at,"
                        " via, stopped, memory_sources) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                        a, 11, &added);
            if (ok && added) ok = memory_message_intent(db, pid, cid, (int64_t)k);
            if (!ok || !added || !msgs[k].docs || !msgs[k].docs[0]) continue;
            char *copy = strdup(msgs[k].docs);
            if (!copy) { ok = false; break; }
            for (char *save = NULL, *nm = copy ? strtok_r(copy, "\n", &save) : NULL; ok && nm;
                 nm = strtok_r(NULL, "\n", &save)) {
                char cur[256], did[40];
                renames_resolve_since(project_dir, nm, msgs[k].ts, cur, sizeof(cur));
                if (!store_document_id_db(db, pid, cur, true, did, sizeof(did))) { ok = false; break; }
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
    size_t n = 0;
    bool ok = records_list_files_checked(project_dir, kind, &recs, &n);
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
        if (ok && strcmp(rid, r->id)) ok = map_add(rec_map, r->id, rid);
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
                    const RevMap *source_rev = source_revision(project_dir, map, nmap, src, cur, sizeof(cur));
                    if (cur[0] && !store_document_id_db(db, pid, cur, true, did, sizeof(did))) { ok = false; break; }
                    rev = source_rev ? source_rev->id : NULL;
                    if (src->revision[0] && !rev) { ok = false; break; }
                } else {
                    DbRows c;
                    const char *chat = mapped(chats_map, src->chat);
                    if (db_query(db, "SELECT id FROM conversations WHERE id = ? AND project_id = ?",
                                 (DbArg[2]){ DB_S(chat), DB_S(pid) }, 2, &c)) {
                        if (c.nrows) conv = chat;
                        db_rows_free(&c);
                    } else { ok = false; break; }
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
    cJSON *st = NULL;
    if (!observer_state_import_files(project_dir, &st)) return false;
    bool ok = true;
    cJSON *doc, *h, *c;
    cJSON_ArrayForEach(doc, cJSON_GetObjectItem(st, "docs"))
    {
        char did[40];
        if (!ok) break;
        if (strchr(doc->string, '/') || !store_document_id_db(db, pid, doc->string, true, did, sizeof(did))) { ok = false; break; }
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
    if (ok) ok = observer_state_import_version_db(db, pid, st);
    cJSON_Delete(st);
    char rules[17];
    if (ok) ok = observer_rules_import_files(project_dir, rules);
    if (ok && rules[0]) ok = observer_rules_set_db(db, pid, rules);
    return ok;
}

/* The old files, set aside (never deleted). */
static bool legacy_path(const char *project_dir, char *out, size_t outlen)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return false;
    time_t t = tv.tv_sec;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    int n = snprintf(out, outlen, "%s/.scribodo/legacy-%s-%06ld-XXXXXX", project_dir, stamp, (long)tv.tv_usec);
    return n >= 0 && (size_t)n < outlen && mkdtemp(out) != NULL;
}

static const char *const k_legacy_items[] = { "chats", "revisions", "notes.jsonl", "canon.jsonl", "findings.jsonl",
                                              "observer", "observer-rules", "renames.jsonl" };

/* Any legacy item still live under .scribodo/ (an interrupted set_aside leaves some). */
static bool legacy_items_live(const char *project_dir)
{
    for (size_t i = 0; i < sizeof(k_legacy_items) / sizeof(k_legacy_items[0]); i++) {
        char path[PATH_MAX + 64];
        struct stat st;
        snprintf(path, sizeof(path), "%s/.scribodo/%s", project_dir, k_legacy_items[i]);
        if (lstat(path, &st) == 0) return true;
    }
    return false;
}

static void set_aside(const char *project_dir)
{
    char legacy[PATH_MAX + 100];
    if (!legacy_path(project_dir, legacy, sizeof(legacy))) return;
    const char *const *items = k_legacy_items;
    for (size_t i = 0; i < sizeof(k_legacy_items) / sizeof(k_legacy_items[0]); i++) {
        char from[PATH_MAX + 64], to[PATH_MAX + 128];
        snprintf(from, sizeof(from), "%s/.scribodo/%s", project_dir, items[i]);
        snprintf(to, sizeof(to), "%s/%s", legacy, items[i]);
        rename(from, to);
        crash_point("import.aside_item"); /* one hit per item, moved or absent */
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
                                         "observer", "observer-rules", "renames.jsonl" };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        char p[PATH_MAX + 64];
        snprintf(p, sizeof(p), "%s/.scribodo/%s", project_dir, items[i]);
        fingerprint_add(p, &h);
    }
    uint64_t docs = 0;
    DIR *d = opendir(project_dir);
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if (!((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) ||
              (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")) ||
              (n > 9 && !strcasecmp(ent->d_name + n - 9, ".markdown")))) continue;
        char p[PATH_MAX + 300];
        snprintf(p, sizeof(p), "%s/%s", project_dir, ent->d_name);
        uint64_t one = 1469598103934665603ull;
        fingerprint_add(p, &one);
        docs ^= one;
    }
    if (d) closedir(d);
    for (size_t i = 0; i < sizeof(docs); i++) h = (h ^ (unsigned char)(docs >> (i * 8))) * 1099511628211ull;
    return h;
}

static bool import_once(Db *db, const char *project_dir, const char *data_dir, const char *pid, uint64_t expected_fingerprint,
                        uint64_t expected_data_fingerprint,
                        bool *changed, char *why, size_t whylen)
{
    *changed = false;
    if (!db_begin(db)) { snprintf(why, whylen, "Couldn't begin project migration: %s", db_error(db)); return false; }
    /* Documents present now. */
    DIR *d = opendir(project_dir);
    struct dirent *ent;
    bool ok = d != NULL;
    while (ok) {
        errno = 0;
        ent = readdir(d);
        if (!ent) { ok = errno == 0; break; }
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if ((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")) ||
            (n > 9 && !strcasecmp(ent->d_name + n - 9, ".markdown"))) {
            char did[40];
            ok = store_document_id_db(db, pid, ent->d_name, true, did, sizeof(did));
        }
    }
    if (d && closedir(d) != 0) ok = false;
    RevMap *map = NULL;
    size_t nmap = 0;
    IdMaps chats = { 0 }, recs = { 0 };
    ok = ok && import_revisions(db, data_dir, pid, &map, &nmap);
    ok = ok && import_saved_files(db, project_dir, pid);
    ok = ok && import_chats(db, data_dir, pid, &chats);
    /* notes and canon before findings: findings name them as related */
    ok = ok && import_records(db, data_dir, pid, REC_NOTE, map, nmap, &recs, &chats);
    ok = ok && import_records(db, data_dir, pid, REC_CANON, map, nmap, &recs, &chats);
    ok = ok && import_records(db, data_dir, pid, REC_FINDING, map, nmap, &recs, &chats);
    ok = ok && import_observer(db, data_dir, pid);
    /* The graph, from the imported findings: derived data, so a failure
     * here never fails the import (red-team 009); it's retried later. */
    if (ok)
        ok = db_run(db,
                    "UPDATE records r JOIN (SELECT v.record_id, MAX(v.ver) AS mv FROM record_versions v JOIN records q ON q.id = v.record_id"
                    " WHERE q.project_id = ? GROUP BY v.record_id) x ON x.record_id = r.id"
                    " JOIN record_versions v ON v.record_id = x.record_id AND v.ver = x.mv SET r.current_status = v.status",
                    (DbArg[1]){ DB_S(pid) }, 1, NULL);
    if (ok) sdb_entities_backfill(db, pid);
    if (ok && (fingerprint(project_dir) != expected_fingerprint || fingerprint(data_dir) != expected_data_fingerprint)) {
        *changed = true;
        ok = false;
    }
    if (ok) ok = db_commit(db);
    if (!ok) db_rollback(db);
    free(map);
    free(chats.m);
    free(recs.m);
    if (!ok && !*changed)
        snprintf(why, whylen, "Moving the project into the store failed: %s",
                 db_error(db)[0] ? db_error(db) : "a project file couldn't be read or decoded");
    return ok;
}

static bool project_atomic_text(const char *project, const char *name, const char *text)
{
    char dir[PATH_MAX + 32], path[PATH_MAX + 64], tmp[PATH_MAX + 80];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    bool ok = fd >= 0;
    size_t n = strlen(text), off = 0;
    while (ok && off < n) {
        ssize_t wrote = write(fd, text + off, n - off);
        if (wrote < 0 && errno == EINTR) continue;
        ok = wrote > 0;
        if (ok) off += (size_t)wrote;
    }
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok && fd >= 0) unlink(tmp);
    if (ok) {
        int parent = open(dir, O_RDONLY);
        ok = parent >= 0 && fsync(parent) == 0;
        if (parent >= 0) close(parent);
    }
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
    char *id = NULL;
    FileTextStatus pending_status = file_text_read(pending, &id);
    if (pending_status == FILE_TEXT_ERROR) {
        snprintf(why, whylen, "Couldn't read the pending project registration; nothing was imported");
        return false;
    }
    if (pending_status == FILE_TEXT_OK) {
        size_t n = strcspn(id, "\r\n");
        bool valid = id[0] == 'p' && n < 40 && strspn(id + n, "\r\n") == strlen(id + n);
        id[n] = '\0';
        valid = valid && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(id);
        if (!valid) {
            free(id);
            snprintf(why, whylen, "The pending project registration is damaged; it was preserved");
            return false;
        }
        DbRows r;
        if (!db_query(db, "SELECT path FROM projects WHERE id = ?", (DbArg[1]){ DB_S(id) }, 1, &r)) {
            free(id);
            snprintf(why, whylen, "Couldn't verify the pending project's owner: %s", db_error(db));
            return false;
        }
        const char *owner = NULL;
        char real[PATH_MAX];
        bool checked = db_rows_has_columns(&r, 1) && r.nrows <= 1 && realpath(project_dir, real) &&
                       (!r.nrows || db_cell_text(&r, 0, 0, &owner));
        bool resume = checked && owner && !strcmp(owner, real);
        if (resume) snprintf(pid, pidlen, "%s", id);
        db_rows_free(&r);
        free(id);
        if (!checked) { snprintf(why, whylen, "Couldn't verify the pending project's owner"); return false; }
        if (resume) return true;
        /* Copied crash residue belongs to another folder, never this import. */
    }
    char real[PATH_MAX], path_hash[65];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    if (!realpath(project_dir, real)) { snprintf(why, whylen, "Couldn't resolve the project folder"); return false; }
    CC_SHA256(real, (CC_LONG)strlen(real), digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(path_hash + i * 2, 3, "%02x", digest[i]);
    if (!db_run(db, "UPDATE projects SET path_hash = CONCAT('superseded-', id) WHERE path_hash = ?",
                (DbArg[1]){ DB_S(path_hash) }, 1, NULL)) {
        snprintf(why, whylen, "Couldn't reserve the project path: %s", db_error(db));
        return false;
    }
    if (!store_project_id_db(db, project_dir, true, pid, pidlen)) {
        snprintf(why, whylen, "Couldn't register the project: %s", db_error(db));
        return false;
    }
    char line[80];
    snprintf(line, sizeof(line), "%s\n", pid);
    bool ok = project_atomic_text(project_dir, "import-pending", line);
    if (!ok) snprintf(why, whylen, "Couldn't persist the pending project registration; nothing was imported");
    return ok;
}

typedef struct {
    char source[64], target[64];
    bool ready;
    uint64_t signature;
} ForkSnapshot;

static FileTextStatus fork_snapshot_read(const char *project, ForkSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    char path[PATH_MAX + 64], *text = NULL;
    snprintf(path, sizeof(path), "%s/.scribodo/fork.json", project);
    FileTextStatus status = file_text_read(path, &text);
    if (status != FILE_TEXT_OK) return status;
    cJSON *j = strlen(text) < 2048 ? json_parse(text) : NULL;
    const cJSON *source = cJSON_GetObjectItem(j, "source"), *target = cJSON_GetObjectItem(j, "target"),
                *ready = cJSON_GetObjectItem(j, "ready"), *sig = cJSON_GetObjectItem(j, "signature");
    bool ok = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "version")) == 1 && cJSON_IsString(source) &&
              source->valuestring[0] == 'p' && strlen(source->valuestring) < sizeof(snapshot->source) &&
              cJSON_IsString(target) && strlen(target->valuestring) < sizeof(snapshot->target) &&
              cJSON_IsBool(ready) && cJSON_IsString(sig) && strlen(sig->valuestring) == 16 &&
              strspn(sig->valuestring, "0123456789abcdef") == 16;
    if (ok) {
        snprintf(snapshot->source, sizeof(snapshot->source), "%s", source->valuestring);
        snprintf(snapshot->target, sizeof(snapshot->target), "%s", target->valuestring);
        snapshot->ready = cJSON_IsTrue(ready);
        snapshot->signature = strtoull(sig->valuestring, NULL, 16);
    }
    cJSON_Delete(j);
    free(text);
    return ok ? FILE_TEXT_OK : FILE_TEXT_ERROR;
}

static bool fork_snapshot_write(const char *project, const ForkSnapshot *snapshot)
{
    char sig[17];
    snprintf(sig, sizeof(sig), "%016llx", (unsigned long long)snapshot->signature);
    cJSON *j = cJSON_CreateObject();
    bool ok = j && cJSON_AddNumberToObject(j, "version", 1) &&
              cJSON_AddStringToObject(j, "source", snapshot->source) &&
              cJSON_AddStringToObject(j, "target", snapshot->target) &&
              cJSON_AddBoolToObject(j, "ready", snapshot->ready) && cJSON_AddStringToObject(j, "signature", sig);
    char *text = ok ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!text) return false;
    ok = project_atomic_text(project, "fork.json", text);
    free(text);
    return ok;
}

static bool fork_snapshot_finish(const char *project, const char *stage)
{
    char source[PATH_MAX + 100], legacy[PATH_MAX + 100], path[PATH_MAX + 64];
    snprintf(source, sizeof(source), "%s/.scribodo", stage);
    struct stat st;
    if (lstat(source, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || !legacy_path(project, legacy, sizeof(legacy))) return false;
        if (rename(source, legacy) != 0) { rmdir(legacy); return false; }
    } else if (errno != ENOENT) return false;
    if (rmdir(stage) != 0 && errno != ENOENT) return false;
    snprintf(path, sizeof(path), "%s/.scribodo/import-pending", project);
    if (unlink(path) != 0 && errno != ENOENT) return false;
    snprintf(path, sizeof(path), "%s/.scribodo/fork.json", project);
    return unlink(path) == 0 || errno == ENOENT;
}

static bool import_locked_from(StoreManager *manager, Db *db, const char *project_dir, const char *data_dir,
                               const char *dir, char *why, size_t whylen)
{
    bool fork = strcmp(project_dir, data_dir) != 0;
    /* Keep the copy marker authoritative until its atomic replacement. A
     * crash must never expose an unmarked folder with empty live metadata. */
    bool ok = fork ? store_problem(manager, project_dir, NULL, 0) == STORE_PROBLEM_COPY
                   : store_mode(manager, project_dir, NULL, 0, NULL) == STORE_FILES;
    char pid[40] = "";
    if (ok) ok = import_project(db, project_dir, pid, sizeof(pid), why, whylen);
    /* The project row and the import-pending witness exist; nothing of the folder is imported yet (GAP-004 matrix). */
    if (ok) crash_point("import.registered");
    if (ok && strcmp(project_dir, data_dir)) {
        ForkSnapshot snapshot;
        ok = fork_snapshot_read(project_dir, &snapshot) == FILE_TEXT_OK && snapshot.ready;
        if (ok) {
            snprintf(snapshot.target, sizeof(snapshot.target), "%s", pid);
            ok = fork_snapshot_write(project_dir, &snapshot);
        }
        if (!ok) snprintf(why, whylen, "Couldn't persist the fork's target; its source snapshot is retained");
    }
    for (int attempt = 0; ok && attempt < 4; attempt++) {
        uint64_t before = fingerprint(project_dir);
        uint64_t data_before = fingerprint(data_dir);
        bool changed = false;
        ok = import_once(db, project_dir, data_dir, pid, before, data_before, &changed, why, whylen);
        if (ok) break;
        if (changed && attempt < 3) {
            ok = true;
            continue;
        }
        if (changed) {
            snprintf(why, whylen, "The project's files kept changing; it stays on its files for now");
        }
    }
    /* The folder's data is committed under pid; the marker is not written, so the files are still authoritative and a
     * restart resumes the import through the witness. */
    if (ok) crash_point("import.committed");
    if (ok && !store_mark_imported(project_dir, pid)) {
        snprintf(why, whylen, "Couldn't write .scribodo/store.json; the project stays on its files for now");
        ok = false;
    }
    if (ok) {
        /* The marker is durable: the store is authoritative; the legacy files are still in place. */
        crash_point("import.marked");
        set_aside(project_dir);
        crash_point("import.set_aside");
        char pending[PATH_MAX + 64];
        snprintf(pending, sizeof(pending), "%s/import-pending", dir);
        if (!strcmp(project_dir, data_dir)) unlink(pending); /* fork cleanup retains its commit witness until archived */
        store_forget_verification(manager);
    }
    return ok;
}

static bool import_locked(StoreManager *manager, Db *db, const char *project_dir, const char *dir, char *why, size_t whylen)
{
    return import_locked_from(manager, db, project_dir, project_dir, dir, why, whylen);
}

/* A plain import that died after writing its marker (GAP-004 matrix, import row, 2026-11-12) left the legacy
 * files live under .scribodo/ and the import-pending witness in place; the store was authoritative, so nothing
 * ever finished the move. Finishing is idempotent and acts only when the witness names the marker's project and
 * either no fork journal is present or the journal's target is that project (a fork that died after its marker;
 * its cleanup is fork_snapshot_finish). True when the move was finished here. */
bool store_finish_import(StoreManager *manager, const char *project_dir)
{
    char pid[40] = "", pending[PATH_MAX + 64], *id = NULL;
    if (!project_dir || !project_dir[0] || store_mode(manager, project_dir, pid, sizeof(pid), NULL) != STORE_DB) return false;
    snprintf(pending, sizeof(pending), "%s/.scribodo/import-pending", project_dir);
    if (file_text_read(pending, &id) != FILE_TEXT_OK) { free(id); return false; }
    id[strcspn(id, "\r\n")] = '\0';
    bool ours = !strcmp(id, pid);
    free(id);
    if (!ours) return false;
    ForkSnapshot snapshot;
    FileTextStatus forked = fork_snapshot_read(project_dir, &snapshot);
    if (forked == FILE_TEXT_OK) {
        /* A fork that died after its marker (GAP-004 matrix, fork row): the journal names this marker as its target, so
         * its staging snapshot is archived and the journal closed, as the next fork of a copy would do. */
        if (strcmp(snapshot.target, pid)) return false;
        char stage[PATH_MAX + 80];
        snprintf(stage, sizeof(stage), "%s/.scribodo/fork-snapshot", project_dir);
        return fork_snapshot_finish(project_dir, stage);
    }
    if (forked != FILE_TEXT_MISSING) return false;
    if (legacy_items_live(project_dir)) set_aside(project_dir); /* no empty archive when the items were already moved */
    return unlink(pending) == 0 || errno == ENOENT;
}

bool store_import(StoreManager *manager, const char *project_dir, char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    if (store_mode(manager, project_dir, NULL, 0, NULL) == STORE_DB) { /* already moved; finish an interrupted move */
        store_finish_import(manager, project_dir);
        return true;
    }
    Db *db = store_db(manager);
    if (!db) {
        snprintf(why, whylen, "The project store is offline");
        return false;
    }
    /* Elect one importer without confusing ordinary readers for a competing
     * importer. The elected importer then waits for shared file operations to
     * drain before taking transition authority and pausing new operations. */
    char dir[PATH_MAX + 32], ownerp[PATH_MAX + 64], lockp[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    mkdir(dir, 0755);
    snprintf(ownerp, sizeof(ownerp), "%s/import.owner.lock", dir);
    int ownerfd = open(ownerp, O_RDWR | O_CREAT, 0644);
    if (ownerfd < 0 || flock(ownerfd, LOCK_EX | LOCK_NB) != 0) {
        if (ownerfd >= 0) close(ownerfd);
        snprintf(why, whylen, "This project is being moved into the store by another window");
        return false;
    }
    snprintf(lockp, sizeof(lockp), "%s/import.lock", dir);
    int lfd = open(lockp, O_RDWR | O_CREAT, 0644);
    int lockrc = -1;
    if (lfd >= 0) {
        do lockrc = flock(lfd, LOCK_EX); while (lockrc != 0 && errno == EINTR);
    }
    if (lockrc != 0) {
        if (lfd >= 0) close(lfd);
        flock(ownerfd, LOCK_UN);
        close(ownerfd);
        snprintf(why, whylen, "Couldn't lock the project's files for import");
        return false;
    }
    store_set_importing(manager, project_dir);
    bool ok = import_locked(manager, db, project_dir, dir, why, whylen);
    store_set_importing(manager, NULL);
    flock(lfd, LOCK_UN);
    close(lfd);
    flock(ownerfd, LOCK_UN);
    close(ownerfd);
    return ok;
}

bool store_needs_import(StoreManager *manager, const char *project_dir)
{
    return project_dir && project_dir[0] && store_mode(manager, project_dir, NULL, 0, NULL) == STORE_FILES;
}

/* ---- copies and missing projects ------------------------------------------------ */

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
static bool export_files_unlocked(StoreManager *manager, Db *db, const char *pid, const char *project_dir)
{
    if (!db_run(db, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", NULL, 0, NULL)) return false;
    if (!db_begin(db)) return false; /* one consistent source snapshot across all repositories */
    DbRows docs;
    if (!db_query(db, "SELECT id, name FROM documents WHERE project_id = ?", (DbArg[1]){ DB_S(pid) }, 1, &docs)) {
        db_rollback(db);
        return false;
    }
    bool ok = db_rows_has_columns(&docs, 2);
    ExportRev *revs = NULL;
    size_t nrevs = 0, cap = 0;
    for (size_t i = 0; ok && i < docs.nrows; i++) {
        const char *document_id, *document_name;
        ok = db_cell_text(&docs, i, 0, &document_id) && strlen(document_id) < 40 &&
             db_cell_text(&docs, i, 1, &document_name);
        if (!ok) break;
        DbRows r;
        if (!db_query(db, "SELECT id, content, origin, note, created_at FROM revisions WHERE document_id = ? ORDER BY seq",
                       (DbArg[1]){ DB_S(document_id) }, 1, &r)) {
            ok = false;
            break;
        }
        char path[PATH_MAX + 300];
        int path_len = snprintf(path, sizeof(path), "%s/%s", project_dir, document_name);
        ok = path_len >= 0 && (size_t)path_len < sizeof(path) && db_rows_has_columns(&r, 5);
        for (size_t k = 0; ok && k < r.nrows; k++) {
            const char *revision_id, *content, *origin, *note;
            double created_at;
            ok = db_cell_text(&r, k, 0, &revision_id) && strlen(revision_id) < sizeof(revs[0].id) &&
                 db_cell_text(&r, k, 1, &content) && db_cell_text(&r, k, 2, &origin) &&
                 db_cell_text(&r, k, 3, &note) && db_cell_double(&r, k, 4, &created_at) &&
                 created_at >= 0 && created_at < 0x1p63;
            if (!ok) break;
            store_set_write_time(manager, created_at);
            bool rec = false;
            ok = revisions_record_files(manager, project_dir, path, content, origin, note, &rec);
            Revision *list = NULL;
            size_t n = 0;
            if (ok) ok = revisions_list_files_checked(project_dir, path, &list, &n) && n > 0;
            if (n && nrevs == cap) {
                size_t next = cap ? cap * 2 : 64;
                if (next < cap || next > SIZE_MAX / sizeof(ExportRev)) ok = false;
                ExportRev *m = ok ? realloc(revs, next * sizeof(ExportRev)) : NULL;
                if (!m) ok = false;
                else {
                    revs = m;
                    cap = next;
                }
            }
            if (ok && n) { /* the newest snapshot is this revision (or the identical one before it) */
                snprintf(revs[nrevs].id, sizeof(revs[0].id), "%s", revision_id);
                snprintf(revs[nrevs].file, sizeof(revs[0].file), "%s", list[0].file);
                nrevs++;
            }
            free(list);
        }
        db_rows_free(&r);
    }
    db_rows_free(&docs);
    ChatSummary *cs = NULL;
    size_t nc = 0;
    if (ok) ok = sdb_chat_list_checked(db, pid, &cs, &nc);
    for (size_t i = 0; ok && i < nc; i++) {
        ChatMessage *m = NULL;
        size_t n = 0;
        if (!sdb_chat_load(db, pid, cs[i].id, &m, &n)) {
            ok = false;
            break;
        }
        for (size_t k = 0; ok && k < n; k++) {
            store_set_write_time(manager, m[k].ts);
            ok = chat_store_append_files(manager, project_dir, cs[i].id, cs[i].title, &m[k]);
        }
        if (ok && n && cs[i].observer_ignore) ok = chat_store_set_observer_files(project_dir, cs[i].id, false);
        chat_store_free_messages(m, n);
    }
    free(cs);
    static const RecordKind kinds[] = { REC_NOTE, REC_CANON, REC_FINDING };
    for (size_t k = 0; ok && k < 3; k++) {
        Record *recs = NULL;
        size_t n = 0;
        ok = sdb_record_list_checked(db, pid, kinds[k], &recs, &n);
        for (size_t i = 0; ok && i < n; i++)
            for (size_t v = 0; ok && v < recs[i].nversions; v++) {
                RecordVersion *ver = &recs[i].versions[v];
                for (size_t q = 0; q < ver->nsources; q++) {
                    SourceRef *src = &ver->sources[q];
                    if (src->kind != SRC_DOC) continue;
                    const char *file = "";
                    for (size_t e = 0; e < nrevs && !file[0]; e++)
                        if (!strcmp(revs[e].id, src->revision)) file = revs[e].file;
                    if (src->revision[0] && !file[0]) { ok = false; break; }
                    snprintf(src->revision, sizeof(src->revision), "%s", file); /* store ids mean nothing in files */
                }
                if (!ok) break;
                store_set_write_time(manager, (double)ver->ts);
                ok = records_append_files(manager, project_dir, kinds[k], recs[i].id, ver->status, ver->content,
                                          ver->sources, ver->nsources,
                                          kinds[k] == REC_FINDING ? &ver->finding : NULL, false, NULL);
            }
        records_free(recs, n);
    }
    cJSON *observer_state = ok ? observer_state_load_db(db, pid) : NULL;
    if (ok) ok = observer_state && observer_state_export_files(project_dir, observer_state);
    cJSON_Delete(observer_state);
    char observer_rules[17];
    if (ok) ok = observer_rules_get_db(db, pid, observer_rules);
    if (ok) ok = observer_rules_set_files(project_dir, observer_rules);
    store_set_write_time(manager, 0);
    free(revs);
    if (ok) ok = db_commit(db);
    if (!ok) db_rollback(db);
    return ok;
}

/* Explicit exports must start fresh. In particular, a failed export has
 * partial append-only logs; retrying in place would duplicate chats/versions.
 * Preserve the stable transition lock, and leave failed output for its owner
 * to remove (backup_project already owns and cleans its temporary export). */
static bool export_target_empty(const char *project_dir)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/.scribodo", project_dir);
    DIR *d = opendir(path);
    if (!d) return false;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(d);
        if (!ent) { ok = errno == 0; break; }
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..") && strcmp(ent->d_name, "import.lock")) {
            ok = false;
            break;
        }
    }
    return closedir(d) == 0 && ok;
}

static bool export_files(StoreManager *manager, Db *db, const char *pid, const char *project_dir, char *why, size_t whylen)
{
    int export_lock = project_store_file_lock_exclusive(project_dir);
    if (export_lock < 0) return false;
    bool ok = export_target_empty(project_dir);
    if (!ok) snprintf(why, whylen, "Export needs a fresh destination with no project data; use a new folder");
    /* The empty-metadata check also excludes any store marker. Do not probe
     * store_mode here: our exclusive lock deliberately pauses other readers. */
    if (ok) ok = export_files_unlocked(manager, db, pid, project_dir);
    project_store_file_unlock(export_lock);
    return ok;
}

bool store_export_project(StoreManager *manager, const char *project_id, const char *dir, char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    Db *db = store_db(manager);
    if (!db) {
        snprintf(why, whylen, "The project store is offline");
        return false;
    }
    char sd[PATH_MAX + 32];
    snprintf(sd, sizeof(sd), "%s/.scribodo", dir);
    mkdir(dir, 0755);
    mkdir(sd, 0755);
    bool ok = export_files(manager, db, project_id, dir, why, whylen);
    if (!ok && why && whylen && !why[0]) snprintf(why, whylen, "Couldn't export the project: %s",
                      db_error(db)[0] ? db_error(db) : "project data couldn't be read or a file couldn't be written");
    return ok;
}

/* A ready snapshot is immutable and reused after registration/import failures.
 * An incomplete export is rebuilt only in its journal-owned staging folder. */
static bool fork_snapshot_prepare(StoreManager *manager, Db *db, const char *project, const char *id,
                                  const char *stage, char *why, size_t whylen)
{
    ForkSnapshot snapshot;
    FileTextStatus status = fork_snapshot_read(project, &snapshot);
    struct stat st;
    if (status == FILE_TEXT_OK && strcmp(snapshot.source, id)) {
        /* A copied folder may contain a journal left after the previous fork's
         * marker committed but before its archive/cleanup completed. */
        if (strcmp(snapshot.target, id) || !fork_snapshot_finish(project, stage)) goto damaged;
        status = FILE_TEXT_MISSING;
    }
    if (status == FILE_TEXT_ERROR) goto damaged;
    if (status == FILE_TEXT_MISSING) {
        if (lstat(stage, &st) == 0 || errno != ENOENT) goto damaged;
        memset(&snapshot, 0, sizeof(snapshot));
        snprintf(snapshot.source, sizeof(snapshot.source), "%s", id);
        if (!fork_snapshot_write(project, &snapshot)) goto failed;
        crash_point("fork.journal_opened"); /* fork.json names the source; no staging yet (GAP-004 matrix, fork row) */
    }
    if (snapshot.ready) {
        if (lstat(stage, &st) != 0 || !S_ISDIR(st.st_mode) || fingerprint(stage) != snapshot.signature) goto damaged;
        return true;
    }
    if (lstat(stage, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || removefile(stage, NULL, REMOVEFILE_RECURSIVE) != 0) goto failed;
    } else if (errno != ENOENT) goto failed;
    if (mkdir(stage, 0700) != 0 || !export_files_unlocked(manager, db, id, stage)) goto failed;
    snapshot.ready = true;
    snapshot.signature = fingerprint(stage);
    if (!fork_snapshot_write(project, &snapshot)) goto failed;
    crash_point("fork.exported"); /* the staging export is complete and signed; nothing imported */
    return true;
damaged:
    snprintf(why, whylen, "The pending fork snapshot is missing, changed or unrecognized; it was preserved, not imported");
    return false;
failed:
    snprintf(why, whylen, "Couldn't prepare the fork snapshot; the copy is unchanged and the export can be retried");
    return false;
}

bool store_fork(StoreManager *manager, const char *project_dir, char *why, size_t whylen)
{
    char original[PATH_MAX];
    if (store_problem(manager, project_dir, original, sizeof(original)) != STORE_PROBLEM_COPY) {
        snprintf(why, whylen, "This folder isn't a copy");
        return false;
    }
    Db *db = store_db(manager);
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
    /* Own the complete export/import/marker transition so competing forks
     * cannot replace each other's snapshot or registration. */
    char dir[PATH_MAX + 32], ownerp[PATH_MAX + 64], lockp[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project_dir);
    snprintf(ownerp, sizeof(ownerp), "%s/import.owner.lock", dir);
    snprintf(lockp, sizeof(lockp), "%s/import.lock", dir);
    int ownerfd = open(ownerp, O_RDWR | O_CREAT, 0644);
    if (ownerfd < 0 || flock(ownerfd, LOCK_EX | LOCK_NB) != 0) {
        if (ownerfd >= 0) close(ownerfd);
        snprintf(why, whylen, "This project is being moved into the store by another window");
        return false;
    }
    int lfd = open(lockp, O_RDWR | O_CREAT, 0644);
    int lockrc = -1;
    if (lfd >= 0) do lockrc = flock(lfd, LOCK_EX); while (lockrc != 0 && errno == EINTR);
    if (lockrc != 0) {
        if (lfd >= 0) close(lfd);
        flock(ownerfd, LOCK_UN);
        close(ownerfd);
        snprintf(why, whylen, "Couldn't lock the project's files for import");
        return false;
    }
    char current[64] = "";
    f = fopen(path, "rb");
    if (f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        const char *k = strstr(buf, "\"project_id\":\"");
        if (k) snprintf(current, sizeof(current), "%.*s", (int)strcspn(k + 14, "\""), k + 14);
    }
    if (store_problem(manager, project_dir, NULL, 0) != STORE_PROBLEM_COPY || strcmp(current, id) != 0) {
        flock(lfd, LOCK_UN);
        close(lfd);
        flock(ownerfd, LOCK_UN);
        close(ownerfd);
        snprintf(why, whylen, "This folder is no longer the same copy");
        return false;
    }
    char stage[PATH_MAX + 80];
    snprintf(stage, sizeof(stage), "%s/fork-snapshot", dir);
    bool ok = fork_snapshot_prepare(manager, db, project_dir, id, stage, why, whylen);
    if (ok) {
        store_set_importing(manager, project_dir);
        ok = import_locked_from(manager, db, project_dir, stage, dir, why, whylen);
    }
    if (ok) {
        crash_point("fork.imported"); /* marker written for the new project; staging, witness and fork.json still present */
        if (!fork_snapshot_finish(project_dir, stage))
            snprintf(why, whylen, "The fork is complete; its staging snapshot was retained because archiving couldn't finish");
    }
    store_set_importing(manager, NULL);
    flock(lfd, LOCK_UN);
    close(lfd);
    flock(ownerfd, LOCK_UN);
    close(ownerfd);
    return ok;
}

bool store_restore(StoreManager *manager, const char *project_dir, char *why, size_t whylen)
{
    if (store_problem(manager, project_dir, NULL, 0) != STORE_PROBLEM_MISSING) {
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
                                             "observer", "observer-rules", "renames.jsonl" };
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
    store_forget_verification(manager);
    if (!newest[0]) {
        snprintf(why, whylen, "No saved files to restore from: the project now starts again on its documents");
        return true;
    }
    return store_import(manager, project_dir, why, whylen);
}
