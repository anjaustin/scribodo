/* Bounded owned history for request preparation, not the durable archive. */
#ifndef SCRIBODO_CHAT_HISTORY_H
#define SCRIBODO_CHAT_HISTORY_H

#include "llm/chat.h"

#define CHAT_HISTORY_MAX_MESSAGES 64u
#define CHAT_HISTORY_MAX_BYTES (64u * 1024u)
#define CHAT_HISTORY_MAX_SCAN 256u

typedef struct {
    ChatMessage *messages; /* only content is owned; docs/via are not copied */
    size_t count, content_bytes, scanned;
    bool limited;
} ChatRequestHistory;

/* Caller protects msgs against mutation. Newest message is mandatory and must
 * fit the byte cap; older messages are optional, never truncated. Remote history
 * excludes local turns BEFORE copying. Errors are excluded. Selection scans at
 * most MAX_SCAN rows, copies at most MAX_MESSAGES/MAX_BYTES, and starts with a
 * user turn. The newest entry must be a non-streaming, non-error user turn with
 * content (and a provider marker for remote mode), or the call fails closed.
 * Failure returns an empty result. No tokenizer/network calls. */
bool chat_request_history_copy(const ChatMessage *msgs, size_t n, bool remote, ChatRequestHistory *out);
void chat_request_history_free(ChatRequestHistory *history);

#endif
