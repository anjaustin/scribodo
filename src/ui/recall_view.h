/* Recall panel (MEMORY-015 S1): a query box, a coverage line and the exact
 * rows a deterministic recall returned. Canon rows are the author's committed
 * record; excerpt rows are verified original text. The view never calls a
 * model or the store: the app runs the recall and hands over the result.
 * Clicking a row only reports it; the app opens the record or the source. */
#ifndef SCRIBODO_RECALL_VIEW_H
#define SCRIBODO_RECALL_VIEW_H

#include <stdbool.h>
#include <stddef.h>

#include "app/recall.h"
#include "raylib.h"
#include "ui/font.h"

typedef enum { RECALL_ACTION_NONE, RECALL_ACTION_OPEN } RecallActionKind;

typedef struct {
    RecallActionKind kind;
    size_t row;
} RecallAction;

typedef struct RecallView RecallView;

RecallView *recall_view_create(SFont *text_font, SFont *label_font);
void recall_view_destroy(RecallView *v);

/* Takes ownership of *result (zeroed afterwards). ok=false shows the limits as an error. */
void recall_view_set_result(RecallView *v, RecallResult *result, bool ok);
void recall_view_clear(RecallView *v);
void recall_view_set_busy(RecallView *v, bool busy);
bool recall_view_busy(const RecallView *v);
const RecallResult *recall_view_result(const RecallView *v);

void recall_view_update(RecallView *v, Rectangle r, bool focused);
RecallAction recall_view_draw(RecallView *v, Rectangle r, bool focused);

/* The query the author just submitted (owned), or NULL. */
char *recall_view_take_query(RecallView *v);
Rectangle recall_view_input_rect(const RecallView *v);

/* "RecallInput", "RecallSearch", "Recall0".. (for UI scripts). */
Rectangle recall_view_element(const RecallView *v, const char *name);
/* "busy" or "idle", then the deterministic result text (owned; tests). */
char *recall_view_text(const RecallView *v);

#endif
