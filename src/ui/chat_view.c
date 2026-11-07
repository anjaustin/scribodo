#define _DARWIN_C_SOURCE
#include "chat_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/text_buffer.h"
#include "core/utf8.h"
#include "core/wrap.h"
#include "llm/llm_text.h"
#include "ui/input.h"

#define PAD 14.0f
#define BUBBLE_PAD 10.0f
#define GAP 14.0f
#define LABEL_H 18.0f
#define DISPLAY_BYTES 8192u
#define PAGE_HEADER 26.0f

typedef struct {
    TextBuffer buf;
    WrapLayout layout;
    size_t src_len;    /* byte length of the content the cache was built from */
    uint64_t src_hash;
    float width;
    float height;      /* text height */
    bool valid, markdown, preview;
} MsgCache;

struct ChatView {
    SFont *text;
    SFont *label;
    ChatSession *chat;
    Editor *input;
    MsgCache *cache;
    size_t ncache;
    size_t cache_capacity;
    float scroll;
    bool stick_to_end;
    uint64_t seen_version;
    bool copied;
    long apply_index;
    long record_index;   /* reply whose Note/Canon link was clicked */
    long reveal_index;   /* scroll to this message on the next update */
    long flash_index;    /* briefly outline this message (a source being shown) */
    double flash_t;
    int record_kind;     /* 0 note, 1 canon */
    Rectangle last_apply; /* the newest reply's links, as last drawn */
    bool can_apply;       /* a document is open to apply a reply to */
    char assistant_name[64]; /* the label on replies */
    char author_name[64];    /* speaker labels with this name are not shown */
    Rectangle last_note, last_canon;
    char *submitted;
    ChatPageAction page;
    Rectangle page_rect[4];
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
    v->apply_index = -1;
    v->can_apply = true;
    snprintf(v->assistant_name, sizeof(v->assistant_name), "Scribodo");
    v->record_index = -1;
    v->reveal_index = -1;
    v->flash_index = -1;
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

static void cache_free(MsgCache *c)
{
    tb_free(&c->buf);
    wrap_free(&c->layout);
    *c = (MsgCache){0};
}

void chat_view_history_changed(ChatView *v)
{
    for (size_t i = 0; i < v->ncache; i++) cache_free(&v->cache[i]);
    free(v->cache); v->cache = NULL; v->ncache = v->cache_capacity = 0;
    v->scroll = 0; v->stick_to_end = true;
    v->apply_index = v->record_index = v->reveal_index = v->flash_index = -1;
    v->page = (ChatPageAction){0};
}

size_t chat_view_cache_bytes(const ChatView *v)
{
    size_t bytes = v->cache_capacity * sizeof(MsgCache);
    for (size_t i = 0; i < v->ncache; i++)
        bytes += v->cache[i].buf.cap * sizeof(uint32_t) + v->cache[i].layout.cap * sizeof(WrapRow);
    return bytes;
}

ChatPageAction chat_view_take_page(ChatView *v) { ChatPageAction a = v->page; v->page = (ChatPageAction){0}; return a; }
Rectangle chat_view_page_rect(ChatView *v, ChatPageKind kind)
{
    return kind >= CHAT_PAGE_OLDER && kind <= CHAT_PAGE_LATEST ? v->page_rect[kind] : (Rectangle){0};
}

static uint64_t hash_str(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

/* Rebuild the wrap cache for message i if its text or width changed. */
/* `markdown`: a model reply, shown without its Markdown markers. */
static MsgCache *refresh(ChatView *v, size_t i, const char *content, bool markdown, float width)
{
    if (i >= v->ncache) {
        if (i >= SIZE_MAX / sizeof(MsgCache)) return NULL;
        MsgCache *c = realloc(v->cache, (i + 1) * sizeof(MsgCache));
        if (!c) return NULL;
        v->cache = c;
        for (size_t k = v->ncache; k <= i; k++) {
            memset(&v->cache[k], 0, sizeof(MsgCache));
            tb_init(&v->cache[k].buf);
        }
        v->ncache = i + 1;
        v->cache_capacity = i + 1;
    }
    MsgCache *c = &v->cache[i];
    char clipped[DISPLAY_BYTES + 1];
    size_t len = strnlen(content, DISPLAY_BYTES + 1);
    bool preview = len > DISPLAY_BYTES;
    if (preview) {
        len = DISPLAY_BYTES;
        while (len && ((unsigned char)content[len] & 0xC0) == 0x80) len--;
    }
    memcpy(clipped, content, len); clipped[len] = '\0';
    uint64_t h = hash_str(clipped, len);
    if (c->valid && c->src_len == len && c->src_hash == h && c->width == width && c->markdown == markdown && c->preview == preview) return c;
    /* Replies saved with speaker labels ("Tripp: ") are shown without them. */
    char *unlabelled = markdown ? llm_strip_speaker_labels(clipped, v->author_name, v->assistant_name) : NULL;
    char *shown = markdown ? llm_markdown_display(unlabelled ? unlabelled : clipped) : NULL;
    free(unlabelled);
    const char *text = shown ? shown : clipped;
    size_t tlen = strlen(text);
    uint32_t *cps = malloc((tlen + 1) * sizeof(uint32_t));
    if (!cps) {
        free(shown);
        cache_free(c);
        return NULL;
    }
    size_t n = utf8_decode((const uint8_t *)text, tlen, cps, NULL);
    free(shown);
    MsgCache staged = {0};
    tb_init(&staged.buf);
    bool ok = tb_set(&staged.buf, cps, n);
    free(cps);
    ok = ok && wrap_compute(&staged.layout, &staged.buf, width > 20 ? width : 20, adv, v->text) == 0;
    if (!ok) { cache_free(&staged); cache_free(c); return NULL; }
    cache_free(c);
    *c = staged;
    c->src_len = len;
    c->src_hash = h;
    c->width = width;
    c->height = (float)c->layout.count * v->text->line_height;
    c->valid = true; c->markdown = markdown; c->preview = preview;
    return c;
}

static float cached_height(ChatView *v, size_t i) { return i < v->ncache && v->cache[i].valid ? v->cache[i].height : LABEL_H; }
static bool footer(const ChatMessage *m) { return m->excerpt || (m->role == CHAT_ASSISTANT && !m->streaming && !m->error && m->content[0]); }

static float content_width(Rectangle list) { return list.width - 2 * PAD - 2 * BUBBLE_PAD; }

/* Total height of the message list for the current layout (lock held). */
static float total_height(ChatView *v, float width)
{
    float y = PAD;
    size_t n = chat_count(v->chat);
    for (size_t i = n; i < v->ncache; i++) cache_free(&v->cache[i]);
    if (n < v->ncache) {
        v->ncache = n;
        if (!n) { free(v->cache); v->cache = NULL; v->cache_capacity = 0; }
        else {
            MsgCache *smaller = realloc(v->cache, n * sizeof(MsgCache));
            if (smaller) { v->cache = smaller; v->cache_capacity = n; }
        }
    }
    for (size_t i = 0; i < n; i++) {
        const ChatMessage *m = chat_message(v->chat, i);
        refresh(v, i, m->content, m->role == CHAT_ASSISTANT && !m->is_edit, width);
        y += LABEL_H + cached_height(v, i) + 2 * BUBBLE_PAD + GAP;
        if (footer(m)) y += LABEL_H;
        if (i < v->ncache && v->cache[i].preview) y += LABEL_H;
    }
    return y;
}

void chat_view_update(ChatView *v, Rectangle list, Rectangle input, bool input_focused)
{
    chat_lock(v->chat);
    ChatWindowInfo info = chat_window_info(v->chat);
    chat_unlock(v->chat);
    memset(v->page_rect, 0, sizeof(v->page_rect));
    if (info.active) {
        for (int k = CHAT_PAGE_OLDER; k <= CHAT_PAGE_LATEST; k++) {
            Rectangle r = { list.x + PAD + (k - 1) * 72, list.y, 68, PAGE_HEADER };
            v->page_rect[k] = r;
            bool enabled = k == CHAT_PAGE_OLDER ? info.older : k == CHAT_PAGE_NEWER ? info.newer : true;
            if (enabled && !input_mouse_blocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), r))
                v->page = (ChatPageAction){ .kind = (ChatPageKind)k };
        }
        list.y += PAGE_HEADER; list.height -= PAGE_HEADER;
    }
    float width = content_width(list);
    uint64_t ver = chat_version(v->chat); /* takes the lock itself: read before locking */
    chat_lock(v->chat);
    float total = total_height(v, width);
    size_t n = chat_count(v->chat);

