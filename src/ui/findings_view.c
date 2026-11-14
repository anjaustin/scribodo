#include "findings_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raygui.h"
#include "ui/input.h"

#define PAD 8.0f
#define GAP 6.0f
#define BTN_H 22.0f
#define MAX_QUOTE_LINES 4

typedef struct {
    char id[40];
    char type[24];
    char status[16];
    char *desc;
    char *quote;
    char source[300];
    char *related;      /* "Canon: …" / "Related note: …", or NULL */
    bool stale;         /* conflict whose canon entry changed or was retracted */
    float height;       /* as last laid out */
    float layout_w;
} Item;

struct FindingsView {
    SFont *text, *label;
    Item *items;
    size_t n;
    char selected[40];
    char press_sel[40];  /* the card that was selected when the mouse went down */
    Vector2 press_pos;
    bool pressed;        /* a press began inside the list */
    float scroll;
    Rectangle r;
    /* Cards and buttons as last drawn. */
    Rectangle *cards;
    Rectangle btn[5];
    FindingAction btn_act[5];
};

FindingsView *findings_view_create(SFont *text_font, SFont *label_font)
{
    FindingsView *v = calloc(1, sizeof(FindingsView));
    if (!v) return NULL;
    v->text = text_font;
    v->label = label_font;
    return v;
}

static void clear_items(FindingsView *v)
{
    for (size_t i = 0; i < v->n; i++) {
        free(v->items[i].desc);
        free(v->items[i].quote);
        free(v->items[i].related);
    }
    free(v->items);
    free(v->cards);
    v->items = NULL;
    v->cards = NULL;
    v->n = 0;
}

void findings_view_destroy(FindingsView *v)
{
    if (!v) return;
    clear_items(v);
    free(v);
}

static char *prefixed(const char *prefix, const char *s)
{
    size_t n = strlen(prefix) + strlen(s) + 1;
    char *o = malloc(n);
    if (o) snprintf(o, n, "%s%s", prefix, s);
    return o;
}

/* A conflict is current while its canon entry is still committed with the
 * same text it was judged against. */
static bool conflict_current(const RecordVersion *cur, const Record *canon, size_t ncanon)
{
    for (size_t c = 0; c < ncanon; c++) {
        if (strcmp(canon[c].id, cur->finding.related)) continue;
        const RecordVersion *cv = record_current(&canon[c]);
        return !strcmp(cv->status, "committed") && cur->finding.related_text && !strcmp(cv->content, cur->finding.related_text);
    }
    return false;
}

void findings_view_set(FindingsView *v, const Record *f, size_t n, const Record *canon, size_t ncanon)
{
    clear_items(v);
    v->items = calloc(n ? n : 1, sizeof(Item));
    v->cards = calloc(n ? n : 1, sizeof(Rectangle));
    if (!v->items || !v->cards) return;
    /* New conflicts first (they matter most), then newest first by when the
     * finding was made, so acting on one doesn't move it. */
    size_t *order = malloc((n ? n : 1) * sizeof(size_t));
    if (!order) return;
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t i = 1; i < n; i++) /* insertion sort, descending created_seq */
        for (size_t k = i; k > 0 && f[order[k - 1]].created_seq < f[order[k]].created_seq; k--) {
            size_t t = order[k];
            order[k] = order[k - 1];
            order[k - 1] = t;
        }
    for (int pass = 0; pass < 2; pass++)
        for (size_t oi = 0; oi < n; oi++) {
            size_t i = order[oi];
            const RecordVersion *cur = record_current(&f[i]);
            /* dismissed by the author, or filtered by the observer's rules */
            if (!strcmp(cur->status, "dismissed") || !strcmp(cur->status, "filtered")) continue;
            bool stale = !strcmp(cur->finding.type, "conflict") && !conflict_current(cur, canon, ncanon);
            bool first = !strcmp(cur->finding.type, "conflict") && !strcmp(cur->status, "new") && !stale;
            if ((pass == 0) != first) continue;
            Item *it = &v->items[v->n];
            snprintf(it->id, sizeof(it->id), "%s", f[i].id);
            it->stale = stale;
            snprintf(it->type, sizeof(it->type), "%s", cur->finding.type[0] ? cur->finding.type : "finding");
            snprintf(it->status, sizeof(it->status), "%s", cur->status);
            it->desc = strdup(cur->content);
            it->quote = strdup(cur->nsources && cur->sources[0].quote ? cur->sources[0].quote : "");
            if (cur->nsources) {
                const SourceRef *s = &cur->sources[0];
                if (s->kind == SRC_DOC) snprintf(it->source, sizeof(it->source), "%s", s->doc);
                else snprintf(it->source, sizeof(it->source), "chat, message %ld", s->message + 1);
            }
            if (cur->finding.related_text)
                it->related = prefixed(!strcmp(cur->finding.type, "conflict") ? "Canon: " : "Related note: ",
                                       cur->finding.related_text);
            it->layout_w = -1;
            if (!it->desc || !it->quote) {
                free(it->desc);
                free(it->quote);
                free(it->related);
                continue;
            }
            v->n++;
        }
    free(order);
}

