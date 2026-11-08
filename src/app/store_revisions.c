/* Revisions in the project store: full text per revision, keyed by the
 * document's stable id (so a rename needs nothing). Called by revisions.c
 * when the project is in the store. */
#define _DARWIN_C_SOURCE
#include "store_backends.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

void sdb_content_hash16(const char *s, char out[17])
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 1099511628211ull;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

typedef struct {
    const char *id;
    const char *origin;
    const char *note;
    const char *hash;
    double created_at;
    uint64_t bytes;
} RevisionRow;

static bool revision_row(const DbRows *rows, size_t row, RevisionRow *out)
{
    bool ok = db_rows_has_columns(rows, 6) && db_cell_text(rows, row, 0, &out->id) &&
           db_cell_text(rows, row, 1, &out->origin) && db_cell_text(rows, row, 2, &out->note) &&
           db_cell_text(rows, row, 3, &out->hash) && db_cell_double(rows, row, 4, &out->created_at) &&
           db_cell_u64(rows, row, 5, &out->bytes) && out->created_at >= 0 && out->created_at < 0x1p63 &&
           out->bytes <= SIZE_MAX;
    return ok && strlen(out->id) < sizeof(((Revision *)0)->file) &&
           strlen(out->origin) < sizeof(((Revision *)0)->origin) && strlen(out->note) < sizeof(((Revision *)0)->note) &&
           strlen(out->hash) < sizeof(((Revision *)0)->hash);
}

/* Record `text` for the document unless it equals the newest revision (or,
 * for a source snapshot, any revision): one transaction. */
bool sdb_revision_record(Db *db, const char *project_id, const char *doc_path, const char *text, const char *origin,
                         const char *note, bool any_match, bool *recorded, char *id_out, size_t idlen)
{
    if (recorded) *recorded = false;
    if (id_out && idlen) id_out[0] = '\0';
    if (!db || !project_id || !doc_path || !text || !origin || db_in_transaction(db)) return false;
    char doc[40];
    if (!store_document_id_db(db, project_id, base_name(doc_path), true, doc, sizeof(doc))) return false;
    char hash[17];
    sdb_content_hash16(text, hash);
    if (!db_begin(db)) return false;
    /* Lock the document's row so two writers (UI, observer) serialise. */
    DbRows lock;
    bool ok = db_query(db, "SELECT id FROM documents WHERE id = ? FOR UPDATE", (DbArg[1]){ DB_S(doc) }, 1, &lock);
    const char *locked_id;
    ok = ok && lock.nrows == 1 && db_rows_has_columns(&lock, 1) && db_cell_text(&lock, 0, 0, &locked_id) &&
         !strcmp(locked_id, doc);
    db_rows_free(&lock);
    DbRows r;
    bool same = false;
    char revision_id[40] = "";
    if (ok && db_query(db,
                       any_match ? "SELECT id, 1 FROM revisions WHERE document_id = ? AND source_hash = SHA2(?,256) ORDER BY seq DESC LIMIT 1"
                                 : "SELECT id, source_hash = SHA2(?,256) FROM revisions WHERE document_id = ? ORDER BY seq DESC LIMIT 1",
                       any_match ? (DbArg[2]){ DB_S(doc), DB_S(text) } : (DbArg[2]){ DB_S(text), DB_S(doc) },
                       2, &r)) {
        const char *existing_id = NULL;
        int64_t matches = 0;
        bool valid = r.nrows <= 1 && db_rows_has_columns(&r, 2);
        if (r.nrows == 1)
            valid = db_cell_text(&r, 0, 0, &existing_id) && strlen(existing_id) < sizeof(revision_id) &&
                    db_cell_i64(&r, 0, 1, &matches) && (matches == 0 || matches == 1);
        ok = ok && valid;
        if (ok && r.nrows == 1 && matches) {
            same = true;
            snprintf(revision_id, sizeof(revision_id), "%s", existing_id);
        }
        db_rows_free(&r);
    } else {
        ok = false;
    }
    if (ok && !same) {
        char id[40];
        store_new_id('r', id, sizeof(id));
        DbArg a[7] = { DB_S(id), DB_S(doc), DB_S(hash), DB_S(text), DB_S(origin), DB_S(note ? note : ""), DB_D(now_s()) };
        ok = db_run(db,
                    "INSERT INTO revisions (id, document_id, hash, content, origin, note, created_at) VALUES (?,?,?,?,?,?,?)",
                    a, 7, NULL) &&
             db_run(db, "UPDATE documents SET current_revision_id = ? WHERE id = ?", (DbArg[2]){ DB_S(id), DB_S(doc) }, 2,
                    NULL);
        if (ok) snprintf(revision_id, sizeof(revision_id), "%s", id);
    }
    if (ok && !any_match && !strcmp(origin, "save"))
        ok = db_run(db, "UPDATE documents SET saved_revision_id = ? WHERE id = ?",
                    (DbArg[2]){ DB_S(revision_id), DB_S(doc) }, 2, NULL);
    if (ok) ok = db_commit(db);
    if (!ok) db_rollback(db);
    if (ok && recorded) *recorded = !same;
    if (ok && id_out) snprintf(id_out, idlen, "%s", revision_id);
    return ok;
}

static bool identity_text(const char *s, size_t limit)
{
    if (!s || !s[0] || strnlen(s, limit) >= limit) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}

