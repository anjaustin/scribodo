#define _DARWIN_C_SOURCE
#include "memory_store.h"
#include "memory_documents.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Client hydration is bounded. Source hashes are persisted generated columns:
 * migration backfills once, and writes recompute once, not once per chunk.
 * Binary SUBSTRING server-side source-window cost still needs measurement. */
#define ELIGIBLE \
    "m.role IN ('user','assistant') AND (m.via IS NULL OR m.via='')" \
    " AND (m.role='user' OR (m.error=0 AND m.stopped=0 AND m.truncated=0))"
#define SCOLS \
    "id,project_id,conversation_id,history_epoch,policy_epoch,generation,cursor_seq,enabled,DATABASE(),pending_seq," \
    "EXISTS(SELECT 1 FROM memory_outbox mo WHERE mo.scope_id=memory_scopes.id" \
    " AND mo.generation=memory_scopes.generation AND mo.state IN ('pending','leased','failed'))," \
    "COALESCE(tokenizer,'')"

static bool valid_text(const char *text, size_t maximum)
{
    return text && text[0] && strnlen(text, maximum + 1) <= maximum;
}

static bool valid_scope(const MemoryScope *scope)
{
    return scope && valid_text(scope->id, sizeof(scope->id) - 1) &&
           valid_text(scope->project_id, 40) &&
           valid_text(scope->conversation_id, sizeof(scope->conversation_id) - 1) &&
           valid_text(scope->database, sizeof(scope->database) - 1) &&
           scope->history_epoch > 0 && scope->history_epoch <= INT64_MAX &&
           scope->policy_epoch > 0 && scope->policy_epoch <= INT64_MAX &&
           scope->generation > 0 && scope->generation <= INT64_MAX && scope->ingested_sequence >= -1 &&
           scope->pending_sequence >= -1 && strnlen(scope->tokenizer, sizeof(scope->tokenizer)) < sizeof(scope->tokenizer);
}

static bool valid_hash(const char *hash)
{
    if (!hash || strnlen(hash, 65) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f'))) return false;
    }
    return true;
}

static bool copy_cell(const DbRows *r, size_t row, size_t col, char *out, size_t cap)
{
    const char *s;
    if (!db_cell_text(r, row, col, &s) || strlen(s) >= cap) return false;
    memcpy(out, s, strlen(s) + 1);
    return true;
}

static bool scope_row(const DbRows *r, MemoryScope *s)
{
    uint64_t enabled, index_pending;
    memset(s, 0, sizeof(*s));
    bool ok = r->nrows == 1 && r->ncols >= 12 && copy_cell(r, 0, 0, s->id, sizeof(s->id)) &&
              copy_cell(r, 0, 1, s->project_id, sizeof(s->project_id)) &&
              copy_cell(r, 0, 2, s->conversation_id, sizeof(s->conversation_id)) &&
              db_cell_u64(r, 0, 3, &s->history_epoch) && db_cell_u64(r, 0, 4, &s->policy_epoch) &&
              db_cell_u64(r, 0, 5, &s->generation) && db_cell_i64(r, 0, 6, &s->ingested_sequence) &&
              db_cell_u64(r, 0, 7, &enabled) && enabled <= 1 &&
              copy_cell(r, 0, 8, s->database, sizeof(s->database)) && s->database[0] &&
              db_cell_i64(r, 0, 9, &s->pending_sequence) &&
              db_cell_u64(r, 0, 10, &index_pending) && index_pending <= 1 &&
              copy_cell(r, 0, 11, s->tokenizer, sizeof(s->tokenizer));
    if (ok) {
        s->enabled = enabled != 0;
        s->index_pending = index_pending != 0;
        ok = valid_scope(s);
    }
    return ok;
}

void memory_unit_free(MemoryUnit *unit)
{
    if (!unit) return;
    char *text = unit->text;
    unit->text = NULL;
    free(text);
    memset(unit, 0, sizeof(*unit));
}

void memory_index_lease_free(MemoryIndexLease *lease)
{
    if (!lease) return;
    memory_unit_free(&lease->unit);
    memset(lease, 0, sizeof(*lease));
}

