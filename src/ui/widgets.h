/* Small drawn controls in the theme (ui/theme.h), with text in the app's own fonts (so any letter the font chain
 * covers shows; raygui, which drew the controls until 2026-11-16, used one face and no fallback).
 *
 * A button is clicked when the mouse is pressed and released inside it, the rule raygui had, so a scripted
 * click (press frame, release frame) works. */
#ifndef SCRIBODO_WIDGETS_H
#define SCRIBODO_WIDGETS_H

#include <stdbool.h>

#include "raylib.h"
#include "ui/font.h"

typedef enum {
    WIDGET_QUIET,   /* text only; a soft fill under the pointer */
    WIDGET_OUTLINE, /* a thin outline */
    WIDGET_PRIMARY, /* filled with the accent */
} WidgetKind;

/* The font a control uses when given none (the interface font; set again whenever fonts reload). */
void widget_set_font(const SFont *f);
/* Once per frame before any control: a press is owned by the control it began on this frame, or by none. */
void widget_frame_begin(void);
/* While locked (a dialog, menu or prompt is open over the panes), controls draw but do not react, as raygui's
 * GuiLock did when it drew them. */
void widget_set_locked(bool locked);
bool widget_locked(void);

/* Draws the button; true on the frame it is clicked (never when disabled or locked). `f` NULL: the default font. */
bool widget_button(Rectangle r, const char *label, const SFont *f, WidgetKind kind, bool enabled);
/* A symbol beside a label (ui/icons.h). */
typedef struct {
    const Texture2D *texture; /* NULL: no icon */
    Vector2 size;             /* points */
} WidgetIcon;
/* A button with an icon before its label (`label` may be ""); `on` as for widget_toggle. */
bool widget_icon_button(Rectangle r, const WidgetIcon *icon, const char *label, const SFont *f, WidgetKind kind,
                        bool enabled, bool on);
/* A quiet button that stays filled while `on` (Observer, Recall). */
bool widget_toggle(Rectangle r, const char *label, const SFont *f, bool on);
/* A check box with its label to the right; flips *value and returns true on a click. */
bool widget_checkbox(Rectangle r, const char *label, const SFont *f, bool *value);

/* The click rule of widget_button without drawing: true on the release of a press that began inside `r`. */
bool widget_clicked(Rectangle r);

/* One line of text, cut with an ellipsis to fit `max_w` (0: no limit). Returns the width drawn. */
float widget_text(const SFont *f, const char *s, float x, float y, float max_w, Color c);

/* Text centred vertically in `r`, cut to its width. */
void widget_text_in(const SFont *f, const char *s, Rectangle r, Color c);

/* A rounded rectangle with radius in pixels (raylib's takes a fraction of the shorter side), and enough segments
 * that the corner stays round on a Retina screen. */
void widget_round_rect(Rectangle r, float radius, Color fill);
/* A soft shadow under a floating surface (a menu, a dialog): layered rounded rectangles in the palette's shadow
 * colour, fading outward and falling slightly below. Draw it before the surface. `depth` in px (8 for a menu, 16 for a
 * dialog). */
void widget_shadow(Rectangle r, float radius, float depth);
void widget_round_outline(Rectangle r, float radius, float thick, Color line);

#endif
