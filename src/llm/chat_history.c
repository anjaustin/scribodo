#define _DARWIN_C_SOURCE
#include "chat_history.h"

#include <stdlib.h>
#include <string.h>

void chat_request_history_free(ChatRequestHistory *h)
{
    if (!h) return;
    for (size_t i = 0; i < h->count; i++) free(h->messages[i].content);
    free(h->messages);
    *h = (ChatRequestHistory){0};
}

bool chat_request_history_copy(const ChatMessage *msgs, size_t n, bool remote, ChatRequestHistory *out)
{
    if (!out) return false;
    *out = (ChatRequestHistory){0};
    if (n && !msgs) return false;
    if (!n) return true;
    /* The newest author request is mandatory. A missing provider marker must
     * fail closed, not turn a remote request into a reply to older history. */
    const ChatMessage *current = &msgs[n - 1];
    if (current->role != CHAT_USER || current->error || current->streaming || current->excerpt || !current->content ||
        (remote && !(current->via && current->via[0]))) return false;
    size_t indices[CHAT_HISTORY_MAX_MESSAGES], lengths[CHAT_HISTORY_MAX_MESSAGES];
    size_t count = 0, bytes = 0, scanned = 0, i = n;
    bool byte_limited = false;
    while (i && scanned < CHAT_HISTORY_MAX_SCAN && count < CHAT_HISTORY_MAX_MESSAGES) {
        const ChatMessage *m = &msgs[--i];
        scanned++;
        if (m->excerpt) { byte_limited = true; continue; }
        if (m->error || (remote && !(m->via && m->via[0]))) continue;
        if (m->role != CHAT_USER && m->role != CHAT_ASSISTANT) return false;
        const char *text = m->content ? m->content : "";
        size_t room = CHAT_HISTORY_MAX_BYTES - bytes;
        size_t len = strnlen(text, room + 1);
        if (len > room) {
            /* Never silently shorten the author's current request. */
            if (i == n - 1) return false;
            byte_limited = true;
            break;
        }
        indices[count] = i;
        lengths[count++] = len;
        bytes += len;
    }
    bool limited = byte_limited || i != 0;
    /* Drop an orphaned assistant at the oldest boundary. */
    while (count && msgs[indices[count - 1]].role != CHAT_USER) {
        bytes -= lengths[--count];
        limited = true;
    }
    ChatMessage *copy = count ? calloc(count, sizeof(*copy)) : NULL;
    if (count && !copy) return false;
    ChatRequestHistory h = { .messages = copy, .count = count, .content_bytes = bytes,
                             .scanned = scanned, .limited = limited };
    for (size_t j = 0; j < count; j++) {
        size_t k = count - j - 1;
        copy[j] = msgs[indices[k]];
        copy[j].content = malloc(lengths[k] + 1);
        copy[j].docs = copy[j].via = NULL;
        if (!copy[j].content) {
            /* Only initialized content pointers may be freed. */
            h.count = j;
            chat_request_history_free(&h);
            return false;
        }
        memcpy(copy[j].content, msgs[indices[k]].content ? msgs[indices[k]].content : "", lengths[k]);
        copy[j].content[lengths[k]] = '\0';
    }
    *out = h;
    return true;
}