size_t findings_view_count(const FindingsView *v) { return v->n; }

/* Greedy word wrap: calls `line` for each wrapped line; returns the count. */
static int wrap(const SFont *f, const char *s, float w, int max_lines,
                void (*line)(void *ctx, const char *p, int n, int idx), void *ctx)
{
    int count = 0;
    const char *p = s;
    while (*p && count < max_lines) {
        while (*p == ' ') p++;
        const char *nl = strchr(p, '\n');
        int avail = nl ? (int)(nl - p) : (int)strlen(p);
        int take = avail, last_space = -1;
        for (int i = 0; i < avail; i++) {
            if (p[i] == ' ') last_space = i;
            if (sfont_measure_utf8(f, p, i + 1) > w) {
                take = last_space > 0 ? last_space : i;
                while (take > 0 && ((unsigned char)p[take] & 0xC0) == 0x80) take--; /* UTF-8 boundary */
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

static bool is_selected(const FindingsView *v, size_t i) { return !strcmp(v->selected, v->items[i].id); }

static const char k_disclosure[] = "Possible conflicts are suggestions: about 1 in 13 was real on the test novels. Each links to its source.";

/* The disclosure's height (wrapped to the pane's width) when the list holds a conflict, else 0. */
static float disclosure_height(FindingsView *v, float w)
{
    for (size_t i = 0; i < v->n; i++)
        if (!strcmp(v->items[i].type, "conflict"))
            return (float)wrap(v->label, k_disclosure, w - 2 * PAD, 4, NULL, NULL) * v->label->line_height + 6.0f;
    return 0.0f;
}

static float item_height(FindingsView *v, size_t i, float w)
{
    Item *it = &v->items[i];
    float inner = w - 2 * PAD;
    float h = PAD + v->label->line_height; /* badge line */
    h += (float)wrap(v->text, it->desc, inner, 12, NULL, NULL) * v->text->line_height;
    if (is_selected(v, i)) {
        if (it->quote[0]) h += GAP + (float)wrap(v->label, it->quote, inner - 8, MAX_QUOTE_LINES, NULL, NULL) * v->label->line_height;
        if (it->related) h += GAP + (float)wrap(v->label, it->related, inner, 4, NULL, NULL) * v->label->line_height;
        h += GAP + v->label->line_height;    /* source line */
        h += GAP + BTN_H * 2 + 4;            /* two rows of buttons */
    }
    return h + PAD;
}

void findings_view_update(FindingsView *v, Rectangle r)
{
    v->r = r;
    if (input_mouse_blocked()) return;
    if (CheckCollisionPointRec(GetMousePosition(), r)) v->scroll -= GetMouseWheelMoveV().y * 30.0f;
    float total = 0;
    for (size_t i = 0; i < v->n; i++) total += item_height(v, i, r.width) + GAP;
    total += disclosure_height(v, r.width);
    float max = total - r.height > 0 ? total - r.height : 0;
    if (v->scroll > max) v->scroll = max;
    if (v->scroll < 0) v->scroll = 0;
    /* Select a card on click (buttons inside the selected card are raygui's). */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) v->pressed = false;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), r)) {
        Vector2 m = GetMousePosition();
        /* Buttons only act for a press that began on them, on a card that
         * was already selected: selecting a card re-lays out the list, and
         * the release must not land on a button that moved under the
         * cursor (red-team 005). */
        snprintf(v->press_sel, sizeof(v->press_sel), "%s", v->selected);
        v->press_pos = m;
        v->pressed = true;
        bool on_button = false;
        for (int b = 0; b < 5; b++) on_button = on_button || (v->btn[b].width > 0 && CheckCollisionPointRec(m, v->btn[b]));
        for (size_t i = 0; i < v->n && !on_button; i++)
            if (v->cards[i].width > 0 && CheckCollisionPointRec(m, v->cards[i]))
                snprintf(v->selected, sizeof(v->selected), "%s", v->items[i].id);
    }
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

static Color type_color(const char *t)
{
    if (!strcmp(t, "conflict")) return (Color){ 0xF2, 0x8B, 0x82, 0xFF };
    if (!strcmp(t, "question")) return (Color){ 0xE6, 0xB4, 0x50, 0xFF };
    if (!strcmp(t, "relationship")) return (Color){ 0xB4, 0x9C, 0xE8, 0xFF };
    if (!strcmp(t, "event")) return (Color){ 0x6C, 0xA8, 0xE0, 0xFF };
    return (Color){ 0x8C, 0xE0, 0xA4, 0xFF };
}

FindingAction findings_view_draw(FindingsView *v, Rectangle r, char *id_out, size_t id_len)
{
    static const Color CARD = { 0x22, 0x24, 0x27, 0xFF }, CARD_SEL = { 0x2A, 0x2D, 0x31, 0xFF };
    static const Color TEXT = { 0xD4, 0xD4, 0xD4, 0xFF }, MUTED = { 0x8A, 0x8A, 0x8A, 0xFF };
    static const Color QUOTE = { 0xB8, 0xB8, 0xB8, 0xFF }, EDGE = { 0x3D, 0x6F, 0xA8, 0xFF };
    v->r = r;
    for (int b = 0; b < 5; b++) v->btn[b] = (Rectangle){ 0 };
    FindingAction act = FA_NONE;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    float y = r.y - v->scroll;
    float dh = disclosure_height(v, r.width);
    if (dh > 0) {
        /* Measured and disclosed where the author decides (MEMORY-015 §4): the conflict path's precision on the test
         * novels, journal/observer-cycle2_quality.md step 4 (7 true in 92 shown), is 1 in 13. */
        LineCtx dc = { v->label, r.x + PAD, y, v->label->line_height, MUTED };
        wrap(v->label, k_disclosure, r.width - 2 * PAD, 4, draw_line, &dc);
        y += dh;
    }
    for (size_t i = 0; i < v->n; i++) {
        Item *it = &v->items[i];
        float h = item_height(v, i, r.width);
        Rectangle card = { r.x, y, r.width, h };
        v->cards[i] = (y + h >= r.y && y <= r.y + r.height) ? card : (Rectangle){ 0 };
        bool sel = is_selected(v, i);
        if (v->cards[i].width > 0) {
            DrawRectangleRounded(card, 0.06f, 6, sel ? CARD_SEL : CARD);
            if (sel) DrawRectangleRec((Rectangle){ card.x, card.y + 4, 3, h - 8 }, EDGE);
            float x = card.x + PAD, cy = card.y + PAD, inner = card.width - 2 * PAD;
            char badge[80];
            /* A conflict is the observer's suggestion, not a verdict: "POSSIBLE CONFLICT" (measured 7 true in 92 on the
             * test novels, journal/observer-cycle2_quality.md; the line above the list says so). */
            const char *kind = !strcmp(it->type, "conflict") ? "possible conflict" : it->type;
            int kn = snprintf(badge, sizeof(badge), "%s", kind);
            for (char *p = badge; *p; p++)
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            snprintf(badge + kn, sizeof(badge) - (size_t)kn, "%s%s%s", strcmp(it->status, "new") ? " \xC2\xB7 " : "",
                     strcmp(it->status, "new") ? it->status : "", it->stale ? " \xC2\xB7 outdated (canon changed)" : "");
            sfont_draw_utf8(v->label, badge, (int)strlen(badge), x, cy, type_color(it->type));
            cy += v->label->line_height;
            LineCtx lc = { v->text, x, cy, v->text->line_height, TEXT };
            cy += (float)wrap(v->text, it->desc, inner, 12, draw_line, &lc) * v->text->line_height;
            if (sel) {
                if (it->quote[0]) {
                    cy += GAP;
                    DrawRectangleRec((Rectangle){ x, cy, 2, v->label->line_height * (float)wrap(v->label, it->quote, inner - 8, MAX_QUOTE_LINES, NULL, NULL) }, MUTED);
                    LineCtx q = { v->label, x + 8, cy, v->label->line_height, QUOTE };
                    cy += (float)wrap(v->label, it->quote, inner - 8, MAX_QUOTE_LINES, draw_line, &q) * v->label->line_height;
                }
                if (it->related) {
                    cy += GAP;
                    LineCtx q = { v->label, x, cy, v->label->line_height, type_color(it->type) };
                    cy += (float)wrap(v->label, it->related, inner, 4, draw_line, &q) * v->label->line_height;
                }
                cy += GAP;
                char src[340];
                snprintf(src, sizeof(src), "Source: %s", it->source[0] ? it->source : "unknown");
                sfont_draw_utf8(v->label, src, (int)strlen(src), x, cy, MUTED);
                cy += v->label->line_height + GAP;
                float bw = (inner - 6) / 2;
                const char *labels[5] = { "Discuss", "Save as note", "Commit to canon", "Dismiss", "Source" };
                FindingAction acts[5] = { FA_DISCUSS, FA_NOTE, FA_CANON, FA_DISMISS, FA_SOURCE };
                Rectangle rects[5] = {
                    { x, cy, bw, BTN_H }, { x + bw + 6, cy, bw, BTN_H },
                    { x, cy + BTN_H + 4, bw, BTN_H }, { x + bw + 6, cy + BTN_H + 4, (bw - 6) / 2, BTN_H },
                    { x + bw + 6 + (bw - 6) / 2 + 6, cy + BTN_H + 4, (bw - 6) / 2, BTN_H },
                };
                bool done = strcmp(it->status, "new") != 0;
                for (int b = 0; b < 5; b++) {
                    v->btn[b] = rects[b];
                    v->btn_act[b] = acts[b];
                    /* Saved/committed findings keep Discuss and Source. */
                    bool off = done && (acts[b] == FA_NOTE || acts[b] == FA_CANON);
                    /* Only a fully visible button, clicked inside the list,
                     * whose press began on it (see findings_view_update). */
                    bool inside = rects[b].y >= r.y && rects[b].y + rects[b].height <= r.y + r.height;
                    bool armed = v->pressed && !strcmp(v->press_sel, it->id) &&
                                 CheckCollisionPointRec(v->press_pos, rects[b]) &&
                                 CheckCollisionPointRec(GetMousePosition(), r);
                    if (!inside) {
                        v->btn[b] = (Rectangle){ 0 };
                        continue;
                    }
                    if (off) GuiDisable();
                    if (GuiButton(rects[b], labels[b]) && !off && armed) {
                        act = acts[b];
                        snprintf(id_out, id_len, "%s", it->id);
                    }
                    GuiEnable();
                }
            }
        }
        y += h + GAP;
    }
    if (v->n == 0) sfont_draw_utf8(v->label, "No findings yet.", 16, r.x, r.y, MUTED);
    EndScissorMode();
    return act;
}

Rectangle findings_view_element(const FindingsView *v, const char *name)
{
    if (!strncmp(name, "Finding", 7) && name[7] >= '0' && name[7] <= '9') {
        size_t i = (size_t)atoi(name + 7);
        return i < v->n && v->cards ? v->cards[i] : (Rectangle){ 0 };
    }
    static const struct { const char *name; FindingAction a; } map[] = {
        { "FindingDiscuss", FA_DISCUSS }, { "FindingNote", FA_NOTE }, { "FindingCanon", FA_CANON },
        { "FindingDismiss", FA_DISMISS }, { "FindingSource", FA_SOURCE },
    };
    for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); k++)
        if (!strcmp(name, map[k].name))
            for (int b = 0; b < 5; b++)
                if (v->btn_act[b] == map[k].a && v->btn[b].width > 0) return v->btn[b];
    return (Rectangle){ 0 };
}

char *findings_view_text(const FindingsView *v)
{
    size_t cap = 1;
    for (size_t i = 0; i < v->n; i++)
        cap += strlen(v->items[i].desc) + (v->items[i].related ? strlen(v->items[i].related) : 0) +
               (v->items[i].quote ? strlen(v->items[i].quote) : 0) + 80;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t k = 0;
    s[0] = '\0';
    for (size_t i = 0; i < v->n; i++)
        /* type|status|description, then what it relates to and its evidence
         * (tests check provenance, not just that a finding exists). */
        k += (size_t)snprintf(s + k, cap - k, "%s|%s%s|%s|%s|quote: %s\n", v->items[i].type, v->items[i].status,
                              v->items[i].stale ? ",outdated" : "", v->items[i].desc,
                              v->items[i].related ? v->items[i].related : "", v->items[i].quote ? v->items[i].quote : "");
    return s;
}