    Vector2 m = GetMousePosition();
    if (!input_mouse_blocked() && CheckCollisionPointRec(m, list)) {
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
                y += LABEL_H + cached_height(v, i) + 2 * BUBBLE_PAD;
                if (i < v->ncache && v->cache[i].preview) y += LABEL_H;
                bool copyable = footer(msg) && i < v->ncache && v->cache[i].valid;
                if (copyable) {
                    if (msg->excerpt) {
                        Rectangle prev = { list.x + PAD + 100, y, 80, LABEL_H }, next = { list.x + PAD + 190, y, 80, LABEL_H };
                        size_t cps = 0;
                        for (const unsigned char *p = (const unsigned char *)msg->content; *p; p++) cps += (*p & 0xC0) != 0x80;
                        if (msg->excerpt_start && CheckCollisionPointRec(m, prev))
                            v->page = (ChatPageAction){ CHAT_PAGE_PART, msg->sequence, msg->excerpt_start > 2048 ? msg->excerpt_start - 2048 : 0 };
                        if (msg->excerpt_more && CheckCollisionPointRec(m, next))
                            v->page = (ChatPageAction){ CHAT_PAGE_PART, msg->sequence, msg->excerpt_start + cps };
                        if (CheckCollisionPointRec(m, (Rectangle){list.x + PAD, y, 92, LABEL_H})) {
                            input_set_clipboard(msg->content); v->copied = true;
                        }
                        y += LABEL_H + GAP;
                        continue;
                    }
                    Rectangle copy = { list.x + PAD, y + 2, 40, LABEL_H };
                    float nx = v->can_apply ? 184 : 52;
                    Rectangle apply = v->can_apply ? (Rectangle){ list.x + PAD + 52, y + 2, 120, LABEL_H } : (Rectangle){ 0 };
                    bool can_record = !info.active || msg->sequence_known;
                    Rectangle note = can_record ? (Rectangle){ list.x + PAD + nx, y + 2, 40, LABEL_H } : (Rectangle){0};
                    Rectangle canon = can_record ? (Rectangle){ list.x + PAD + nx + 50, y + 2, 50, LABEL_H } : (Rectangle){0};
                    if (CheckCollisionPointRec(m, copy)) {
                        input_set_clipboard(msg->content);
                        v->copied = true;
                    } else if (CheckCollisionPointRec(m, apply)) {
                        v->apply_index = (long)i;
                    } else if (CheckCollisionPointRec(m, note) || CheckCollisionPointRec(m, canon)) {
                        v->record_index = (long)i;
                        v->record_kind = CheckCollisionPointRec(m, canon) ? 1 : 0;
                    }
                    y += LABEL_H;
                } else if (footer(msg)) y += LABEL_H;
                y += GAP;
            }
        }
    }
    float max_scroll = total - list.height;
    if (max_scroll < 0) max_scroll = 0;
    if (v->reveal_index >= 0 && (size_t)v->reveal_index < n) {
        float y = PAD;
        for (size_t i = 0; i < (size_t)v->reveal_index; i++) {
            const ChatMessage *msg = chat_message(v->chat, i);
            y += LABEL_H + cached_height(v, i) + 2 * BUBBLE_PAD + GAP;
            if (footer(msg)) y += LABEL_H;
            if (i < v->ncache && v->cache[i].preview) y += LABEL_H;
        }
        v->scroll = y - PAD;
        v->stick_to_end = false;
        v->seen_version = ver;
        v->reveal_index = -1;
    }
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

