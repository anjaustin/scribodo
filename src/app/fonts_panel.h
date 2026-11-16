/* The Appearance panel (⌘, or Scribodo ▸ Appearance…): the palette, the writing aids, and a face and a size for each
 * font role of app/appearance.h. Changes show at
 * once; Done saves them, Cancel (or Esc) puts back what was saved. Choosing a face opens a list of the installed fonts
 * (app/font_catalog.h), filtered as the author types, with the bundled default first.
 *
 * Its elements, for UI scripts: FontsTheme<n> (0 Dark, 1 Light, 2 Paper), FontsTypewriter, FontsDim, FontsDone, FontsCancel, FontsDefaults, FontsPick<role>, FontsMinus<role>,
 * FontsPlus<role> and, in the list, FontsItem<n> (the n-th row on screen; before scrolling, row 0 is the default)
 * and FontsBack. */
#ifndef SCRIBODO_FONTS_PANEL_H
#define SCRIBODO_FONTS_PANEL_H

#include <stdbool.h>
#include <stddef.h>

#include "app/appearance.h"
#include "app/font_catalog.h"
#include "raylib.h"

#define FONTS_PANEL_ROWS 12 /* list rows on screen */

typedef struct {
    bool open;
    Appearance saved;      /* as on disk when the panel opened: Cancel restores it */
    FontCatalog catalog;
    bool scanned;
    int picking;           /* the role whose face is being chosen, or -1 */
    char filter[64];
    size_t *matches;       /* indexes into catalog.items */
    size_t nmatch;
    int sel;               /* the highlighted list row: 0 is the default, 1.. the matches */
    int top;               /* the first list row shown */
    Rectangle done, cancel, defaults, back;
    Rectangle pick[APPEARANCE_ROLE_COUNT], minus[APPEARANCE_ROLE_COUNT], plus[APPEARANCE_ROLE_COUNT];
    Rectangle item[FONTS_PANEL_ROWS];
    Rectangle theme[3], typewriter, dim; /* the palettes (THEME_COUNT), the writing aids */
    int nitem;
    int fit;               /* list rows that fit above the footer this frame */
    bool picked;           /* a row was picked this frame: its Enter or click must not reach the roles view */
} FontsPanel;

/* fonts_panel_open(App *) and fonts_panel_draw(App *, Rectangle screen) are declared in app_internal.h, which holds
 * the App this panel belongs to. */
Rectangle fonts_panel_element(const FontsPanel *p, const char *name);
void fonts_panel_destroy(FontsPanel *p);

#endif
