#include "recall_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raygui.h"
#include "ui/editor.h"
#include "ui/input.h"

#define PAD 8.0f
#define GAP 6.0f
#define INPUT_H 30.0f
#define BTN_W 72.0f
#define MAX_TEXT_LINES 8
#define MAX_COVERAGE_LINES 5

struct RecallView {
    SFont *text, *label;
    Editor *input;
    char *submitted;
    RecallResult result;
    bool has_result, ok, busy;
    float scroll;
    Rectangle r, input_rect, button_rect, list_rect;
    Rectangle *cards; /* as last drawn; zero when off screen */
    size_t ncards;
    long clicked; /* row pressed this frame, or -1 */
    char coverage[640];
};

RecallView *recall_view_create(SFont *text_font, SFont *label_font)
{
    RecallView *v = calloc(1, sizeof(RecallView));
    if (!v) return NULL;
    v->text = text_font;
    v->label = label_font;
    v->clicked = -1;
    v->input = editor_create(text_font);
    if (!v->input) {
        free(v);
        return NULL;
    }
    editor_set_scratch(v->input, "Search canon and original text\xE2\x80\xA6  (Enter)");
    editor_set_layout(v->input, 8.0f, 6.0f, 4000.0f);
    editor_set_single_line(v->input, true);
    editor_set_submit_on_enter(v->input, true);
    editor_set_max_chars(v->input, RECALL_QUERY_BYTES);
    return v;
}

void recall_view_destroy(RecallView *v)
{
    if (!v) return;
    editor_destroy(v->input);
    recall_result_free(&v->result);
    free(v->submitted);
    free(v->cards);
    free(v);
}

static void describe(RecallView *v)
{
    const RecallResult *r = &v->result;
    if (!v->has_result) {
        snprintf(v->coverage, sizeof(v->coverage), "%s", v->busy ? "Searching\xE2\x80\xA6" : "Exact committed canon and verified original excerpts. Lexical matching; no model.");
        return;
    }
    if (!v->ok) {
        snprintf(v->coverage, sizeof(v->coverage), "Recall failed: %s", r->limits[0] ? r->limits : "unknown error");
        return;
    }
    size_t canon = 0;
    for (size_t i = 0; i < r->count; i++) canon += r->rows[i].kind == RECALL_CANON;
    int n = snprintf(v->coverage, sizeof(v->coverage),
                     "Searched %zu committed canon entries (%zu retracted excluded): %zu matched. ", r->canon_committed,
                     r->canon_retracted, canon);
    if (r->excerpts_available)
        n += snprintf(v->coverage + n, sizeof(v->coverage) - (size_t)n,
                      "Searched original text in %zu of %zu conversations with memory on (%zu off, %zu still indexing): "
                      "%zu verified excerpts, %zu candidates rejected. ",
                      r->memory_on, r->conversations, r->memory_off, r->memory_indexing, r->count - canon, r->rejected);
    else
        n += snprintf(v->coverage + n, sizeof(v->coverage) - (size_t)n, "Original excerpts were not searched. ");
    if (r->count == 0 && n < (int)sizeof(v->coverage))
        n += snprintf(v->coverage + n, sizeof(v->coverage) - (size_t)n, "Nothing matched in what was searched. ");
    if (r->limits[0] && n < (int)sizeof(v->coverage))
        snprintf(v->coverage + n, sizeof(v->coverage) - (size_t)n, "Limits: %s.", r->limits);
}

void recall_view_set_result(RecallView *v, RecallResult *result, bool ok)
{
    if (!v) return;
    recall_result_free(&v->result);
    if (result) {
        v->result = *result;
        memset(result, 0, sizeof(*result));
    }
    v->has_result = result != NULL;
    v->ok = ok;
    v->busy = false;
    v->scroll = 0;
    free(v->cards);
    v->cards = calloc(v->result.count ? v->result.count : 1, sizeof(Rectangle));
    v->ncards = v->cards ? v->result.count : 0;
    describe(v);
}

void recall_view_clear(RecallView *v)
{
    if (!v) return;
    recall_view_set_result(v, NULL, true);
    v->busy = false;
    describe(v);
}

void recall_view_set_busy(RecallView *v, bool busy)
{
    if (!v) return;
    v->busy = busy;
    describe(v);
}

bool recall_view_busy(const RecallView *v) { return v && v->busy; }
const RecallResult *recall_view_result(const RecallView *v) { return v && v->has_result ? &v->result : NULL; }