bool memory_scope_get(Db *db, const char *p, const char *c, MemoryScope *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !out || !valid_text(p, 40) || !valid_text(c, 39)) return false;
    DbRows r;
    if (!db_query(db, "SELECT " SCOLS " FROM memory_scopes WHERE project_id=? AND conversation_id=?",
                  (DbArg[]){ DB_S(p), DB_S(c) }, 2, &r)) return false;
    bool ok = scope_row(&r, out);
    db_rows_free(&r);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static bool scope_change(Db *db, const char *p, const char *c, bool enabled, bool rebuild, MemoryScope *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !out || !valid_text(p, 40) || !valid_text(c, 39) || db_in_transaction(db) || !db_begin(db)) return false;
    /* Match append's conversation -> scope lock order. */
    DbRows r;
    bool ok = db_query(db, "SELECT id FROM conversations WHERE project_id=? AND id=? FOR UPDATE",
                       (DbArg[]){ DB_S(p), DB_S(c) }, 2, &r);
    if (ok) {
        ok = r.nrows == 1;
        db_rows_free(&r);
    }
    ok = ok && db_run(db,
                      "INSERT INTO memory_scopes(id,project_id,conversation_id)"
                      " SELECT UUID(),project_id,id FROM conversations WHERE project_id=? AND id=?"
                      " ON DUPLICATE KEY UPDATE id=memory_scopes.id",
                      (DbArg[]){ DB_S(p), DB_S(c) }, 2, NULL);
    MemoryScope old;
    ok = ok && memory_scope_get(db, p, c, &old);
    /* Rebuild never grants consent. An absent/disabled scope is already off;
     * acknowledge the command without blocking later consent controls. */
    if (ok && (rebuild ? old.enabled : old.enabled != enabled)) {
        ok = db_run(db, "UPDATE memory_scopes SET enabled=?,policy_epoch=policy_epoch+1,"
                        " history_epoch=history_epoch+1,generation=generation+1,cursor_seq=-1,byte_offset=0,"
                        " lease_until=NULL,fence=fence+1,attempts=0,retry_after=NULL,tokenizer=NULL,"
                        " pending_seq=(SELECT COALESCE(MAX(seq),-1) FROM messages WHERE conversation_id=?) WHERE id=?",
                    (DbArg[]){ DB_I(enabled), DB_S(c), DB_S(old.id) }, 3, NULL);
    }
    ok = ok && memory_scope_get(db, p, c, out);
    if (ok) {
        ok = db_commit(db);
    } else {
        db_rollback(db);
    }
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

bool memory_scope_set(Db *db, const char *p, const char *c, bool enabled, MemoryScope *out)
{
    return scope_change(db, p, c, enabled, false, out);
}

bool memory_scope_rebuild(Db *db, const char *p, const char *c, MemoryScope *out)
{
    return scope_change(db, p, c, true, true, out);
}

bool memory_message_intent(Db *db, const char *p, const char *c, int64_t seq)
{
    if (!db || !valid_text(p, 40) || !valid_text(c, 39) || seq < 0 || seq > INT_MAX || !db_in_transaction(db)) return false;
    /* One indexed no-op when no opted-in scope exists; committed with source. */
    return db_run(db, "UPDATE memory_scopes SET pending_seq=GREATEST(pending_seq,?)"
                      " WHERE project_id=? AND conversation_id=? AND enabled=1",
                  (DbArg[]){ DB_I(seq), DB_S(p), DB_S(c) }, 3, NULL);
}

static size_t boundary(const char *s, size_t n)
{
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
    return n;
}

static bool ingest_release(Db *db, const char *id, const char *owner, uint64_t fence, bool failed)
{
    const char *sql = failed ?
        "UPDATE memory_scopes SET lease_until=NULL,retry_after=DATE_ADD(NOW(6),INTERVAL 5 SECOND)"
        " WHERE id=? AND lease_owner=? AND fence=?" :
        "UPDATE memory_scopes SET lease_until=NULL,attempts=0,retry_after=NULL"
        " WHERE id=? AND lease_owner=? AND fence=?";
    return db_run(db, sql, (DbArg[]){ DB_S(id), DB_S(owner), DB_I(fence) }, 3, NULL);
}

