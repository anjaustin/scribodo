#include "review_view.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#include "core/diff.h"
#include "core/text_buffer.h"
#include "core/utf8.h"
#include "core/wrap.h"

#define PAD 14.0f
#define HEADER_H 38.0f

enum { ST_EQUAL = 0, ST_DEL = 1, ST_INS = 2 };

struct ReviewView {
    SFont *text, *label;
    bool active;
    TextBuffer buf;      /* combined diff text */
    unsigned char *style;
    size_t style_cap;
    WrapLayout layout;
    float layout_w;
    float scroll;
    int removed, added;
    char header[256];
    char warning[200];
};

static float adv(uint32_t cp, void *ctx) { return sfont_advance((const SFont *)ctx, cp); }

ReviewView *review_create(SFont *text_font, SFont *label_font)
{
    ReviewView *v = calloc(1, sizeof(ReviewView));
    if (!v) return NULL;
    v->text = text_font;
    v->label = label_font;
    tb_init(&v->buf);
    return v;
}

void review_destroy(ReviewView *v)
{
    if (!v) return;
    tb_free(&v->buf);
    wrap_free(&v->layout);
    free(v->style);
    free(v);
}

void review_clear(ReviewView *v) { v->active = false; }
bool review_active(const ReviewView *v) { return v->active; }

void review_counts(const ReviewView *v, int *removed, int *added)
{
    *removed = v->removed;
    *added = v->added;
}

static int count_words(const char *s, size_t n)
{
    int w = 0;
    bool in = false;
    for (size_t i = 0; i < n; i++) {
        bool sp = s[i] == ' ' || s[i] == '\n' || s[i] == '\t';
        if (!sp && !in) w++;
        in = !sp;
    }
    return w;
}

/* Append UTF-8 bytes [p, p+n) to the combined buffer with one style. */
static bool add_span(ReviewView *v, const char *p, size_t n, unsigned char st)
{
    uint32_t *cps = malloc((n + 1) * sizeof(uint32_t));
    if (!cps) return false;
    size_t k = utf8_decode((const uint8_t *)p, n, cps, NULL);
    size_t at = tb_len(&v->buf);
    if (at + k > v->style_cap) {
        size_t cap = v->style_cap ? v->style_cap : 1024;
        while (cap < at + k) cap *= 2;
        unsigned char *s = realloc(v->style, cap);
        if (!s) {
            free(cps);
            return false;
        }
        v->style = s;
        v->style_cap = cap;
    }
    bool ok = tb_insert(&v->buf, at, cps, k);
    if (ok) memset(v->style + at, st, k);
    free(cps);
    return ok;
}

const char *review_warning(const ReviewView *v) { return v->warning; }

bool review_set(ReviewView *v, const char *original, const char *replacement, const char *note, bool truncated)
{
    Diff d;
    if (!diff_words(original, replacement, 0, &d)) return false;
    tb_set(&v->buf, NULL, 0);
    v->removed = v->added = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < d.count; i++) {
        const DiffOp *o = &d.ops[i];
        const char *src = o->kind == DIFF_INSERT ? replacement : original;
        unsigned char st = o->kind == DIFF_EQUAL ? ST_EQUAL : o->kind == DIFF_DELETE ? ST_DEL : ST_INS;
        ok = add_span(v, src + o->pos, o->len, st);
        if (o->kind == DIFF_DELETE) v->removed += count_words(src + o->pos, o->len);
        if (o->kind == DIFF_INSERT) v->added += count_words(src + o->pos, o->len);
    }
    diff_free(&d);
    if (!ok) return false;
    v->layout_w = -1;
    v->scroll = 0;
    if (v->removed == 0 && v->added == 0)
        snprintf(v->header, sizeof(v->header), "Proposed edit \xC2\xB7 no changes%s%s", note ? " \xC2\xB7 " : "",
                 note ? note : "");
    else
        snprintf(v->header, sizeof(v->header), "Proposed edit \xC2\xB7 %d word%s removed, %d added%s%s", v->removed,
                 v->removed == 1 ? "" : "s", v->added, note && note[0] ? " \xC2\xB7 " : "", note ? note : "");
    /* Warn about proposals that are probably not what the author asked for
     * (red-team 004: a long passage came back 95% shorter, cut mid-sentence;
     * a question came back as the passage plus commentary). */
    int orig_words = count_words(original, strlen(original));
    int new_words = count_words(replacement, strlen(replacement));
    v->warning[0] = '\0';
    if (truncated)
        snprintf(v->warning, sizeof(v->warning), "\xE2\x9A\xA0 The reply is incomplete (stopped, or at the length limit) \xE2\x80\x94 the end may be missing.");
    else if (orig_words >= 12 && new_words * 100 < orig_words * 50)
        snprintf(v->warning, sizeof(v->warning), "\xE2\x9A\xA0 The proposal is much shorter than the passage (%d of %d words). Check before accepting.",
                 new_words, orig_words);
    else if (orig_words >= 4 && new_words * 100 > orig_words * 250)
        snprintf(v->warning, sizeof(v->warning), "\xE2\x9A\xA0 The proposal is much longer than the passage (%d vs %d words) \xE2\x80\x94 it may include commentary.",
                 new_words, orig_words);
    v->active = true;
    return true;
}

