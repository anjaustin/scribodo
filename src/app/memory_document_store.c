#define _DARWIN_C_SOURCE
#include "memory_documents.h"
#include "core/text_cut.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every SQL admission binds the namespace, owner, consent and generation. */
#define SCOPE_ID \
    "s.id=? AND s.project_id=? AND s.conversation_id=? AND s.enabled=1" \
    " AND s.history_epoch=? AND s.policy_epoch=? AND s.generation=? AND DATABASE()=?"
#define SCOPE SCOPE_ID " AND COALESCE(s.tokenizer,'')=?"
#define IDARGS(s) \
    DB_S((s)->id), DB_S((s)->project_id), DB_S((s)->conversation_id), DB_I((s)->history_epoch), \
    DB_I((s)->policy_epoch), DB_I((s)->generation), DB_S((s)->database)
#define SARGS(s) IDARGS(s), DB_S((s)->tokenizer)
#define SOURCE \
    "d.project_id=s.project_id AND d.id=? AND d.saved_revision_id=?" \
    " AND r.source_hash=? AND OCTET_LENGTH(r.content)=?"
#define RARGS(r) DB_S((r)->document_id), DB_S((r)->revision_id), DB_S((r)->source_hash), DB_I((r)->bytes)

static bool bounded(const char *s, size_t n)
{
    return s && s[0] && strnlen(s, n) < n;
}

static bool identity(const char *s, size_t n)
{
    if (!bounded(s, n)) return false;
    for (size_t i = 0; s[i]; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) {
            return false;
        }
    }
    return true;
}

static bool hash(const char *s)
{
    if (!s || strnlen(s, 65) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}

static bool valid(const MemoryScope *s, const MemoryDocumentSet *set)
{
    if (!s || !identity(s->id, sizeof(s->id)) || !identity(s->project_id, 41) ||
        !identity(s->conversation_id, sizeof(s->conversation_id)) || !identity(s->database, sizeof(s->database)) ||
        strnlen(s->tokenizer, sizeof(s->tokenizer)) == sizeof(s->tokenizer) ||
        !s->generation || s->generation > INT64_MAX || !s->history_epoch || s->history_epoch > INT64_MAX ||
        !s->policy_epoch || s->policy_epoch > INT64_MAX || (set && set->count > MEMORY_DOCUMENTS_MAX)) {
        return false;
    }
    for (size_t i = 0; set && i < set->count; i++) {
        const MemoryDocumentRef *r = &set->refs[i];
        if (!identity(r->database, sizeof(r->database)) || !identity(r->project_id, sizeof(r->project_id)) ||
            !identity(r->document_id, sizeof(r->document_id)) || !identity(r->revision_id, sizeof(r->revision_id)) ||
            !bounded(r->name, sizeof(r->name)) || !hash(r->source_hash) || r->bytes > 16777216 ||
            strcmp(r->database, s->database) || strcmp(r->project_id, s->project_id)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(r->document_id, set->refs[j].document_id)) return false;
        }
    }
    return true;
}

static bool source(Db *db, const MemoryScope *s, const MemoryDocumentRef *ref, bool *found)
{
    DbRows rows;
    if (!db_query(db, "SELECT s.id FROM memory_scopes s JOIN documents d ON d.project_id=s.project_id"
                      " JOIN revisions r ON r.document_id=d.id AND r.id=d.saved_revision_id"
                      " WHERE " SCOPE_ID " AND " SOURCE " FOR UPDATE",
                  (DbArg[]){ IDARGS(s), RARGS(ref) }, 11, &rows)) return false;
    *found = rows.nrows == 1;
    db_rows_free(&rows);
    return true;
}

