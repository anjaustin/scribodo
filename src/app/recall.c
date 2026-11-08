#define _DARWIN_C_SOURCE
#include "recall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/db.h"
#include "core/recall_terms.h"
#include "app/memory_store.h"
#include "app/project_store.h"
#include "app/store.h"

static void note(RecallResult *r, const char *what)
{
    size_t used = strlen(r->limits);
    if (used >= sizeof(r->limits) - 1) return;
    snprintf(r->limits + used, sizeof(r->limits) - used, "%s%s", used ? "; " : "", what);
}

static size_t codepoints_in(const char *s, size_t bytes)
{
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) n += ((unsigned char)s[i] & 0xC0) != 0x80;
    return n;
}

static void row_free(RecallRow *row)
{
    free(row->text);
    records_free_sources(row->sources, row->nsources);
    memset(row, 0, sizeof(*row));
}

static bool push_row(RecallResult *r, size_t *capacity, RecallRow **slot)
{
    if (r->count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 16;
        RecallRow *grown = realloc(r->rows, next * sizeof(*grown));
        if (!grown) return false;
        r->rows = grown;
        *capacity = next;
    }
    *slot = &r->rows[r->count++];
    memset(*slot, 0, sizeof(**slot));
    return true;
}

static void drop_rows_from(RecallResult *r, size_t from)
{
    for (size_t i = from; i < r->count; i++) row_free(&r->rows[i]);
    if (from < r->count) r->count = from;
}

/* ---- canon ------------------------------------------------------------- */

static bool recall_canon(StoreManager *store, const char *dir, const RecallTerm *terms, unsigned nterms,
                         RecallResult *r, size_t *capacity)
{
    Record *records = NULL;
    size_t n = 0;
    if (!records_list_checked(store, dir, REC_CANON, &records, &n)) {
        note(r, "canon could not be read from the project store");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        const RecordVersion *v = record_current(&records[i]);
        if (strcmp(v->status, "committed") != 0) {
            r->canon_retracted++;
            continue;
        }
        r->canon_committed++;
        unsigned matched = recall_match(v->content ? v->content : "", terms, nterms);
        if (!matched) continue;
        RecallRow *row;
        ok = push_row(r, capacity, &row);
        if (!ok) break;
        row->kind = RECALL_CANON;
        row->matched = matched;
        row->ver = v->ver;
        row->ts = v->ts;
        snprintf(row->record_id, sizeof(row->record_id), "%s", records[i].id);
        row->text = strdup(v->content ? v->content : "");
        row->codepoints = row->text ? codepoints_in(row->text, strlen(row->text)) : 0;
        row->text_bytes = row->text ? strlen(row->text) : 0;
        if (v->nsources) {
            row->sources = records_copy_sources(v->sources, v->nsources);
            row->nsources = row->sources ? v->nsources : 0;
        }
        ok = row->text && (!v->nsources || row->sources);
    }
    records_free(records, n);
    if (!ok) note(r, "canon rows could not be built");
    return ok;
}

/* ---- excerpts ---------------------------------------------------------- */

typedef struct {
    char id[40], title[256];
    bool enabled;
    bool indexing;
} ConversationRow;

static bool list_conversations(Db *db, const char *project_id, ConversationRow *out, size_t *count, bool *truncated)
{
    *count = 0;
    *truncated = false;
    DbRows rows;
    if (!db_query(db,
                  "SELECT c.id, c.title, COALESCE(s.enabled,0), COALESCE(s.tokenizer,'')"
                  " FROM conversations c LEFT JOIN memory_scopes s"
                  " ON s.project_id=c.project_id AND s.conversation_id=c.id"
                  " WHERE c.project_id=? ORDER BY c.updated_at DESC, c.id LIMIT ?",
                  (DbArg[]){ DB_S(project_id), DB_I(RECALL_CONVERSATIONS_MAX + 1) }, 2, &rows))
        return false;
    bool ok = rows.ncols == 4;
    for (size_t i = 0; ok && i < rows.nrows && i < RECALL_CONVERSATIONS_MAX; i++) {
        const char *id, *title, *tokenizer;
        int64_t enabled;
        ok = db_cell_text(&rows, i, 0, &id) && db_cell_text(&rows, i, 1, &title) &&
             db_cell_i64(&rows, i, 2, &enabled) && db_cell_text(&rows, i, 3, &tokenizer) &&
             strlen(id) < sizeof(out[i].id);
        if (!ok) break;
        snprintf(out[i].id, sizeof(out[i].id), "%s", id);
        snprintf(out[i].title, sizeof(out[i].title), "%s", title);
        out[i].enabled = enabled != 0;
        out[i].indexing = out[i].enabled && !tokenizer[0];
        (*count)++;
    }
    *truncated = rows.nrows > RECALL_CONVERSATIONS_MAX;
    db_rows_free(&rows);
    return ok;
}

