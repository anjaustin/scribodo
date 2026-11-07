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
/* Drop old window layouts and queued positional actions after paging/eviction. */
void chat_view_history_changed(ChatView *v);
typedef enum { CHAT_PAGE_NONE, CHAT_PAGE_OLDER, CHAT_PAGE_NEWER, CHAT_PAGE_LATEST, CHAT_PAGE_PART } ChatPageKind;
typedef struct { ChatPageKind kind; int64_t sequence; size_t start; } ChatPageAction;
ChatPageAction chat_view_take_page(ChatView *v);
Rectangle chat_view_page_rect(ChatView *v, ChatPageKind kind);
size_t chat_view_cache_bytes(const ChatView *v);

/* For tests and notices: text the last "Copy" click put on the clipboard. */
bool chat_view_take_copied(ChatView *v);

/* Index of the reply whose "Apply" link was clicked (propose it as a
 * document edit), or -1. */
long chat_view_take_apply(ChatView *v);
/* Index whose retrieval provenance was requested, or -1 (no other action). */
long chat_view_take_sources(ChatView *v);
Rectangle chat_view_last_sources_rect(const ChatView *v);
/* Where the newest reply's Apply link was drawn (for UI scripts). */
Rectangle chat_view_last_apply_rect(const ChatView *v);
/* Whether "Apply to document" is offered (a writable document is open). */
void chat_view_set_can_apply(ChatView *v, bool can);
/* The label on the assistant's replies (the stored name). */
void chat_view_set_assistant_name(ChatView *v, const char *name);
/* The author's name: replies' speaker labels with it are hidden. */
void chat_view_set_author_name(ChatView *v, const char *name);
/* Scroll to a message and outline it briefly (a finding's source). */
void chat_view_reveal(ChatView *v, long index);
/* A reply's "Note" or "Canon" link was clicked: its index (and which), or -1. */
long chat_view_take_record(ChatView *v, bool *canon);
/* Add text to the end of the message box (not sent), keeping what the
 * author typed; one undoable step. The author sees, edits and sends it. */
void chat_view_append_input(ChatView *v, const char *text);
Rectangle chat_view_last_note_rect(const ChatView *v, bool canon);

#endif