bool memory_documents_seed(Db *db, const MemoryScope *s, const MemoryDocumentSet *set)
{
    /* Receipts are metadata-only intent. They may precede the generation's
     * first tokenizer claim; measured units still require tokenizer_lock(). */
    if (!db || !valid(s, set) || db_in_transaction(db) || !db_begin(db)) return false;
    DbRows rows;
    bool ok = db_query(db, "SELECT s.id FROM memory_scopes s WHERE " SCOPE_ID " FOR UPDATE",
                       (DbArg[]){ IDARGS(s) }, 7, &rows);
    if (ok) {
        ok = rows.nrows == 1;
        db_rows_free(&rows);
    }
    for (size_t i = 0; set && ok && i < set->count; i++) {
        const MemoryDocumentRef *r = &set->refs[i];
        bool found = false;
        ok = source(db, s, r, &found) && found;
        ok = ok && db_run(db, "INSERT INTO memory_document_receipts(scope_id,project_id,conversation_id,generation,"
                             "document_id,revision_id,source_hash,source_bytes)"
                             " VALUES(?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE scope_id=VALUES(scope_id)",
                         (DbArg[]){ DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id),
                                    DB_I(s->generation), RARGS(r) }, 8, NULL);
    }
    if (ok) return db_commit(db);
    db_rollback(db);
    return false;
}

bool memory_documents_complete(Db *db, const MemoryScope *s, const MemoryDocumentSet *set, bool *complete)
{
    if (complete) *complete = false;
    if (!db || !complete || !valid(s, set) || db_in_transaction(db) || !db_begin(db)) return false;
    DbRows rows;
    bool ok = db_query(db, "SELECT s.id FROM memory_scopes s WHERE " SCOPE_ID " FOR UPDATE",
                       (DbArg[]){ IDARGS(s) }, 7, &rows);
    if (ok) {
        ok = rows.nrows == 1;
        db_rows_free(&rows);
    }
    bool done = true;
    for (size_t i = 0; ok && set && i < set->count; i++) {
        const MemoryDocumentRef *r = &set->refs[i];
        bool found = false;
        ok = source(db, s, r, &found) && found;
        ok = ok && db_query(db, "SELECT complete FROM memory_document_receipts"
                               " WHERE scope_id=? AND generation=? AND revision_id=?"
                               " AND document_id=? AND source_hash=? AND source_bytes=?"
                               " AND complete=1 AND byte_offset=source_bytes",
                           (DbArg[]){ DB_S(s->id), DB_I(s->generation), DB_S(r->revision_id), DB_S(r->document_id),
                                      DB_S(r->source_hash), DB_I(r->bytes) }, 6, &rows);
        if (ok) {
            if (rows.nrows != 1) done = false;
            db_rows_free(&rows);
        }
    }
    if (ok) {
        ok = db_commit(db);
        *complete = ok && done;
        return ok;
    }
    db_rollback(db);
    return false;
}

static void release(Db *db, const MemoryScope *s, const MemoryDocumentRef *r, const char *owner, uint64_t fence)
{
    db_run(db, "UPDATE memory_document_receipts SET lease_until=NULL,retry_after=DATE_ADD(NOW(6),INTERVAL 5 SECOND)"
               " WHERE scope_id=? AND generation=? AND revision_id=? AND lease_owner=? AND fence=?",
           (DbArg[]){ DB_S(s->id), DB_I(s->generation), DB_S(r->revision_id), DB_S(owner), DB_I(fence) }, 5, NULL);
}

static bool tokenizer_lock(Db *db, const MemoryScope *s, const char *tokenizer, bool allow_unpinned)
{
    DbRows rows;
    if (!db_query(db, "SELECT s.id FROM memory_scopes s WHERE " SCOPE_ID
                      " AND (s.tokenizer=? OR (?=1 AND s.tokenizer IS NULL)) FOR UPDATE",
                  (DbArg[]){ IDARGS(s), DB_S(tokenizer), DB_I(allow_unpinned) }, 9, &rows)) return false;
    bool ok = rows.nrows == 1;
    db_rows_free(&rows);
    return ok;
}