/* The unit's text must still be exactly the bytes of the original message at
 * its recorded range. Also yields the codepoint offset the chat view uses. */
static bool verify_chat_unit(Db *db, const MemoryScope *scope, const MemoryUnit *unit, RecallRow *row, bool *exact)
{
    *exact = false;
    DbRows rows;
    if (!db_query(db,
                  "SELECT CAST(u.text AS BINARY)=SUBSTRING(CAST(m.content AS BINARY),u.byte_start+1,OCTET_LENGTH(u.text)),"
                  " CHAR_LENGTH(CONVERT(SUBSTRING(CAST(m.content AS BINARY),1,u.byte_start) USING utf8mb4)), m.seq"
                  " FROM memory_units u"
                  " JOIN messages m ON m.id=u.message_id AND m.conversation_id=u.conversation_id"
                  " WHERE u.id=? AND u.scope_id=? AND u.text_hash=? AND u.source_hash=m.source_hash",
                  (DbArg[]){ DB_S(unit->id), DB_S(scope->id), DB_S(unit->text_hash) }, 3, &rows))
        return false;
    bool ok = true;
    if (rows.nrows == 1 && rows.ncols == 3) {
        int64_t flag = 0, start = 0, seq = 0;
        ok = db_cell_i64(&rows, 0, 0, &flag) && db_cell_i64(&rows, 0, 1, &start) && db_cell_i64(&rows, 0, 2, &seq);
        if (ok && flag == 1 && start >= 0 && seq >= 0) {
            *exact = true;
            row->codepoint_start = (size_t)start;
            row->sequence = seq;
        }
    }
    db_rows_free(&rows);
    return ok;
}

static bool chat_excerpts(Db *db, const MemoryScope *scope, const ConversationRow *conversation, const char *query,
                          const RecallTerm *terms, unsigned nterms, RecallResult *r, size_t *capacity)
{
    MemoryCandidate candidates[MEMORY_CANDIDATES];
    size_t n = 0;
    if (!memory_lexical_candidates(db, scope, query, RECALL_CANDIDATES_PER_SCOPE, candidates, &n)) return false;
    r->candidates += n;
    for (size_t i = 0; i < n; i++) {
        MemoryUnit unit = { 0 };
        bool found = false;
        if (!memory_candidate_hydrate(db, scope, &candidates[i], &unit, &found)) return false;
        if (!found) {
            r->rejected++;
            continue;
        }
        RecallRow staged = { 0 };
        bool exact = false;
        bool ok = verify_chat_unit(db, scope, &unit, &staged, &exact);
        if (!ok) {
            memory_unit_free(&unit);
            return false;
        }
        unsigned matched = exact ? recall_match(unit.text, terms, nterms) : 0;
        if (!exact || !matched) {
            if (!exact) r->rejected++;
            memory_unit_free(&unit);
            continue;
        }
        RecallRow *row;
        if (!push_row(r, capacity, &row)) {
            memory_unit_free(&unit);
            return false;
        }
        row->kind = RECALL_CHAT_EXCERPT;
        row->matched = matched;
        row->score = candidates[i].score;
        row->text = unit.text; /* ownership moves */
        unit.text = NULL;
        row->text_bytes = unit.text_bytes;
        row->codepoints = codepoints_in(row->text, row->text_bytes);
        row->codepoint_start = staged.codepoint_start;
        row->sequence = staged.sequence;
        row->role = unit.role;
        row->byte_start = unit.byte_start;
        snprintf(row->conversation_id, sizeof(row->conversation_id), "%s", conversation->id);
        snprintf(row->conversation_title, sizeof(row->conversation_title), "%s", conversation->title);
        snprintf(row->unit_id, sizeof(row->unit_id), "%s", unit.id);
        memory_unit_free(&unit);
    }
    return true;
}

static bool document_seen(const RecallResult *r, size_t from, const char *revision_id, uint64_t byte_start)
{
    for (size_t i = from; i < r->count; i++) {
        const RecallRow *row = &r->rows[i];
        if (row->kind == RECALL_DOCUMENT_EXCERPT && row->byte_start == byte_start &&
            strcmp(row->revision_id, revision_id) == 0)
            return true;
    }
    return false;
}

