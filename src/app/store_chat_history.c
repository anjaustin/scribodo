#include "app/chat_history.h"
#include "app/store_backends.h"
#include "core/utf8.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void chat_source_page_free(ChatSourcePage *page)
{
    if (!page) return;
    for (size_t i = 0; i < page->count; i++) free(page->chunks[i].text);
    *page = (ChatSourcePage){0};
}

static bool read_row_bool(const DbRows *rows, size_t row, size_t col, bool *out)
{
    int64_t n;
    if (!db_cell_i64(rows, row, col, &n) || (n != 0 && n != 1)) return false;
    *out = n == 1;
    return true;
}

static bool read_bool(const DbRows *rows, size_t col, bool *out) { return read_row_bool(rows, 0, col, out); }

bool sdb_chat_source_page(Db *db, const char *project, const char *conversation,
                          const ChatSourceCursor *cursor, ChatSourcePage *out)
{
    if (!out) return false;
    *out = (ChatSourcePage){0};
    if (!db || !project || !conversation || !cursor) return false;
    ChatSourceCursor next = *cursor;
    if (next.seq < 0 || next.seq > (int64_t)INT_MAX + 1 || next.byte_offset > INT64_MAX - 1 ||
        (next.started && (next.end_seq < -1 || next.end_seq > INT_MAX)) ||
        (next.started && (next.seq > next.end_seq + 1 || (next.seq > next.end_seq && next.byte_offset))) ||
        (!next.started && (next.seq || next.byte_offset))) return false;
    DbRows rows;
    if (!next.started) {
        if (!db_query(db,
                      "SELECT m.seq FROM messages m"
                      " JOIN conversations c ON c.id = m.conversation_id"
                      " WHERE m.conversation_id = ? AND c.project_id = ? ORDER BY m.seq DESC LIMIT 1",
                      (DbArg[2]){ DB_S(conversation), DB_S(project) }, 2, &rows)) return false;
        next.end_seq = -1;
        bool ok = rows.nrows <= 1 && db_rows_has_columns(&rows, 1) &&
                  (!rows.nrows || (db_cell_i64(&rows, 0, 0, &next.end_seq) && next.end_seq >= 0 && next.end_seq <= INT_MAX));
        db_rows_free(&rows);
        if (!ok) return false;
        next.started = true;
    }
    ChatSourcePage page = {0};
    bool ok = true;
    while (next.seq <= next.end_seq && page.count < CHAT_SOURCE_PAGE_ROWS &&
           CHAT_SOURCE_PAGE_BYTES - page.text_bytes >= 4) {
        size_t cap = CHAT_SOURCE_PAGE_BYTES - page.text_bytes;
        if (cap > CHAT_SOURCE_CHUNK_BYTES) cap = CHAT_SOURCE_CHUNK_BYTES;
        /* Fetch at most cap+3 bytes to recognize a UTF-8 boundary without ever
         * hydrating the full body. The sequence index supplies a keyset seek. */
        DbArg args[] = { DB_I(next.seq), DB_I(next.byte_offset + 1), DB_I(cap + 3),
                         DB_S(conversation), DB_S(project), DB_I(next.seq), DB_I(next.end_seq) };
        if (!db_query(db,
                      "SELECT m.id, m.seq, m.role,"
                      " SUBSTRING(CAST(m.content AS BINARY), IF(m.seq = ?, ?, 1), ?),"
                      " m.error, m.is_edit, m.truncated, m.created_at,"
                      " (m.via IS NOT NULL AND OCTET_LENGTH(m.via) > 0), OCTET_LENGTH(m.content), m.stopped"
                      " FROM messages m JOIN conversations c ON c.id = m.conversation_id"
                      " WHERE m.conversation_id = ? AND c.project_id = ? AND m.seq >= ? AND m.seq <= ?"
                      " ORDER BY m.seq LIMIT 1", args, 7, &rows)) { ok = false; break; }
        if (rows.nrows == 0) {
            db_rows_free(&rows);
            /* A nonzero offset must not silently skip a deleted source. */
            ok = next.byte_offset == 0;
            next.seq = next.end_seq + 1;
            break;
        }
        ChatSourceChunk chunk = {0};
        const char *role = NULL, *text = NULL;
        ok = rows.nrows == 1 && db_rows_has_columns(&rows, 11) && read_bool(&rows, 10, &chunk.stopped) &&
             db_cell_i64(&rows, 0, 0, &chunk.message_id) && chunk.message_id > 0 &&
             db_cell_i64(&rows, 0, 1, &chunk.seq) && chunk.seq >= next.seq && chunk.seq <= next.end_seq &&
             db_cell_text(&rows, 0, 2, &role) && (!strcmp(role, "user") || !strcmp(role, "assistant")) &&
             db_cell_text(&rows, 0, 3, &text) && read_bool(&rows, 4, &chunk.error) &&
             read_bool(&rows, 5, &chunk.is_edit) && read_bool(&rows, 6, &chunk.truncated) &&
             db_cell_double(&rows, 0, 7, &chunk.created_at) && chunk.created_at >= 0 && chunk.created_at < 0x1p63 &&
             read_bool(&rows, 8, &chunk.remote) && db_cell_u64(&rows, 0, 9, &chunk.total_bytes);
        if (ok) {
            chunk.role = !strcmp(role, "user") ? CHAT_USER : CHAT_ASSISTANT;
            chunk.byte_offset = chunk.seq == next.seq ? next.byte_offset : 0;
            ok = !(chunk.seq != next.seq && next.byte_offset) && chunk.byte_offset <= chunk.total_bytes;
        }
        size_t copied = 0;
        if (ok) {
            uint64_t remaining = chunk.total_bytes - chunk.byte_offset;
            size_t expected = remaining > cap + 3 ? cap + 3 : (size_t)remaining;
            /* Also reject embedded NUL and caller cursors inside a codepoint. */
            ok = strlen(text) == expected && (!expected || ((unsigned char)text[0] & 0xC0) != 0x80) &&
                 (!chunk.byte_offset || remaining);
            if (ok) {
                chunk.text = malloc(cap + 1);
                ok = chunk.text != NULL;
                if (ok) {
                    copied = utf8_clip(chunk.text, cap + 1, text, cap);
                    ok = !remaining || copied > 0;
                }
            }
        }
        db_rows_free(&rows);
        if (!ok) { free(chunk.text); break; }
        page.chunks[page.count++] = chunk;
        page.text_bytes += copied;
        next.seq = chunk.seq;
        next.byte_offset = chunk.byte_offset + copied;
        if (next.byte_offset == chunk.total_bytes) {
            next.seq++;
            next.byte_offset = 0;
        }
    }
    if (!ok) { chat_source_page_free(&page); return false; }
    page.next = next;
    page.has_more = next.seq <= next.end_seq;
    *out = page;
    return true;
}

