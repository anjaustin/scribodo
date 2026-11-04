/* Chat pane: streaming message list plus an input box (a scratch Editor).
 *
 * Layout is owned by the app (Clay); the view gets the rectangles for the
 * message list and the input each frame.
 */
#ifndef SCRIBODO_CHAT_VIEW_H
#define SCRIBODO_CHAT_VIEW_H

#include <stdbool.h>

#include "llm/chat.h"
#include "raylib.h"
#include "ui/editor.h"
#include "ui/font.h"

typedef struct ChatView ChatView;

ChatView *chat_view_create(SFont *text_font, SFont *label_font, ChatSession *chat);
void chat_view_destroy(ChatView *v);

Editor *chat_view_input(ChatView *v);

/* Height the input box wants for `width` (2–8 lines). */
float chat_view_input_height(ChatView *v, float width);

/* Process input. `input_focused` routes the keyboard to the input box. */
void chat_view_update(ChatView *v, Rectangle list, Rectangle input, bool input_focused);
void chat_view_draw(ChatView *v, Rectangle list, Rectangle input, bool input_focused);

/* If the author submitted (Enter or chat_view_submit_now), returns the text
 * (malloc'd); otherwise NULL. The input is kept until chat_view_clear_input,
 * so a message that fails to send is not lost. */
char *chat_view_take_submit(ChatView *v);
void chat_view_submit_now(ChatView *v);
void chat_view_clear_input(ChatView *v);

/* Scroll to the newest message (e.g. after loading a conversation). */
void chat_view_scroll_to_end(ChatView *v);

/* For tests and notices: text the last "Copy" click put on the clipboard. */
bool chat_view_take_copied(ChatView *v);

/* Index of the reply whose "Apply" link was clicked (propose it as a
 * document edit), or -1. */
long chat_view_take_apply(ChatView *v);
/* Where the newest reply's Apply link was drawn (for UI scripts). */
Rectangle chat_view_last_apply_rect(const ChatView *v);
/* A reply's "Note" or "Canon" link was clicked: its index (and which), or -1. */
long chat_view_take_record(ChatView *v, bool *canon);
/* Put text in the message box (not sent): the author sees and sends it. */
void chat_view_set_input(ChatView *v, const char *text);
Rectangle chat_view_last_note_rect(const ChatView *v, bool canon);

#endif
