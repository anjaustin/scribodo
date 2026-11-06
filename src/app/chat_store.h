/* Chat persistence: one append-only JSON-lines file per conversation in
 * <project>/.scribodo/chats/<id>.jsonl.
 *
 *   {"type":"meta","version":1,"title":"…","created":1730700000}
 *   {"type":"message","role":"user","content":"…","ts":…}
 *   {"type":"message","role":"assistant","content":"…","ts":…,"error":false}
 *
 * Each finished message is appended and fsync'd, so a crash loses at most
 * the reply being written. Interim storage until the MariaDB-backed project
 * store (SOW §5) exists.
 */
#ifndef SCRIBODO_CHAT_STORE_H
#define SCRIBODO_CHAT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "llm/chat.h"

typedef struct {
    char id[40];
    char title[160];
    time_t updated;
    long updated_nsec; /* orders chats changed within one second */
    bool observer_ignore; /* the author told the observer not to read it */
} ChatSummary;

/* Directory for a project rooted at `project_dir` (created on demand). */
bool chat_store_dir(const char *project_dir, char *out, size_t outlen);

/* New conversation id ("YYYYMMDD-HHMMSS-xxxx"). */
void chat_store_new_id(char *out, size_t outlen);

/* Append one message; writes the meta line first if the file is new.
 * `title` is used only for a new file. */
bool chat_store_append(const char *project_dir, const char *id, const char *title, const ChatMessage *m);

/* Rename a stored conversation (a title line is appended). */
bool chat_store_rename(const char *project_dir, const char *id, const char *title);
/* Whether the observer reads this chat (the author's choice; a line is
 * appended to the chat's file, or the store updated). */
bool chat_store_set_observer(const char *project_dir, const char *id, bool read);

/* List conversations, newest first. Caller frees *out. */
size_t chat_store_list(const char *project_dir, ChatSummary **out);

/* Load a conversation's messages. Caller frees with chat_store_free_messages. */
bool chat_store_load(const char *project_dir, const char *id, ChatMessage **msgs, size_t *n);
void chat_store_free_messages(ChatMessage *msgs, size_t n);

#endif