/* Saved-revision excerpts ingested under this scope. The same SQL admission
 * conditions as the planner's document path, plus byte-exact verification
 * against the revision text and whether that revision is still the saved one. */
static bool document_excerpts(Db *db, const MemoryScope *scope, const char *query, const RecallTerm *terms,
                              unsigned nterms, RecallResult *r, size_t *capacity, size_t excerpts_from)
{
    DbRows rows;
    if (!db_query(db,
                  "SELECT u.id, u.text, u.byte_start, OCTET_LENGTH(u.text), d.id, d.name, r.id,"
                  " CAST(u.text AS BINARY)=SUBSTRING(CAST(r.content AS BINARY),u.byte_start+1,OCTET_LENGTH(u.text)),"
                  " CHAR_LENGTH(CONVERT(SUBSTRING(CAST(r.content AS BINARY),1,u.byte_start) USING utf8mb4)),"
                  " COALESCE(d.saved_revision_id=r.id,0), MATCH(u.text) AGAINST (? IN NATURAL LANGUAGE MODE)"
                  " / POW(CHAR_LENGTH(u.text)," MEMORY_LEXICAL_LENGTH_NORM ") AS score"
                  " FROM memory_units u JOIN memory_scopes s ON s.id=u.scope_id"
                  " JOIN documents d ON d.id=u.document_id AND d.project_id=u.project_id"
                  " JOIN revisions r ON r.id=u.revision_id AND r.document_id=d.id"
                  " WHERE s.id=? AND s.project_id=? AND s.conversation_id=? AND s.enabled=1"
                  " AND s.history_epoch=? AND s.policy_epoch=? AND s.generation=? AND DATABASE()=?"
                  " AND COALESCE(s.tokenizer,'')=? AND u.source_kind='document'"
                  " AND u.generation=s.generation AND u.history_epoch=s.history_epoch"
                  " AND u.policy_epoch=s.policy_epoch AND u.tokenizer=s.tokenizer"
                  " AND u.source_hash=r.source_hash AND u.text_hash=SHA2(u.text,256)"
                  " AND OCTET_LENGTH(u.text)<=2000 AND MATCH(u.text) AGAINST (? IN NATURAL LANGUAGE MODE)>0"
                  " ORDER BY score DESC, u.id LIMIT ?",
                  (DbArg[]){ DB_S(query), DB_S(scope->id), DB_S(scope->project_id), DB_S(scope->conversation_id),
                             DB_I(scope->history_epoch), DB_I(scope->policy_epoch), DB_I(scope->generation),
                             DB_S(scope->database), DB_S(scope->tokenizer), DB_S(query),
                             DB_I(RECALL_CANDIDATES_PER_SCOPE) },
                  11, &rows))
        return false;
    bool ok = rows.ncols == 11;
    r->candidates += ok ? rows.nrows : 0;
    for (size_t i = 0; ok && i < rows.nrows; i++) {
        const char *id, *text, *document_id, *name, *revision_id;
        uint64_t byte_start, bytes;
        int64_t exact, start, saved;
        double score;
        ok = db_cell_text(&rows, i, 0, &id) && db_cell_text(&rows, i, 1, &text) &&
             db_cell_u64(&rows, i, 2, &byte_start) && db_cell_u64(&rows, i, 3, &bytes) &&
             db_cell_text(&rows, i, 4, &document_id) && db_cell_text(&rows, i, 5, &name) &&
             db_cell_text(&rows, i, 6, &revision_id) && db_cell_i64(&rows, i, 7, &exact) &&
             db_cell_i64(&rows, i, 8, &start) && db_cell_i64(&rows, i, 9, &saved) &&
             db_cell_double(&rows, i, 10, &score) && strlen(text) == bytes && strlen(id) < sizeof(((RecallRow *)0)->unit_id) &&
             strlen(document_id) < sizeof(((RecallRow *)0)->document_id) &&
             strlen(revision_id) < sizeof(((RecallRow *)0)->revision_id);
        if (!ok) break;
        if (exact != 1 || start < 0) {
            r->rejected++;
            continue;
        }
        unsigned matched = recall_match(text, terms, nterms);
        if (!matched || document_seen(r, excerpts_from, revision_id, byte_start)) continue;
        RecallRow *row;
        ok = push_row(r, capacity, &row);
        if (!ok) break;
        row->kind = RECALL_DOCUMENT_EXCERPT;
        row->matched = matched;
        row->score = score;
        row->text = strdup(text);
        ok = row->text != NULL;
        row->text_bytes = bytes;
        row->codepoints = codepoints_in(text, bytes);
        row->codepoint_start = (size_t)start;
        row->byte_start = byte_start;
        row->role = CHAT_USER;
        row->current_saved = saved == 1;
        snprintf(row->document_id, sizeof(row->document_id), "%s", document_id);
        snprintf(row->document_name, sizeof(row->document_name), "%s", name);
        snprintf(row->revision_id, sizeof(row->revision_id), "%s", revision_id);
        snprintf(row->unit_id, sizeof(row->unit_id), "%s", id);
        snprintf(row->conversation_id, sizeof(row->conversation_id), "%s", scope->conversation_id);
    }
    db_rows_free(&rows);
    return ok;
}

