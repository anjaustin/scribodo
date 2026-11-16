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
 *
 * A face can be any .ttf/.otf file or one face of a collection (.ttc), read
 * through core/font_file. A fallback face is scaled so its capital H matches
 * the primary face's and is set on the primary face's baseline, so a letter
 * the primary face lacks (the bundled Share has no ř) sits in the line at the
 * same height instead of jumping (assets/fonts/README.md).
 *
 * `version` changes on every load: a view that caches layout by font compares
 * it, so changing a font in the Fonts panel reflows every view.
 */
#ifndef SCRIBODO_FONT_H
#define SCRIBODO_FONT_H

#include <stdbool.h>
#include <stdint.h>

#include "raylib.h"

#define SFONT_MAX_FACES 4
#define SFONT_LUT_SIZE  0x10000 /* BMP */

typedef struct {
    const char *path;
    int index; /* the face within a collection; 0 for a single-face file */
} SFontSource;

typedef struct {
    Font faces[SFONT_MAX_FACES];
    float face_scale[SFONT_MAX_FACES]; /* logical size / face base size, times the cap-height match */
    float face_shift[SFONT_MAX_FACES]; /* logical px added to a glyph's top: the baseline match */
    unsigned version;  /* distinct per load; 0 for a font built by hand (tests) */
    int primary_source; /* which of the sources became faces[0] */
    int nfaces;
    float size;        /* logical pixel size (em height) */
    float line_height; /* logical pixels */
    float dpi;
    float tofu_advance;
    int32_t *lut;      /* cp -> (face << 24) | glyph index, or -1 */
    float *advance;    /* cp -> advance in logical px */
    char primary[200]; /* "Family Style" of faces[0] */
} SFont;

/* Load `n` candidate faces in fallback order, at most SFONT_MAX_FACES. A
 * source that fails to load (missing, not a font, a bad index) is skipped.
 * Returns false if no face loaded; `f` is then zeroed. */
bool sfont_load(SFont *f, const SFontSource *src, int n, float size,
                float line_height_mult, float dpi);
/* The family and style of the face that loaded first, for the Fonts panel
 * ("" when built by hand). */
const char *sfont_primary_name(const SFont *f);
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
