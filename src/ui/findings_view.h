/* Observer sidebar: the findings list. Each card shows the finding's type
 * and description; the selected card also shows its evidence, source and
 * any related canon entry or note, with the author's actions: Discuss,
 * Note, Canon, Dismiss (and Source). The view only reports actions; the app
 * carries them out. Dismissed findings are not listed.
 */
#ifndef SCRIBODO_FINDINGS_VIEW_H
#define SCRIBODO_FINDINGS_VIEW_H

#include <stdbool.h>

#include "app/records.h"
#include "raylib.h"
#include "ui/font.h"

typedef enum { FA_NONE, FA_DISCUSS, FA_NOTE, FA_CANON, FA_DISMISS, FA_SOURCE } FindingAction;

typedef struct FindingsView FindingsView;

FindingsView *findings_view_create(SFont *text_font, SFont *label_font);
void findings_view_destroy(FindingsView *v);

/* Replace the list (copied). Keeps the selection if it is still listed. */
/* `canon` lets conflicts whose canon entry has since changed or been
 * retracted be shown as outdated (and not listed first). */
void findings_view_set(FindingsView *v, const Record *findings, size_t n, const Record *canon, size_t ncanon);
size_t findings_view_count(const FindingsView *v);

void findings_view_update(FindingsView *v, Rectangle r);
/* Draws; returns the action clicked this frame and its finding's id. */
FindingAction findings_view_draw(FindingsView *v, Rectangle r, char *id_out, size_t id_len);

/* "Finding0".., "FindingDiscuss", "FindingNote", "FindingCanon",
 * "FindingDismiss", "FindingSource" (for UI scripts). */
Rectangle findings_view_element(const FindingsView *v, const char *name);
/* Every listed finding as "type|status|description\n" (malloc'd; tests). */
char *findings_view_text(const FindingsView *v);

#endif