/* Greedy word wrap: calls `line` for each wrapped line; returns the count. */
static int wrap(const SFont *f, const char *s, float w, int max_lines,
                void (*line)(void *ctx, const char *p, int n, int idx), void *ctx)
{
    int count = 0;
    const char *p = s;
    if (w < 8) w = 8;
    while (*p && count < max_lines) {
        while (*p == ' ') p++;
        const char *nl = strchr(p, '\n');
        int avail = nl ? (int)(nl - p) : (int)strlen(p);
        int take = avail, last_space = -1;
        for (int i = 0; i < avail; i++) {
            if (p[i] == ' ') last_space = i;
            if (sfont_measure_utf8(f, p, i + 1) > w) {
                take = last_space > 0 ? last_space : i;
                while (take > 0 && ((unsigned char)p[take] & 0xC0) == 0x80) take--;
                if (take == 0) take = i > 0 ? i : 1;
                break;
            }
        }
        bool last = count == max_lines - 1 && take < avail;
        if (line) line(ctx, p, take, last ? -1 - count : count);
        count++;
        p += take;
        if (*p == '\n') p++;
    }
    return count;
}

typedef struct {
    const SFont *f;
    float x, y, lh;
    Color c;
} LineCtx;

static void draw_line(void *ctx, const char *p, int n, int idx)
{
    LineCtx *l = ctx;
    int row = idx < 0 ? -1 - idx : idx;
    float y = l->y + (float)row * l->lh;
    sfont_draw_utf8(l->f, p, n, l->x, y, l->c);
    if (idx < 0) sfont_draw_utf8(l->f, "\xE2\x80\xA6", 3, l->x + sfont_measure_utf8(l->f, p, n), y, l->c);
}

static void badge_for(const RecallRow *row, char *out, size_t cap)
{
    if (row->kind == RECALL_CANON)
        snprintf(out, cap, "CANON \xC2\xB7 v%d \xC2\xB7 committed", row->ver);
    else if (row->kind == RECALL_CHAT_EXCERPT)
        snprintf(out, cap, "CHAT \xC2\xB7 %s \xC2\xB7 message %lld \xC2\xB7 %s", row->conversation_title,
                 (long long)row->sequence + 1, row->role == CHAT_USER ? "you" : "assistant");
    else
        snprintf(out, cap, "DOCUMENT \xC2\xB7 %s \xC2\xB7 %s revision", row->document_name,
                 row->current_saved ? "saved" : "superseded");
}

static float row_height(RecallView *v, size_t i, float w)
{
    const RecallRow *row = &v->result.rows[i];
    float inner = w - 2 * PAD;
    float h = PAD + v->label->line_height;
    h += (float)wrap(v->text, row->text ? row->text : "", inner, MAX_TEXT_LINES, NULL, NULL) * v->text->line_height;
    return h + PAD;
}

static void layout(RecallView *v, Rectangle r)
{
    v->r = r;
    v->input_rect = (Rectangle){ r.x, r.y, r.width - BTN_W - GAP, INPUT_H };
    v->button_rect = (Rectangle){ r.x + r.width - BTN_W, r.y, BTN_W, INPUT_H };
    float cov_h = (float)wrap(v->label, v->coverage, r.width, MAX_COVERAGE_LINES, NULL, NULL) * v->label->line_height;
    float top = r.y + INPUT_H + GAP + cov_h + GAP;
    v->list_rect = (Rectangle){ r.x, top, r.width, r.y + r.height - top };
    if (v->list_rect.height < 0) v->list_rect.height = 0;
}

void recall_view_update(RecallView *v, Rectangle r, bool focused)
{
    if (!v) return;
    layout(v, r);
    editor_update(v->input, v->input_rect, focused);
    if (focused && editor_take_submit(v->input)) {
        char *text = editor_text_utf8(v->input);
        bool blank = true;
        for (const char *p = text; p && *p; p++)
            if (*p != ' ' && *p != '\n' && *p != '\t') blank = false;
        free(v->submitted);
        v->submitted = blank ? NULL : text;
        if (blank) free(text);
    }
    v->clicked = -1;
    if (input_mouse_blocked()) return;
    Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, v->list_rect)) v->scroll -= GetMouseWheelMoveV().y * 30.0f;
    float total = 0;
    for (size_t i = 0; i < v->result.count; i++) total += row_height(v, i, r.width) + GAP;
    float max = total - v->list_rect.height > 0 ? total - v->list_rect.height : 0;
    if (v->scroll > max) v->scroll = max;
    if (v->scroll < 0) v->scroll = 0;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, v->list_rect))
        for (size_t i = 0; i < v->ncards && i < v->result.count; i++)
            if (v->cards[i].width > 0 && CheckCollisionPointRec(m, v->cards[i])) {
                v->clicked = (long)i;
                break;
            }
}

