/* Notes, canon and findings in the project store: records, every version,
 * and sources by id. Called by records.c for projects in the store. */
#define _DARWIN_C_SOURCE
#include "store_backends.h"
#include "store_entities.h"

#include <limits.h>
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
    const char *found_id;
    bool e = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_text(&r, 0, 0, &found_id) &&
             !strcmp(found_id, id);
    db_rows_free(&r);
    return e;
}

static const char *nz(const char *s) { return s && s[0] ? s : NULL; }

typedef struct {
    const char *id;
    int ver;
    double created_at;
    const char *status;
    const char *content;
    const char *finding_type;
    const char *subject;
    const char *relation;
    const char *object;
    const char *related_id;
    const char *related_text;
    double record_created_at;
    double changed_at;
} RecordVersionRow;

typedef struct {
    const char *record_id;
    int ver;
    SourceKind kind;
    const char *document;
    const char *revision;
    const char *conversation;
    long message;
    size_t start;
    size_t len;
    const char *quote;
} SourceRow;

static bool int_cell(const DbRows *rows, size_t row, size_t col, int *out)
{
    int64_t value;
    if (!db_cell_i64(rows, row, col, &value) || value < INT_MIN || value > INT_MAX) return false;
    *out = (int)value;
    return true;
}

static bool record_version_row(const DbRows *rows, size_t row, RecordVersionRow *out)
{
    return db_rows_has_columns(rows, 13) && db_cell_text(rows, row, 0, &out->id) &&
           int_cell(rows, row, 1, &out->ver) && out->ver > 0 &&
           db_cell_double(rows, row, 2, &out->created_at) && out->created_at >= 0 &&
           out->created_at <= (double)INT64_MAX && db_cell_text(rows, row, 3, &out->status) &&
           db_cell_text(rows, row, 4, &out->content) && db_cell_nullable(rows, row, 5, &out->finding_type) &&
           db_cell_nullable(rows, row, 6, &out->subject) && db_cell_nullable(rows, row, 7, &out->relation) &&
           db_cell_nullable(rows, row, 8, &out->object) && db_cell_nullable(rows, row, 9, &out->related_id) &&
           db_cell_nullable(rows, row, 10, &out->related_text) &&
           db_cell_double(rows, row, 11, &out->record_created_at) && out->record_created_at >= 0 &&
           out->record_created_at <= (double)SIZE_MAX / 1e6 && db_cell_double(rows, row, 12, &out->changed_at) &&
           out->changed_at >= 0 && out->changed_at <= (double)INT64_MAX;
}