bool memory_document_ingest_step(Db *db, const MemoryScope *s, const MemoryDocumentSet *set, const char *owner,
                                 MemoryTokenCount count, void *context, const char *tokenizer,
                                 bool *progress, char *why, size_t whylen)
{
    if (progress) *progress = false;
    if (why && whylen) why[0] = 0;
    if (!db || !progress || (whylen && !why) || !valid(s, set) || !bounded(owner, 64) ||
        !bounded(tokenizer, 192) || !count || db_in_transaction(db)) return false;
    if (!set || !set->count || !s->enabled) return true;
    /* An empty snapshot can predate chat's first tokenizer claim. The supplied
     * fingerprint, not that empty snapshot, must match SQL under the lock. */
    if (s->tokenizer[0] && strcmp(s->tokenizer, tokenizer)) return false;
    if (!memory_documents_seed(db, s, set)) return false;
    for (size_t i = 0; i < set->count; i++) {
        const MemoryDocumentRef *ref = &set->refs[i];
        DbRows rows;
        if (!db_begin(db)) return false;
        bool found = false;
        bool ok = tokenizer_lock(db, s, tokenizer, true) && source(db, s, ref, &found) && found;
        ok = ok && db_query(db, "SELECT byte_offset,fence FROM memory_document_receipts"
                               " WHERE scope_id=? AND generation=? AND revision_id=?"
                               " AND source_hash=? AND source_bytes=? AND complete=0 AND attempts<3"
                               " AND (lease_until IS NULL OR lease_until<=NOW(6))"
                               " AND (retry_after IS NULL OR retry_after<=NOW(6)) FOR UPDATE",
                           (DbArg[]){ DB_S(s->id), DB_I(s->generation), DB_S(ref->revision_id),
                                      DB_S(ref->source_hash), DB_I(ref->bytes) }, 5, &rows);
        if (!ok) {
            db_rollback(db);
            return false;
        }
        if (!rows.nrows) {
            db_rows_free(&rows);
            if (!db_commit(db)) return false;
            continue;
        }
        uint64_t off = 0, fence = 0;
        ok = rows.nrows == 1 && db_cell_u64(&rows, 0, 0, &off) && off <= ref->bytes &&
             db_cell_u64(&rows, 0, 1, &fence) && fence < INT64_MAX;
        db_rows_free(&rows);
        ok = ok && db_run(db, "UPDATE memory_scopes SET tokenizer=COALESCE(tokenizer,?) WHERE id=?",
                         (DbArg[]){ DB_S(tokenizer), DB_S(s->id) }, 2, NULL);
        ok = ok && db_run(db, "UPDATE memory_document_receipts SET lease_owner=?,"
                             "lease_until=DATE_ADD(NOW(6),INTERVAL 30 SECOND),fence=fence+1,attempts=attempts+1"
                             " WHERE scope_id=? AND generation=? AND revision_id=?",
                         (DbArg[]){ DB_S(owner), DB_S(s->id), DB_I(s->generation), DB_S(ref->revision_id) }, 4, NULL);
        if (!ok) {
            db_rollback(db);
            return false;
        }
        if (!db_commit(db)) return false;
        ++fence;

        /* Hydrate only one bounded source window, then leave SQL transactions
         * entirely before invoking the measured tokenizer callback. */
        ok = db_query(db, "SELECT SUBSTRING(CAST(content AS BINARY),?+1,2003) FROM revisions"
                          " WHERE document_id=? AND id=? AND source_hash=?",
                      (DbArg[]){ DB_I(off), DB_S(ref->document_id), DB_S(ref->revision_id), DB_S(ref->source_hash) },
                      4, &rows);
        char text[2004] = "";
        size_t n = 0;
        long tokens = -1;
        if (ok) {
            const char *window = NULL;
            ok = rows.nrows == 1 && db_cell_text(&rows, 0, 0, &window);
            if (ok) {
                size_t available = strlen(window), expected = ref->bytes - off;
                if (expected > 2003) expected = 2003;
                ok = available == expected;
                if (ok) {
                    memcpy(text, window, available + 1);
                    n = available > 2000 ? text_cut(text, 2000) : available;
                    text[n] = 0;
                }
            }
            db_rows_free(&rows);
        }
        if (ok) {
            const char *small = getenv("SCRIBODO_MEMORY_SMALL_UNITS");
            size_t token_limit = small && !strcmp(small, "1") ? MEMORY_DOCUMENT_SMALL_UNIT_TOKENS : MEMORY_UNIT_TOKENS;
            tokens = count(text, context);
            if (db_in_transaction(db)) return false;
            while (tokens > (long)token_limit && n) {
                n = text_cut(text, n / 2);
                text[n] = 0;
                tokens = count(text, context);
                if (db_in_transaction(db)) return false;
            }
            ok = tokens >= 0 && tokens <= (long)token_limit && (n || off == ref->bytes);
        }
        if (db_in_transaction(db)) return false;
        if (!ok) {
            release(db, s, ref, owner, fence);
            if (whylen) snprintf(why, whylen, "Document/token count invalid");
            return false;
        }
        if (!db_begin(db)) {
            release(db, s, ref, owner, fence);
            return false;
        }

        /* Publish only while the same consent, source and DB-clock lease are
         * current. Unit, outbox intent and receipt cursor commit together. */
        ok = tokenizer_lock(db, s, tokenizer, false) && source(db, s, ref, &found) && found;
        ok = ok && db_query(db, "SELECT revision_id FROM memory_document_receipts"
                               " WHERE scope_id=? AND generation=? AND revision_id=?"
                               " AND source_hash=? AND source_bytes=? AND byte_offset=? AND complete=0"
                               " AND lease_owner=? AND fence=? AND lease_until>NOW(6) FOR UPDATE",
                           (DbArg[]){ DB_S(s->id), DB_I(s->generation), DB_S(ref->revision_id), DB_S(ref->source_hash),
                                      DB_I(ref->bytes), DB_I(off), DB_S(owner), DB_I(fence) }, 8, &rows);
        if (ok) {
            ok = rows.nrows == 1;
            db_rows_free(&rows);
        }
        if (ok && n) {
            ok = db_run(db, "INSERT INTO memory_units(id,scope_id,project_id,conversation_id,history_epoch,policy_epoch,"
                           "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role,source_kind,document_id,revision_id)"
                           " VALUES(SHA2(CONCAT_WS(':','document',?,?,?,?,?,?,?,?,SHA2(?,256)),256),"
                           "?,?,?,?,?,?,?,?,SHA2(?,256),?,?,?,'user','document',?,?)",
                        (DbArg[]){ DB_S(s->id), DB_I(s->history_epoch), DB_I(s->policy_epoch), DB_I(s->generation),
                                   DB_S(ref->document_id), DB_S(ref->revision_id), DB_S(ref->source_hash),
                                   DB_I(off), DB_S(text), DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id),
                                   DB_I(s->history_epoch), DB_I(s->policy_epoch), DB_I(s->generation), DB_I(off),
                                   DB_S(ref->source_hash), DB_S(text), DB_S(text), DB_I(tokens), DB_S(tokenizer),
                                   DB_S(ref->document_id), DB_S(ref->revision_id) }, 23, NULL);
            ok = ok && db_run(db, "INSERT INTO memory_outbox(scope_id,unit_id,generation)"
                                 " SELECT scope_id,id,generation FROM memory_units"
                                 " WHERE scope_id=? AND generation=? AND revision_id=? AND byte_start=?",
                             (DbArg[]){ DB_S(s->id), DB_I(s->generation), DB_S(ref->revision_id), DB_I(off) }, 4, NULL);
        }
        ok = ok && db_run(db, "UPDATE memory_document_receipts SET byte_offset=?,complete=?,lease_until=NULL,attempts=0,retry_after=NULL"
                             " WHERE scope_id=? AND generation=? AND revision_id=? AND fence=?",
                         (DbArg[]){ DB_I(off + n), DB_I(off + n == ref->bytes), DB_S(s->id), DB_I(s->generation),
                                    DB_S(ref->revision_id), DB_I(fence) }, 6, NULL);
        if (ok) {
            ok = db_commit(db);
            *progress = ok;
            return ok;
        }
        db_rollback(db);
        release(db, s, ref, owner, fence);
        return false;
    }
    return true;
}

