#include "record_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raygui.h"
#include "ui/input.h"
#include "ui/editor.h"

#define PAD 12.0f
#define HEADER_H 36.0f
#define LINE_H 22.0f
#define MAX_HISTORY 6

typedef struct {
    int ver;
    time_t ts;
    char status[16];
    char *content;
} HistItem;

struct RecordView {
    SFont *text, *label;
    Editor *ed;
    bool active;
    RecordKind kind;
    char id[40];
    char status[16];
    int ver;
    char *saved;          /* content as last stored (or the draft's start) */
    uint64_t saved_cid;   /* editor content id at that point */
    SourceRef *sources;
    size_t nsources;
    HistItem *hist;       /* older versions, newest first */
    size_t nhist;
    char header[200];
    char source_line[300];
    /* Layout, as last computed. */
    Rectangle r, ed_rect, src_rect, hist_rect[MAX_HISTORY];
    Rectangle btn[4];
    RecordAction btn_action[4];
    const char *btn_label[4];
    int nbtn;
};

RecordView *record_view_create(SFont *text_font, SFont *label_font)
{
    RecordView *v = calloc(1, sizeof(RecordView));
    if (!v) return NULL;
    v->text = text_font;
    v->label = label_font;
    v->ed = editor_create(text_font);
    if (!v->ed) {
        free(v);
        return NULL;
    }
    editor_set_scratch(v->ed, "Write the note\xE2\x80\xA6");
    editor_set_layout(v->ed, 8.0f, 6.0f, 4000.0f);
    return v;
}

static void clear_state(RecordView *v)
{
    free(v->saved);
    v->saved = NULL;
    records_free_sources(v->sources, v->nsources);
    v->sources = NULL;
    v->nsources = 0;
    for (size_t i = 0; i < v->nhist; i++) free(v->hist[i].content);
    free(v->hist);
    v->hist = NULL;
    v->nhist = 0;
    v->id[0] = '\0';
    v->ver = 0;
}

void record_view_destroy(RecordView *v)
{
    if (!v) return;
    clear_state(v);
    editor_destroy(v->ed);
    free(v);
}

static void clip_utf8(char *s, size_t max_bytes)
{
    if (strlen(s) <= max_bytes) return;
    size_t n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    snprintf(s + n, 4, "\xE2\x80\xA6");
}

static void flatten(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '\n' || *p == '\t' || *p == '\r') *p = ' ';
}

static void refresh_text(RecordView *v)
{
    if (v->kind == REC_NOTE && !v->id[0])
        snprintf(v->header, sizeof(v->header), "New note");
    else if (v->kind == REC_NOTE)
        snprintf(v->header, sizeof(v->header), "Note \xC2\xB7 v%d", v->ver);
    else if (!v->id[0])
        snprintf(v->header, sizeof(v->header), "Canon draft \xE2\x80\x94 state it as a fact, then Commit");
    else
        snprintf(v->header, sizeof(v->header), "Canon \xC2\xB7 v%d \xC2\xB7 %s", v->ver, v->status);
    if (v->nsources == 0) {
        snprintf(v->source_line, sizeof(v->source_line), "No source");
        return;
    }
    const SourceRef *s = &v->sources[0];
    char quote[120];
    snprintf(quote, sizeof(quote), "%s", s->quote ? s->quote : "");
    flatten(quote);
    clip_utf8(quote, 60);
    if (s->kind == SRC_DOC)
        snprintf(v->source_line, sizeof(v->source_line), "Source: %s \xE2\x80\x94 \xE2\x80\x9C%s\xE2\x80\x9D", s->doc, quote);
    else
        snprintf(v->source_line, sizeof(v->source_line), "Source: chat reply %ld \xE2\x80\x94 \xE2\x80\x9C%s\xE2\x80\x9D", s->message + 1,
                 quote);
}