static bool source_row(const DbRows *rows, size_t row, SourceRow *out)
{
    const char *kind;
    int64_t message;
    uint64_t start, len;
    if (!db_rows_has_columns(rows, 10) || !db_cell_text(rows, row, 0, &out->record_id) ||
        !int_cell(rows, row, 1, &out->ver) || out->ver <= 0 || !db_cell_text(rows, row, 2, &kind) ||
        !db_cell_nullable(rows, row, 3, &out->document) || !db_cell_nullable(rows, row, 4, &out->revision) ||
        !db_cell_nullable(rows, row, 5, &out->conversation) || !db_cell_i64(rows, row, 6, &message) ||
        message < LONG_MIN || message > LONG_MAX || !db_cell_u64(rows, row, 7, &start) || start > SIZE_MAX ||
        !db_cell_u64(rows, row, 8, &len) || len > SIZE_MAX || !db_cell_text(rows, row, 9, &out->quote))
        return false;
    if (!strcmp(kind, "doc")) out->kind = SRC_DOC;
    else if (!strcmp(kind, "chat")) out->kind = SRC_CHAT;
    else return false;
    out->message = (long)message;
    out->start = (size_t)start;
    out->len = (size_t)len;
    return true;
}

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
        const char *owner, *stored_kind;
        ok = r.nrows == 1 && db_rows_has_columns(&r, 2) && db_cell_text(&r, 0, 0, &owner) &&
             db_cell_text(&r, 0, 1, &stored_kind) && !strcmp(owner, project_id) && !strcmp(stored_kind, kind_name(kind));
        db_rows_free(&r);
    }
    int ver = 1;
    if (ok && !fresh) {
        ok = db_query(db, "SELECT COALESCE(MAX(ver), 0) + 1 FROM record_versions WHERE record_id = ?", (DbArg[1]){ DB_S(id) },
                      1, &r);
        if (ok) {
            ok = r.nrows == 1 && db_rows_has_columns(&r, 1) && int_cell(&r, 0, 0, &ver) && ver > 0;
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
            if (!strchr(s->doc, '/') && s->doc[0]) store_document_id_db(db, project_id, s->doc, true, doc, sizeof(doc));
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
    /* The record's current status, kept with it for fast filtering. */
    ok = ok && db_run(db, "UPDATE records SET current_status = ? WHERE id = ?", (DbArg[2]){ DB_S(status), DB_S(id) }, 2, NULL);
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
    DbArg query_args[2] = { DB_S(project_id), DB_S(kind_name(kind)) };
    DbRows versions, sources;
    if (!db_query(db,
                  "SELECT r.id, v.ver, v.created_at, v.status, v.content, v.finding_type, v.subject, v.relation, v.object,"
                  " v.related_id, v.related_text, r.created_at,"
                  " (SELECT MAX(created_at) FROM record_versions WHERE record_id = r.id) AS changed"
                  " FROM records r JOIN record_versions v ON v.record_id = r.id"
                  " WHERE r.project_id = ? AND r.kind = ? ORDER BY changed DESC, r.id, v.ver",
                  query_args, 2, &versions))
        return 0;
    if (!db_query(db,
                  "SELECT s.record_id, s.ver, s.kind, d.name, s.revision_id, s.conversation_id, s.message_seq, s.start,"
                  " s.len, s.quote FROM source_refs s JOIN records r ON r.id = s.record_id"
                  " LEFT JOIN documents d ON d.id = s.document_id"
                  " WHERE r.project_id = ? AND r.kind = ? ORDER BY s.id",
                  query_args, 2, &sources)) {
        db_rows_free(&versions);
        return 0;
    }

    bool ok = db_rows_has_columns(&versions, 13) && db_rows_has_columns(&sources, 10);
    Record *list = NULL;
    size_t count = 0, capacity = 0;
    for (size_t i = 0; ok && i < versions.nrows; i++) {
        RecordVersionRow row;
        ok = record_version_row(&versions, i, &row);
        ok = ok && strlen(row.id) < sizeof(list[0].id) && strlen(row.status) < sizeof(((RecordVersion *)0)->status) &&
             (!row.finding_type || strlen(row.finding_type) < sizeof(((FindingInfo *)0)->type)) &&
             (!row.subject || strlen(row.subject) < sizeof(((FindingInfo *)0)->subject)) &&
             (!row.relation || strlen(row.relation) < sizeof(((FindingInfo *)0)->relation)) &&
             (!row.object || strlen(row.object) < sizeof(((FindingInfo *)0)->object)) &&
             (!row.related_id || strlen(row.related_id) < sizeof(((FindingInfo *)0)->related));
        if (!ok) break;
        if (!count || strcmp(list[count - 1].id, row.id)) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 32;
                if (next < capacity || next > SIZE_MAX / sizeof(*list)) {
                    ok = false;
                    break;
                }
                Record *grown = realloc(list, next * sizeof(*list));
                if (!grown) {
                    ok = false;
                    break;
                }
                list = grown;
                capacity = next;
            }
            memset(&list[count], 0, sizeof(list[count]));
            snprintf(list[count].id, sizeof(list[count].id), "%s", row.id);
            list[count].created_seq = (size_t)(row.record_created_at * 1e6);
            count++;
        }

        Record *record = &list[count - 1];
        if (record->nversions == SIZE_MAX / sizeof(RecordVersion)) {
            ok = false;
            break;
        }
        RecordVersion *grown = realloc(record->versions, (record->nversions + 1) * sizeof(*grown));
        if (!grown) {
            ok = false;
            break;
        }
        record->versions = grown;
        RecordVersion *version = &grown[record->nversions++];
        memset(version, 0, sizeof(*version));
        version->ver = row.ver;
        version->ts = (time_t)row.created_at;
        snprintf(version->status, sizeof(version->status), "%s", row.status);
        version->content = strdup(row.content);
        FindingInfo *finding = &version->finding;
        if (row.finding_type) snprintf(finding->type, sizeof(finding->type), "%s", row.finding_type);
        if (row.subject) snprintf(finding->subject, sizeof(finding->subject), "%s", row.subject);
        if (row.relation) snprintf(finding->relation, sizeof(finding->relation), "%s", row.relation);
        if (row.object) snprintf(finding->object, sizeof(finding->object), "%s", row.object);
        if (row.related_id) snprintf(finding->related, sizeof(finding->related), "%s", row.related_id);
        finding->related_text = row.related_text ? strdup(row.related_text) : NULL;
        ok = version->content && (!row.related_text || finding->related_text);
    }

    IdIndex *index = ok && count ? malloc(count * sizeof(*index)) : NULL;
    if (ok && count && !index) ok = false;
    for (size_t i = 0; ok && i < count; i++) index[i] = (IdIndex){ list[i].id, i };
    if (ok && index) qsort(index, count, sizeof(*index), by_id);

    for (size_t i = 0; ok && i < sources.nrows; i++) {
        SourceRow row;
        ok = source_row(&sources, i, &row);
        ok = ok && (!row.document || strlen(row.document) < sizeof(((SourceRef *)0)->doc)) &&
             (!row.revision || strlen(row.revision) < sizeof(((SourceRef *)0)->revision)) &&
             (!row.conversation || strlen(row.conversation) < sizeof(((SourceRef *)0)->chat));
        if (!ok) break;
        if (!index) {
            ok = false;
            break;
        }
        IdIndex key = { row.record_id, 0 };
        IdIndex *hit = bsearch(&key, index, count, sizeof(*index), by_id);
        if (!hit) {
            ok = false;
            break;
        }
        Record *record = &list[hit->index];
        RecordVersion *version = NULL;
        for (size_t k = 0; k < record->nversions && !version; k++)
            if (record->versions[k].ver == row.ver) version = &record->versions[k];
        if (!version || version->nsources == SIZE_MAX / sizeof(SourceRef)) {
            ok = false;
            break;
        }
        SourceRef *grown = realloc(version->sources, (version->nsources + 1) * sizeof(*grown));
        if (!grown) {
            ok = false;
            break;
        }
        version->sources = grown;
        SourceRef *source = &grown[version->nsources];
        memset(source, 0, sizeof(*source));
        source->kind = row.kind;
        if (row.document) snprintf(source->doc, sizeof(source->doc), "%s", row.document);
        if (row.revision) snprintf(source->revision, sizeof(source->revision), "%s", row.revision);
        if (row.conversation) snprintf(source->chat, sizeof(source->chat), "%s", row.conversation);
        source->message = row.message;
        source->start = row.start;
        source->len = row.len;
        source->quote = strdup(row.quote);
        if (!source->quote) {
            ok = false;
            break;
        }
        version->nsources++;
    }

    free(index);
    db_rows_free(&versions);
    db_rows_free(&sources);
    if (!ok) {
        records_free(list, count);
        return 0;
    }
    *out = list;
    return count;
}