bool memory_ingest_step(Db *db, const char *id, const char *owner, MemoryTokenCount count, void *ctx,
                        const char *tokenizer, bool *progress, char *why, size_t whylen)
{
    if (progress) *progress = false;
    if (why && whylen) why[0] = 0;
    if (!db || !progress || (whylen && !why) || !valid_text(id, 39) || !valid_text(owner, 63) ||
        !count || !valid_text(tokenizer, 191) || db_in_transaction(db)) return false;
    if (!db_begin(db)) return false;
    DbRows r;
    bool ok = db_query(db, "SELECT " SCOLS ",byte_offset,fence,tokenizer FROM memory_scopes"
                          " WHERE id=? AND enabled=1 AND attempts<3"
                          " AND (lease_until IS NULL OR lease_until<=NOW(6))"
                          " AND (retry_after IS NULL OR retry_after<=NOW(6)) FOR UPDATE",
                       (DbArg[]){ DB_S(id) }, 1, &r);
    MemoryScope s;
    uint64_t off = 0, fence = 0;
    if (ok && !r.nrows) {
        db_rows_free(&r);
        return db_commit(db);
    }
    if (ok) {
        const char *pinned_tokenizer;
        ok = scope_row(&r, &s) && db_cell_u64(&r, 0, 12, &off) && off <= INT64_MAX &&
             db_cell_u64(&r, 0, 13, &fence) && fence < INT64_MAX &&
             db_cell_nullable(&r, 0, 14, &pinned_tokenizer);
        if (ok && pinned_tokenizer && strcmp(pinned_tokenizer, tokenizer)) {
            if (whylen) snprintf(why, whylen, "Tokenizer changed; explicitly rebuild memory before ingestion");
            ok = false;
        }
        db_rows_free(&r);
    }
    ok = ok && db_run(db,
                      "UPDATE memory_scopes SET lease_owner=?,lease_until=DATE_ADD(NOW(6),INTERVAL 30 SECOND),"
                      " fence=fence+1,attempts=attempts+1,tokenizer=COALESCE(tokenizer,?) WHERE id=?",
                      (DbArg[]){ DB_S(owner), DB_S(tokenizer), DB_S(id) }, 3, NULL);
    if (!ok) {
        db_rollback(db);
        return false;
    }
    if (!db_commit(db)) return false;
    ++fence;
    /* Keyset seek, never MAX(completed). Read at most 2003 bytes, allowing
     * trimming a UTF-8 boundary without hydrating LONGTEXT into the client. */
    ok = db_query(db, "SELECT m.id,m.seq,m.role,m.source_hash,OCTET_LENGTH(m.content),(" ELIGIBLE "),"
                      " SUBSTRING(CAST(m.content AS BINARY),?+1,2003) FROM messages m"
                      " WHERE m.conversation_id=? AND m.seq>? ORDER BY m.seq LIMIT 1",
                  (DbArg[]){ DB_I(off), DB_S(s.conversation_id), DB_I(s.ingested_sequence) }, 3, &r);
    if (!ok) {
        ingest_release(db, id, owner, fence, true);
        return false;
    }
    if (!r.nrows) {
        db_rows_free(&r);
        if (off) {
            if (whylen) snprintf(why, whylen, "Partial source disappeared; explicitly rebuild memory");
            ingest_release(db, id, owner, fence, true);
            return false;
        }
        return ingest_release(db, id, owner, fence, false);
    }
    int64_t mid, seq;
    uint64_t total, eligible;
    char hash[65], role[16];
    const char *window;
    ok = r.ncols == 7 && db_cell_i64(&r, 0, 0, &mid) && mid > 0 &&
         db_cell_i64(&r, 0, 1, &seq) && seq > s.ingested_sequence && seq <= INT_MAX &&
         copy_cell(&r, 0, 2, role, sizeof(role)) && (!strcmp(role, "user") || !strcmp(role, "assistant")) &&
         copy_cell(&r, 0, 3, hash, sizeof(hash)) && valid_hash(hash) &&
         db_cell_u64(&r, 0, 4, &total) && total <= INT64_MAX &&
         db_cell_u64(&r, 0, 5, &eligible) && eligible <= 1 && db_cell_text(&r, 0, 6, &window) && off <= total;
    char text[2004] = "";
    size_t n = 0;
    long tokens = 0;
    if (ok && eligible) {
        size_t available = strlen(window);
        ok = available <= 2003 && available == (total - off > 2003 ? 2003 : total - off);
        if (ok) {
            memcpy(text, window, available + 1);
            n = available > 2000 ? boundary(text, 2000) : available;
            text[n] = 0;
            tokens = count(text, ctx); /* never inside a transaction */
            while (tokens > 512 && n) {
                n = boundary(text, n / 2);
                text[n] = 0;
                tokens = count(text, ctx);
            }
            ok = tokens >= 0 && tokens <= 512 && (n > 0 || total == 0);
        }
    }
    db_rows_free(&r);
    /* A callback must not smuggle an open transaction back into publication.
     * Leave any such caller-owned transaction untouched; this lease expires. */
    if (db_in_transaction(db)) {
        if (whylen) snprintf(why, whylen, "Tokenizer returned with an open transaction; ingestion refused");
        return false;
    }
    if (!ok) {
        if (whylen) snprintf(why, whylen, "Source/token count invalid; memory ingestion failed closed");
        ingest_release(db, id, owner, fence, true);
        return false;
    }
    bool complete = !eligible || off + n == total;
    if (!db_begin(db)) {
        ingest_release(db, id, owner, fence, true);
        return false;
    }
    /* Lock consent and fence, revalidate current source/flags/hash BEFORE
     * publication. A late worker cannot publish or move the cursor. */
    ok = db_query(db, "SELECT s.id FROM memory_scopes s JOIN messages m ON m.conversation_id=s.conversation_id"
                      " WHERE s.id=? AND s.enabled=1 AND s.generation=? AND s.history_epoch=? AND s.policy_epoch=?"
                      " AND s.fence=? AND s.lease_owner=? AND s.lease_until>NOW(6) AND s.cursor_seq=? AND s.byte_offset=?"
                      " AND m.id=? AND m.source_hash=? AND (" ELIGIBLE ")=? AND s.tokenizer=? AND m.role=? FOR UPDATE",
                  (DbArg[]){ DB_S(id), DB_I(s.generation), DB_I(s.history_epoch), DB_I(s.policy_epoch), DB_I(fence),
                             DB_S(owner), DB_I(s.ingested_sequence), DB_I(off), DB_I(mid), DB_S(hash), DB_I(eligible),
                             DB_S(tokenizer), DB_S(role) }, 13, &r);
    if (ok) {
        ok = r.nrows == 1;
        db_rows_free(&r);
    }
    /* Each partial continuation also verifies the original receipt hash. */
    if (ok && off) {
        ok = db_query(db, "SELECT source_hash FROM memory_receipts WHERE scope_id=? AND generation=?"
                          " AND message_id=? AND byte_offset=? AND complete=0",
                      (DbArg[]){ DB_S(id), DB_I(s.generation), DB_I(mid), DB_I(off) }, 4, &r);
        if (ok) {
            const char *original_hash;
            ok = r.nrows == 1 && db_cell_text(&r, 0, 0, &original_hash) && !strcmp(original_hash, hash);
            db_rows_free(&r);
        }
    }
    if (ok && eligible && n) {
        ok = db_run(db, "INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,"
                        " history_epoch,policy_epoch,generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
                        " VALUES (SHA2(CONCAT_WS(':',?,?,?,?,?,SHA2(?,256)),256),"
                        " ?,?,?,?,?,?,?,?,?,?,SHA2(?,256),?,?,?,?)",
                    (DbArg[]){ DB_S(id), DB_I(s.generation), DB_I(mid), DB_I(off), DB_S(hash), DB_S(text), DB_S(id),
                               DB_S(s.project_id), DB_S(s.conversation_id), DB_I(mid), DB_I(seq), DB_I(s.history_epoch),
                               DB_I(s.policy_epoch), DB_I(s.generation), DB_I(off), DB_S(hash), DB_S(text), DB_S(text),
                               DB_I(tokens), DB_S(tokenizer), DB_S(role) }, 21, NULL);
        ok = ok && db_run(db, "INSERT INTO memory_outbox(scope_id,unit_id,generation)"
                              " SELECT scope_id,id,generation FROM memory_units"
                              " WHERE scope_id=? AND generation=? AND message_id=? AND byte_start=?",
                          (DbArg[]){ DB_S(id), DB_I(s.generation), DB_I(mid), DB_I(off) }, 4, NULL);
    }
    ok = ok && db_run(db, "INSERT INTO memory_receipts(scope_id,project_id,conversation_id,message_id,generation,"
                          " sequence,source_hash,byte_offset,complete,eligible) VALUES (?,?,?,?,?,?,?,?,?,?)"
                          " ON DUPLICATE KEY UPDATE byte_offset=VALUES(byte_offset),"
                          " complete=VALUES(complete),eligible=VALUES(eligible)",
                      (DbArg[]){ DB_S(id), DB_S(s.project_id), DB_S(s.conversation_id), DB_I(mid), DB_I(s.generation),
                                 DB_I(seq), DB_S(hash), DB_I(off + n), DB_I(complete), DB_I(eligible) }, 10, NULL);
    ok = ok && db_run(db, "UPDATE memory_scopes SET cursor_seq=?,byte_offset=?,lease_until=NULL,attempts=0,"
                          " retry_after=NULL WHERE id=? AND fence=?",
                      (DbArg[]){ DB_I(complete ? seq : s.ingested_sequence), DB_I(complete ? 0 : off + n),
                                 DB_S(id), DB_I(fence) }, 4, NULL);
    if (ok) {
        ok = db_commit(db);
        *progress = ok;
        return ok;
    }
    db_rollback(db);
    ingest_release(db, id, owner, fence, true);
    return false;
}

