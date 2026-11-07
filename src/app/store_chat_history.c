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

static bool read_bool(const DbRows *rows, size_t col, bool *out)
{
    int64_t n;
    if (!db_cell_i64(rows, 0, col, &n) || (n != 0 && n != 1)) return false;
    *out = n == 1;
    return true;
}

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
                      " (m.via IS NOT NULL AND OCTET_LENGTH(m.via) > 0), OCTET_LENGTH(m.content)"
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
        ok = rows.nrows == 1 && db_rows_has_columns(&rows, 10) &&
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
