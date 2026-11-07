/* Database-only, bounded source traversal for memory ingestion. This is not a
 * display-message loader: no attachment history or revision evidence is implied. */
#ifndef SCRIBODO_STORE_CHAT_HISTORY_H
#define SCRIBODO_STORE_CHAT_HISTORY_H

#include "app/store.h"
#include "llm/chat.h"

#define CHAT_SOURCE_PAGE_ROWS 64u
#define CHAT_SOURCE_PAGE_BYTES (64u * 1024u)
#define CHAT_SOURCE_CHUNK_BYTES (8u * 1024u)

typedef struct {
    bool started;
    int64_t end_seq; /* captured append high-water mark; new messages excluded */
    int64_t seq;     /* next sequence (gaps allowed), inclusive */
    uint64_t byte_offset;
} ChatSourceCursor;

typedef struct {
    int64_t message_id, seq;
    uint64_t byte_offset, total_bytes;
    ChatRole role;
    bool error, is_edit, truncated, remote;
    double created_at;
    char *text; /* owned exact UTF-8 bytes, <= CHUNK_BYTES, NUL terminated */
} ChatSourceChunk;

typedef struct {
    ChatSourceChunk chunks[CHAT_SOURCE_PAGE_ROWS];
    size_t count, text_bytes;
    ChatSourceCursor next;
    bool has_more;
} ChatSourcePage;

/* Begin with a zero cursor. Reuse page.next ONLY with the same conversation and
 * project. On failure page is empty; the input cursor is never changed. Reads
 * are append-stable, not a snapshot against historical mutations. Callers must
 * eventually add history-epoch checks when mutation/brief publication exists.
 * File-backed/paused projects fail explicitly; never load the full file as a
 * fallback. Free each successful page before reusing the output object. */
bool chat_store_source_page(StoreManager *manager, const char *project_dir, const char *conversation,
                            const ChatSourceCursor *cursor, ChatSourcePage *out);
void chat_source_page_free(ChatSourcePage *page);

#endif