void chat_view_append_input(ChatView *v, const char *text)
{
    size_t n = editor_length(v->input);
    editor_set_selection(v->input, n, n);
    if (n > 0) {
        size_t len = strlen(text) + 3;
        char *t = malloc(len);
        if (!t) return;
        snprintf(t, len, "\n\n%s", text);
        editor_paste_utf8(v->input, t); /* undoable */
        free(t);
    } else {
        editor_paste_utf8(v->input, text);
    }
    v->stick_to_end = true;
}

void chat_view_clear_input(ChatView *v)
{
    editor_clear(v->input);
    v->stick_to_end = true;
}

Rectangle chat_view_last_apply_rect(const ChatView *v) { return v->last_apply; }

void chat_view_reveal(ChatView *v, long index)
{
    v->reveal_index = index;
    v->flash_index = index;
    v->flash_t = GetTime();
}
Rectangle chat_view_last_note_rect(const ChatView *v, bool canon) { return canon ? v->last_canon : v->last_note; }

long chat_view_take_record(ChatView *v, bool *canon)
{
    long i = v->record_index;
    *canon = v->record_kind == 1;
    v->record_index = -1;
    return i;
}

long chat_view_take_apply(ChatView *v)
{
    long i = v->apply_index;
    v->apply_index = -1;
    return i;
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
    chat_lock(v->chat);
    ChatWindowInfo info = chat_window_info(v->chat);
    chat_unlock(v->chat);
    if (info.active) {
        const char *labels[] = { "", "Older", "Newer", "Latest" };
        for (int k = CHAT_PAGE_OLDER; k <= CHAT_PAGE_LATEST; k++) {
            bool enabled = k == CHAT_PAGE_OLDER ? info.older : k == CHAT_PAGE_NEWER ? info.newer : true;
            sfont_draw_utf8(v->label, labels[k], (int)strlen(labels[k]), list.x + PAD + (k - 1) * 72, list.y + 3, enabled ? COL_LINK : COL_LABEL);
        }
        list.y += PAGE_HEADER; list.height -= PAGE_HEADER;
    }
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
    v->last_apply = v->last_note = v->last_canon = (Rectangle){ 0 };
    for (size_t i = 0; i < n; i++) {
        const ChatMessage *m = chat_message(v->chat, i);
        const MsgCache *c = refresh(v, i, m->content, m->role == CHAT_ASSISTANT && !m->is_edit, width);
        /* A message that went to (or came from) a remote model says so,
         * also when the chat is reopened later. */
        char who[480];
        if (m->via && m->via[0])
            snprintf(who, sizeof(who), m->role == CHAT_USER ? "You \xC2\xB7 sent to %s" : "%s", m->via);
        else snprintf(who, sizeof(who), "%s", m->role == CHAT_USER ? "You" : v->assistant_name);
        if (m->excerpt) {
            size_t used = strlen(who);
            snprintf(who + used, sizeof(who) - used, " · excerpt at character %zu", m->excerpt_start + 1);
        }
        sfont_draw_utf8(v->label, who, (int)strlen(who), list.x + PAD, y, m->via && m->via[0] ? (Color){ 0xE6, 0xB4, 0x50, 0xFF } : COL_LABEL);
        y += LABEL_H;
        float bh = (c ? c->height : LABEL_H) + 2 * BUBBLE_PAD;
        if ((long)i == v->flash_index && GetTime() - v->flash_t < 2.5)
            DrawRectangleLinesEx((Rectangle){ list.x + PAD - 3, y - 3, list.width - 2 * PAD + 6, bh + 6 }, 2.0f,
                                 (Color){ 0xE6, 0xB4, 0x50, 0xFF });
        if (m->role == CHAT_USER)
            DrawRectangleRounded((Rectangle){ list.x + PAD, y, list.width - 2 * PAD, bh }, 0.12f, 6, COL_USER_BG);
        if (c) draw_rows(v, c, list.x + PAD + BUBBLE_PAD, y + BUBBLE_PAD, m->error ? COL_ERROR : COL_TEXT, list);
        else sfont_draw_utf8(v->label, "Layout unavailable", 18, list.x + PAD, y, COL_ERROR);
        if (c && m->streaming && blink) {
            /* Streaming caret after the last character. */
            size_t rows = c->layout.count ? c->layout.count : 1;
            const WrapRow *last = c->layout.count ? &c->layout.rows[c->layout.count - 1] : NULL;
            float cx = list.x + PAD + BUBBLE_PAD + (last ? last->width : 0);
            float cy = y + BUBBLE_PAD + (float)(rows - 1) * v->text->line_height;
            DrawRectangleRec((Rectangle){ cx + 1, cy + 3, 7, v->text->line_height - 6 }, COL_LABEL);
        }
        y += bh;
        if (c && c->preview) {
            const char *preview = "Display preview; actions use the complete message";
            sfont_draw_utf8(v->label, preview, (int)strlen(preview), list.x + PAD, y, COL_LABEL);
            y += LABEL_H;
        }
        if (m->excerpt) {
            if (i == n - 1) v->last_apply = v->last_note = v->last_canon = (Rectangle){0};
            sfont_draw_utf8(v->label, "Copy excerpt", 12, list.x + PAD, y + 2, COL_LINK);
            if (m->excerpt_start) sfont_draw_utf8(v->label, "Earlier part", 12, list.x + PAD + 100, y + 2, COL_LINK);
            if (m->excerpt_more) sfont_draw_utf8(v->label, "Next part", 9, list.x + PAD + 190, y + 2, COL_LINK);
            y += LABEL_H;
        } else if (m->role == CHAT_ASSISTANT && !m->streaming && !m->error && m->content[0]) {
            sfont_draw_utf8(v->label, "Copy", 4, list.x + PAD, y + 2, COL_LINK);
            /* "Apply to document" only with a document to apply it to. */
            float nx = v->can_apply ? 184 : 52;
            if (v->can_apply) {
                const char *apply = "Apply to document";
                sfont_draw_utf8(v->label, apply, (int)strlen(apply), list.x + PAD + 52, y + 2, COL_LINK);
                v->last_apply = (Rectangle){ list.x + PAD + 52, y + 2, 120, LABEL_H };
            }
            if (!info.active || m->sequence_known) {
                sfont_draw_utf8(v->label, "Note", 4, list.x + PAD + nx, y + 2, COL_LINK);
                sfont_draw_utf8(v->label, "Canon", 5, list.x + PAD + nx + 50, y + 2, COL_LINK);
                v->last_note = (Rectangle){ list.x + PAD + nx, y + 2, 40, LABEL_H };
                v->last_canon = (Rectangle){ list.x + PAD + nx + 50, y + 2, 50, LABEL_H };
            } else if (i == n - 1) v->last_note = v->last_canon = (Rectangle){0};
            if (m->truncated || m->stopped) {
                const char *note = m->stopped ? "Stopped before the end" : "Reply stopped at the length limit";
                sfont_draw_utf8(v->label, note, (int)strlen(note), list.x + PAD + 296, y + 2, COL_LABEL);
            }
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

void chat_view_set_can_apply(ChatView *v, bool can) { v->can_apply = can; }

void chat_view_set_assistant_name(ChatView *v, const char *name)
{
    snprintf(v->assistant_name, sizeof(v->assistant_name), "%s", name && name[0] ? name : "Scribodo");
    for (size_t i = 0; i < v->ncache; i++) v->cache[i].src_len = (size_t)-1; /* re-lay out */
}

void chat_view_set_author_name(ChatView *v, const char *name)
{
    snprintf(v->author_name, sizeof(v->author_name), "%s", name ? name : "");
    for (size_t i = 0; i < v->ncache; i++) v->cache[i].src_len = (size_t)-1;
}
