/* A creative-chat conversation with the local model.
 *
 * chat_send() appends the author's message and an empty assistant message,
 * then streams the reply into it on a worker thread. The UI reads messages
 * under chat_lock()/chat_unlock() and redraws when chat_version() changes.
 *
 * Every request applies the rules from docs/MODEL-001:
 *   - all user and document text is neutralised (special-token injection);
 *   - earlier reasoning is never sent back, and stray <think> blocks are
 *     removed from what the author sees;
 *   - thinking is disabled (enable_thinking=false);
 *   - the conversation is trimmed to the slot's context budget, keeping the
 *     system prompt and document (stable prefix) first.
 */
#ifndef SCRIBODO_CHAT_H
#define SCRIBODO_CHAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llm/server.h"

typedef enum { CHAT_USER = 0, CHAT_ASSISTANT = 1 } ChatRole;

typedef struct {
    ChatRole role;
    char *content;   /* visible text (UTF-8) */
    bool error;      /* assistant message that failed */
    bool streaming;  /* still being generated */
} ChatMessage;

typedef struct {
    const char *doc_title; /* NULL = no document attached */
    const char *doc_text;
} ChatContext;

typedef struct ChatSession ChatSession;

/* Called on the UI thread from chat_poll() when a message is final. */
typedef void (*ChatMessageDone)(ChatSession *s, size_t index, void *ctx);

/* `server` may be NULL to use the built-in fake model (UI tests). */
ChatSession *chat_create(LlmServer *server);
void chat_destroy(ChatSession *s);

/* Starts streaming a reply. Fails (returns false with a reason) if a reply
 * is already streaming or the model isn't ready. */
bool chat_send(ChatSession *s, const char *text, const ChatContext *ctx, char *err, size_t errlen);
void chat_cancel(ChatSession *s);
bool chat_busy(ChatSession *s);

/* Load an existing conversation (replaces the current one). Not while busy. */
bool chat_load_messages(ChatSession *s, const ChatMessage *msgs, size_t n);
void chat_clear(ChatSession *s);

/* Call each frame on the UI thread: fires ChatMessageDone for finished
 * messages. Returns true if anything changed since the last poll. */
bool chat_poll(ChatSession *s);
void chat_on_message_done(ChatSession *s, ChatMessageDone fn, void *ctx);

void chat_lock(ChatSession *s);
void chat_unlock(ChatSession *s);
size_t chat_count(ChatSession *s);                    /* lock held */
const ChatMessage *chat_message(ChatSession *s, size_t i); /* lock held */
uint64_t chat_version(ChatSession *s);

/* ---- exposed for tests ------------------------------------------------ */

/* Build the JSON request body for the conversation `msgs[0..n)` (the
 * in-progress assistant message excluded). Returns malloc'd JSON. If the
 * document had to be truncated, *doc_truncated is set. */
char *chat_build_request(const ChatMessage *msgs, size_t n, const ChatContext *ctx, int ctx_tokens,
                         bool *doc_truncated);

/* Parse one server-sent-event line. Appends content to *acc. Returns:
 * 1 = more to come, 0 = finished, -1 = error (message in err). */
int chat_parse_sse_line(const char *line, char **acc, size_t *acc_len, char *err, size_t errlen);

#endif