bool record_view_open(RecordView *v, RecordKind kind, const Record *rec, const char *content, const SourceRef *sources,
                      size_t nsources)
{
    clear_state(v);
    v->kind = kind;
    const char *text = content ? content : "";
    if (rec && rec->nversions) {
        const RecordVersion *cur = record_current(rec);
        snprintf(v->id, sizeof(v->id), "%s", rec->id);
        snprintf(v->status, sizeof(v->status), "%s", cur->status);
        v->ver = cur->ver;
        text = cur->content;
        sources = cur->sources;
        nsources = cur->nsources;
        if (rec->nversions > 1) {
            v->hist = calloc(rec->nversions - 1, sizeof(HistItem));
            for (size_t i = 0; v->hist && i + 1 < rec->nversions; i++) {
                const RecordVersion *o = &rec->versions[rec->nversions - 2 - i];
                HistItem *h = &v->hist[v->nhist];
                h->ver = o->ver;
                h->ts = o->ts;
                snprintf(h->status, sizeof(h->status), "%s", o->status);
                h->content = strdup(o->content);
                if (h->content) v->nhist++;
            }
        }
    } else {
        snprintf(v->status, sizeof(v->status), "draft");
    }
    v->saved = strdup(text);
    v->sources = records_copy_sources(sources, nsources);
    v->nsources = v->sources ? nsources : 0;
    if (!v->saved || !editor_set_text_utf8(v->ed, text)) {
        clear_state(v);
        return false;
    }
    editor_set_placeholder(v->ed, kind == REC_NOTE ? "Write the note\xE2\x80\xA6" : "State the fact\xE2\x80\xA6");
    v->saved_cid = editor_content_id(v->ed);
    size_t end = editor_length(v->ed);
    editor_set_selection(v->ed, end, end);
    refresh_text(v);
    v->active = true;
    return true;
}

void record_view_close(RecordView *v)
{
    clear_state(v);
    v->active = false;
}

bool record_view_active(const RecordView *v) { return v->active; }
RecordKind record_view_kind(const RecordView *v) { return v->kind; }
const char *record_view_id(const RecordView *v) { return v->id; }
const char *record_view_status(const RecordView *v) { return v->status; }
const char *record_view_header(const RecordView *v) { return v->header; }
Rectangle record_view_editor_rect(const RecordView *v) { return v->ed_rect; }

const SourceRef *record_view_sources(const RecordView *v, size_t *n)
{
    *n = v->nsources;
    return v->sources;
}

char *record_view_text(const RecordView *v) { return editor_text_utf8(v->ed); }

bool record_view_dirty(const RecordView *v)
{
    if (editor_content_id(v->ed) == v->saved_cid) return false;
    char *t = editor_text_utf8(v->ed);
    bool d = !t || strcmp(t, v->saved) != 0;
    free(t);
    return d;
}

bool record_view_stored(RecordView *v, const char *id, int ver, const char *status, const char *content)
{
    /* Ids carry their kind ("n-", "c-"); never let a note's id become the
     * panel's canon entry or the reverse (red-team 005). */
    char want = v->kind == REC_NOTE ? 'n' : v->kind == REC_CANON ? 'c' : 'f';
    if (!v->active || id[0] != want || (v->id[0] && strcmp(v->id, id) != 0)) return false;
    /* The version that was current becomes history (canon only lists it). */
    if (v->id[0] && v->ver > 0) {
        HistItem *h = realloc(v->hist, (v->nhist + 1) * sizeof(HistItem));
        if (h) {
            v->hist = h;
            memmove(&v->hist[1], &v->hist[0], v->nhist * sizeof(HistItem));
            v->hist[0] = (HistItem){ .ver = v->ver, .ts = time(NULL), .content = strdup(v->saved) };
            snprintf(v->hist[0].status, sizeof(v->hist[0].status), "%s", v->status);
            if (v->hist[0].content) v->nhist++;
            else memmove(&v->hist[0], &v->hist[1], v->nhist * sizeof(HistItem));
        }
    }
    snprintf(v->id, sizeof(v->id), "%s", id);
    snprintf(v->status, sizeof(v->status), "%s", status);
    v->ver = ver;
    char *s = strdup(content);
    if (s) {
        free(v->saved);
        v->saved = s;
    }
    v->saved_cid = editor_content_id(v->ed);
    refresh_text(v);
    return true;
}

