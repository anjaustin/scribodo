#include "font.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/font_file.h"
#include "core/utf8.h"

/* Codepoint ranges rasterised into each face's atlas. Covers Latin (incl.
 * Vietnamese), Greek, Cyrillic, combining marks, and the typographic
 * punctuation prose relies on (curly quotes, dashes, ellipsis, primes).
 * Everything else shows as tofu until we add a dynamic glyph cache. */
static const uint32_t k_ranges[][2] = {
    { 0x0020, 0x007E }, { 0x00A0, 0x024F }, { 0x02B0, 0x036F },
    { 0x0370, 0x03FF }, { 0x0400, 0x04FF }, { 0x1E00, 0x1EFF }, { 0x1F00, 0x1FFF },
    { 0x2000, 0x206F }, { 0x20A0, 0x20CF }, { 0x2100, 0x214F },
    { 0x2190, 0x21FF }, { 0x2200, 0x22FF }, /* arrows; mathematical operators: − × ≤ */
    { 0x2300, 0x23FF }, /* ⌘ ⌥ ⇧ and other UI symbols */
    { 0x25A0, 0x25FF }, /* geometric shapes: ● ○ ▸ */
    { 0x2600, 0x26FF }, /* misc symbols: ⚠ */
    { 0x2700, 0x27BF }, /* dingbats: ✎ ✓ ✗ */
    { 0x2E3A, 0x2E3B }, { 0xFFFD, 0xFFFD },
};

static int *build_codepoints(int *count)
{
    int n = 0;
    for (size_t r = 0; r < sizeof(k_ranges) / sizeof(k_ranges[0]); r++)
        n += (int)(k_ranges[r][1] - k_ranges[r][0] + 1);
    int *cps = malloc((size_t)n * sizeof(int));
    if (!cps) return NULL;
    int k = 0;
    for (size_t r = 0; r < sizeof(k_ranges) / sizeof(k_ranges[0]); r++)
        for (uint32_t c = k_ranges[r][0]; c <= k_ranges[r][1]; c++) cps[k++] = (int)c;
    *count = n;
    return cps;
}

static float glyph_advance(const Font *font, int gi, float scale)
{
    const GlyphInfo *g = &font->glyphs[gi];
    float a = g->advanceX ? (float)g->advanceX : font->recs[gi].width + (float)g->offsetX;
    return a * scale;
}

/* One face from a file or a collection: the bytes are checked and, for a collection, the face is copied out
 * standalone, because raylib's loader reads only a file's first face. */
static bool load_face(const SFontSource *s, int px, int *cps, int ncps, Font *out, char *name, size_t namelen)
{
    if (!s->path || !s->path[0] || !FileExists(s->path)) return false;
    int len = 0;
    unsigned char *data = LoadFileData(s->path, &len);
    if (!data || len <= 0) {
        if (data) UnloadFileData(data);
        return false;
    }
    bool ok = false;
    FontFaceName fn;
    size_t xn = 0;
    uint8_t *face = font_file_face_count(data, (size_t)len) > s->index
                        ? font_file_extract_face(data, (size_t)len, s->index, &xn) : NULL;
    if (face && xn <= INT_MAX) {
        const char *ext = font_file_face_name(face, xn, 0, &fn) ? ".ttf" : NULL;
        Font font = ext ? LoadFontFromMemory(ext, face, (int)xn, px, cps, ncps) : (Font){ 0 };
        if (IsFontValid(font) && font.glyphCount > 0) {
            *out = font;
            snprintf(name, namelen, "%s %s", fn.family, fn.style);
            ok = true;
        } else if (font.glyphs) {
            UnloadFont(font);
        }
    }
    free(face);
    UnloadFileData(data);
    return ok;
}

/* The face's capital H: its height and where it sits (its bottom is the baseline), in the face's own pixels. */
static bool cap_metrics(const Font *font, float *cap, float *base)
{
    for (int gi = 0; gi < font->glyphCount; gi++) {
        if (font->glyphs[gi].value != 'H') continue;
        if (font->recs[gi].height <= 0) return false;
        *cap = font->recs[gi].height;
        *base = (float)font->glyphs[gi].offsetY + font->recs[gi].height;
        return true;
    }
    return false;
}

static unsigned g_versions;

const char *sfont_primary_name(const SFont *f) { return f->primary; }