bool memory_candidate_hydrate_with_documents(Db *db, const MemoryScope *s, const MemoryDocumentSet *set,
                                             const MemoryCandidate *candidate, MemoryUnit *out, bool *found)
{
    if (out) memset(out, 0, sizeof(*out));
    if (found) *found = false;
    if (!db || !out || !found || !valid(s, set) || !candidate ||
        !hash(candidate->unit_id) || !hash(candidate->text_hash)) return false;
    bool chat_ok = memory_candidate_hydrate(db, s, candidate, out, found);
    if (!chat_ok || *found) return chat_ok;
    if (!s->enabled || candidate->generation != s->generation || candidate->history_epoch != s->history_epoch) return true;
    for (size_t i = 0; set && i < set->count; i++) {
        const MemoryDocumentRef *ref = &set->refs[i];
        DbRows rows;
        bool ok = db_query(db, "SELECT u.text,u.tokens,u.byte_start,u.source_hash,d.name FROM memory_units u"
                               " JOIN memory_scopes s ON s.id=u.scope_id"
                               " JOIN documents d ON d.id=u.document_id AND d.project_id=u.project_id"
                               " JOIN revisions r ON r.document_id=d.id AND r.id=u.revision_id"
                               " WHERE " SCOPE " AND " SOURCE
                               " AND u.id=? AND u.text_hash=? AND u.source_kind='document' AND u.revision_id=r.id"
                               " AND u.generation=s.generation AND u.history_epoch=s.history_epoch"
                               " AND u.policy_epoch=s.policy_epoch AND u.tokenizer=s.tokenizer"
                               " AND u.source_hash=r.source_hash AND u.text_hash=SHA2(u.text,256)"
                               " AND OCTET_LENGTH(u.text)<=2000 AND u.tokens BETWEEN 0 AND 512"
                               " AND CAST(u.text AS BINARY)=SUBSTRING(CAST(r.content AS BINARY),"
                               "u.byte_start+1,OCTET_LENGTH(u.text))",
                           (DbArg[]){ SARGS(s), RARGS(ref), DB_S(candidate->unit_id), DB_S(candidate->text_hash) },
                           14, &rows);
        if (!ok) return false;
        if (rows.nrows) {
            const char *text, *name, *digest;
            uint64_t tokens;
            ok = rows.nrows == 1 && db_cell_text(&rows, 0, 0, &text) &&
                 db_cell_u64(&rows, 0, 1, &tokens) && tokens <= 512 &&
                 db_cell_u64(&rows, 0, 2, &out->byte_start) && db_cell_text(&rows, 0, 3, &digest) &&
                 hash(digest) && db_cell_text(&rows, 0, 4, &name);
            if (ok) {
                out->text = strdup(text);
                ok = out->text != NULL;
                out->kind = MEMORY_SOURCE_DOCUMENT;
                out->role = CHAT_USER;
                out->tokens = tokens;
                out->text_bytes = strlen(text);
                snprintf(out->id, sizeof(out->id), "%s", candidate->unit_id);
                snprintf(out->text_hash, sizeof(out->text_hash), "%s", candidate->text_hash);
                snprintf(out->source_hash, sizeof(out->source_hash), "%s", digest);
                snprintf(out->document_id, sizeof(out->document_id), "%s", ref->document_id);
                snprintf(out->revision_id, sizeof(out->revision_id), "%s", ref->revision_id);
                snprintf(out->document_name, sizeof(out->document_name), "%s", name);
            }
            db_rows_free(&rows);
            *found = ok;
            if (!ok) memory_unit_free(out);
            return ok;
        }
        db_rows_free(&rows);
    }
    return true;
}

