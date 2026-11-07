#define _DARWIN_C_SOURCE
#include "memory_brief_store.h"
#include "llm/memory_brief.h"

#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text(const char *s, size_t cap)
{
    return s && s[0] && strnlen(s, cap) < cap;
}

static bool hash(const char *s)
{
    if (!s || strnlen(s, 65) != 64) return false;
    return strspn(s, "0123456789abcdef") == 64;
}

static bool scope_valid(const MemoryScope *s)
{
    return s && s->enabled && text(s->id, sizeof(s->id)) && text(s->project_id, 41) &&
           text(s->conversation_id, sizeof(s->conversation_id)) && text(s->database, sizeof(s->database)) &&
           text(s->tokenizer, sizeof(s->tokenizer)) && s->generation && s->generation <= INT64_MAX &&
           s->history_epoch && s->history_epoch <= INT64_MAX && s->policy_epoch && s->policy_epoch <= INT64_MAX &&
           s->ingested_sequence >= -1;
}

static bool cell_copy(const DbRows *r, size_t row, size_t col, char *out, size_t cap)
{
    const char *s;
    if (!db_cell_text(r, row, col, &s) || strlen(s) >= cap) return false;
    memcpy(out, s, strlen(s) + 1);
    return true;
}

void memory_brief_lease_free(MemoryBriefLease *l)
{
    if (!l) return;
    for (size_t i = 0; i < MEMORY_BRIEF_INPUTS; ++i) memory_unit_free(&l->inputs[i]);
    memset(l, 0, sizeof(*l));
}

void memory_brief_free(MemoryBrief *b)
{
    if (!b) return;
    for (size_t i = 0; i < MEMORY_BRIEF_INPUTS; ++i) memory_unit_free(&b->inputs[i]);
    memset(b, 0, sizeof(*b));
}

/* Match scope-changing code's conversation -> scope lock order. A false live
 * result is a normal stale lease, not a database error. */
static bool lock_scope(Db *db, const MemoryScope *s, bool *live)
{
    *live = false;
    DbRows r = {0};
    if (!db_query(db, "SELECT id FROM conversations WHERE project_id=? AND id=? FOR UPDATE",
                  (DbArg[]){DB_S(s->project_id), DB_S(s->conversation_id)}, 2, &r)) return false;
    bool exists = r.nrows == 1;
    db_rows_free(&r);
    if (!exists) return true;
    bool ok = db_query(db, "SELECT id FROM memory_scopes WHERE id=? AND project_id=? AND conversation_id=?"
                           " AND enabled=1 AND history_epoch=? AND policy_epoch=? AND generation=?"
                           " AND tokenizer=? AND DATABASE()=? AND cursor_seq>=? FOR UPDATE",
                       (DbArg[]){DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id),
                                 DB_I(s->history_epoch), DB_I(s->policy_epoch), DB_I(s->generation),
                                 DB_S(s->tokenizer), DB_S(s->database), DB_I(s->ingested_sequence)}, 9, &r);
    if (ok) *live = r.nrows == 1;
    db_rows_free(&r);
    return ok;
}

/* No source bodies here. The receipt must cover the entire current message,
 * including bytes beyond the selected chunk. Locks persist through publish. */