void chat_window_page_free(ChatWindowPage *page)
{
    if (!page) return;
    chat_store_free_messages(page->messages, page->count);
    free(page->latest_user_docs);
    *page = (ChatWindowPage){0};
}

bool sdb_chat_window(Db *db, const char *project, const char *conversation,
                      const ChatWindowRequest *request, ChatWindowPage *out)
{
    if (!out) return false;
    *out = (ChatWindowPage){0};
    if (!db || !project || !conversation || !request || request->direction < CHAT_WINDOW_LATEST ||
        request->direction > CHAT_WINDOW_FOCUS || request->sequence < 0 || request->sequence > INT_MAX ||
        request->start >= INT64_MAX || (request->direction != CHAT_WINDOW_FOCUS && request->start)) return false;
    bool ascending = request->direction == CHAT_WINDOW_AFTER;
    const char *sql = ascending
        ? "SELECT m.id,m.seq,m.role FROM messages m JOIN conversations c ON c.id=m.conversation_id"
          " WHERE c.project_id=? AND m.conversation_id=? AND m.seq>? ORDER BY m.seq LIMIT 64"
        : request->direction == CHAT_WINDOW_BEFORE
        ? "SELECT m.id,m.seq,m.role FROM messages m JOIN conversations c ON c.id=m.conversation_id"
          " WHERE c.project_id=? AND m.conversation_id=? AND m.seq<? ORDER BY m.seq DESC LIMIT 64"
        : "SELECT m.id,m.seq,m.role FROM messages m JOIN conversations c ON c.id=m.conversation_id"
          " WHERE c.project_id=? AND m.conversation_id=? AND m.seq<=? ORDER BY m.seq DESC LIMIT 64";
    int64_t anchor = request->direction == CHAT_WINDOW_LATEST ? INT_MAX : request->sequence;
    DbRows metadata;
    if (!db_query(db, sql, (DbArg[3]){ DB_S(project), DB_S(conversation), DB_I(anchor) }, 3, &metadata)) return false;
    ChatWindowPage page = {0};
    page.messages = calloc(CHAT_SOURCE_PAGE_ROWS, sizeof(*page.messages));
    bool ok = page.messages && db_rows_has_columns(&metadata, 3) && metadata.nrows <= CHAT_SOURCE_PAGE_ROWS;
    int64_t latest_user = -1, latest_user_seq = -1;
    for (size_t i = 0; ok && i < metadata.nrows; i++) {
        int64_t id, seq;
        const char *role;
        ok = db_cell_i64(&metadata, i, 0, &id) && id > 0 && db_cell_i64(&metadata, i, 1, &seq) && seq >= 0 && seq <= INT_MAX &&
             db_cell_text(&metadata, i, 2, &role) && (!strcmp(role, "user") || !strcmp(role, "assistant"));
        if (ok && !strcmp(role, "user") && seq > latest_user_seq) { latest_user = id; latest_user_seq = seq; }
    }
    page.attachments_known = latest_user >= 0 || metadata.nrows < CHAT_SOURCE_PAGE_ROWS;
    page.latest_user_docs = ok ? calloc(8 * 256 + 1, 1) : NULL;
    ok = ok && page.latest_user_docs;
    if (ok && latest_user >= 0) {
        DbRows docs;
        ok = db_query(db,
                      "SELECT LEFT(CAST(d.name AS BINARY),256),OCTET_LENGTH(d.name) FROM message_documents md"
                      " JOIN documents d ON d.id=md.document_id JOIN messages m ON m.id=md.message_id"
                      " JOIN conversations c ON c.id=m.conversation_id"
                      " WHERE md.message_id=? AND m.conversation_id=? AND c.project_id=? AND d.project_id=? ORDER BY d.name LIMIT 9",
                      (DbArg[4]){ DB_I(latest_user), DB_S(conversation), DB_S(project), DB_S(project) }, 4, &docs);
        if (ok) {
            ok = db_rows_has_columns(&docs, 2);
            bool compatible = docs.nrows <= 8;
            for (size_t i = 0; ok && i < docs.nrows; i++) {
                const char *name;
                uint64_t bytes;
                ok = db_cell_text(&docs, i, 0, &name) && db_cell_u64(&docs, i, 1, &bytes);
                if (ok && (!bytes || bytes >= 256 || strlen(name) != bytes || strchr(name, '\n'))) compatible = false;
                if (ok && compatible) { if (i) strcat(page.latest_user_docs, "\n"); strcat(page.latest_user_docs, name); }
            }
            if (!compatible) { page.attachments_known = false; page.latest_user_docs[0] = '\0'; }
            db_rows_free(&docs);
        }
    }
    for (size_t i = 0; ok && i < metadata.nrows && CHAT_SOURCE_PAGE_BYTES - page.text_bytes >= 4;) {
        /* Small fixed batches avoid a round trip per displayed message while
         * bounding scratch hydration by the remaining page budget (+ lookahead).
         * All IDs come from the scoped keyset query, never from vector payloads. */
        size_t remaining = CHAT_SOURCE_PAGE_BYTES - page.text_bytes;
        size_t batch = metadata.nrows - i;
        if (batch > 8) batch = 8;
        if (batch > remaining / 4) batch = remaining / 4;
        size_t cap = remaining / batch;
        if (cap > CHAT_SOURCE_CHUNK_BYTES) cap = CHAT_SOURCE_CHUNK_BYTES;
        int64_t focus = request->direction == CHAT_WINDOW_FOCUS ? request->sequence : -1;
        DbArg args[17] = { DB_I(focus), DB_I(request->start + 1), DB_I(cap + 3), DB_I(cap + 3),
                           DB_I(focus), DB_I(request->start + 1), DB_I(cap + 3) };
        for (size_t r = 0; r < 8; r++) {
            int64_t id = -1;
            if (r < batch) ok = ok && db_cell_i64(&metadata, i + r, 0, &id);
            args[7 + r] = DB_I(id);
        }
        args[15] = DB_S(conversation); args[16] = DB_S(project);
        if (!ok) break;
        const char *body_sql = ascending
            ? "SELECT m.role,LEFT(CAST(SUBSTRING(m.content,IF(m.seq=?, ?,1),?) AS BINARY),?),m.error,m.is_edit,m.truncated,"
              " LEFT(CAST(m.via AS BINARY),513),m.created_at,OCTET_LENGTH(m.content),"
               " OCTET_LENGTH(SUBSTRING(m.content,IF(m.seq=?, ?,1),?)),COALESCE(OCTET_LENGTH(m.via),0),m.seq,m.stopped"
              " FROM messages m JOIN conversations c ON c.id=m.conversation_id"
              " WHERE m.id IN (?,?,?,?,?,?,?,?) AND m.conversation_id=? AND c.project_id=? ORDER BY m.seq"
            : "SELECT m.role,LEFT(CAST(SUBSTRING(m.content,IF(m.seq=?, ?,1),?) AS BINARY),?),m.error,m.is_edit,m.truncated,"
              " LEFT(CAST(m.via AS BINARY),513),m.created_at,OCTET_LENGTH(m.content),"
               " OCTET_LENGTH(SUBSTRING(m.content,IF(m.seq=?, ?,1),?)),COALESCE(OCTET_LENGTH(m.via),0),m.seq,m.stopped"
              " FROM messages m JOIN conversations c ON c.id=m.conversation_id"
              " WHERE m.id IN (?,?,?,?,?,?,?,?) AND m.conversation_id=? AND c.project_id=? ORDER BY m.seq DESC";
        DbRows row;
        ok = db_query(db, body_sql, args, 17, &row);
        if (!ok) break;
        ok = row.nrows == batch && db_rows_has_columns(&row, 12);
        for (size_t r = 0; ok && r < batch; r++) {
            int64_t seq = -1, expected = -1;
            ok = db_cell_i64(&row, r, 10, &seq) && db_cell_i64(&metadata, i + r, 1, &expected) && seq == expected;
            if (!i && !r && request->direction == CHAT_WINDOW_FOCUS && seq != focus) ok = false;
            if (!ok) break;
            size_t start = seq == focus ? request->start : 0;
            ChatMessage *m = &page.messages[page.count++];
            const char *role = NULL, *text = NULL, *via = NULL;
            uint64_t total = 0, part = 0, via_bytes = 0;
            ok = read_row_bool(&row, r, 11, &m->stopped) && db_cell_text(&row, r, 0, &role) && (!strcmp(role, "user") || !strcmp(role, "assistant")) &&
                 db_cell_text(&row, r, 1, &text) && read_row_bool(&row, r, 2, &m->error) &&
                 read_row_bool(&row, r, 3, &m->is_edit) && read_row_bool(&row, r, 4, &m->truncated) &&
                 db_cell_nullable(&row, r, 5, &via) && db_cell_double(&row, r, 6, &m->ts) && m->ts >= 0 && m->ts < 0x1p63 &&
                 db_cell_u64(&row, r, 7, &total) && db_cell_u64(&row, r, 8, &part) && db_cell_u64(&row, r, 9, &via_bytes) &&
                 via_bytes <= 512 && (!via_bytes || (via && strlen(via) == via_bytes)) &&
                 strlen(text) == (part > cap + 3 ? cap + 3 : part) && (!start || part);
            if (ok) {
                char clipped[CHAT_SOURCE_CHUNK_BYTES + 1];
                size_t copied = utf8_clip(clipped, sizeof(clipped), text, cap);
                m->content = malloc(copied + 1);
                if (m->content) memcpy(m->content, clipped, copied + 1);
                m->via = via ? strdup(via) : NULL;
                ok = m->content && (!via || m->via);
                if (ok) {
                    m->role = !strcmp(role, "user") ? CHAT_USER : CHAT_ASSISTANT;
                    m->sequence_known = true; m->sequence = seq;
                    m->excerpt_start = start;
                    m->excerpt_more = part > copied;
                    m->excerpt = start || copied < total;
                    page.text_bytes += copied;
                }
            }
        }
        db_rows_free(&row);
        i += batch;
    }
    /* A stale/inapplicable navigation request must not erase the displayed
     * window and masquerade as an empty conversation. Only Latest may be empty. */
    if (ok && request->direction != CHAT_WINDOW_LATEST && !page.count) ok = false;
    db_rows_free(&metadata);
    if (ok && !ascending) {
        for (size_t i = 0; i < page.count / 2; i++) {
            ChatMessage tmp = page.messages[i];
            page.messages[i] = page.messages[page.count - i - 1];
            page.messages[page.count - i - 1] = tmp;
        }
    }
    if (ok && page.count) {
        DbRows flags;
        ok = db_query(db,
                      "SELECT EXISTS(SELECT 1 FROM messages WHERE conversation_id=? AND seq<?),"
                      " EXISTS(SELECT 1 FROM messages WHERE conversation_id=? AND seq>?)",
                      (DbArg[4]){ DB_S(conversation), DB_I(page.messages[0].sequence), DB_S(conversation),
                                  DB_I(page.messages[page.count - 1].sequence) }, 4, &flags);
        if (ok) {
            ok = flags.nrows == 1 && db_rows_has_columns(&flags, 2) && read_bool(&flags, 0, &page.older) && read_bool(&flags, 1, &page.newer);
            db_rows_free(&flags);
        }
    }
    if (!ok) { chat_window_page_free(&page); return false; }
    *out = page;
    return true;
}
