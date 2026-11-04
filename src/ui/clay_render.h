/* Clay <-> raylib bridge.
 *
 * Adapted from Clay's reference raylib renderer
 * (third_party/clay/renderers/raylib, zlib license). Differences: text is
 * measured and drawn through SFont, which handles UTF-8 (the reference
 * renderer indexes glyphs by `byte - 32`); and custom elements are left to
 * the caller, which looks up their bounds with Clay_GetElementData().
 */
#ifndef SCRIBODO_CLAY_RENDER_H
#define SCRIBODO_CLAY_RENDER_H

#include <stdbool.h>

#include "clay.h"
#include "ui/font.h"

/* Text measurement callback for Clay_SetMeasureTextFunction. `user_data`
 * must point to an array of SFont* indexed by Clay fontId. */
Clay_Dimensions clay_measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *user_data);

void clay_render(Clay_RenderCommandArray commands, SFont *const *fonts);

/* True only if the element was declared in the most recent layout. Use it
 * before trusting Clay_GetElementData(), which also returns stale boxes. */
bool clay_element_current(Clay_ElementId id);

static inline Rectangle clay_to_rect(Clay_BoundingBox b)
{
    return (Rectangle){ b.x, b.y, b.width, b.height };
}

#endif