static bool hydrate(Db *db, const MemoryScope *s, const MemoryCandidate *c,
                    const char *source_hash, MemoryUnit *out, bool *found)
{
    *found = false;
    DbRows r = {0};
    bool ok = db_query(db, "SELECT u.id FROM memory_units u JOIN memory_receipts r"
                           " ON r.scope_id=u.scope_id AND r.generation=u.generation AND r.message_id=u.message_id"
                           " JOIN messages m ON m.id=u.message_id AND m.conversation_id=u.conversation_id"
                           " WHERE u.id=? AND u.scope_id=? AND u.generation=? AND u.source_kind='chat'"
                           " AND u.sequence<=? AND r.complete=1 AND r.eligible=1 AND r.sequence=u.sequence"
                           " AND m.seq=u.sequence"
                           " AND r.source_hash=u.source_hash AND u.source_hash=? AND r.source_hash=m.source_hash"
                           " AND r.byte_offset=OCTET_LENGTH(m.content)"
                           " AND u.byte_start+OCTET_LENGTH(u.text)<=r.byte_offset FOR UPDATE",
                       (DbArg[]){DB_S(c->unit_id), DB_S(s->id), DB_I(s->generation),
                                 DB_I(s->ingested_sequence), DB_S(source_hash)}, 5, &r);
    bool present = ok && r.nrows == 1;
    db_rows_free(&r);
    if (!ok || !present) return ok;
    return memory_candidate_hydrate(db, s, c, out, found);
}

/* Walk the next eligible receipt, not MAX(completed) or a filtered unit list.
 * Missing/partial receipts or chunks block the frontier rather than creating
 * false coverage. Ineligible and empty messages need no summary input. */
static bool select_inputs(Db *db, MemoryBriefLease *l, size_t limit)
{
    int64_t sequence = l->previous_sequence;
    uint64_t byte_end = l->previous_byte_end;
    size_t bytes = 0, tokens = 0;
    for (size_t i = 0; i < limit; ++i) {
        DbRows r = {0};
        bool ok = db_query(db, "SELECT sequence,complete,source_hash FROM memory_receipts"
                               " WHERE scope_id=? AND generation=? AND eligible=1 AND (byte_offset>0 OR complete=0) AND sequence<=?"
                               " AND (sequence>? OR (sequence=? AND (byte_offset>? OR complete=0))) ORDER BY sequence LIMIT 1 FOR UPDATE",
                           (DbArg[]){DB_S(l->scope.id), DB_I(l->scope.generation), DB_I(l->scope.ingested_sequence),
                                     DB_I(sequence), DB_I(sequence), DB_I(byte_end)}, 6, &r);
        if (!ok) return false;
        if (!r.nrows) { db_rows_free(&r); break; }
        int64_t next;
        uint64_t complete;
        char source[65];
        ok = r.nrows == 1 && db_cell_i64(&r, 0, 0, &next) && next >= 0 &&
             db_cell_u64(&r, 0, 1, &complete) && complete <= 1 &&
             cell_copy(&r, 0, 2, source, sizeof(source)) && hash(source);
        db_rows_free(&r);
        if (!ok) return false;
        if (!complete) break;
        uint64_t start = next == sequence ? byte_end : 0;
        /* Independently seek the next original. A missing/stale receipt must
         * not make us jump over its still-present original, and a missing
         * original must not make us jump over its receipt. */
        ok = db_query(db, "SELECT id,text_hash,sequence,byte_start FROM memory_units WHERE scope_id=? AND generation=?"
                          " AND source_kind='chat' AND sequence<=? AND (sequence>? OR (sequence=? AND byte_start>=?))"
                          " ORDER BY sequence,byte_start LIMIT 1 FOR UPDATE",
                      (DbArg[]){DB_S(l->scope.id), DB_I(l->scope.generation), DB_I(l->scope.ingested_sequence),
                                DB_I(sequence), DB_I(sequence), DB_I(byte_end)}, 6, &r);
        if (!ok) return false;
        if (!r.nrows) { db_rows_free(&r); break; }
        MemoryCandidate c = {.history_epoch=l->scope.history_epoch, .generation=l->scope.generation};
        int64_t unit_sequence;
        uint64_t unit_start;
        ok = r.nrows == 1 && cell_copy(&r, 0, 0, c.unit_id, sizeof(c.unit_id)) && hash(c.unit_id) &&
             cell_copy(&r, 0, 1, c.text_hash, sizeof(c.text_hash)) && hash(c.text_hash) &&
             db_cell_i64(&r, 0, 2, &unit_sequence) && unit_sequence >= 0 &&
             db_cell_u64(&r, 0, 3, &unit_start) && unit_start <= INT64_MAX;
        db_rows_free(&r);
        if (!ok) return false;
        if (unit_sequence != next || unit_start != start) break;
        bool found = false;
        if (!hydrate(db, &l->scope, &c, source, &l->inputs[i], &found)) return false;
        if (!found) break;
        MemoryUnit *u = &l->inputs[i];
        if (!u->text_bytes || u->sequence != next || u->byte_start != start ||
            u->text_bytes > 8000 - bytes || u->tokens > 2048 - tokens) {
            memory_unit_free(u);
            break;
        }
        bytes += u->text_bytes;
        tokens += u->tokens;
        sequence = next;
        byte_end = start + u->text_bytes;
        ++l->count;
    }
    return true;
}

