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
    bool has_ts; /* the line carried a numeric ts */
} ChatTitle;
bool chat_store_titles_files(const char *project_dir, const char *id, ChatTitle **titles, size_t *count);
void chat_store_titles_free(ChatTitle *titles, size_t count);
bool chat_store_set_observer_files(const char *project_dir, const char *id, bool read);
size_t chat_store_list_files(const char *project_dir, ChatSummary **out);
bool chat_store_list_files_checked(const char *project_dir, ChatSummary **out, size_t *count);
bool chat_store_load_files(const char *project_dir, const char *id, ChatMessage **messages, size_t *count);

#endif