static void layout(RecordView *v, Rectangle r)
{
    v->r = r;
    v->nbtn = 0;
#define BTN(lbl, act) (v->btn_label[v->nbtn] = (lbl), v->btn_action[v->nbtn++] = (act))
    if (v->kind == REC_NOTE) {
        BTN("Save", RA_SAVE);
        BTN("To canon", RA_TO_CANON);
        BTN("Delete", RA_DELETE);
        BTN("Close", RA_CLOSE);
    } else if (!v->id[0]) {
        BTN("Commit", RA_COMMIT);
        BTN("Discard", RA_CLOSE);
    } else if (!strcmp(v->status, "retracted")) {
        BTN("Recommit", RA_RECOMMIT);
        BTN("Close", RA_CLOSE);
    } else {
        BTN("Revise", RA_REVISE);
        BTN("Retract", RA_RETRACT);
        BTN("Close", RA_CLOSE);
    }
#undef BTN
    float x = r.x + r.width - PAD;
    for (int i = v->nbtn - 1; i >= 0; i--) {
        x -= 76;
        v->btn[i] = (Rectangle){ x, r.y + 6, 72, 24 };
    }
    size_t nh = v->kind == REC_CANON ? (v->nhist < MAX_HISTORY ? v->nhist : MAX_HISTORY) : 0;
    float bottom = r.y + r.height - PAD - (float)nh * LINE_H - LINE_H;
    v->ed_rect = (Rectangle){ r.x + PAD, r.y + HEADER_H, r.width - 2 * PAD, bottom - (r.y + HEADER_H) - 4 };
    v->src_rect = (Rectangle){ r.x + PAD, bottom, r.width - 2 * PAD, LINE_H };
    for (size_t i = 0; i < MAX_HISTORY; i++)
        v->hist_rect[i] = i < nh ? (Rectangle){ r.x + PAD, bottom + LINE_H * (float)(i + 1), r.width - 2 * PAD, LINE_H }
                                 : (Rectangle){ 0 };
}

float record_view_height(RecordView *v, float width, float max_h)
{
    (void)width;
    size_t nh = v->kind == REC_CANON ? (v->nhist < MAX_HISTORY ? v->nhist : MAX_HISTORY) : 0;
    float lines = editor_content_height(v->ed);
    if (lines < 3 * v->text->line_height) lines = 3 * v->text->line_height;
    if (lines > 8 * v->text->line_height) lines = 8 * v->text->line_height;
    float h = HEADER_H + lines + 8 + LINE_H + (float)nh * LINE_H + PAD;
    return h < max_h ? h : max_h;
}

void record_view_update(RecordView *v, Rectangle r, bool focused)
{
    layout(v, r);
    editor_update(v->ed, v->ed_rect, focused);
    /* A history line loads that wording into the editor; Revise stores it. */
    if (!input_mouse_blocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Vector2 m = GetMousePosition();
        for (size_t i = 0; i < MAX_HISTORY && i < v->nhist; i++)
            if (v->hist_rect[i].width > 0 && CheckCollisionPointRec(m, v->hist_rect[i])) {
                size_t n = editor_length(v->ed);
                editor_replace_range_utf8(v->ed, 0, n, v->hist[i].content); /* undoable */
            }
    }
}

