/* File-backend chat operations used by routing and transitions. */
#ifndef SCRIBODO_CHAT_STORE_INTERNAL_H
#define SCRIBODO_CHAT_STORE_INTERNAL_H

#include "app/chat_store.h"

bool chat_store_append_files(StoreManager *manager, const char *project_dir, const char *id, const char *title, const ChatMessage *message);
bool chat_store_rename_files(const char *project_dir, const char *id, const char *title);
/* One title line ({"type":"title"}) stamped `ts`: a rename made at that time. The export writes the archived titles
 * with it, oldest first, so the file store keeps what conversation_titles keeps (AUDIT-004 F11). */
bool chat_store_title_line_files(const char *project_dir, const char *id, const char *title, double ts);

/* Every title a chat file has carried, oldest first: the meta line's, then each title line's. titles[k].ts is when
 * titles[k] became the title (0 for the meta line's, whose time is the chat's start), so titles[k] was replaced at
 * titles[k + 1].ts; the last is the current title. */
typedef struct {
    char *title;
    double ts;
    bool has_ts; /* the line carried a ts in the message range: finite, 0 <= ts < 2^63 */
} ChatTitle;
bool chat_store_titles_files(const char *project_dir, const char *id, ChatTitle **titles, size_t *count);
void chat_store_titles_free(ChatTitle *titles, size_t count);
/* The same two reads from a chat file at `path` (a set-aside legacy copy, for the title backfill): nothing created. */
bool chat_store_titles_path(const char *path, ChatTitle **titles, size_t *count);
bool chat_store_load_path(const char *path, ChatMessage **messages, size_t *count);
bool chat_store_set_observer_files(const char *project_dir, const char *id, bool read);
bool chat_store_set_archived_files(const char *project_dir, const char *id, bool archived);
bool chat_store_set_group_files(const char *project_dir, const char *id, const char *group);
bool chat_store_delete_files(const char *project_dir, const char *id, ChatDeleteStats *out);
bool chat_store_rename_group_files(const char *project_dir, const char *from, const char *to);
size_t chat_store_list_files(const char *project_dir, ChatSummary **out);
bool chat_store_list_files_checked(const char *project_dir, ChatSummary **out, size_t *count);
bool chat_store_load_files(const char *project_dir, const char *id, ChatMessage **messages, size_t *count);

#endif