bool sfont_load(SFont *f, const SFontSource *src, int n, float size,
                float line_height_mult, float dpi)
{
    memset(f, 0, sizeof(*f));
    f->size = size;
    f->line_height = roundf(size * line_height_mult);
    f->dpi = dpi > 0 ? dpi : 1.0f;

    int ncps = 0;
    int *cps = build_codepoints(&ncps);
    if (!cps) return false;

    int px = (int)lroundf(size * f->dpi);
    for (int i = 0; i < n && f->nfaces < SFONT_MAX_FACES; i++) {
        Font font;
        char name[sizeof(f->primary)];
        if (!load_face(&src[i], px, cps, ncps, &font, name, sizeof(name))) continue;
        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        if (f->nfaces == 0) {
            snprintf(f->primary, sizeof(f->primary), "%s", name);
            f->primary_source = i;
        }
        f->faces[f->nfaces] = font;
        f->face_scale[f->nfaces] = size / (float)font.baseSize;
        f->nfaces++;
    }
    free(cps);
    if (f->nfaces == 0) {
        memset(f, 0, sizeof(*f));
        return false;
    }

    /* Fallback faces: the primary face's cap height, on its baseline. A face without an H keeps its own size and
     * the shared top (the ascent raylib already applied). */
    float cap0, base0;
    if (cap_metrics(&f->faces[0], &cap0, &base0)) {
        float base0_px = base0 * f->face_scale[0];
        for (int i = 1; i < f->nfaces; i++) {
            float cap, base;
            if (!cap_metrics(&f->faces[i], &cap, &base)) continue;
            float k = cap0 / cap;
            k = k < 0.75f ? 0.75f : k > 1.33f ? 1.33f : k;
            f->face_scale[i] *= k;
            f->face_shift[i] = base0_px - base * f->face_scale[i];
        }
    }

    f->lut = malloc(SFONT_LUT_SIZE * sizeof(int32_t));
    f->advance = calloc(SFONT_LUT_SIZE, sizeof(float));
    if (!f->lut || !f->advance) {
        sfont_unload(f);
        return false;
    }
    memset(f->lut, 0xFF, SFONT_LUT_SIZE * sizeof(int32_t));

    /* Earlier faces win: fill from the last fallback to the primary. */
    for (int fi = f->nfaces - 1; fi >= 0; fi--) {
        const Font *font = &f->faces[fi];
        for (int gi = 0; gi < font->glyphCount; gi++) {
            int cp = font->glyphs[gi].value;
            if (cp <= 0 || cp >= SFONT_LUT_SIZE) continue;
            f->lut[cp] = (fi << 24) | gi;
            f->advance[cp] = glyph_advance(font, gi, f->face_scale[fi]);
        }
    }

    float space = f->advance[' '] > 0 ? f->advance[' '] : size * 0.25f;
    f->tofu_advance = roundf(size * 0.6f);
    for (uint32_t cp = 0; cp < SFONT_LUT_SIZE; cp++) {
        if (f->lut[cp] >= 0) continue;
        if (cp < 0x20 || cp == 0x7F) f->advance[cp] = 0.0f;           /* controls */
        else if (cp >= 0x0300 && cp <= 0x036F) f->advance[cp] = 0.0f; /* combining */
        else f->advance[cp] = f->tofu_advance;
    }
    f->advance['\t'] = space * 4.0f;
    f->advance['\n'] = 0.0f;
    f->version = ++g_versions;
    return true;
}

void sfont_unload(SFont *f)
{
    for (int i = 0; i < f->nfaces; i++) UnloadFont(f->faces[i]);
    free(f->lut);
    free(f->advance);
    memset(f, 0, sizeof(*f));
}

static inline float snap(float v, float dpi) { return roundf(v * dpi) / dpi; }

void sfont_draw(const SFont *f, uint32_t cp, float x, float y, Color color)
{
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return;
    float top = y + (f->line_height - f->size) * 0.5f;

    int32_t e = cp < SFONT_LUT_SIZE ? f->lut[cp] : -1;
    if (e < 0) {
        if (sfont_advance(f, cp) <= 0.0f) return;
        /* Tofu: outlined box marking a glyph we cannot render yet. */
        Rectangle r = { snap(x + 1.5f, f->dpi), snap(top + f->size * 0.15f, f->dpi),
                        f->tofu_advance - 3.0f, f->size * 0.75f };
        DrawRectangleLinesEx(r, 1.0f, Fade(color, 0.6f));
        return;
    }

    const Font *font = &f->faces[e >> 24];
    int gi = e & 0xFFFFFF;
    float s = f->face_scale[e >> 24];
    float pad = (float)font->glyphPadding;
    const Rectangle *rec = &font->recs[gi];
    const GlyphInfo *g = &font->glyphs[gi];

    Rectangle src = { rec->x - pad, rec->y - pad, rec->width + 2.0f * pad, rec->height + 2.0f * pad };
    Rectangle dst = { snap(x + ((float)g->offsetX - pad) * s, f->dpi),
                      snap(top + f->face_shift[e >> 24] + ((float)g->offsetY - pad) * s, f->dpi),
                      src.width * s, src.height * s };
    DrawTexturePro(font->texture, src, dst, (Vector2){ 0, 0 }, 0.0f, color);
}

float sfont_measure_utf8(const SFont *f, const char *s, int len)
{
    float w = 0.0f;
    const uint8_t *p = (const uint8_t *)s;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i += utf8_decode_one(p + i, (size_t)(len - i), &cp);
        w += sfont_advance(f, cp);
    }
    return w;
}

void sfont_draw_utf8(const SFont *f, const char *s, int len, float x, float y, Color color)
{
    const uint8_t *p = (const uint8_t *)s;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i += utf8_decode_one(p + i, (size_t)(len - i), &cp);
        sfont_draw(f, cp, x, y, color);
        x += sfont_advance(f, cp);
    }
}
