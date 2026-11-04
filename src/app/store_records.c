/* Notes, canon and findings in the project store: records, every version,
 * and sources by id. Called by records.c for projects in the store. */
#define _DARWIN_C_SOURCE
#include "store_backends.h"
#include "store_entities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *kind_name(RecordKind k) { return k == REC_NOTE ? "note" : k == REC_CANON ? "canon" : "finding"; }

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* Does the row exist *in this project* (sql takes the id, then the scope)?
 * Live sources only point within their own project (red-team 008). */
static bool exists(Db *db, const char *sql, const char *id, const char *scope)
{
    DbRows r;
    if (!id || !id[0] || !scope || !scope[0] || !db_query(db, sql, (DbArg[2]){ DB_S(id), DB_S(scope) }, 2, &r)) return false;
    bool e = r.nrows > 0;
    db_rows_free(&r);
    return e;
}

static const char *nz(const char *s) { return s && s[0] ? s : NULL; }

bool sdb_record_append(Db *db, const char *project_id, RecordKind kind, const char *id, const char *status,
                       const char *content, const SourceRef *sources, size_t nsources, const FindingInfo *finding,
                       bool fresh, int *ver_out)
{
    double t = now_s();
    if (!db_begin(db)) return false;
    DbArg rec[4] = { DB_S(id), DB_S(project_id), DB_S(kind_name(kind)), DB_D(t) };
    bool ok = db_run(db, "INSERT IGNORE INTO records (id, project_id, kind, created_at) VALUES (?,?,?,?)", rec, 4, NULL);
    /* The record must be this project's and this kind (never an id from
     * elsewhere), and its row is locked so version numbers can't race. */
    DbRows r;
    ok = ok && db_query(db, "SELECT project_id, kind FROM records WHERE id = ? FOR UPDATE", (DbArg[1]){ DB_S(id) }, 1, &r);
    if (ok) {
        ok = r.nrows == 1 && db_cell(&r, 0, 0) && !strcmp(db_cell(&r, 0, 0), project_id) && db_cell(&r, 0, 1) &&
             !strcmp(db_cell(&r, 0, 1), kind_name(kind));
        db_rows_free(&r);
    }
    int ver = 1;
    if (ok && !fresh) {
        ok = db_query(db, "SELECT COALESCE(MAX(ver), 0) + 1 FROM record_versions WHERE record_id = ?", (DbArg[1]){ DB_S(id) },
                      1, &r);
        if (ok) {
            ver = r.nrows && db_cell(&r, 0, 0) ? atoi(db_cell(&r, 0, 0)) : 1;
            db_rows_free(&r);
        }
    }
    const FindingInfo *f = finding;
    const char *related = f && exists(db, "SELECT id FROM records WHERE id = ? AND project_id = ?", f->related, project_id)
                              ? f->related
                              : NULL;
    DbArg v[11] = { DB_S(id), DB_I(ver), DB_S(status), DB_S(content ? content : ""),
                    DB_S(f ? nz(f->type) : NULL), DB_S(f ? nz(f->subject) : NULL), DB_S(f ? nz(f->relation) : NULL),
                    DB_S(f ? nz(f->object) : NULL), DB_S(related), DB_S(f ? f->related_text : NULL),
                    DB_D(t) };
    ok = ok && db_run(db,
                      "INSERT INTO record_versions (record_id, ver, status, content, finding_type, subject, relation, object,"
                      " related_id, related_text, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                      v, 11, NULL);
    for (size_t i = 0; ok && i < nsources; i++) {
        const SourceRef *s = &sources[i];
        char doc[40] = "";
        const char *rev = NULL, *conv = NULL;
        if (s->kind == SRC_DOC) {
            if (!strchr(s->doc, '/') && s->doc[0]) store_document_id(project_id, s->doc, true, doc, sizeof(doc));
            /* the snapshot must be of this very document */
            if (exists(db, "SELECT id FROM revisions WHERE id = ? AND document_id = ?", s->revision, doc)) rev = s->revision;
        } else if (exists(db, "SELECT id FROM conversations WHERE id = ? AND project_id = ?", s->chat, project_id)) {
            conv = s->chat;
        }
        DbArg a[10] = { DB_S(id), DB_I(ver), DB_S(s->kind == SRC_DOC ? "doc" : "chat"), DB_S(nz(doc)), DB_S(rev),
                        DB_S(conv), DB_I(s->kind == SRC_CHAT ? s->message : 0), DB_I(s->start), DB_I(s->len),
                        DB_S(s->quote ? s->quote : "") };
        ok = db_run(db,
                    "INSERT INTO source_refs (record_id, ver, kind, document_id, revision_id, conversation_id, message_seq,"
                    " start, len, quote) VALUES (?,?,?,?,?,?,?,?,?,?)",
                    a, 10, NULL);
    }
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    /* A new finding's entities and link (the graph is derived from
     * findings; a failure here leaves it to the next backfill). */
    if (ok && kind == REC_FINDING && finding && ver == 1) sdb_entities_from_finding(db, project_id, id, finding);
    if (ok && ver_out) *ver_out = ver;
    return ok;
}

typedef struct {
    const char *id;
    size_t index;
} IdIndex;

static int by_id(const void *a, const void *b) { return strcmp(((const IdIndex *)a)->id, ((const IdIndex *)b)->id); }

size_t sdb_record_list(Db *db, const char *project_id, RecordKind kind, Record **out)
{
    *out = NULL;
    DbArg k[2] = { DB_S(project_id), DB_S(kind_name(kind)) };
    DbRows rv, rs;
    /* Records ordered by their newest change (most recent first). */
    if (!db_query(db,
                  "SELECT r.id, v.ver, v.created_at, v.status, v.content, v.finding_type, v.subject, v.relation, v.object,"
                  " v.related_id, v.related_text, r.created_at,"
                  " (SELECT MAX(created_at) FROM record_versions WHERE record_id = r.id) AS changed"
                  " FROM records r JOIN record_versions v ON v.record_id = r.id"
                  " WHERE r.project_id = ? AND r.kind = ? ORDER BY changed DESC, r.id, v.ver",
                  k, 2, &rv))
        return 0;
    if (!db_query(db,
                  "SELECT s.record_id, s.ver, s.kind, d.name, s.revision_id, s.conversation_id, s.message_seq, s.start,"
                  " s.len, s.quote FROM source_refs s JOIN records r ON r.id = s.record_id"
                  " LEFT JOIN documents d ON d.id = s.document_id"
                  " WHERE r.project_id = ? AND r.kind = ? ORDER BY s.id",
                  k, 2, &rs)) {
        db_rows_free(&rv);
        return 0;
    }
    Record *list = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < rv.nrows; i++) {
        const char *id = db_cell(&rv, i, 0);
        if (!n || strcmp(list[n - 1].id, id)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                Record *l = realloc(list, cap * sizeof(Record));
                if (!l) break;
                list = l;
            }
            memset(&list[n], 0, sizeof(Record));
            snprintf(list[n].id, sizeof(list[n].id), "%s", id);
            /* creation order: the record's created_at in microseconds */
            list[n].created_seq = (size_t)(atof(db_cell(&rv, i, 11) ? db_cell(&rv, i, 11) : "0") * 1e6);
            n++;
        }
        Record *r = &list[n - 1];
        RecordVersion *vs = realloc(r->versions, (r->nversions + 1) * sizeof(RecordVersion));
        if (!vs) break;
        r->versions = vs;
        RecordVersion *v = &vs[r->nversions++];
        memset(v, 0, sizeof(*v));
        v->ver = atoi(db_cell(&rv, i, 1));
        v->ts = (time_t)atof(db_cell(&rv, i, 2) ? db_cell(&rv, i, 2) : "0");
        snprintf(v->status, sizeof(v->status), "%s", db_cell(&rv, i, 3) ? db_cell(&rv, i, 3) : "");
        v->content = strdup(db_cell(&rv, i, 4) ? db_cell(&rv, i, 4) : "");
        FindingInfo *f = &v->finding;
        if (db_cell(&rv, i, 5)) snprintf(f->type, sizeof(f->type), "%s", db_cell(&rv, i, 5));
        if (db_cell(&rv, i, 6)) snprintf(f->subject, sizeof(f->subject), "%s", db_cell(&rv, i, 6));
        if (db_cell(&rv, i, 7)) snprintf(f->relation, sizeof(f->relation), "%s", db_cell(&rv, i, 7));
        if (db_cell(&rv, i, 8)) snprintf(f->object, sizeof(f->object), "%s", db_cell(&rv, i, 8));
        if (db_cell(&rv, i, 9)) snprintf(f->related, sizeof(f->related), "%s", db_cell(&rv, i, 9));
        if (db_cell(&rv, i, 10)) f->related_text = strdup(db_cell(&rv, i, 10));
    }
    /* Attach sources to their versions (binary search by record id). */
    IdIndex *idx = n ? malloc(n * sizeof(IdIndex)) : NULL;
    for (size_t i = 0; idx && i < n; i++) idx[i] = (IdIndex){ list[i].id, i };
    if (idx) qsort(idx, n, sizeof(IdIndex), by_id);
    for (size_t i = 0; idx && i < rs.nrows; i++) {
        IdIndex key = { db_cell(&rs, i, 0), 0 };
        IdIndex *hit = bsearch(&key, idx, n, sizeof(IdIndex), by_id);
        if (!hit) continue;
        Record *r = &list[hit->index];
        int ver = atoi(db_cell(&rs, i, 1));
        RecordVersion *v = NULL;
        for (size_t k2 = 0; k2 < r->nversions && !v; k2++)
            if (r->versions[k2].ver == ver) v = &r->versions[k2];
        if (!v) continue;
        SourceRef *s = realloc(v->sources, (v->nsources + 1) * sizeof(SourceRef));
        if (!s) continue;
        v->sources = s;
        SourceRef *src = &s[v->nsources];
        memset(src, 0, sizeof(*src));
        src->kind = db_cell(&rs, i, 2) && !strcmp(db_cell(&rs, i, 2), "chat") ? SRC_CHAT : SRC_DOC;
        if (db_cell(&rs, i, 3)) snprintf(src->doc, sizeof(src->doc), "%s", db_cell(&rs, i, 3)); /* current name */
        if (db_cell(&rs, i, 4)) snprintf(src->revision, sizeof(src->revision), "%s", db_cell(&rs, i, 4));
        if (db_cell(&rs, i, 5)) snprintf(src->chat, sizeof(src->chat), "%s", db_cell(&rs, i, 5));
        src->message = db_cell(&rs, i, 6) ? atol(db_cell(&rs, i, 6)) : 0;
        src->start = (size_t)atoll(db_cell(&rs, i, 7) ? db_cell(&rs, i, 7) : "0");
        src->len = (size_t)atoll(db_cell(&rs, i, 8) ? db_cell(&rs, i, 8) : "0");
        src->quote = strdup(db_cell(&rs, i, 9) ? db_cell(&rs, i, 9) : "");
        if (src->quote) v->nsources++;
    }
    free(idx);
    db_rows_free(&rv);
    db_rows_free(&rs);
    *out = list;
    return n;
}