static bool recall_excerpts(StoreManager *store, const char *dir, const char *query, const RecallTerm *terms,
                            unsigned nterms, RecallResult *r, size_t *capacity)
{
    ProjectStoreInspection inspection = project_store_inspect(store, dir);
    if (!(inspection.capabilities & PROJECT_STORE_CAP_LINKED_DOCUMENTS)) {
        note(r, inspection.blocked_by == PROJECT_STORE_BLOCK_NONE
                    ? "original excerpts need the project store; this project uses files"
                    : "original excerpts unavailable: the project store is paused or offline");
        return true;
    }
    char project_id[64];
    if (!project_store_feature_key(store, dir, PROJECT_STORE_CAP_SEMANTIC, project_id, sizeof(project_id))) {
        note(r, "original excerpts unavailable: the project is not identified in the store");
        return true;
    }
    Db *db = store_db(store);
    if (!db || db_in_transaction(db)) {
        note(r, "original excerpts unavailable: no project store connection");
        return true;
    }
    ConversationRow conversations[RECALL_CONVERSATIONS_MAX];
    size_t n = 0;
    bool truncated = false;
    if (!list_conversations(db, project_id, conversations, &n, &truncated)) {
        note(r, "original excerpts unavailable: conversations could not be listed");
        return true;
    }
    r->conversations = n;
    if (truncated) note(r, "only the 64 most recently updated conversations were searched");
    size_t excerpts_from = r->count;
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        const ConversationRow *c = &conversations[i];
        if (!c->enabled) {
            r->memory_off++;
            continue;
        }
        if (c->indexing) {
            r->memory_indexing++;
            continue;
        }
        MemoryScope scope;
        if (!memory_scope_get(db, project_id, c->id, &scope) || !scope.enabled) {
            r->memory_off++;
            continue;
        }
        if (!scope.tokenizer[0]) {
            r->memory_indexing++;
            continue;
        }
        r->memory_on++;
        ok = chat_excerpts(db, &scope, c, query, terms, nterms, r, capacity) &&
             document_excerpts(db, &scope, query, terms, nterms, r, capacity, excerpts_from);
    }
    if (!ok) {
        drop_rows_from(r, excerpts_from);
        r->candidates = r->rejected = 0;
        note(r, "original excerpts unavailable: a project store read failed");
        return true;
    }
    r->excerpts_available = true;
    return true;
}

/* ---- ranking ----------------------------------------------------------- */

static int compare_canon(const void *a, const void *b)
{
    const RecallRow *x = a, *y = b;
    if (x->matched != y->matched) return x->matched > y->matched ? -1 : 1;
    if (x->ts != y->ts) return x->ts > y->ts ? -1 : 1;
    return strcmp(x->record_id, y->record_id);
}

static int compare_excerpt(const void *a, const void *b)
{
    const RecallRow *x = a, *y = b;
    if (x->matched != y->matched) return x->matched > y->matched ? -1 : 1;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    return strcmp(x->unit_id, y->unit_id);
}

static void rank_and_bound(RecallResult *r, size_t canon_count)
{
    if (canon_count > 1) qsort(r->rows, canon_count, sizeof(*r->rows), compare_canon);
    if (r->count - canon_count > 1)
        qsort(r->rows + canon_count, r->count - canon_count, sizeof(*r->rows), compare_excerpt);
    if (canon_count > RECALL_CANON_ROWS_MAX) {
        /* Keep the best canon rows; move the excerpts down to follow them. */
        size_t excerpts = r->count - canon_count;
        for (size_t i = RECALL_CANON_ROWS_MAX; i < canon_count; i++) row_free(&r->rows[i]);
        memmove(r->rows + RECALL_CANON_ROWS_MAX, r->rows + canon_count, excerpts * sizeof(*r->rows));
        r->count = RECALL_CANON_ROWS_MAX + excerpts;
        canon_count = RECALL_CANON_ROWS_MAX;
        note(r, "only the 32 best-matching canon entries are shown");
    }
    if (r->count > RECALL_ROWS_MAX) {
        drop_rows_from(r, RECALL_ROWS_MAX);
        note(r, "only the 64 best-matching rows are shown");
    }
}

