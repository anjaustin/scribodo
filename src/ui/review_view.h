/* Review panel for a proposed edit: a word-level diff of the selected
 * passage against the proposed replacement (deletions struck through in
 * red, insertions in green). Accept/Reject buttons are drawn by the app at
 * review_accept_rect()/review_reject_rect().
 */
#ifndef SCRIBODO_REVIEW_VIEW_H
#define SCRIBODO_REVIEW_VIEW_H

#include <stdbool.h>

#include "raylib.h"
#include "ui/font.h"

typedef struct ReviewView ReviewView;

ReviewView *review_create(SFont *text_font, SFont *label_font);
void review_destroy(ReviewView *v);

/* Show a proposal (copies both strings). `note` is shown in the header;
 * `truncated` marks an incomplete reply (length limit, or stopped). */
bool review_set(ReviewView *v, const char *original, const char *replacement, const char *note, bool truncated);
/* Warning shown above the diff ("" if none), e.g. a proposal far shorter
 * than the passage. */
const char *review_warning(const ReviewView *v);
void review_clear(ReviewView *v);
bool review_active(const ReviewView *v);

/* Height the panel wants for `width`, capped at `max_h`. */
float review_height(ReviewView *v, float width, float max_h);

void review_update(ReviewView *v, Rectangle r);
void review_draw(ReviewView *v, Rectangle r);

Rectangle review_accept_rect(Rectangle r);
Rectangle review_reject_rect(Rectangle r);

/* Words removed / added, for tests and the status line. */
void review_counts(const ReviewView *v, int *removed, int *added);

#endif