RecordAction record_view_draw(RecordView *v, Rectangle r, bool focused)
{
    static const Color BG = { 0x23, 0x25, 0x29, 0xFF }, BORDER = { 0xB0, 0x8A, 0x3E, 0xFF };
    static const Color NOTE_BORDER = { 0x5A, 0x7F, 0x5A, 0xFF }, HEAD = { 0xCC, 0xCC, 0xCC, 0xFF };
    static const Color MUTED = { 0x8A, 0x8A, 0x8A, 0xFF }, LINK = { 0x6C, 0xA8, 0xE0, 0xFF };
    static const Color FIELD = { 0x1B, 0x1C, 0x1F, 0xFF };
    layout(v, r);
    DrawRectangleRec(r, BG);
    DrawRectangleLinesEx(r, 1.0f, v->kind == REC_CANON ? BORDER : NOTE_BORDER);
    sfont_draw_utf8(v->label, v->header, (int)strlen(v->header), r.x + PAD, r.y + 11, HEAD);
    DrawRectangleRec(v->ed_rect, FIELD);
    editor_draw(v->ed, v->ed_rect, focused);

    char dirty_note[64] = "";
    if (record_view_dirty(v))
        snprintf(dirty_note, sizeof(dirty_note), "%s", v->kind == REC_NOTE ? " \xC2\xB7 unsaved" : v->id[0] ? " \xC2\xB7 edited (Revise to keep)" : "");
    char line[400];
    snprintf(line, sizeof(line), "%s%s", v->source_line, dirty_note);
    sfont_draw_utf8(v->label, line, (int)strlen(line), v->src_rect.x, v->src_rect.y + 3, v->nsources ? LINK : MUTED);
    for (size_t i = 0; i < MAX_HISTORY && i < v->nhist; i++) {
        if (v->hist_rect[i].width <= 0) continue;
        struct tm tmv;
        localtime_r(&v->hist[i].ts, &tmv);
        char when[32], text[100], h[200];
        strftime(when, sizeof(when), "%b %d %H:%M", &tmv);
        snprintf(text, sizeof(text), "%s", v->hist[i].content);
        flatten(text);
        clip_utf8(text, 64);
        snprintf(h, sizeof(h), "v%d \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %s", v->hist[i].ver, when, v->hist[i].status, text);
        sfont_draw_utf8(v->label, h, (int)strlen(h), v->hist_rect[i].x, v->hist_rect[i].y + 3, MUTED);
    }

    RecordAction act = RA_NONE;
    for (int i = 0; i < v->nbtn; i++) {
        bool disable = (v->btn_action[i] == RA_SAVE || v->btn_action[i] == RA_REVISE) && !record_view_dirty(v);
        if (disable) GuiDisable();
        if (GuiButton(v->btn[i], v->btn_label[i])) act = v->btn_action[i];
        GuiEnable();
    }
    if (!input_mouse_blocked() && v->nsources && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), v->src_rect))
        act = RA_SOURCE;
    return act;
}

Rectangle record_view_element(const RecordView *v, const char *name)
{
    if (!v->active) return (Rectangle){ 0 };
    if (!strcmp(name, "RecordPanel")) return v->r;
    if (!strcmp(name, "RecordEditor")) return v->ed_rect;
    if (!strcmp(name, "RecordSource")) return v->nsources ? v->src_rect : (Rectangle){ 0 };
    if (!strncmp(name, "RecordHistory", 13)) {
        int i = atoi(name + 13);
        return i >= 0 && i < MAX_HISTORY ? v->hist_rect[i] : (Rectangle){ 0 };
    }
    static const struct { const char *name; RecordAction a; } map[] = {
        { "RecordSave", RA_SAVE },       { "RecordCanon", RA_TO_CANON }, { "RecordDelete", RA_DELETE },
        { "RecordClose", RA_CLOSE },     { "RecordCommit", RA_COMMIT },  { "RecordRevise", RA_REVISE },
        { "RecordRetract", RA_RETRACT }, { "RecordRecommit", RA_RECOMMIT },
    };
    for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); k++)
        if (!strcmp(name, map[k].name))
            for (int i = 0; i < v->nbtn; i++)
                if (v->btn_action[i] == map[k].a) return v->btn[i];
    return (Rectangle){ 0 };
}
