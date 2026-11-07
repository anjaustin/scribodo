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
    bool error, is_edit, truncated, remote, stopped;
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

typedef enum { CHAT_WINDOW_LATEST, CHAT_WINDOW_BEFORE, CHAT_WINDOW_AFTER, CHAT_WINDOW_FOCUS } ChatWindowDirection;
typedef struct {
    ChatWindowDirection direction;
    int64_t sequence;
    size_t start; /* codepoint offset, FOCUS target only */
} ChatWindowRequest;
typedef struct {
    ChatMessage *messages;
    size_t count, text_bytes;
    bool older, newer;
    bool attachments_known;
    char *latest_user_docs; /* bounded current names, not historical revisions */
} ChatWindowPage;

/* DB-only display windows: <=64 messages, <=64 KiB body bytes, <=8 KiB per
 * displayed body. Explicit excerpt flags prevent treating a partial original
 * as a complete model turn or applying it as a whole reviewed replacement. */
bool chat_store_window(StoreManager *manager, const char *project_dir, const char *conversation,
                        const ChatWindowRequest *request, ChatWindowPage *out);
void chat_window_page_free(ChatWindowPage *page);

#endif