static void digest(const MemoryBriefLease *l, char out[65])
{
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    char key[1024];
    int n = snprintf(key, sizeof(key), "%s|%s|%s|%llu|%llu|%llu|%lld|%llu|%s|%s|%zu|",
                     l->scope.database, l->scope.id, l->scope.tokenizer,
                     (unsigned long long)l->scope.history_epoch, (unsigned long long)l->scope.policy_epoch,
                     (unsigned long long)l->scope.generation, (long long)l->previous_sequence,
                     (unsigned long long)l->previous_byte_end, l->scope.project_id, l->scope.conversation_id, l->count);
    CC_SHA256_Update(&ctx, key, (CC_LONG)n);
    for (size_t i = 0; i < l->count; ++i) {
        const MemoryUnit *u = &l->inputs[i];
        n = snprintf(key, sizeof(key), "%s|%s|%s|%lld|%lld|%llu|%zu|%zu|",
                     u->id, u->source_hash, u->text_hash, (long long)u->message_id,
                     (long long)u->sequence, (unsigned long long)u->byte_start, u->text_bytes, u->tokens);
        CC_SHA256_Update(&ctx, key, (CC_LONG)n);
        CC_SHA256_Update(&ctx, u->text, (CC_LONG)u->text_bytes);
    }
    unsigned char raw[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(raw, &ctx);
    for (size_t i = 0; i < sizeof(raw); ++i) snprintf(out + 2*i, 3, "%02x", raw[i]);
}

static bool lease_valid(const MemoryBriefLease *l)
{
    return l && scope_valid(&l->scope) && text(l->owner, sizeof(l->owner)) &&
           l->fence && l->fence <= INT64_MAX && hash(l->input_digest) &&
           l->count && l->count <= MEMORY_BRIEF_INPUTS && l->previous_sequence >= -1 &&
           l->previous_byte_end <= INT64_MAX;
}

static bool lock_lease(Db *db, const MemoryBriefLease *l, bool *live)
{
    DbRows r = {0};
    *live = false;
    bool ok = db_query(db, "SELECT scope_id FROM memory_brief_state WHERE scope_id=? AND generation=?"
                           " AND owner=? AND fence=? AND lease_until>NOW(6) AND attempts BETWEEN 1 AND 3"
                           " AND t1_sequence=? AND t1_byte_end=? FOR UPDATE",
                       (DbArg[]){DB_S(l->scope.id), DB_I(l->scope.generation), DB_S(l->owner), DB_I(l->fence),
                                 DB_I(l->previous_sequence), DB_I(l->previous_byte_end)}, 6, &r);
    if (ok) *live = r.nrows == 1;
    db_rows_free(&r);
    return ok;
}

bool memory_brief_claim(Db *db, const MemoryScope *s, const char *owner, MemoryBriefLease *out, bool *claimed)
{
    if (out) memset(out, 0, sizeof(*out));
    if (claimed) *claimed = false;
    if (!db || !out || !claimed || !scope_valid(s) || !text(owner, 64) || db_in_transaction(db) || !db_begin(db)) return false;
    bool live;
    bool ok = lock_scope(db, s, &live);
    if (!ok || !live) goto done;
    ok = db_run(db, "INSERT IGNORE INTO memory_brief_state(scope_id,generation) VALUES (?,?)",
                (DbArg[]){DB_S(s->id), DB_I(s->generation)}, 2, NULL);
    DbRows r = {0};
    if (ok) ok = db_query(db, "SELECT t1_sequence,t1_byte_end,fence FROM memory_brief_state"
                              " WHERE scope_id=? AND generation=? AND attempts<3"
                              " AND (lease_until IS NULL OR lease_until<=NOW(6))"
                              " AND (retry_after IS NULL OR retry_after<=NOW(6)) FOR UPDATE",
                          (DbArg[]){DB_S(s->id), DB_I(s->generation)}, 2, &r);
    if (!ok) goto done;
    if (!r.nrows) { db_rows_free(&r); goto done; }
    out->scope = *s;
    snprintf(out->owner, sizeof(out->owner), "%s", owner);
    ok = r.nrows == 1 && db_cell_i64(&r, 0, 0, &out->previous_sequence) && out->previous_sequence >= -1 &&
         db_cell_u64(&r, 0, 1, &out->previous_byte_end) && out->previous_byte_end <= INT64_MAX &&
         db_cell_u64(&r, 0, 2, &out->fence) && out->fence < INT64_MAX;
    db_rows_free(&r);
    if (ok) ok = select_inputs(db, out, MEMORY_BRIEF_INPUTS);
    if (!ok || !out->count) goto done;
    ++out->fence;
    digest(out, out->input_digest);
    ok = db_run(db, "UPDATE memory_brief_state SET owner=?,fence=?,attempts=attempts+1,"
                    " lease_until=DATE_ADD(NOW(6),INTERVAL 60 SECOND),retry_after=NULL WHERE scope_id=? AND generation=?",
                (DbArg[]){DB_S(owner), DB_I(out->fence), DB_S(s->id), DB_I(s->generation)}, 4, NULL);
    *claimed = ok;
done:
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    if (!ok || !*claimed) { *claimed = false; memory_brief_lease_free(out); }
    return ok;
}

bool memory_brief_publish(Db *db, const MemoryBriefLease *l, const char *json, size_t tokens,
                          const char *model, const char *protocol, MemoryBriefAdmission admitted,
                          void *context, bool *published)
{
    if (published) *published = false;
    if (!db || !published || !lease_valid(l) || !hash(model) || !protocol || strcmp(protocol, MEMORY_BRIEF_PROTOCOL) ||
        tokens > MEMORY_BRIEF_SUMMARY_TOKENS || !memory_brief_validate_json(json, l->count) ||
        !admitted || !admitted(context) || db_in_transaction(db) || !db_begin(db)) return false;
    bool live;
    bool ok = lock_scope(db, &l->scope, &live);
    MemoryBriefLease checked = {.scope=l->scope, .previous_sequence=l->previous_sequence, .previous_byte_end=l->previous_byte_end};
    if (!ok || !live) goto done;
    ok = lock_lease(db, l, &live);
    if (!ok || !live) goto done;
    ok = select_inputs(db, &checked, l->count);
    if (!ok) goto done;
    char actual[65];
    digest(&checked, actual);
    if (checked.count != l->count || strcmp(actual, l->input_digest) || !admitted(context)) goto done;
    const MemoryUnit *first = &checked.inputs[0], *last = &checked.inputs[checked.count - 1];
    /* The digest is a deterministic publication ID: duplicate workers cannot
     * create another brief or advance an already advanced frontier. */
    ok = db_run(db, "INSERT INTO memory_briefs(id,scope_id,project_id,conversation_id,history_epoch,policy_epoch,generation,"
                    " begin_sequence,begin_byte,end_sequence,end_byte,input_digest,tokenizer,model_fingerprint,"
                    " prompt_schema,summary_json,summary_tokens,input_count,previous_sequence,previous_byte_end)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (DbArg[]){DB_S(actual), DB_S(l->scope.id), DB_S(l->scope.project_id), DB_S(l->scope.conversation_id),
                          DB_I(l->scope.history_epoch), DB_I(l->scope.policy_epoch), DB_I(l->scope.generation),
                          DB_I(first->sequence), DB_I(first->byte_start), DB_I(last->sequence), DB_I(last->byte_start+last->text_bytes),
                          DB_S(actual), DB_S(l->scope.tokenizer), DB_S(model), DB_S(protocol), DB_S(json), DB_I(tokens), DB_I(l->count),
                          DB_I(l->previous_sequence), DB_I(l->previous_byte_end)}, 20, NULL);
    for (size_t i = 0; ok && i < checked.count; ++i) {
        const MemoryUnit *u = &checked.inputs[i];
        ok = db_run(db, "INSERT INTO memory_brief_inputs(brief_id,ordinal,unit_id,scope_id,generation,source_hash,text_hash)"
                        " VALUES (?,?,?,?,?,?,?)",
                    (DbArg[]){DB_S(actual), DB_I(i+1), DB_S(u->id), DB_S(l->scope.id), DB_I(l->scope.generation),
                              DB_S(u->source_hash), DB_S(u->text_hash)}, 7, NULL);
    }
    uint64_t changed = 0;
    if (ok && admitted(context)) ok = db_run(db, "UPDATE memory_brief_state SET t1_sequence=?,t1_byte_end=?,owner='',"
                                                " lease_until=NULL,attempts=0,retry_after=NULL WHERE scope_id=? AND generation=?"
                                                " AND owner=? AND fence=? AND lease_until>NOW(6)",
                                            (DbArg[]){DB_I(last->sequence), DB_I(last->byte_start+last->text_bytes),
                                                      DB_S(l->scope.id), DB_I(l->scope.generation), DB_S(l->owner), DB_I(l->fence)}, 6, &changed);
    else ok = false;
    ok = ok && changed == 1 && admitted(context);
    *published = ok;
done:
    memory_brief_lease_free(&checked);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    if (!ok) *published = false;
    return ok;
}