bool memory_candidate_hydrate(Db *db, const MemoryScope *s, const MemoryCandidate *c, MemoryUnit *out, bool *found)
{
    if (out) memset(out, 0, sizeof(*out));
    if (found) *found = false;
    if (!db || !out || !found || !valid_scope(s) || !c || !valid_hash(c->unit_id) || !valid_hash(c->text_hash) ||
        !c->generation || c->generation > INT64_MAX || !c->history_epoch || c->history_epoch > INT64_MAX) return false;
    if (!s->enabled || c->generation != s->generation || c->history_epoch != s->history_epoch) return true;
    DbRows r;
    /* Eligibility/hash validation is in the same statement as bounded hydration. */
    bool ok = db_query(db, "SELECT u.id,u.text_hash,u.source_hash,u.message_id,u.sequence,u.byte_start,"
                          " OCTET_LENGTH(u.text),u.tokens,u.role,u.text FROM memory_units u"
                          " JOIN memory_scopes s ON s.id=u.scope_id"
                          " JOIN messages m ON m.id=u.message_id AND m.conversation_id=u.conversation_id"
                          " WHERE u.id=? AND u.text_hash=? AND u.scope_id=? AND s.project_id=?"
                          " AND s.conversation_id=? AND s.enabled=1 AND s.history_epoch=?"
                          " AND s.policy_epoch=? AND s.generation=? AND u.history_epoch=s.history_epoch"
                          " AND u.policy_epoch=s.policy_epoch AND u.generation=s.generation"
                           " AND u.source_kind='chat' AND u.source_hash=m.source_hash AND u.tokenizer=s.tokenizer AND u.role=m.role"
                          " AND u.text_hash=SHA2(u.text,256) AND (" ELIGIBLE ")"
                          " AND OCTET_LENGTH(u.text)<=2000 AND u.tokens<=512 AND DATABASE()=? AND s.tokenizer=?",
                       (DbArg[]){ DB_S(c->unit_id), DB_S(c->text_hash), DB_S(s->id), DB_S(s->project_id),
                                  DB_S(s->conversation_id), DB_I(s->history_epoch), DB_I(s->policy_epoch),
                                  DB_I(s->generation), DB_S(s->database), DB_S(s->tokenizer) }, 10, &r);
    if (!ok) return false;
    if (r.nrows) {
        uint64_t bytes, tokens;
        const char *role, *text;
        ok = r.nrows == 1 && r.ncols == 10 && copy_cell(&r, 0, 0, out->id, sizeof(out->id)) &&
             copy_cell(&r, 0, 1, out->text_hash, sizeof(out->text_hash)) && valid_hash(out->text_hash) &&
             copy_cell(&r, 0, 2, out->source_hash, sizeof(out->source_hash)) && valid_hash(out->source_hash) &&
             db_cell_i64(&r, 0, 3, &out->message_id) && out->message_id > 0 &&
             db_cell_i64(&r, 0, 4, &out->sequence) && out->sequence >= 0 && out->sequence <= INT_MAX &&
             db_cell_u64(&r, 0, 5, &out->byte_start) && out->byte_start <= INT64_MAX &&
             db_cell_u64(&r, 0, 6, &bytes) && bytes <= MEMORY_UNIT_BYTES &&
             db_cell_u64(&r, 0, 7, &tokens) && tokens <= MEMORY_UNIT_TOKENS &&
             db_cell_text(&r, 0, 8, &role) && (!strcmp(role, "user") || !strcmp(role, "assistant")) &&
             db_cell_text(&r, 0, 9, &text);
        if (ok) {
            out->role = !strcmp(role, "user") ? CHAT_USER : CHAT_ASSISTANT;
            out->text_bytes = bytes;
            out->tokens = tokens;
            out->text = strdup(text);
            ok = out->text && strlen(out->text) == bytes;
        }
        *found = ok;
    }
    db_rows_free(&r);
    if (!ok) memory_unit_free(out);
    return ok;
}