bool memory_lexical_candidates_with_documents(Db *db, const MemoryScope *s, const MemoryDocumentSet *set,
                                              const char *query, size_t limit, MemoryCandidate *out, size_t *count)
{
    return memory_lexical_candidates_with_documents_before(db, s, set, query, INT64_MAX, limit, out, count);
}

bool memory_lexical_candidates_with_documents_before(Db *db, const MemoryScope *s, const MemoryDocumentSet *set,
                                              const char *query, int64_t before, size_t limit, MemoryCandidate *out, size_t *count)
{
    if (count) *count = 0;
    if (out && limit > 0 && limit <= MEMORY_CANDIDATES) memset(out, 0, limit * sizeof(*out));
    if (!db || !out || !count || before < 0 || !valid(s, set) || !bounded(query, 2001) || !limit || limit > MEMORY_CANDIDATES) return false;
    /* At most eight indexed revision streams, filtered before their limit.
     * Merge their top-k with chat top-k; no attachment can starve chats. */
    MemoryCandidate all[MEMORY_CANDIDATES * (MEMORY_DOCUMENTS_MAX + 1)];
    size_t n = 0;
    if (!memory_lexical_candidates_before(db, s, query, before, limit, all, &n)) return false;
    for (size_t i = 0; set && i < set->count; i++) {
        const MemoryDocumentRef *ref = &set->refs[i];
        DbRows rows;
        bool ok = db_query(db, "SELECT u.id,u.text_hash,MATCH(u.text) AGAINST (? IN NATURAL LANGUAGE MODE)"
                               " / POW(CHAR_LENGTH(u.text)," MEMORY_LEXICAL_LENGTH_NORM ") AS score"
                               " FROM memory_units u JOIN memory_scopes s ON s.id=u.scope_id"
                               " JOIN documents d ON d.id=u.document_id AND d.project_id=u.project_id"
                               " JOIN revisions r ON r.document_id=d.id AND r.id=u.revision_id"
                               " WHERE " SCOPE " AND " SOURCE " AND u.source_kind='document' AND u.revision_id=r.id"
                               " AND u.generation=s.generation AND u.history_epoch=s.history_epoch"
                               " AND u.policy_epoch=s.policy_epoch AND u.tokenizer=s.tokenizer"
                               " AND u.source_hash=r.source_hash AND u.text_hash=SHA2(u.text,256)"
                               " AND OCTET_LENGTH(u.text)<=2000 AND u.tokens BETWEEN 0 AND 512"
                               " AND CAST(u.text AS BINARY)=SUBSTRING(CAST(r.content AS BINARY),"
                               "u.byte_start+1,OCTET_LENGTH(u.text))"
                               " AND MATCH(u.text) AGAINST (? IN NATURAL LANGUAGE MODE)>0"
                               " ORDER BY score DESC,u.id LIMIT ?",
                           (DbArg[]){ DB_S(query), SARGS(s), RARGS(ref), DB_S(query), DB_I(limit) }, 15, &rows);
        if (!ok) return false;
        ok = rows.nrows <= limit;
        for (size_t j = 0; ok && j < rows.nrows; j++) {
            const char *id, *digest;
            MemoryCandidate *c = &all[n];
            memset(c, 0, sizeof(*c));
            ok = db_cell_text(&rows, j, 0, &id) && hash(id) &&
                 db_cell_text(&rows, j, 1, &digest) && hash(digest) && db_cell_double(&rows, j, 2, &c->score);
            if (ok) {
                snprintf(c->unit_id, sizeof(c->unit_id), "%s", id);
                snprintf(c->text_hash, sizeof(c->text_hash), "%s", digest);
                c->generation = s->generation;
                c->history_epoch = s->history_epoch;
                ++n;
            }
        }
        db_rows_free(&rows);
        if (!ok) return false;
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (all[j].score > all[i].score ||
                (all[j].score == all[i].score && strcmp(all[j].unit_id, all[i].unit_id) < 0)) {
                MemoryCandidate t = all[i];
                all[i] = all[j];
                all[j] = t;
            }
        }
    }
    *count = n < limit ? n : limit;
    memcpy(out, all, *count * sizeof(*out));
    return true;
}
