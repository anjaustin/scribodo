/* Font faces with fallback and O(1) glyph lookup.
 *
 * raylib's GetGlyphIndex() is a linear scan over the glyph table, which is too
 * slow to call per character per frame. SFont builds a direct lookup table for
 * the Basic Multilingual Plane, plus a fallback chain of faces so that glyphs
 * missing from the primary face (e.g. Georgia) come from a broader face (e.g.
 * Arial Unicode). Codepoints not covered by any face render as a box (tofu).
 *
 * Faces are rasterised at size * DPI scale for crisp HiDPI rendering; all
 * metrics exposed here are in logical pixels.
 */
#ifndef SCRIBODO_FONT_H
#define SCRIBODO_FONT_H

#include <stdbool.h>
#include <stdint.h>

#include "raylib.h"

#define SFONT_MAX_FACES 3
#define SFONT_LUT_SIZE  0x10000 /* BMP */

typedef struct {
    Font faces[SFONT_MAX_FACES];
    float face_scale[SFONT_MAX_FACES]; /* logical size / face base size */
    int nfaces;
    float size;        /* logical pixel size (em height) */
    float line_height; /* logical pixels */
    float dpi;
    float tofu_advance;
    int32_t *lut;      /* cp -> (face << 24) | glyph index, or -1 */
    float *advance;    /* cp -> advance in logical px */
} SFont;

/* Load `npaths` candidate font files in fallback order. Paths that fail to
 * load are skipped. Returns false if no face loaded. */
bool sfont_load(SFont *f, const char *const *paths, int npaths, float size,
                float line_height_mult, float dpi);
void sfont_unload(SFont *f);

static inline float sfont_advance(const SFont *f, uint32_t cp)
{
    return cp < SFONT_LUT_SIZE ? f->advance[cp] : f->tofu_advance;
}

/* Draw one codepoint with its line box's top-left at (x, y). */
void sfont_draw(const SFont *f, uint32_t cp, float x, float y, Color color);

/* Measure / draw a UTF-8 string on a single line (no wrapping). */
float sfont_measure_utf8(const SFont *f, const char *s, int len);
void sfont_draw_utf8(const SFont *f, const char *s, int len, float x, float y, Color color);

#endif