bool memory_lexical_candidates(Db *db, const MemoryScope *s, const char *q, size_t limit,
                               MemoryCandidate out[MEMORY_CANDIDATES], size_t *n)
{
    if (n) *n = 0;
    if (out && limit > 0 && limit <= MEMORY_CANDIDATES) memset(out, 0, limit * sizeof(*out));
    if (!db || !n || !out || !valid_scope(s) || !valid_text(q, 2000) || !limit || limit > MEMORY_CANDIDATES) return false;
    DbRows r;
    bool ok = db_query(db, "SELECT u.id,u.text_hash,MATCH(u.text) AGAINST (? IN NATURAL LANGUAGE MODE) AS score"
                          " FROM memory_units u JOIN memory_scopes s ON s.id=u.scope_id"
                          " WHERE s.id=? AND s.project_id=? AND s.conversation_id=? AND s.enabled=1"
                          " AND s.history_epoch=? AND s.policy_epoch=? AND s.generation=?"
                           " AND u.source_kind='chat' AND u.generation=s.generation AND u.tokenizer=s.tokenizer"
                          " AND u.history_epoch=s.history_epoch AND u.policy_epoch=s.policy_epoch"
                          " AND DATABASE()=? AND s.tokenizer=? AND MATCH(u.text) AGAINST (? IN NATURAL LANGUAGE MODE)>0"
                          " ORDER BY score DESC,u.id LIMIT ?",
                       (DbArg[]){ DB_S(q), DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id),
                                  DB_I(s->history_epoch), DB_I(s->policy_epoch), DB_I(s->generation),
                                  DB_S(s->database), DB_S(s->tokenizer), DB_S(q), DB_I(limit) }, 11, &r);
    if (!ok) return false;
    ok = r.ncols == 3 && r.nrows <= limit;
    for (size_t i = 0; ok && i < r.nrows; i++) {
        MemoryCandidate *candidate = &out[i];
        ok = copy_cell(&r, i, 0, candidate->unit_id, sizeof(candidate->unit_id)) && valid_hash(candidate->unit_id) &&
             copy_cell(&r, i, 1, candidate->text_hash, sizeof(candidate->text_hash)) && valid_hash(candidate->text_hash) &&
             db_cell_double(&r, i, 2, &candidate->score);
        candidate->history_epoch = s->history_epoch;
        candidate->generation = s->generation;
        if (ok) ++*n;
    }
    db_rows_free(&r);
    if (!ok) {
        memset(out, 0, limit * sizeof(*out));
        *n = 0;
    }
    return ok;
}