bool sdb_saved_revision_identity(Db *db, const char *project_id, const char *doc_name, SavedRevisionIdentity *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!db || !identity_text(project_id, sizeof(out->project_id)) || !identity_text(doc_name, 256) ||
        strchr(doc_name, '/') || strchr(doc_name, '\\') || strstr(doc_name, "..") || !strcmp(doc_name, "."))
        return false;
    char doc[40];
    if (!store_document_id_db(db, project_id, doc_name, false, doc, sizeof(doc))) return false;
    DbRows rows;
    if (!db_query(db, "SELECT DATABASE(),p.id,d.id,r.id,r.source_hash,OCTET_LENGTH(r.content)"
                      " FROM projects p JOIN documents d ON d.project_id=p.id"
                      " JOIN revisions r ON r.document_id=d.id AND r.id=d.saved_revision_id"
                      " WHERE p.id=? AND d.id=? AND d.name=?",
                  (DbArg[3]){ DB_S(project_id), DB_S(doc), DB_S(doc_name) }, 3, &rows)) return false;
    const char *values[5] = {0};
    const size_t limits[5] = {64, 40, 40, 40, 65};
    SavedRevisionIdentity identity = {0};
    bool ok = rows.nrows == 1 && db_rows_has_columns(&rows, 6);
    for (size_t i = 0; ok && i < 5; i++)
        ok = db_cell_text(&rows, 0, i, &values[i]) && identity_text(values[i], limits[i]);
    if (ok) {
        ok = !strcmp(values[1], project_id) && !strcmp(values[2], doc) && strlen(values[4]) == 64 &&
             strspn(values[4], "0123456789abcdef") == 64 && db_cell_u64(&rows, 0, 5, &identity.bytes);
    }
    if (ok) {
        snprintf(identity.database, sizeof(identity.database), "%s", values[0]);
        snprintf(identity.project_id, sizeof(identity.project_id), "%s", values[1]);
        snprintf(identity.document_id, sizeof(identity.document_id), "%s", values[2]);
        snprintf(identity.revision_id, sizeof(identity.revision_id), "%s", values[3]);
        snprintf(identity.source_hash, sizeof(identity.source_hash), "%s", values[4]);
        *out = identity;
    }
    db_rows_free(&rows);
    return ok;
}

bool sdb_revision_list_checked(Db *db, const char *project_id, const char *doc_path, Revision **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char doc[40];
    DbRows document;
    if (!db_query(db, "SELECT id FROM documents WHERE project_id = ? AND name = ?",
                  (DbArg[2]){ DB_S(project_id), DB_S(base_name(doc_path)) }, 2, &document))
        return false;
    const char *document_id = NULL;
    bool document_ok = document.nrows <= 1 && db_rows_has_columns(&document, 1) &&
                       (!document.nrows || (db_cell_text(&document, 0, 0, &document_id) && strlen(document_id) < sizeof(doc)));
    if (document_ok && document.nrows) snprintf(doc, sizeof(doc), "%s", document_id);
    bool found = document.nrows == 1;
    db_rows_free(&document);
    if (!document_ok) return false;
    if (!found) return true;
    DbRows r;
    if (!db_query(db,
                  "SELECT id, origin, note, hash, created_at, CHAR_LENGTH(content) FROM revisions WHERE document_id = ? "
                  "ORDER BY seq DESC",
                  (DbArg[1]){ DB_S(doc) }, 1, &r))
        return false;
    bool ok = db_rows_has_columns(&r, 6);
    Revision *list = ok && r.nrows ? calloc(r.nrows, sizeof(Revision)) : NULL;
    if (r.nrows && !list) ok = false;
    size_t n = 0;
    for (size_t i = 0; ok && i < r.nrows; i++) {
        RevisionRow row;
        ok = revision_row(&r, i, &row);
        if (!ok) break;
        Revision *v = &list[n++];
        snprintf(v->file, sizeof(v->file), "%s", row.id);
        snprintf(v->origin, sizeof(v->origin), "%s", row.origin);
        snprintf(v->note, sizeof(v->note), "%s", row.note);
        snprintf(v->hash, sizeof(v->hash), "%s", row.hash);
        v->ts = (time_t)row.created_at;
        v->bytes = (size_t)row.bytes;
    }
    db_rows_free(&r);
    if (!ok) {
        free(list);
        return false;
    }
    *out = list;
    *count = n;
    return true;
}

size_t sdb_revision_list(Db *db, const char *project_id, const char *doc_path, Revision **out)
{
    size_t count = 0;
    (void)sdb_revision_list_checked(db, project_id, doc_path, out, &count);
    return count;
}

char *sdb_revision_load(Db *db, const char *project_id, const char *doc_path, const Revision *rev)
{
    char doc[40];
    if (!store_document_id_db(db, project_id, base_name(doc_path), false, doc, sizeof(doc))) return NULL;
    DbRows r;
    /* The revision must belong to this document. */
    if (!db_query(db, "SELECT content, hash FROM revisions WHERE id = ? AND document_id = ?",
                  (DbArg[2]){ DB_S(rev->file), DB_S(doc) }, 2, &r))
        return NULL;
    char *text = NULL;
    const char *content = NULL, *stored_hash = NULL;
    bool valid = r.nrows <= 1 && db_rows_has_columns(&r, 2);
    if (valid && r.nrows == 1)
        valid = db_cell_text(&r, 0, 0, &content) && db_cell_nullable(&r, 0, 1, &stored_hash);
    if (valid && r.nrows == 1) {
        char h[17];
        sdb_content_hash16(content, h);
        if (!stored_hash || !strcmp(h, stored_hash)) text = strdup(content);
    }
    db_rows_free(&r);
    return text;
}
