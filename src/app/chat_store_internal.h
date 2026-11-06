/* File-backend chat operations used by routing and transitions. */
#ifndef SCRIBODO_CHAT_STORE_INTERNAL_H
#define SCRIBODO_CHAT_STORE_INTERNAL_H

#include "app/chat_store.h"

bool chat_store_append_files(const char *project_dir, const char *id, const char *title, const ChatMessage *message);
bool chat_store_rename_files(const char *project_dir, const char *id, const char *title);
bool chat_store_set_observer_files(const char *project_dir, const char *id, bool read);
size_t chat_store_list_files(const char *project_dir, ChatSummary **out);
bool chat_store_list_files_checked(const char *project_dir, ChatSummary **out, size_t *count);
bool chat_store_load_files(const char *project_dir, const char *id, ChatMessage **messages, size_t *count);

#endif