/* Index retention is not model authorization. Reconstruct only current SQL
 * identity for generic outbox delivery, never an attachment allowset. */
static bool index_hydrate(Db *db, const MemoryScope *s, const MemoryCandidate *c, MemoryUnit *u, bool *found)
{
    MemoryDocumentSet set = {0};
    DbRows r;
    if (!db_query(db, "SELECT d.id,r.id,r.source_hash,OCTET_LENGTH(r.content),d.name FROM memory_units u"
                      " JOIN documents d ON d.id=u.document_id AND d.project_id=u.project_id"
                      " JOIN revisions r ON r.document_id=d.id AND r.id=d.saved_revision_id"
                      " WHERE u.id=? AND u.scope_id=? AND u.source_kind='document' AND u.revision_id=r.id FOR UPDATE",
                  (DbArg[]){ DB_S(c->unit_id), DB_S(s->id) }, 2, &r)) return false;
    if (r.nrows) {
        MemoryDocumentRef *ref = &set.refs[0];
        bool decoded = r.nrows == 1 && copy_cell(&r, 0, 0, ref->document_id, sizeof(ref->document_id)) &&
                       copy_cell(&r, 0, 1, ref->revision_id, sizeof(ref->revision_id)) &&
                       copy_cell(&r, 0, 2, ref->source_hash, sizeof(ref->source_hash)) &&
                       db_cell_u64(&r, 0, 3, &ref->bytes);
        /* Names are display-only and may grow beyond the captured-name bound.
         * A changed/oversized source is stale, not a poison queue head. */
        snprintf(ref->name, sizeof(ref->name), "document");
        snprintf(ref->database, sizeof(ref->database), "%s", s->database);
        snprintf(ref->project_id, sizeof(ref->project_id), "%s", s->project_id);
        if (decoded && ref->bytes <= 16777216 && !strcmp(ref->project_id, s->project_id)) set.count = 1;
    }
    db_rows_free(&r);
    if (!set.count) return memory_candidate_hydrate(db, s, c, u, found);
    return memory_candidate_hydrate_with_documents(db, s, &set, c, u, found);
}

