#include "chat_view.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/text_buffer.h"
#include "core/utf8.h"
#include "core/wrap.h"
#include "ui/input.h"

#define PAD 14.0f
#define BUBBLE_PAD 10.0f
#define GAP 14.0f
#define LABEL_H 18.0f

typedef struct {
    TextBuffer buf;
    WrapLayout layout;
    size_t src_len;    /* byte length of the content the cache was built from */
    uint64_t src_hash;
    float width;
    float height;      /* text height */
} MsgCache;

struct ChatView {
    SFont *text;
    SFont *label;
    ChatSession *chat;
    Editor *input;
    MsgCache *cache;
    size_t ncache;
    float scroll;
    bool stick_to_end;
    uint64_t seen_version;
    bool copied;
    char *submitted;
};

static float adv(uint32_t cp, void *ctx) { return sfont_advance((const SFont *)ctx, cp); }

ChatView *chat_view_create(SFont *text_font, SFont *label_font, ChatSession *chat)
{
    ChatView *v = calloc(1, sizeof(ChatView));
    if (!v) return NULL;
    v->text = text_font;
    v->label = label_font;
    v->chat = chat;
    v->stick_to_end = true;
    v->input = editor_create(text_font);
    if (!v->input) {
        free(v);
        return NULL;
    }
    editor_set_scratch(v->input, "Message Scribodo\xE2\x80\xA6  (Enter to send, Shift+Enter for a new line)");
    editor_set_layout(v->input, 10.0f, 8.0f, 4000.0f);
    editor_set_submit_on_enter(v->input, true);
    return v;
}

void chat_view_destroy(ChatView *v)
{
    if (!v) return;
    for (size_t i = 0; i < v->ncache; i++) {
        tb_free(&v->cache[i].buf);
        wrap_free(&v->cache[i].layout);
    }
    free(v->cache);
    editor_destroy(v->input);
    free(v->submitted);
    free(v);
}

Editor *chat_view_input(ChatView *v) { return v->input; }

float chat_view_input_height(ChatView *v, float width)
{
    (void)width;
    float lh = v->text->line_height;
    float h = editor_content_height(v->input) + 8.0f;
    if (h < 2 * lh + 16) h = 2 * lh + 16;
    if (h > 8 * lh + 16) h = 8 * lh + 16;
    return floorf(h);
}

void chat_view_scroll_to_end(ChatView *v) { v->stick_to_end = true; }

static uint64_t hash_str(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

/* Rebuild the wrap cache for message i if its text or width changed. */
static void refresh(ChatView *v, size_t i, const char *content, float width)
{
    if (i >= v->ncache) {
        MsgCache *c = realloc(v->cache, (i + 1) * sizeof(MsgCache));
        if (!c) return;
        v->cache = c;
        for (size_t k = v->ncache; k <= i; k++) {
            memset(&v->cache[k], 0, sizeof(MsgCache));
            tb_init(&v->cache[k].buf);
        }
        v->ncache = i + 1;
    }
    MsgCache *c = &v->cache[i];
    size_t len = strlen(content);
    uint64_t h = hash_str(content, len);
    if (c->src_len == len && c->src_hash == h && c->width == width && c->layout.count) return;
    uint32_t *cps = malloc((len + 1) * sizeof(uint32_t));
    if (!cps) return;
    size_t n = utf8_decode((const uint8_t *)content, len, cps, NULL);
    tb_set(&c->buf, cps, n);
    free(cps);
    wrap_compute(&c->layout, &c->buf, width > 20 ? width : 20, adv, v->text);
    c->src_len = len;
    c->src_hash = h;
    c->width = width;
    c->height = (float)c->layout.count * v->text->line_height;
}

static float content_width(Rectangle list) { return list.width - 2 * PAD - 2 * BUBBLE_PAD; }

/* Total height of the message list for the current layout (lock held). */
static float total_height(ChatView *v, float width)
{
    float y = PAD;
    size_t n = chat_count(v->chat);
    for (size_t i = 0; i < n; i++) {
        const ChatMessage *m = chat_message(v->chat, i);
        refresh(v, i, m->content, width);
        y += LABEL_H + v->cache[i].height + 2 * BUBBLE_PAD + GAP;
        if (m->role == CHAT_ASSISTANT && !m->streaming && !m->error && m->content[0]) y += LABEL_H;
    }
    return y;
}

void chat_view_update(ChatView *v, Rectangle list, Rectangle input, bool input_focused)
{
    float width = content_width(list);
    uint64_t ver = chat_version(v->chat); /* takes the lock itself: read before locking */
    chat_lock(v->chat);
    float total = total_height(v, width);
    size_t n = chat_count(v->chat);

    Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, list)) {
        float wheel = GetMouseWheelMoveV().y;
        if (wheel != 0.0f) {
            v->scroll -= wheel * v->text->line_height * 1.5f;
            v->stick_to_end = false;
        }
        /* "Copy" links under finished assistant replies. */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            float y = list.y + PAD - v->scroll;
            for (size_t i = 0; i < n; i++) {
                const ChatMessage *msg = chat_message(v->chat, i);
                y += LABEL_H + v->cache[i].height + 2 * BUBBLE_PAD;
                bool copyable = msg->role == CHAT_ASSISTANT && !msg->streaming && !msg->error && msg->content[0];
                if (copyable) {
                    Rectangle link = { list.x + PAD, y + 2, 60, LABEL_H };
                    if (CheckCollisionPointRec(m, link)) {
                        input_set_clipboard(msg->content);
                        v->copied = true;
                    }
                    y += LABEL_H;
                }
                y += GAP;
            }
        }
    }
    float max_scroll = total - list.height;
    if (max_scroll < 0) max_scroll = 0;
    if (ver != v->seen_version && v->stick_to_end) v->scroll = max_scroll;
    v->seen_version = ver;
    if (v->scroll >= max_scroll - 2) v->stick_to_end = true;
    if (v->scroll > max_scroll) v->scroll = max_scroll;
    if (v->scroll < 0) v->scroll = 0;
    chat_unlock(v->chat);

    editor_update(v->input, input, input_focused);
    if (input_focused && editor_take_submit(v->input)) {
        char *text = editor_text_utf8(v->input);
        /* Ignore whitespace-only submissions. */
        bool blank = true;
        for (const char *p = text; p && *p; p++)
            if (*p != ' ' && *p != '\n' && *p != '\t') blank = false;
        if (text && !blank) {
            free(v->submitted);
            v->submitted = text;
        } else {
            free(text);
        }
    }
}