bool memory_brief_fail(Db *db, const MemoryBriefLease *l, bool retryable, bool canceled)
{
    if (!db || !lease_valid(l) || db_in_transaction(db) || !db_begin(db)) return false;
    bool live;
    bool ok = lock_scope(db, &l->scope, &live);
    if (ok && live) ok = lock_lease(db, l, &live);
    if (ok && live) ok = db_run(db, "UPDATE memory_brief_state SET owner='',lease_until=NULL,"
                                    " attempts=CASE WHEN ?=1 THEN GREATEST(attempts-1,0) WHEN ?=0 THEN 3 ELSE attempts END,"
                                    " retry_after=DATE_ADD(NOW(6),INTERVAL 2 SECOND) WHERE scope_id=? AND generation=? AND fence=?",
                                (DbArg[]){DB_I(canceled), DB_I(retryable), DB_S(l->scope.id), DB_I(l->scope.generation), DB_I(l->fence)}, 5, NULL);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}

bool memory_brief_recall(Db *db, const MemoryScope *s, int64_t older_than, const char *model,
                         MemoryBrief out[MEMORY_BRIEF_INPUTS], size_t *count)
{
    if (count) *count = 0;
    if (out) memset(out, 0, MEMORY_BRIEF_INPUTS * sizeof(*out));
    if (!db || !out || !count || !scope_valid(s) || older_than < 0 || !hash(model) ||
        db_in_transaction(db) || !db_begin(db)) return false;
    bool live;
    bool ok = lock_scope(db, s, &live);
    DbRows rows = {0};
    if (!ok || !live) goto done;
    /* Limit before any original hydration. No archive or unbounded summary
     * search, and no summary is itself a canonical source. */
    ok = db_query(db, "SELECT id,summary_json,summary_tokens,input_count,previous_sequence,previous_byte_end,"
                      " input_digest,begin_sequence,begin_byte,end_sequence,end_byte,SHA2(summary_json,256),"
                      " CAST(created_at AS CHAR) FROM memory_briefs"
                      " WHERE scope_id=? AND project_id=? AND conversation_id=? AND generation=?"
                      " AND history_epoch=? AND policy_epoch=? AND tokenizer=? AND model_fingerprint=?"
                      " AND prompt_schema=? AND end_sequence<? ORDER BY end_sequence DESC,id LIMIT 4",
                  (DbArg[]){DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id), DB_I(s->generation),
                            DB_I(s->history_epoch), DB_I(s->policy_epoch), DB_S(s->tokenizer), DB_S(model),
                            DB_S(MEMORY_BRIEF_PROTOCOL), DB_I(older_than)}, 10, &rows);
    for (size_t row = 0; ok && row < rows.nrows; ++row) {
        MemoryBrief *b = &out[*count];
        uint64_t n, tokens, begin_byte, end_byte;
        int64_t begin_sequence, end_sequence;
        MemoryBriefLease snapshot = {.scope=*s};
        char stored_digest[65];
        ok = cell_copy(&rows, row, 0, b->id, sizeof(b->id)) && hash(b->id) &&
             cell_copy(&rows, row, 1, b->json, sizeof(b->json)) && db_cell_u64(&rows, row, 2, &tokens) && tokens <= 256 &&
             db_cell_u64(&rows, row, 3, &n) && n && n <= 4 && memory_brief_validate_json(b->json, n) &&
             db_cell_i64(&rows, row, 4, &snapshot.previous_sequence) && snapshot.previous_sequence >= -1 &&
             db_cell_u64(&rows, row, 5, &snapshot.previous_byte_end) && snapshot.previous_byte_end <= INT64_MAX &&
             cell_copy(&rows, row, 6, stored_digest, sizeof(stored_digest)) && hash(stored_digest) && !strcmp(stored_digest, b->id) &&
             db_cell_i64(&rows, row, 7, &begin_sequence) && begin_sequence >= 0 &&
             db_cell_u64(&rows, row, 8, &begin_byte) && begin_byte <= INT64_MAX &&
             db_cell_i64(&rows, row, 9, &end_sequence) && end_sequence >= begin_sequence && end_sequence <= s->ingested_sequence &&
             db_cell_u64(&rows, row, 10, &end_byte) && end_byte <= INT64_MAX &&
             cell_copy(&rows, row, 11, b->summary_hash, sizeof(b->summary_hash)) && hash(b->summary_hash) &&
             cell_copy(&rows, row, 12, b->created_at, sizeof(b->created_at)) && b->created_at[0];
        if (!ok) break;
        b->tokens = tokens;
        snprintf(b->model_fingerprint, sizeof(b->model_fingerprint), "%s", model);
        DbRows inputs = {0};
        ok = db_query(db, "SELECT ordinal,unit_id,text_hash,source_hash FROM memory_brief_inputs"
                          " WHERE brief_id=? AND scope_id=? AND generation=? ORDER BY ordinal LIMIT 5",
                      (DbArg[]){DB_S(b->id), DB_S(s->id), DB_I(s->generation)}, 3, &inputs);
        if (ok && inputs.nrows != n) ok = false; /* malformed provenance, not stale coverage */
        bool valid = ok;
        size_t bytes = 0, source_tokens = 0;
        for (size_t i = 0; ok && valid && i < inputs.nrows; ++i) {
            uint64_t ordinal;
            char source[65];
            MemoryCandidate c = {.history_epoch=s->history_epoch, .generation=s->generation};
            ok = db_cell_u64(&inputs, i, 0, &ordinal) && ordinal == i+1 &&
                 cell_copy(&inputs, i, 1, c.unit_id, sizeof(c.unit_id)) && hash(c.unit_id) &&
                 cell_copy(&inputs, i, 2, c.text_hash, sizeof(c.text_hash)) && hash(c.text_hash) &&
                 cell_copy(&inputs, i, 3, source, sizeof(source)) && hash(source);
            bool found = false;
            if (ok) ok = hydrate(db, s, &c, source, &b->inputs[i], &found);
            valid = ok && found;
            if (valid) {
                ++b->count;
                bytes += b->inputs[i].text_bytes;
                source_tokens += b->inputs[i].tokens;
                valid = bytes <= 8000 && source_tokens <= 2048;
            }
        }
        db_rows_free(&inputs);
        if (ok && valid) {
            snapshot.count = b->count;
            memcpy(snapshot.inputs, b->inputs, sizeof(snapshot.inputs)); /* borrowed for hashing only */
            char current_digest[65];
            digest(&snapshot, current_digest);
            const MemoryUnit *first = &b->inputs[0], *last = &b->inputs[b->count-1];
            valid = !strcmp(current_digest, stored_digest) && first->sequence == begin_sequence && first->byte_start == begin_byte &&
                    last->sequence == end_sequence && last->byte_start+last->text_bytes == end_byte;
        }
        if (ok && valid) ++*count;
        else memory_brief_free(b);
    }