bool memory_outbox_claim(Db *db, const char *id, const char *owner, MemoryIndexLease *out, bool *claimed)
{
    if (out) memset(out, 0, sizeof(*out));
    if (claimed) *claimed = false;
    if (!db || !out || !claimed || !valid_text(id, 39) || !valid_text(owner, 63) ||
        db_in_transaction(db) || !db_begin(db)) return false;
    DbRows r;
    bool ok = db_query(db, "SELECT " SCOLS " FROM memory_scopes WHERE id=? AND enabled=1 FOR UPDATE",
                       (DbArg[]){ DB_S(id) }, 1, &r);
    if (ok && !r.nrows) {
        db_rows_free(&r);
        memory_index_lease_free(out);
        return db_commit(db);
    }
    if (ok) {
        ok = scope_row(&r, &out->scope);
        db_rows_free(&r);
    }
    /* A crashed third delivery is terminal after its lease expires. Retire at
     * most 64 active-generation jobs per poll; never scan retired generations. */
    uint64_t retired = 0;
    ok = ok && db_run(db, "UPDATE memory_outbox SET state='failed',lease_until=NULL"
                          " WHERE scope_id=? AND generation=? AND state='leased' AND attempts>=3"
                          " AND lease_until<=NOW(6) ORDER BY id LIMIT 64",
                      (DbArg[]){ DB_S(id), DB_I(out->scope.generation) }, 2, &retired);
    bool maintenance = retired != 0;
    /* Retire stale heads in one bounded batch rather than sleeping after each
     * locator. Include terminal cleanup above in the same 64-job budget. */
    for (uint64_t job = retired; ok && job < 64; job++) {
        ok = db_query(db, "SELECT id,fence,unit_id FROM memory_outbox WHERE scope_id=? AND generation=?"
                          " AND attempts<3 AND state IN ('pending','leased')"
                          " AND (lease_until IS NULL OR lease_until<=NOW(6))"
                          " AND (retry_after IS NULL OR retry_after<=NOW(6)) ORDER BY id LIMIT 1 FOR UPDATE",
                      (DbArg[]){ DB_S(id), DB_I(out->scope.generation) }, 2, &r);
        MemoryCandidate c = {0};
        if (ok && !r.nrows) {
            db_rows_free(&r);
            break;
        }
        if (ok) {
            ok = r.ncols == 3 && db_cell_u64(&r, 0, 0, &out->id) && out->id > 0 && out->id <= INT64_MAX &&
                 db_cell_u64(&r, 0, 1, &out->fence) && out->fence < INT64_MAX &&
                 copy_cell(&r, 0, 2, c.unit_id, sizeof(c.unit_id));
            db_rows_free(&r);
        }
        ok = ok && db_query(db, "SELECT text_hash FROM memory_units WHERE id=?", (DbArg[]){ DB_S(c.unit_id) }, 1, &r);
        if (ok) {
            ok = r.nrows == 1 && copy_cell(&r, 0, 0, c.text_hash, sizeof(c.text_hash));
            db_rows_free(&r);
        }
        c.history_epoch = out->scope.history_epoch;
        c.generation = out->scope.generation;
        bool found = false;
        /* Malformed SQL locators are undeliverable, not poison queue heads.
         * Genuine SQL failures still roll the entire bounded batch back. */
        if (ok && valid_hash(c.unit_id) && valid_hash(c.text_hash)) {
            ok = index_hydrate(db, &out->scope, &c, &out->unit, &found);
        }
        ok = ok && db_run(db, "UPDATE memory_outbox SET owner=?,fence=fence+1,attempts=attempts+1,"
                             " lease_until=DATE_ADD(NOW(6),INTERVAL 30 SECOND),state=? WHERE id=?",
                         (DbArg[]){ DB_S(owner), DB_S(found ? "leased" : "done"), DB_I(out->id) }, 3, NULL);
        if (ok && found) {
            ++out->fence;
            snprintf(out->owner, sizeof(out->owner), "%s", owner);
            *claimed = true;
            break;
        }
        if (ok) maintenance = true;
    }
    if (ok) {
        ok = db_commit(db);
    } else {
        db_rollback(db);
    }
    if (!ok) *claimed = false;
    if (!*claimed) memory_index_lease_free(out);
    out->maintenance = ok && maintenance;
    return ok;
}