RecallAction recall_view_draw(RecallView *v, Rectangle r, bool focused)
{
    static const Color CARD = { 0x22, 0x24, 0x27, 0xFF }, HOVER = { 0x2A, 0x2D, 0x31, 0xFF };
    static const Color TEXT = { 0xD4, 0xD4, 0xD4, 0xFF }, MUTED = { 0x8A, 0x8A, 0x8A, 0xFF };
    static const Color CANON = { 0x8C, 0xE0, 0xA4, 0xFF }, CHAT = { 0x6C, 0xA8, 0xE0, 0xFF }, DOC = { 0xE6, 0xB4, 0x50, 0xFF };
    static const Color ERR = { 0xF2, 0x8B, 0x82, 0xFF };
    RecallAction act = { RECALL_ACTION_NONE, 0 };
    if (!v) return act;
    layout(v, r);
    editor_draw(v->input, v->input_rect, focused);
    if (v->busy) GuiDisable();
    if (GuiButton(v->button_rect, "Search") && !v->busy) {
        char *text = editor_text_utf8(v->input);
        bool blank = true;
        for (const char *p = text; p && *p; p++)
            if (*p != ' ' && *p != '\n' && *p != '\t') blank = false;
        free(v->submitted);
        v->submitted = blank ? NULL : text;
        if (blank) free(text);
    }
    GuiEnable();
    LineCtx cov = { v->label, r.x, r.y + INPUT_H + GAP, v->label->line_height, v->has_result && !v->ok ? ERR : MUTED };
    wrap(v->label, v->coverage, r.width, MAX_COVERAGE_LINES, draw_line, &cov);

    Rectangle list = v->list_rect;
    if (list.height <= 0) return act;
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float y = list.y - v->scroll;
    Vector2 m = GetMousePosition();
    for (size_t i = 0; i < v->result.count; i++) {
        const RecallRow *row = &v->result.rows[i];
        float h = row_height(v, i, r.width);
        Rectangle card = { list.x, y, list.width, h };
        bool visible = y + h >= list.y && y <= list.y + list.height;
        if (i < v->ncards) v->cards[i] = visible ? card : (Rectangle){ 0 };
        if (visible) {
            bool hover = !input_mouse_blocked() && CheckCollisionPointRec(m, list) && CheckCollisionPointRec(m, card);
            DrawRectangleRounded(card, 0.06f, 6, hover ? HOVER : CARD);
            Color edge = row->kind == RECALL_CANON ? CANON : row->kind == RECALL_CHAT_EXCERPT ? CHAT : DOC;
            DrawRectangleRec((Rectangle){ card.x, card.y + 4, 3, h - 8 }, edge);
            float x = card.x + PAD, cy = card.y + PAD, inner = card.width - 2 * PAD;
            char badge[400];
            badge_for(row, badge, sizeof(badge));
            sfont_draw_utf8(v->label, badge, (int)strlen(badge), x, cy, edge);
            cy += v->label->line_height;
            LineCtx lc = { v->text, x, cy, v->text->line_height, TEXT };
            wrap(v->text, row->text ? row->text : "", inner, MAX_TEXT_LINES, draw_line, &lc);
        }
        y += h + GAP;
    }
    EndScissorMode();
    if (v->clicked >= 0 && (size_t)v->clicked < v->result.count) {
        act.kind = RECALL_ACTION_OPEN;
        act.row = (size_t)v->clicked;
        v->clicked = -1;
    }
    return act;
}

char *recall_view_take_query(RecallView *v)
{
    if (!v) return NULL;
    char *q = v->submitted;
    v->submitted = NULL;
    return q;
}

Rectangle recall_view_input_rect(const RecallView *v) { return v ? v->input_rect : (Rectangle){ 0 }; }

Rectangle recall_view_element(const RecallView *v, const char *name)
{
    if (!v) return (Rectangle){ 0 };
    if (!strcmp(name, "RecallInput")) return v->input_rect;
    if (!strcmp(name, "RecallSearch")) return v->button_rect;
    if (!strncmp(name, "Recall", 6) && name[6] >= '0' && name[6] <= '9') {
        size_t i = (size_t)atoi(name + 6);
        return i < v->ncards ? v->cards[i] : (Rectangle){ 0 };
    }
    return (Rectangle){ 0 };
}

char *recall_view_text(const RecallView *v)
{
    if (!v) return NULL;
    char *body = v->has_result ? recall_result_text(&v->result) : strdup("(no result)\n");
    if (!body) return NULL;
    size_t n = strlen(body) + 32;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s\n%s", v->busy ? "busy" : "idle", body);
    free(body);
    return out;
}
