/* Icons: SF Symbols, the Mac's own icon set, drawn by AppKit into textures at the screen's scale and tinted with the
 * palette's colours when drawn, so they sit beside the text the way they do in Apple's apps.
 *
 * Each icon is rendered once per size (the interface font's size, so icons grow with the text) as an alpha mask and
 * kept as a white texture; icon_draw tints it. A symbol the system does not have (an older macOS) is simply absent:
 * icon_size returns 0 and callers draw the label alone. */
#ifndef SCRIBODO_ICONS_H
#define SCRIBODO_ICONS_H

#include <stdbool.h>

#include "raylib.h"

typedef enum {
    ICON_OPEN,      /* folder */
    ICON_SAVE,      /* square.and.arrow.down */
    ICON_RECALL,    /* magnifyingglass */
    ICON_OBSERVER,  /* eye */
    ICON_NEW_CHAT,  /* square.and.pencil */
    ICON_DOCUMENTS, /* doc.text */
    ICON_CHATS,     /* bubble.left.and.bubble.right */
    ICON_HISTORY,   /* clock.arrow.circlepath */
    ICON_NOTES,     /* note.text */
    ICON_CANON,     /* book.closed */
    ICON_ENTITIES,  /* person.2 */
    ICON_COUNT
} IconId;

/* Renders every icon at `pt` points for a screen of scale `dpi` (again whenever the interface size changes). */
void icons_load(float pt, float dpi);
void icons_unload(void);
/* The icon's size in points (0 x 0 when absent). */
Vector2 icon_size(IconId id);
/* Draws the icon with its top-left at (x, y), in `tint`. */
void icon_draw(IconId id, float x, float y, Color tint);
/* The texture (for Clay image elements); NULL when absent. */
const Texture2D *icon_texture(IconId id);

#endif