/* ---- entry points ------------------------------------------------------ */

bool recall_run(StoreManager *store, const char *project_dir, const char *query, RecallResult *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!store || !project_dir || !project_dir[0] || !query) {
        note(out, "no project or query");
        return false;
    }
    if (strnlen(query, RECALL_QUERY_BYTES + 1) > RECALL_QUERY_BYTES) {
        note(out, "the query is longer than 512 bytes");
        return false;
    }
    RecallTerm terms[RECALL_TERMS_MAX];
    out->terms = recall_terms(query, terms);
    if (!out->terms) {
        out->canon_ok = true;
        note(out, "no searchable terms (two or more characters); nothing was searched");
        return true;
    }
    size_t capacity = 0;
    if (!recall_canon(store, project_dir, terms, out->terms, out, &capacity)) {
        drop_rows_from(out, 0);
        return false;
    }
    out->canon_ok = true;
    size_t canon_count = out->count;
    (void)recall_excerpts(store, project_dir, query, terms, out->terms, out, &capacity);
    rank_and_bound(out, canon_count);
    return true;
}

void recall_result_free(RecallResult *result)
{
    if (!result) return;
    for (size_t i = 0; i < result->count; i++) row_free(&result->rows[i]);
    free(result->rows);
    memset(result, 0, sizeof(*result));
}

static void append(char **buffer, size_t *used, size_t *capacity, const char *text)
{
    size_t length = strlen(text);
    if (*used + length + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 1024;
        while (*used + length + 1 > next) next *= 2;
        char *grown = realloc(*buffer, next);
        if (!grown) return;
        *buffer = grown;
        *capacity = next;
    }
    memcpy(*buffer + *used, text, length + 1);
    *used += length;
}

static void append_flat(char **buffer, size_t *used, size_t *capacity, const char *text)
{
    size_t length = strlen(text);
    char *flat = malloc(length + 1);
    if (!flat) return;
    for (size_t i = 0; i <= length; i++) flat[i] = text[i] == '\n' || text[i] == '\r' ? ' ' : text[i];
    append(buffer, used, capacity, flat);
    free(flat);
}

char *recall_result_text(const RecallResult *r)
{
    if (!r) return NULL;
    char *buffer = NULL;
    size_t used = 0, capacity = 0;
    char line[640];
    snprintf(line, sizeof(line),
             "coverage|terms=%u|canon_committed=%zu|canon_retracted=%zu|conversations=%zu|memory_on=%zu"
             "|memory_off=%zu|memory_indexing=%zu|candidates=%zu|rejected=%zu|canon_ok=%s|excerpts=%s|rows=%zu|limits=",
             r->terms, r->canon_committed, r->canon_retracted, r->conversations, r->memory_on, r->memory_off,
             r->memory_indexing, r->candidates, r->rejected, r->canon_ok ? "yes" : "no",
             r->excerpts_available ? "yes" : "no", r->count);
    append(&buffer, &used, &capacity, line);
    append_flat(&buffer, &used, &capacity, r->limits);
    append(&buffer, &used, &capacity, "\n");
    for (size_t i = 0; i < r->count; i++) {
        const RecallRow *row = &r->rows[i];
        if (row->kind == RECALL_CANON)
            snprintf(line, sizeof(line), "canon|%s|v%d|committed|matched=%u|", row->record_id, row->ver, row->matched);
        else if (row->kind == RECALL_CHAT_EXCERPT)
            snprintf(line, sizeof(line), "chat|%s|seq=%lld|%s|start=%zu|matched=%u|", row->conversation_title,
                     (long long)row->sequence, row->role == CHAT_USER ? "author" : "assistant", row->codepoint_start,
                     row->matched);
        else
            snprintf(line, sizeof(line), "document|%s|revision=%s|%s|start=%zu|matched=%u|", row->document_name,
                     row->revision_id, row->current_saved ? "saved" : "superseded", row->codepoint_start, row->matched);
        append(&buffer, &used, &capacity, line);
        append_flat(&buffer, &used, &capacity, row->text ? row->text : "");
        append(&buffer, &used, &capacity, "\n");
    }
    return buffer;
}