static void relayout(ReviewView *v, float width)
{
    if (v->layout_w == width) return;
    wrap_compute(&v->layout, &v->buf, width > 40 ? width : 40, adv, v->text);
    v->layout_w = width;
}

float review_height(ReviewView *v, float width, float max_h)
{
    relayout(v, width - 2 * PAD);
    float h = HEADER_H + (v->warning[0] ? 22.0f : 0.0f) + (float)v->layout.count * v->text->line_height + PAD;
    return h < max_h ? h : max_h;
}

Rectangle review_accept_rect(Rectangle r) { return (Rectangle){ r.x + r.width - PAD - 168, r.y + 7, 80, 24 }; }
Rectangle review_reject_rect(Rectangle r) { return (Rectangle){ r.x + r.width - PAD - 80, r.y + 7, 80, 24 }; }

void review_update(ReviewView *v, Rectangle r)
{
    relayout(v, r.width - 2 * PAD);
    if (CheckCollisionPointRec(GetMousePosition(), r)) {
        float w = GetMouseWheelMoveV().y;
        v->scroll -= w * v->text->line_height * 1.5f;
    }
    float content = (float)v->layout.count * v->text->line_height;
    float view = r.height - HEADER_H - PAD;
    float max = content - view > 0 ? content - view : 0;
    if (v->scroll > max) v->scroll = max;
    if (v->scroll < 0) v->scroll = 0;
}

void review_draw(ReviewView *v, Rectangle r)
{
    static const Color BG = { 0x23, 0x25, 0x29, 0xFF }, BORDER = { 0x3D, 0x6F, 0xA8, 0xFF };
    static const Color TEXT = { 0xD4, 0xD4, 0xD4, 0xFF }, DEL = { 0xF2, 0x8B, 0x82, 0xFF };
    static const Color INS = { 0x8C, 0xE0, 0xA4, 0xFF }, INS_BG = { 0x1F, 0x3A, 0x2A, 0xFF };
    static const Color HEAD = { 0xCC, 0xCC, 0xCC, 0xFF };
    DrawRectangleRec(r, BG);
    DrawRectangleLinesEx(r, 1.0f, BORDER);
    /* Header: clipped to the space left of the buttons, with an ellipsis. */
    float max_w = review_accept_rect(r).x - (r.x + PAD) - 12;
    int len = (int)strlen(v->header);
    if (sfont_measure_utf8(v->label, v->header, len) > max_w) {
        float ell = sfont_measure_utf8(v->label, "\xE2\x80\xA6", 3);
        while (len > 0 && sfont_measure_utf8(v->label, v->header, len) + ell > max_w) {
            len--;
            while (len > 0 && ((unsigned char)v->header[len] & 0xC0) == 0x80) len--; /* UTF-8 boundary */
        }
        char clipped[300];
        snprintf(clipped, sizeof(clipped), "%.*s\xE2\x80\xA6", len, v->header);
        sfont_draw_utf8(v->label, clipped, (int)strlen(clipped), r.x + PAD, r.y + 11, HEAD);
    } else {
        sfont_draw_utf8(v->label, v->header, len, r.x + PAD, r.y + 11, HEAD);
    }

    float warn_h = 0;
    if (v->warning[0]) {
        static const Color WARN = { 0xE6, 0xB4, 0x50, 0xFF };
        sfont_draw_utf8(v->label, v->warning, (int)strlen(v->warning), r.x + PAD, r.y + HEADER_H - 4, WARN);
        warn_h = 22.0f;
    }
    relayout(v, r.width - 2 * PAD);
    Rectangle body = { r.x, r.y + HEADER_H + warn_h, r.width, r.height - HEADER_H - warn_h };
    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
    float lh = v->text->line_height;
    for (size_t row = 0; row < v->layout.count; row++) {
        float y = body.y + (float)row * lh - v->scroll;
        if (y + lh < body.y || y > body.y + body.height) continue;
        const WrapRow *w = &v->layout.rows[row];
        float x = r.x + PAD;
        for (size_t i = w->start; i < w->start + w->len; i++) {
            uint32_t cp = tb_get(&v->buf, i);
            float a = sfont_advance(v->text, cp);
            unsigned char st = v->style[i];
            if (st == ST_INS) DrawRectangleRec((Rectangle){ x, y + 2, a, lh - 4 }, INS_BG);
            if (cp == '\n' && st != ST_EQUAL) {
                /* A changed line break is otherwise invisible: show a pilcrow. */
                sfont_draw(v->text, 0x00B6, x, y, st == ST_DEL ? DEL : INS);
                if (st == ST_DEL) DrawRectangleRec((Rectangle){ x, y + lh * 0.52f, sfont_advance(v->text, 0x00B6), 1.5f }, DEL);
            } else {
                sfont_draw(v->text, cp, x, y, st == ST_DEL ? DEL : st == ST_INS ? INS : TEXT);
                if (st == ST_DEL) DrawRectangleRec((Rectangle){ x, y + lh * 0.52f, a, 1.5f }, DEL);
            }
            x += a;
        }
    }
    EndScissorMode();
}
