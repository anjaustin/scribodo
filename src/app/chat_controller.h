#ifndef SCRIBODO_CHAT_CONTROLLER_H
#define SCRIBODO_CHAT_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/chat.h"
#include "ui/chat_view.h"

#define CHAT_CONTROLLER_MAX_ATTACH 8

typedef struct {
    ChatSession *chat;
    ChatView *chat_view;
    char attached[CHAT_CONTROLLER_MAX_ATTACH][256];
    size_t nattached;
    bool attach_touched;
    uint64_t attachment_revision;
} ChatController;

void chat_controller_init(ChatController *controller);
/* Adopts both objects. May be called only on an empty controller. */
bool chat_controller_adopt(ChatController *controller, ChatSession *chat, ChatView *view);
bool chat_controller_has_attachment(const ChatController *controller, const char *name);
bool chat_controller_add_attachment(ChatController *controller, const char *name);
bool chat_controller_remove_attachment(ChatController *controller, size_t index);
bool chat_controller_rename_attachment(ChatController *controller, const char *from, const char *to);
void chat_controller_mark_attachments_changed(ChatController *controller);
void chat_controller_use_no_attachments(ChatController *controller);
void chat_controller_clear_attachments(ChatController *controller);
void chat_controller_destroy(ChatController *controller);

#endif