bool memory_outbox_ack(Db *db, const MemoryIndexLease *l, bool delivered)
{
    if (!db || !l || !valid_scope(&l->scope) || !l->scope.enabled || !l->id || l->id > INT64_MAX ||
        !l->fence || l->fence > INT64_MAX || !valid_text(l->owner, 63) || !valid_hash(l->unit.id) ||
        !valid_hash(l->unit.text_hash) ||
        db_in_transaction(db) || !db_begin(db)) return false;
    DbRows r;
    bool ok = db_query(db, "SELECT id FROM memory_scopes WHERE id=? AND project_id=? AND conversation_id=?"
                          " AND enabled=1 AND history_epoch=? AND policy_epoch=? AND generation=?"
                          " AND DATABASE()=? AND tokenizer=? FOR UPDATE",
                       (DbArg[]){ DB_S(l->scope.id), DB_S(l->scope.project_id), DB_S(l->scope.conversation_id),
                                  DB_I(l->scope.history_epoch), DB_I(l->scope.policy_epoch), DB_I(l->scope.generation),
                                  DB_S(l->scope.database), DB_S(l->scope.tokenizer) }, 8, &r);
    if (ok) {
        ok = r.nrows == 1;
        db_rows_free(&r);
    }
    MemoryCandidate candidate = {0};
    snprintf(candidate.unit_id, sizeof(candidate.unit_id), "%s", l->unit.id);
    snprintf(candidate.text_hash, sizeof(candidate.text_hash), "%s", l->unit.text_hash);
    candidate.history_epoch = l->scope.history_epoch;
    candidate.generation = l->scope.generation;
    MemoryUnit canonical = {0};
    bool found = false;
    ok = ok && index_hydrate(db, &l->scope, &candidate, &canonical, &found) && found;
    memory_unit_free(&canonical);
    uint64_t changed = 0;
    ok = ok && db_run(db, "UPDATE memory_outbox o JOIN memory_units u ON u.id=o.unit_id AND u.scope_id=o.scope_id"
                          " JOIN memory_scopes s ON s.id=o.scope_id AND s.tokenizer=u.tokenizer"
                           " LEFT JOIN messages m ON m.id=u.message_id AND m.conversation_id=u.conversation_id"
                           " LEFT JOIN documents d ON d.id=u.document_id AND d.project_id=u.project_id"
                           " LEFT JOIN revisions r ON r.document_id=d.id AND r.id=d.saved_revision_id"
                          " SET o.state=IF(?='done','done',IF(o.attempts>=3,'failed','pending')),"
                          " o.lease_until=NULL,o.retry_after=DATE_ADD(NOW(6),INTERVAL 5 SECOND)"
                          " WHERE o.id=? AND o.scope_id=? AND o.unit_id=? AND o.generation=? AND o.owner=?"
                          " AND o.fence=? AND o.state='leased' AND o.lease_until>NOW(6)"
                           " AND ((u.source_kind='chat' AND u.source_hash=m.source_hash AND u.role=m.role AND (" ELIGIBLE "))"
                           " OR (u.source_kind='document' AND u.revision_id=r.id AND u.source_hash=r.source_hash))",
                      (DbArg[]){ DB_S(delivered ? "done" : "pending"), DB_I(l->id), DB_S(l->scope.id),
                                 DB_S(l->unit.id), DB_I(l->scope.generation), DB_S(l->owner), DB_I(l->fence) },
                      7, &changed) && changed == 1;
    if (ok) return db_commit(db);
    db_rollback(db);
    return false;
}
