/* Revisions in the project store: full text per revision, keyed by the
 * document's stable id (so a rename needs nothing). Called by revisions.c
 * when the project is in the store. */
#define _DARWIN_C_SOURCE
#include "store_backends.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static void hash16(const char *s, char out[17])
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

/* Record `text` for the document unless it equals the newest revision (or,
 * for a source snapshot, any revision): one transaction. */
bool sdb_revision_record(Db *db, const char *project_id, const char *doc_path, const char *text, const char *origin,
                         const char *note, bool any_match, bool *recorded, char *id_out, size_t idlen)
{
    if (recorded) *recorded = false;
    char doc[40];
    if (!store_document_id_db(db, project_id, base_name(doc_path), true, doc, sizeof(doc))) return false;
    char hash[17];
    hash16(text, hash);
    if (!db_begin(db)) return false;
    /* Lock the document's row so two writers (UI, observer) serialise. */
    DbRows lock;
    bool ok = db_query(db, "SELECT id FROM documents WHERE id = ? FOR UPDATE", (DbArg[1]){ DB_S(doc) }, 1, &lock);
    db_rows_free(&lock);
    DbRows r;
    bool same = false;
    if (ok && db_query(db,
                       any_match ? "SELECT id FROM revisions WHERE document_id = ? AND hash = ? ORDER BY seq DESC LIMIT 1"
                                 : "SELECT id, hash FROM revisions WHERE document_id = ? ORDER BY seq DESC LIMIT 1",
                       any_match ? (DbArg[2]){ DB_S(doc), DB_S(hash) } : (DbArg[2]){ DB_S(doc), DB_S(NULL) },
                       any_match ? 2 : 1, &r)) {
        if (r.nrows && (any_match || (db_cell(&r, 0, 1) && !strcmp(db_cell(&r, 0, 1), hash)))) {
            same = true;
            if (id_out) snprintf(id_out, idlen, "%s", db_cell(&r, 0, 0));
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
        if (ok && id_out) snprintf(id_out, idlen, "%s", id);
        if (ok && recorded) *recorded = true;
    }
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}

size_t sdb_revision_list(Db *db, const char *project_id, const char *doc_path, Revision **out)
{
    *out = NULL;
    char doc[40];
    if (!store_document_id_db(db, project_id, base_name(doc_path), false, doc, sizeof(doc))) return 0;
    DbRows r;
    if (!db_query(db,
                  "SELECT id, origin, note, hash, created_at, CHAR_LENGTH(content) FROM revisions WHERE document_id = ? "
                  "ORDER BY seq DESC",
                  (DbArg[1]){ DB_S(doc) }, 1, &r))
        return 0;
    Revision *list = r.nrows ? calloc(r.nrows, sizeof(Revision)) : NULL;
    size_t n = 0;
    for (size_t i = 0; list && i < r.nrows; i++) {
        Revision *v = &list[n++];
        snprintf(v->file, sizeof(v->file), "%s", db_cell(&r, i, 0));
        snprintf(v->origin, sizeof(v->origin), "%s", db_cell(&r, i, 1) ? db_cell(&r, i, 1) : "");
        snprintf(v->note, sizeof(v->note), "%s", db_cell(&r, i, 2) ? db_cell(&r, i, 2) : "");
        snprintf(v->hash, sizeof(v->hash), "%s", db_cell(&r, i, 3) ? db_cell(&r, i, 3) : "");
        v->ts = (time_t)atof(db_cell(&r, i, 4) ? db_cell(&r, i, 4) : "0");
        v->bytes = (size_t)atoll(db_cell(&r, i, 5) ? db_cell(&r, i, 5) : "0");
    }
    db_rows_free(&r);
    *out = list;
    return n;
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
    if (r.nrows && db_cell(&r, 0, 0)) {
        char h[17];
        hash16(db_cell(&r, 0, 0), h);
        if (!db_cell(&r, 0, 1) || !strcmp(h, db_cell(&r, 0, 1))) text = strdup(db_cell(&r, 0, 0));
    }
    db_rows_free(&r);
    return text;
}
