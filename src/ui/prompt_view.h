/* A small modal prompt with one line of text ("New document", "Rename…"):
 * a title, a text field, an error line, and OK/Cancel. Enter confirms, Esc
 * cancels. While it is open the app routes all input to it.
 */
#ifndef SCRIBODO_PROMPT_VIEW_H
#define SCRIBODO_PROMPT_VIEW_H

#include <stdbool.h>

#include "raylib.h"
#include "ui/font.h"

typedef enum { PR_NONE, PR_OK, PR_CANCEL } PromptResult;

typedef struct PromptView PromptView;

PromptView *prompt_create(SFont *text_font, SFont *label_font);
void prompt_destroy(PromptView *v);

/* `tag` says what the prompt is for (the app's own code). The name part of
 * `initial` (before its extension) is selected, ready to type over. */
void prompt_open(PromptView *v, int tag, const char *title, const char *initial, const char *ok_label);
void prompt_close(PromptView *v);
bool prompt_active(const PromptView *v);
int prompt_tag(const PromptView *v);
/* Shown under the field (e.g. "That name is taken"); "" clears it. */
void prompt_set_error(PromptView *v, const char *msg);
/* The text (malloc'd). */
char *prompt_text(const PromptView *v);

/* Centred in `screen`. Update handles typing and Enter/Esc; draw draws and
 * handles the buttons. Each returns PR_OK/PR_CANCEL once. */
PromptResult prompt_update(PromptView *v, Rectangle screen);
PromptResult prompt_draw(PromptView *v, Rectangle screen);
Rectangle prompt_element(const PromptView *v, const char *name); /* PromptField/PromptOK/PromptCancel */

#endif
