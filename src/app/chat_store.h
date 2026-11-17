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
#include "app/store.h"

typedef struct {
    char id[40];
    char title[160];
    time_t updated;
    long updated_nsec; /* orders chats changed within one second */
    bool observer_ignore; /* the author told the observer not to read it */
    bool archived;        /* the author archived it: listed under Archived, set aside from memory recall */
    char group[48];       /* the group it is filed under (as read back: chat_group_name_stored), "" none */
} ChatSummary;

/* A group's name as kept (user request 2026-11-17): blanks of any kind trimmed; control, bidi and direction characters, the zero-width
 * space and invalid UTF-8 refused (joiners kept); at least one visible character; not "Archived" (that heading's name); at most
 * CHAT_GROUP_MAX bytes cut between characters. False when refused or nothing is left (no group). */
#define CHAT_GROUP_MAX 40
bool chat_group_name_clean(const char *in, char out[48]);
/* The same for a name read back from the store: one kept before a rule was added is still shown. */
bool chat_group_name_stored(const char *in, char out[48]);

/* Directory for a project rooted at `project_dir` (created on demand). */
bool chat_store_dir(const char *project_dir, char *out, size_t outlen);

/* New conversation id ("YYYYMMDD-HHMMSS-xxxx"). */
void chat_store_new_id(char *out, size_t outlen);

/* Append one message; writes the meta line first if the file is new.
 * `title` is used only for a new file. */
bool chat_store_append(StoreManager *manager, const char *project_dir, const char *id, const char *title, const ChatMessage *m);
/* Return the committed DB sequence; file mode returns -1. Failure never assigns
 * a sequence. A failed/uncertain append must not be retried automatically. */
bool chat_store_append_sequenced(StoreManager *manager, const char *project_dir, const char *id, const char *title,
                                const ChatMessage *m, int64_t *sequence);

/* Messages whose append the store refused are saved when it answers again (red-team 2026-11-14: the status told
 * the author to keep them and nothing re-appended them, so sending stayed refused). Exactly the messages marked
 * save_failed are appended, in order; a reply whose done callback has not run yet carries no mark, and neither does
 * a chat loaded from files before its project moved into the store (second pass: inferring "unsaved" from a missing
 * sequence re-appended both). Each append that returns a sequence clears its mark (chat_mark_saved). Nothing is done
 * while `chat` is busy. Returns how many marked messages there were (0 when none, busy, or out of memory); `*ok` is
 * false when an append failed, and the rest are then left marked. */
size_t chat_store_resave_failed(StoreManager *manager, ChatSession *chat, const char *project_dir, const char *id,
                                const char *title, bool *ok);

/* Rename a stored conversation (a title line is appended). */
bool chat_store_rename(StoreManager *manager, const char *project_dir, const char *id, const char *title);
/* Whether the observer reads this chat (the author's choice; a line is
 * appended to the chat's file, or the store updated). */
bool chat_store_set_observer(StoreManager *manager, const char *project_dir, const char *id, bool read);

/* Archive a stored conversation, or restore it (a line is appended to the chat's file, or the store updated). An
 * archived chat is set aside from memory recall; nothing of it is deleted. */
bool chat_store_set_archived(StoreManager *manager, const char *project_dir, const char *id, bool archived);

/* File a stored conversation under a group ("" takes it out of its group). */
bool chat_store_set_group(StoreManager *manager, const char *project_dir, const char *id, const char *group);
/* Every chat filed under `from` is filed under `to` instead ("" ungroups them; an existing name merges the two). */
bool chat_store_rename_group(StoreManager *manager, const char *project_dir, const char *from, const char *to);

/* Delete a stored conversation (user request 2026-11-17). In the project store, what memory and the observer derived
 * from it goes too (sdb_chat_delete) and its semantic-index points are listed for removal (semantic_forget_pending);
 * in the file store its file is removed. Cannot be undone; backups made before keep it. */
typedef struct {
    uint64_t messages, memory_rows, findings, finding_sources, entities;
    bool derived; /* the project store: what memory and the observer derived went too (the file store: only the file) */
} ChatDeleteStats;
bool chat_store_delete(StoreManager *manager, const char *project_dir, const char *id, ChatDeleteStats *out);

/* List conversations, newest first. Caller frees *out. */
size_t chat_store_list(StoreManager *manager, const char *project_dir, ChatSummary **out);
bool chat_store_list_checked(StoreManager *manager, const char *project_dir, ChatSummary **out, size_t *count);

/* Load a conversation's messages. Caller frees with chat_store_free_messages. */
bool chat_store_load(StoreManager *manager, const char *project_dir, const char *id, ChatMessage **msgs, size_t *n);
void chat_store_free_messages(ChatMessage *msgs, size_t n);

#endif
