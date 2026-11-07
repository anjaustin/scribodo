#include "chat_controller.h"

#include <string.h>
#include <stdio.h>

void chat_controller_init(ChatController *controller)
{
    if (controller) memset(controller, 0, sizeof(*controller));
}

bool chat_controller_adopt(ChatController *controller, ChatSession *chat, ChatView *view)
{
    if (!controller || controller->chat || controller->chat_view) return false;
    controller->chat = chat;
    controller->chat_view = view;
    return true;
}

bool chat_controller_has_attachment(const ChatController *controller, const char *name)
{
    if (!controller || !name) return false;
    for (size_t i = 0; i < controller->nattached; i++)
        if (!strcmp(controller->attached[i], name)) return true;
    return false;
}

bool chat_controller_add_attachment(ChatController *controller, const char *name)
{
    if (!controller || !name || !name[0] || strlen(name) >= sizeof(controller->attached[0]) ||
        controller->nattached >= CHAT_CONTROLLER_MAX_ATTACH) return false;
    if (chat_controller_has_attachment(controller, name)) return true;
    snprintf(controller->attached[controller->nattached++], sizeof(controller->attached[0]), "%s", name);
    controller->attachment_revision++;
    chat_memory_attachments_changed(controller->chat);
    return true;
}

bool chat_controller_remove_attachment(ChatController *controller, size_t index)
{
    if (!controller || index >= controller->nattached) return false;
    memmove(&controller->attached[index], &controller->attached[index + 1],
            (controller->nattached - index - 1) * sizeof(controller->attached[0]));
    controller->nattached--;
    controller->attachment_revision++;
    chat_memory_attachments_changed(controller->chat);
    return true;
}

bool chat_controller_rename_attachment(ChatController *controller, const char *from, const char *to)
{
    if (!controller || !from || !to || !to[0] || strlen(to) >= sizeof(controller->attached[0])) return false;
    bool renamed = false;
    for (size_t i = 0; i < controller->nattached; i++) {
        if (strcmp(controller->attached[i], from)) continue;
        snprintf(controller->attached[i], sizeof(controller->attached[i]), "%s", to);
        renamed = true;
    }
    if (renamed) { controller->attachment_revision++; chat_memory_attachments_changed(controller->chat); }
    return renamed;
}

void chat_controller_mark_attachments_changed(ChatController *controller)
{
    if (controller) {
        controller->attach_touched = true; controller->attachment_revision++;
        chat_memory_attachments_changed(controller->chat);
    }
}

void chat_controller_use_no_attachments(ChatController *controller)
{
    if (!controller) return;
    memset(controller->attached, 0, sizeof(controller->attached));
    controller->nattached = 0;
    controller->attach_touched = true;
    controller->attachment_revision++;
    chat_memory_attachments_changed(controller->chat);
}

void chat_controller_clear_attachments(ChatController *controller)
{
    if (!controller) return;
    memset(controller->attached, 0, sizeof(controller->attached));
    controller->nattached = 0;
    controller->attach_touched = false;
    controller->attachment_revision++;
    chat_memory_attachments_changed(controller->chat);
}

void chat_controller_destroy(ChatController *controller)
{
    if (!controller) return;
    chat_view_destroy(controller->chat_view);
    chat_destroy(controller->chat);
    memset(controller, 0, sizeof(*controller));
}