char *chat_view_take_submit(ChatView *v)
{
    char *t = v->submitted;
    v->submitted = NULL;
    return t;
}

void chat_view_submit_now(ChatView *v)
{
    char *text = editor_text_utf8(v->input);
    bool blank = true;
    for (const char *p = text; p && *p; p++)
        if (*p != ' ' && *p != '\n' && *p != '\t') blank = false;
    free(v->submitted);
    v->submitted = blank ? NULL : text;
    if (blank) free(text);
}

void chat_view_clear_input(ChatView *v)
{
    editor_clear(v->input);
    v->stick_to_end = true;
}

bool chat_view_take_copied(ChatView *v)
{
    bool c = v->copied;
    v->copied = false;
    return c;
}

static const Color COL_TEXT = { 0xD4, 0xD4, 0xD4, 0xFF };
static const Color COL_LABEL = { 0x8B, 0x8B, 0x8B, 0xFF };
static const Color COL_USER_BG = { 0x2A, 0x2D, 0x32, 0xFF };
static const Color COL_ERROR = { 0xE5, 0x73, 0x73, 0xFF };
static const Color COL_LINK = { 0x4D, 0xA3, 0xFF, 0xFF };
static const Color COL_INPUT_BG = { 0x25, 0x25, 0x26, 0xFF };
static const Color COL_INPUT_BORDER = { 0x3C, 0x3C, 0x3C, 0xFF };
static const Color COL_FOCUS = { 0x00, 0x78, 0xD4, 0xFF };

static void draw_rows(ChatView *v, const MsgCache *c, float x, float y, Color col, Rectangle clip)
{
    float lh = v->text->line_height;
    for (size_t r = 0; r < c->layout.count; r++) {
        float ry = y + (float)r * lh;
        if (ry + lh < clip.y || ry > clip.y + clip.height) continue;
        const WrapRow *row = &c->layout.rows[r];
        float cx = x;
        for (size_t k = row->start; k < row->start + row->len; k++) {
            uint32_t cp = tb_get(&c->buf, k);
            sfont_draw(v->text, cp, cx, ry, col);
            cx += sfont_advance(v->text, cp);
        }
    }
}

void chat_view_draw(ChatView *v, Rectangle list, Rectangle input, bool input_focused)
{
    float width = content_width(list);
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    chat_lock(v->chat);
    size_t n = chat_count(v->chat);
    if (n == 0) {
        const char *hint = "Start a conversation: brainstorm, ask about your draft, or try a scene.";
        sfont_draw_utf8(v->label, hint, (int)strlen(hint), list.x + PAD, list.y + PAD, COL_LABEL);
    }
    float y = list.y + PAD - v->scroll;
    bool blink = fmod(GetTime(), 1.0) < 0.6;
    for (size_t i = 0; i < n; i++) {
        const ChatMessage *m = chat_message(v->chat, i);
        refresh(v, i, m->content, width);
        const MsgCache *c = &v->cache[i];
        const char *who = m->role == CHAT_USER ? "You" : "Scribodo";
        sfont_draw_utf8(v->label, who, (int)strlen(who), list.x + PAD, y, COL_LABEL);
        y += LABEL_H;
        float bh = c->height + 2 * BUBBLE_PAD;
        if (m->role == CHAT_USER)
            DrawRectangleRounded((Rectangle){ list.x + PAD, y, list.width - 2 * PAD, bh }, 0.12f, 6, COL_USER_BG);
        draw_rows(v, c, list.x + PAD + BUBBLE_PAD, y + BUBBLE_PAD, m->error ? COL_ERROR : COL_TEXT, list);
        if (m->streaming && blink) {
            /* Streaming caret after the last character. */
            size_t rows = c->layout.count ? c->layout.count : 1;
            const WrapRow *last = c->layout.count ? &c->layout.rows[c->layout.count - 1] : NULL;
            float cx = list.x + PAD + BUBBLE_PAD + (last ? last->width : 0);
            float cy = y + BUBBLE_PAD + (float)(rows - 1) * v->text->line_height;
            DrawRectangleRec((Rectangle){ cx + 1, cy + 3, 7, v->text->line_height - 6 }, COL_LABEL);
        }
        y += bh;
        if (m->role == CHAT_ASSISTANT && !m->streaming && !m->error && m->content[0]) {
            sfont_draw_utf8(v->label, "Copy", 4, list.x + PAD, y + 2, COL_LINK);
            y += LABEL_H;
        }
        y += GAP;
    }
    chat_unlock(v->chat);
    EndScissorMode();

    DrawRectangleRec(input, COL_INPUT_BG);
    DrawRectangleLinesEx(input, 1.0f, input_focused ? COL_FOCUS : COL_INPUT_BORDER);
    editor_draw(v->input, input, input_focused);
}