done:
    db_rows_free(&rows);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    if (!ok) {
        for (size_t i = 0; i < MEMORY_BRIEF_INPUTS; ++i) memory_brief_free(&out[i]);
        *count = 0;
    }
    return ok;
}

static bool same_original(const MemoryUnit *a, const MemoryUnit *b)
{
    return a->kind == MEMORY_SOURCE_CHAT && b->kind == MEMORY_SOURCE_CHAT &&
           !strcmp(a->id, b->id) && !strcmp(a->text_hash, b->text_hash) && !strcmp(a->source_hash, b->source_hash) &&
           a->message_id == b->message_id && a->sequence == b->sequence && a->byte_start == b->byte_start &&
           a->text_bytes == b->text_bytes && a->tokens == b->tokens && a->role == b->role &&
           a->text && b->text && !strcmp(a->text, b->text);
}

bool memory_brief_admit(Db *db, const MemoryScope *s, const MemoryBrief *b)
{
    if (!db || !scope_valid(s) || !b || !hash(b->id) || !hash(b->model_fingerprint) || !hash(b->summary_hash) ||
        !text(b->created_at, sizeof(b->created_at)) || !b->count || b->count > MEMORY_BRIEF_INPUTS ||
        b->tokens > MEMORY_BRIEF_SUMMARY_TOKENS || !memory_brief_validate_json(b->json, b->count) ||
        db_in_transaction(db)) return false;
    for (size_t i = 0; i < b->count; ++i) {
        const MemoryUnit *u = &b->inputs[i];
        if (u->kind != MEMORY_SOURCE_CHAT || !hash(u->id) || !hash(u->text_hash) || !hash(u->source_hash) ||
            !u->text || !u->text_bytes || u->text_bytes > MEMORY_UNIT_BYTES ||
            strnlen(u->text, MEMORY_UNIT_BYTES+1) != u->text_bytes || u->tokens > MEMORY_UNIT_TOKENS ||
            u->byte_start > INT64_MAX-u->text_bytes) return false;
    }
    if (!db_begin(db)) return false;
    bool live = false;
    bool ok = lock_scope(db, s, &live);
    DbRows row = {0}, inputs = {0};
    MemoryBriefLease snapshot = {.scope=*s};
    int64_t begin_sequence = -1, end_sequence = -1;
    uint64_t begin_byte = 0, end_byte = 0;
    char stored_digest[65];
    if (!ok || !live) { ok = false; goto done; }
    /* A byte-for-byte summary comparison plus its cached digest fences both
     * a changed SQL row and a changed in-memory rendering snapshot. */
    ok = db_query(db, "SELECT previous_sequence,previous_byte_end,input_digest,begin_sequence,begin_byte,end_sequence,end_byte"
                      " FROM memory_briefs WHERE id=? AND scope_id=? AND project_id=? AND conversation_id=?"
                      " AND generation=? AND history_epoch=? AND policy_epoch=? AND tokenizer=?"
                      " AND model_fingerprint=? AND prompt_schema=? AND BINARY summary_json=BINARY ?"
                      " AND SHA2(summary_json,256)=? AND summary_tokens=? AND input_count=?"
                      " AND CAST(created_at AS CHAR)=? LIMIT 1 FOR UPDATE",
                  (DbArg[]){DB_S(b->id), DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id),
                            DB_I(s->generation), DB_I(s->history_epoch), DB_I(s->policy_epoch), DB_S(s->tokenizer),
                            DB_S(b->model_fingerprint), DB_S(MEMORY_BRIEF_PROTOCOL), DB_S(b->json), DB_S(b->summary_hash),
                            DB_I(b->tokens), DB_I(b->count), DB_S(b->created_at)}, 15, &row);
    ok = ok && row.nrows == 1 && db_cell_i64(&row, 0, 0, &snapshot.previous_sequence) && snapshot.previous_sequence >= -1 &&
         db_cell_u64(&row, 0, 1, &snapshot.previous_byte_end) && snapshot.previous_byte_end <= INT64_MAX &&
         cell_copy(&row, 0, 2, stored_digest, sizeof(stored_digest)) && hash(stored_digest) && !strcmp(stored_digest, b->id) &&
         db_cell_i64(&row, 0, 3, &begin_sequence) && begin_sequence >= 0 &&
         db_cell_u64(&row, 0, 4, &begin_byte) && begin_byte <= INT64_MAX &&
         db_cell_i64(&row, 0, 5, &end_sequence) && end_sequence >= begin_sequence && end_sequence <= s->ingested_sequence &&
         db_cell_u64(&row, 0, 6, &end_byte) && end_byte <= INT64_MAX;
    if (!ok) goto done;
    ok = db_query(db, "SELECT ordinal,unit_id,text_hash,source_hash FROM memory_brief_inputs"
                      " WHERE brief_id=? AND scope_id=? AND generation=? ORDER BY ordinal LIMIT 5 FOR UPDATE",
                  (DbArg[]){DB_S(b->id), DB_S(s->id), DB_I(s->generation)}, 3, &inputs);
    ok = ok && inputs.nrows == b->count;
    size_t bytes = 0, source_tokens = 0;
    for (size_t i = 0; ok && i < inputs.nrows; ++i) {
        uint64_t ordinal;
        char source[65];
        MemoryCandidate c = {.history_epoch=s->history_epoch, .generation=s->generation};
        ok = db_cell_u64(&inputs, i, 0, &ordinal) && ordinal == i+1 &&
             cell_copy(&inputs, i, 1, c.unit_id, sizeof(c.unit_id)) && hash(c.unit_id) &&
             cell_copy(&inputs, i, 2, c.text_hash, sizeof(c.text_hash)) && hash(c.text_hash) &&
             cell_copy(&inputs, i, 3, source, sizeof(source)) && hash(source);
        bool found = false;
        if (ok) ok = hydrate(db, s, &c, source, &snapshot.inputs[i], &found);
        ok = ok && found && same_original(&snapshot.inputs[i], &b->inputs[i]);
        if (ok) {
            ++snapshot.count;
            bytes += snapshot.inputs[i].text_bytes;
            source_tokens += snapshot.inputs[i].tokens;
            ok = bytes <= 8000 && source_tokens <= 2048;
        }
    }
    if (ok) {
        char current[65];
        digest(&snapshot, current);
        const MemoryUnit *first = &snapshot.inputs[0], *last = &snapshot.inputs[snapshot.count-1];
        ok = !strcmp(current, stored_digest) && first->sequence == begin_sequence && first->byte_start == begin_byte &&
             last->sequence == end_sequence && last->byte_start+last->text_bytes == end_byte;
    }
done:
    db_rows_free(&row);
    db_rows_free(&inputs);
    memory_brief_lease_free(&snapshot);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}
