#include "editor.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/doc_io.h"
#include "core/text_buffer.h"
#include "core/undo.h"
#include "core/utf8.h"
#include "core/wrap.h"
#include "ui/input.h"

/* ---- stb_textedit configuration (header mode) ---------------------------- */

#define STB_TEXTEDIT_CHARTYPE       uint32_t
#define STB_TEXTEDIT_POSITIONTYPE   int
/* stb_textedit still maintains its own undo buffer internally even though we
 * never send K_UNDO/K_REDO. It is left at the library defaults (99 states,
 * 999 chars): shrinking it to a size the library is not tested with buys a
 * few KB and risks hitting untested paths in its ring-buffer code. */
#include "stb_textedit.h"

#define MAX_COLUMN_WIDTH 720.0f
#define MIN_SIDE_PADDING 32.0f
#define TOP_PADDING      28.0f
#define CLICK_INTERVAL_S 0.4
#define OVERWRITE_ARM_S  30.0
/* Keep every position inside stb_textedit's int range with headroom. */
#define EDITOR_MAX_CHARS (DOC_MAX_FILE_BYTES)

struct Editor {
    TextBuffer buf;
    UndoLog undo;
    WrapLayout layout;
    bool layout_dirty;
    float layout_width;

    SFont *font;
    STB_TexteditState st;

    char *path;
    bool had_bom;
    bool crlf;
    bool read_only;
    char read_only_reason[160];
    DocFileId disk_id;      /* identity of the file as last loaded/saved */
    /* After a save is refused because the file changed on disk, a second
     * save may overwrite -- but only the exact disk state the user was warned
     * about, only if they have not edited since, and only for a short time. */
    bool overwrite_armed;
    DocFileId armed_id;
    uint64_t armed_version;
    double armed_time;
    size_t max_chars;
    char notice[256];       /* one-shot message for the status bar */

    /* Layout (defaults are the prose column; the chat input uses tighter ones). */
    float side_pad, top_pad, max_col;
    bool scratch;           /* editable buffer with no file (chat input) */
    bool submit_on_enter;   /* Enter submits, Shift+Enter inserts a newline */
    bool submit_pending;
    bool single_line;       /* a one-line field: no overscroll or scrollbar */
    bool modal_owner;       /* belongs to the prompt: not blocked while it is open */
    char placeholder[128];
    /* Tracked anchor: the passage an edit request/proposal targets. Its
     * position follows every insert/delete (including undo/redo); any change
     * inside it (or at an insertion point) marks it touched. */
    bool anc_active, anc_touched;
    size_t anc_start, anc_len;
    uint64_t saved_id;      /* undo state id at last load/save */
    uint64_t version; /* bumps on every content change */

    /* Geometry of the text column, updated every frame. */
    float text_x, text_y, text_w, view_h;
    float scroll_y;
    bool want_cursor_visible;
    double blink_t0;

    /* Mouse */
    bool mouse_selecting;
    int click_count;
    double last_click_time;
    Vector2 last_click_pos;
    Vector2 last_drag_pos;

    /* Cached stats */
    uint64_t stats_version;
    int stats_cursor;
    EditorStats stats;
};

/* ---- stb_textedit callbacks ---------------------------------------------- */

static float adv_cb(uint32_t cp, void *ctx) { return sfont_advance((const SFont *)ctx, cp); }

static void ensure_layout(Editor *e)
{
    float w = e->text_w > 1.0f ? e->text_w : 600.0f;
    if (!e->layout_dirty && e->layout_width == w) return;
    wrap_compute(&e->layout, &e->buf, w, adv_cb, e->font);
    e->layout_dirty = false;
    e->layout_width = w;
}

static void anchor_op(void *ctx, bool insert, size_t pos, size_t n)
{
    Editor *e = ctx;
    if (!e->anc_active || e->anc_touched || n == 0) return;
    size_t s = e->anc_start, end = s + e->anc_len;
    if (insert) {
        if (e->anc_len == 0 ? pos < s : pos <= s) e->anc_start += n;
        else if (e->anc_len == 0 ? pos == s : pos < end) e->anc_touched = true;
    } else {
        if (pos + n <= s) e->anc_start -= n;
        else if (e->anc_len == 0 ? pos < s : pos < end) e->anc_touched = true;
    }
}

static void reset_undo(Editor *e)
{
    undo_free(&e->undo);
    undo_init(&e->undo);
    e->undo.on_op = anchor_op;
    e->undo.on_op_ctx = e;
    e->anc_active = false;
}

static void mark_changed(Editor *e)
{
    e->layout_dirty = true;
    e->version++;
}

static void layout_row(StbTexteditRow *r, Editor *e, int n)
{
    ensure_layout(e);
    const WrapRow *row = &e->layout.rows[wrap_row_for_index(&e->layout, (size_t)n)];
    float lh = e->font->line_height;
    r->x0 = 0.0f;
    r->x1 = row->width;
    r->baseline_y_delta = lh;
    r->ymin = 0.0f;
    r->ymax = lh;
    r->num_chars = (int)(row->start + row->len - (size_t)n);
}

static float char_width(Editor *e, int n, int i)
{
    uint32_t c = tb_get(&e->buf, (size_t)(n + i));
    if (c == '\n') return -1.0f; /* STB_TEXTEDIT_GETWIDTH_NEWLINE */
    return sfont_advance(e->font, c);
}

/* If an edit cannot be recorded, history no longer matches the buffer and
 * replaying it would corrupt text, so drop it. undo_clear() moves to a fresh
 * state id, so the document still reads as dirty. */
static void undo_lost(Editor *e)
{
    undo_clear(&e->undo);
}

static void set_notice(Editor *e, const char *msg)
{
    snprintf(e->notice, sizeof(e->notice), "%s", msg);
}

static int insert_chars(Editor *e, int pos, const uint32_t *cps, int n)
{
    if (n < 0 || tb_len(&e->buf) + (size_t)n > e->max_chars) return 0;
    if (!tb_insert(&e->buf, (size_t)pos, cps, (size_t)n)) return 0;
    anchor_op(e, true, (size_t)pos, (size_t)n);
    if (!undo_record_insert(&e->undo, (size_t)pos, cps, (size_t)n)) undo_lost(e);
    mark_changed(e);
    return 1;
}

static void delete_chars(Editor *e, int pos, int n)
{
    if (n <= 0) return;
    uint32_t stack[256];
    uint32_t *tmp = n <= 256 ? stack : malloc((size_t)n * sizeof(uint32_t));
    bool recorded = false;
    if (tmp) {
        tb_copy(&e->buf, (size_t)pos, (size_t)n, tmp);
        recorded = undo_record_delete(&e->undo, (size_t)pos, tmp, (size_t)n);
        if (tmp != stack) free(tmp);
    }
    if (!recorded) undo_lost(e);
    tb_delete(&e->buf, (size_t)pos, (size_t)n);
    anchor_op(e, false, (size_t)pos, (size_t)n);
    mark_changed(e);
}

/* Word characters: letters/digits in any script, plus an apostrophe between
 * two letters ("don't", "don’t"). Quotes, dashes and other punctuation are
 * not part of words. */
static bool is_word_cp(uint32_t c)
{
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    if (c >= 0x2000 && c <= 0x206F) return false; /* general punctuation */
    if (c >= 0x3000 && c <= 0x303F) return false; /* CJK punctuation */
    if (c == 0xA0 || c == 0xA1 || c == 0xAB || c == 0xB7 || c == 0xBB || c == 0xBF) return false;
    return true;
}

static bool is_word_at(const Editor *e, int i)
{
    int n = (int)tb_len(&e->buf);
    if (i < 0 || i >= n) return false;
    uint32_t c = tb_get(&e->buf, (size_t)i);
    if (is_word_cp(c)) return true;
    if ((c == '\'' || c == 0x2019) && i > 0 && i + 1 < n)
        return is_word_cp(tb_get(&e->buf, (size_t)i - 1)) && is_word_cp(tb_get(&e->buf, (size_t)i + 1));
    return false;
}

static int word_left(Editor *e, int i)
{
    while (i > 0 && !is_word_at(e, i - 1)) i--;
    while (i > 0 && is_word_at(e, i - 1)) i--;
    return i;
}

static int word_right(Editor *e, int i)
{
    int n = (int)tb_len(&e->buf);
    while (i < n && !is_word_at(e, i)) i++;
    while (i < n && is_word_at(e, i)) i++;
    return i;
}

#define ED_K_KEYDOWN 0x40000000
#define ED_K_SHIFT   0x20000000

#define STB_TEXTEDIT_STRING              Editor
#define STB_TEXTEDIT_STRINGLEN(e)        ((int)tb_len(&(e)->buf))
#define STB_TEXTEDIT_LAYOUTROW(r, e, n)  layout_row(r, e, n)
#define STB_TEXTEDIT_GETWIDTH(e, n, i)   char_width(e, n, i)
#define STB_TEXTEDIT_KEYTOTEXT(k)        (((k) & ED_K_KEYDOWN) ? -1 : (int)(k))
#define STB_TEXTEDIT_GETCHAR(e, i)       tb_get(&(e)->buf, (size_t)(i))
#define STB_TEXTEDIT_NEWLINE             '\n'
#define STB_TEXTEDIT_GETWIDTH_NEWLINE    -1.0f
#define STB_TEXTEDIT_IS_SPACE(c)         wrap_is_space(c)
#define STB_TEXTEDIT_DELETECHARS(e, i, n)    delete_chars(e, i, n)
#define STB_TEXTEDIT_INSERTCHARS(e, i, c, n) insert_chars(e, i, c, n)
#define STB_TEXTEDIT_MOVEWORDLEFT(e, i)  word_left(e, i)
#define STB_TEXTEDIT_MOVEWORDRIGHT(e, i) word_right(e, i)

#define STB_TEXTEDIT_K_SHIFT     ED_K_SHIFT
#define STB_TEXTEDIT_K_LEFT      (ED_K_KEYDOWN | 1)
#define STB_TEXTEDIT_K_RIGHT     (ED_K_KEYDOWN | 2)
#define STB_TEXTEDIT_K_UP        (ED_K_KEYDOWN | 3)
#define STB_TEXTEDIT_K_DOWN      (ED_K_KEYDOWN | 4)
#define STB_TEXTEDIT_K_PGUP      (ED_K_KEYDOWN | 5)
#define STB_TEXTEDIT_K_PGDOWN    (ED_K_KEYDOWN | 6)
#define STB_TEXTEDIT_K_LINESTART (ED_K_KEYDOWN | 7)
#define STB_TEXTEDIT_K_LINEEND   (ED_K_KEYDOWN | 8)
#define STB_TEXTEDIT_K_TEXTSTART (ED_K_KEYDOWN | 9)
#define STB_TEXTEDIT_K_TEXTEND   (ED_K_KEYDOWN | 10)
#define STB_TEXTEDIT_K_DELETE    (ED_K_KEYDOWN | 11)
#define STB_TEXTEDIT_K_BACKSPACE (ED_K_KEYDOWN | 12)
#define STB_TEXTEDIT_K_UNDO      (ED_K_KEYDOWN | 13) /* never sent */
#define STB_TEXTEDIT_K_REDO      (ED_K_KEYDOWN | 14) /* never sent */
#define STB_TEXTEDIT_K_WORDLEFT  (ED_K_KEYDOWN | 15)
#define STB_TEXTEDIT_K_WORDRIGHT (ED_K_KEYDOWN | 16)

#define STB_TEXTEDIT_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "stb_textedit.h"
#pragma GCC diagnostic pop

/* ---- actions --------------------------------------------------------------- */

static UndoCursor cursor_state(const Editor *e)
{
    return (UndoCursor){ e->st.cursor, e->st.select_start, e->st.select_end };
}

static bool has_selection(const Editor *e) { return e->st.select_start != e->st.select_end; }

static void sel_range(const Editor *e, int *lo, int *hi)
{
    int a = e->st.select_start, b = e->st.select_end;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

static void action_begin(Editor *e, UndoMergeKind kind, bool allow_merge)
{
    undo_begin(&e->undo, cursor_state(e), kind, allow_merge, GetTime());
}

static void action_end(Editor *e)
{
    /* stb_textedit leaves stale (equal) selection endpoints after edits with
     * no selection, possibly past the end of the text. It ignores them, but
     * normalise so every consumer sees a valid range. */
    if (e->st.select_start == e->st.select_end) e->st.select_start = e->st.select_end = e->st.cursor;
    undo_end(&e->undo, cursor_state(e), GetTime());
    e->want_cursor_visible = true;
    e->blink_t0 = GetTime();
}

static void key(Editor *e, int k)
{
    action_begin(e, UNDO_MERGE_NONE, false);
    stb_textedit_key(e, &e->st, k);
    action_end(e);
}

/* Would replacing the selection with `n` codepoints exceed the size limit?
 * Checked before stb_textedit deletes the selection, because once it has, a
 * refused insert would leave the selection silently deleted. */
static bool would_exceed(Editor *e, size_t n)
{
    int lo, hi;
    sel_range(e, &lo, &hi);
    size_t after = tb_len(&e->buf) - (size_t)(hi - lo);
    if (n <= e->max_chars && after <= e->max_chars - n) return false;
    set_notice(e, "Document size limit reached; nothing was inserted");
    return true;
}

/* stb deletes the selection before inserting. Reserve the net growth before
 * changing selection, anchors or undo state so a refused insert changes nothing.
 * Deletion itself supplies the rest of the required gap. */
static bool reserve_replacement(Editor *e, size_t n, size_t removed)
{
    if (n <= removed || tb_reserve(&e->buf, n - removed)) return true;
    set_notice(e, "Not enough memory; nothing was inserted");
    return false;
}

static void type_char(Editor *e, uint32_t cp)
{
    if (would_exceed(e, 1)) return;
    int lo, hi;
    sel_range(e, &lo, &hi);
    if (!reserve_replacement(e, 1, (size_t)(hi - lo))) return;
    bool sel = has_selection(e);
    UndoMergeKind kind = (cp == '\n' || sel) ? UNDO_MERGE_NONE : UNDO_MERGE_TYPING;
    /* Start a fresh undo group at the beginning of each word. */
    bool allow_merge = true;
    if (kind == UNDO_MERGE_TYPING && !wrap_is_space(cp) && e->st.cursor > 0 &&
        wrap_is_space(tb_get(&e->buf, (size_t)e->st.cursor - 1)))
        allow_merge = false;
    action_begin(e, kind, allow_merge);
    stb_textedit_key(e, &e->st, (int)cp);
    action_end(e);
}

/* ---- visual-line motion --------------------------------------------------
 *
 * Vertical movement, line start/end and hit-testing use our wrap layout
 * directly. stb_textedit scans to '\n' for line start/end (so "line" means
 * paragraph), steps PgUp by paragraph, and treats the end of the text as an
 * empty extra row, which sends Up-from-the-end to the start of the line. */

/* Last caret position on a visual row. On a soft-wrapped row or one ending
 * in '\n', the position after the last character belongs to the next row,
 * so the caret stops just before it. */
static size_t row_last_pos(const Editor *e, size_t r)
{
    const WrapRow *row = &e->layout.rows[r];
    size_t end = row->start + row->len;
    return (r + 1 < e->layout.count && row->len > 0) ? end - 1 : end;
}

static size_t locate_in_row(const Editor *e, size_t r, float x)
{
    const WrapRow *row = &e->layout.rows[r];
    size_t last = row_last_pos(e, r);
    float cx = 0.0f;
    for (size_t i = row->start; i < last; i++) {
        float w = sfont_advance(e->font, tb_get(&e->buf, i));
        if (x < cx + w * 0.5f) return i;
        cx += w;
    }
    return last;
}

/* Text-column coordinates (scroll included) -> buffer position. */
static size_t hit_test(Editor *e, float lx, float ly)
{
    ensure_layout(e);
    if (ly < 0) return 0;
    size_t r = (size_t)(ly / e->font->line_height);
    if (r >= e->layout.count) return tb_len(&e->buf);
    return locate_in_row(e, r, lx);
}

static void move_to(Editor *e, size_t pos, bool extend)
{
    if (extend) {
        if (!has_selection(e)) e->st.select_start = e->st.cursor;
        e->st.select_end = (int)pos;
    } else {
        e->st.select_start = e->st.select_end = (int)pos;
    }
    e->st.cursor = (int)pos;
    e->want_cursor_visible = true;
    e->blink_t0 = GetTime();
}

static float row_x(const Editor *e, const WrapRow *row, size_t idx);

static void move_vertical(Editor *e, long rows, bool extend)
{
    ensure_layout(e);
    size_t cur = (size_t)e->st.cursor;
    size_t r = wrap_row_for_index(&e->layout, cur);
    float x = e->st.has_preferred_x ? e->st.preferred_x : row_x(e, &e->layout.rows[r], cur);
    long target = (long)r + rows;
    size_t pos;
    if (target < 0) pos = 0;                                   /* past the top: start */
    else if ((size_t)target >= e->layout.count) pos = tb_len(&e->buf); /* past the bottom: end */
    else pos = locate_in_row(e, (size_t)target, x);
    move_to(e, pos, extend);
    e->st.has_preferred_x = 1;
    e->st.preferred_x = x;
}

static size_t line_edge(Editor *e, bool end)
{
    ensure_layout(e);
    size_t r = wrap_row_for_index(&e->layout, (size_t)e->st.cursor);
    return end ? row_last_pos(e, r) : e->layout.rows[r].start;
}

static void move_line_edge(Editor *e, bool end, bool extend)
{
    move_to(e, line_edge(e, end), extend);
    e->st.has_preferred_x = 0;
}

static int page_rows(const Editor *e)
{
    int n = (int)(e->view_h / e->font->line_height) - 1;
    return n < 1 ? 1 : n;
}

typedef enum { UNIT_CHAR, UNIT_WORD, UNIT_LINE } DeleteUnit;

static void erase(Editor *e, bool backward, DeleteUnit unit)
{
    bool sel = has_selection(e);
    UndoMergeKind kind = UNDO_MERGE_NONE;
    if (!sel && unit == UNIT_CHAR) kind = backward ? UNDO_MERGE_BACKSPACE : UNDO_MERGE_DELETE;
    action_begin(e, kind, true);
    if (!sel && unit == UNIT_WORD)
        stb_textedit_key(e, &e->st, (backward ? STB_TEXTEDIT_K_WORDLEFT : STB_TEXTEDIT_K_WORDRIGHT) | ED_K_SHIFT);
    if (!sel && unit == UNIT_LINE) {
        /* Visual line, as on macOS. At the line start this selects nothing
         * and the backspace below joins with the previous line. */
        size_t edge = line_edge(e, !backward);
        e->st.select_start = e->st.cursor;
        e->st.select_end = (int)edge;
        e->st.cursor = (int)edge;
    }
    stb_textedit_key(e, &e->st, backward ? STB_TEXTEDIT_K_BACKSPACE : STB_TEXTEDIT_K_DELETE);
    action_end(e);
}

static void set_cursor(Editor *e, UndoCursor c)
{
    int n = (int)tb_len(&e->buf);
#define CLAMP_POS(v) ((v) < 0 ? 0 : (v) > n ? n : (v))
    e->st.cursor = CLAMP_POS(c.cursor);
    e->st.select_start = CLAMP_POS(c.sel_start);
    e->st.select_end = CLAMP_POS(c.sel_end);
#undef CLAMP_POS
    e->st.has_preferred_x = 0;
    e->want_cursor_visible = true;
    e->blink_t0 = GetTime();
}

static void do_undo(Editor *e, bool redo)
{
    UndoCursor c;
    bool ok = redo ? undo_redo(&e->undo, &e->buf, &c) : undo_undo(&e->undo, &e->buf, &c);
    if (!ok) return;
    mark_changed(e);
    set_cursor(e, c);
}

static void select_all(Editor *e)
{
    int n = (int)tb_len(&e->buf);
    set_cursor(e, (UndoCursor){ n, 0, n });
}

static char *selection_utf8(Editor *e)
{
    int lo, hi;
    sel_range(e, &lo, &hi);
    size_t n = (size_t)(hi - lo);
    uint32_t *cps = malloc((n ? n : 1) * sizeof(uint32_t));
    if (!cps) return NULL;
    tb_copy(&e->buf, (size_t)lo, n, cps);
    size_t bytes = utf8_encoded_len(cps, n);
    char *s = malloc(bytes + 1);
    if (s) {
        utf8_encode(cps, n, (uint8_t *)s);
        s[bytes] = '\0';
    }
    free(cps);
    return s;
}

static void copy(Editor *e)
{
    if (!has_selection(e)) return;
    char *s = selection_utf8(e);
    if (s) input_set_clipboard(s);
    free(s);
}

static void cut(Editor *e)
{
    if (!has_selection(e)) return;
    copy(e);
    action_begin(e, UNDO_MERGE_NONE, false);
    stb_textedit_cut(e, &e->st);
    action_end(e);
}

/* Normalise line endings and drop control characters other than \t and \n. */
static size_t sanitize_text(uint32_t *cps, size_t n)
{
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        uint32_t c = cps[r];
        if (c == '\r') {
            if (r + 1 < n && cps[r + 1] == '\n') continue;
            c = '\n';
        }
        if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F) continue;
        cps[w++] = c;
    }
    return w;
}

bool editor_paste_utf8(Editor *e, const char *s)
{
    if (e->read_only || !s || !*s) return false;
    size_t len = strlen(s);
    if (len > e->max_chars) {
        set_notice(e, "Clipboard text is too large to paste");
        return false;
    }
    uint32_t *cps = malloc(len * sizeof(uint32_t));
    if (!cps) return false;
    size_t n = sanitize_text(cps, utf8_decode((const uint8_t *)s, len, cps, NULL));
    int lo, hi;
    sel_range(e, &lo, &hi);
    if (would_exceed(e, n) || !reserve_replacement(e, n, (size_t)(hi - lo))) {
        free(cps);
        return false;
    }
    action_begin(e, UNDO_MERGE_NONE, false);
    int ok = stb_textedit_paste(e, &e->st, cps, (int)n);
    action_end(e);
    free(cps);
    return ok != 0;
}

static void paste(Editor *e) { editor_paste_utf8(e, input_get_clipboard()); }

bool editor_replace_range(Editor *e, size_t pos, size_t len, const uint32_t *cps, size_t n)
{
    size_t total = tb_len(&e->buf);
    if (e->read_only || pos > total || len > total - pos) return false;
    if (n > e->max_chars || total - len > e->max_chars - n) return false;
    if (!reserve_replacement(e, n, len)) return false;
    e->st.select_start = (int)pos;
    e->st.select_end = (int)(pos + len);
    e->st.cursor = (int)(pos + len);
    action_begin(e, UNDO_MERGE_NONE, false);
    int ok = stb_textedit_paste(e, &e->st, cps, (int)n);
    action_end(e);
    return ok != 0;
}

/* ---- lifecycle ------------------------------------------------------------- */

Editor *editor_create(SFont *font)
{
    Editor *e = calloc(1, sizeof(Editor));
    if (!e) return NULL;
    e->font = font;
    tb_init(&e->buf);
    undo_init(&e->undo);
    reset_undo(e);
    stb_textedit_initialize_state(&e->st, 0);
    e->layout_dirty = true;
    e->stats_version = UINT64_MAX;
    e->max_chars = EDITOR_MAX_CHARS;
    e->side_pad = MIN_SIDE_PADDING;
    e->top_pad = TOP_PADDING;
    e->max_col = MAX_COLUMN_WIDTH;
    /* Nothing is editable until a document with a usable path is open, so
     * text can never be typed into a buffer that has nowhere to be saved. */
    e->read_only = true;
    snprintf(e->read_only_reason, sizeof(e->read_only_reason), "No document is open");
    return e;
}

void editor_destroy(Editor *e)
{
    if (!e) return;
    tb_free(&e->buf);
    undo_free(&e->undo);
    wrap_free(&e->layout);
    free(e->path);
    free(e);
}

bool editor_load_file(Editor *e, const char *path, char *err, size_t errlen)
{
    DocLoad d;
    DocStatus st = doc_load(path, &d, err, errlen);
    if (st != DOC_OK) {
        if (!err[0]) snprintf(err, errlen, "Could not open %s (out of memory)", path);
        return false;
    }
    char *path_copy = strdup(path);
    if (!path_copy || !tb_set(&e->buf, d.cps, d.len)) {
        free(path_copy);
        doc_load_free(&d);
        snprintf(err, errlen, "Out of memory loading %s", path);
        return false;
    }

    reset_undo(e);
    stb_textedit_initialize_state(&e->st, 0);
    free(e->path);
    e->path = path_copy;
    e->had_bom = d.bom;
    e->crlf = d.crlf;
    e->disk_id = d.id;
    e->overwrite_armed = false;
    /* Invalid UTF-8 cannot round-trip (it decodes to U+FFFD), so saving
     * would destroy the original bytes: open read-only. Likewise for files we
     * may not write (or, for a new file, a directory we cannot create in). */
    e->read_only = d.invalid_utf8 > 0 || !d.writable;
    if (d.invalid_utf8)
        snprintf(e->read_only_reason, sizeof(e->read_only_reason), "File contains invalid UTF-8");
    else if (!d.writable)
        snprintf(e->read_only_reason, sizeof(e->read_only_reason),
                 d.created ? "Cannot create files in this folder" : "File is write-protected");
    else
        e->read_only_reason[0] = '\0';
    e->saved_id = undo_state_id(&e->undo);
    e->scroll_y = 0;
    e->mouse_selecting = false;
    mark_changed(e);

    if (d.invalid_utf8)
        snprintf(err, errlen, "%s has %zu invalid UTF-8 sequence%s; opened read-only",
                 path, d.invalid_utf8, d.invalid_utf8 == 1 ? "" : "s");
    else if (!d.writable)
        snprintf(err, errlen, "%s: %s; opened read-only", path, e->read_only_reason);
    else if (d.mixed_newlines || d.lone_cr)
        snprintf(err, errlen, "%s has mixed line endings; saving will use %s throughout",
                 path, d.crlf ? "CRLF" : "LF");
    else if (d.created)
        snprintf(err, errlen, "New file: %s", path);
    else
        err[0] = '\0';
    doc_load_free(&d);
    return true;
}

void editor_close(Editor *e)
{
    tb_set(&e->buf, NULL, 0);
    reset_undo(e);
    stb_textedit_initialize_state(&e->st, 0);
    free(e->path);
    e->path = NULL;
    e->overwrite_armed = false;
    e->read_only = true;
    snprintf(e->read_only_reason, sizeof(e->read_only_reason), "No document is open");
    e->saved_id = undo_state_id(&e->undo);
    e->scroll_y = 0;
    e->mouse_selecting = false;
    mark_changed(e);
}

bool editor_set_path(Editor *e, const char *path)
{
    /* The file was renamed on disk: same file (inode, contents), new name.
     * The buffer, undo history and external-change tracking carry over. */
    char *p = strdup(path);
    if (!p || !e->path) {
        free(p);
        return false;
    }
    free(e->path);
    e->path = p;
    return true;
}

static bool overwrite_valid(const Editor *e)
{
    return e->overwrite_armed && e->armed_version == e->version &&
           GetTime() - e->armed_time <= OVERWRITE_ARM_S;
}

/* "<dir>/<name> (Scribodo conflict 2026-11-04 071530).<ext>" */
static void conflict_path(const char *path, char *out, size_t outlen)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) dot = path + strlen(path);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M%S", &tmv);
    snprintf(out, outlen, "%.*s (Scribodo conflict %s)%s", (int)(dot - path), path, stamp, dot);
}

bool editor_save(Editor *e, char *err, size_t errlen)
{
    if (!e->path || e->read_only) {
        snprintf(err, errlen, "Read-only: %s", e->read_only_reason[0] ? e->read_only_reason : "cannot save");
        return false;
    }
    size_t n = tb_len(&e->buf);
    uint32_t *cps = malloc((n ? n : 1) * sizeof(uint32_t));
    if (!cps) {
        snprintf(err, errlen, "Out of memory");
        return false;
    }
    tb_copy(&e->buf, 0, n, cps);

    bool confirmed = overwrite_valid(e);
    const DocFileId *expected = confirmed ? &e->armed_id : &e->disk_id;
    DocFileId new_id;
    DocStatus st = doc_save(e->path, cps, n, e->had_bom, e->crlf, expected, &new_id, err, errlen);

    if (st == DOC_ERR_CHANGED_ON_DISK) {
        /* Keep both versions: theirs stays in place, ours goes to a sibling
         * conflict copy (never overwriting an existing file). */
        char copy[1200];
        conflict_path(e->path, copy, sizeof(copy));
        DocFileId must_not_exist = { 0 };
        char cerr[256];
        bool copied = doc_save(copy, cps, n, e->had_bom, e->crlf, &must_not_exist, NULL,
                               cerr, sizeof(cerr)) == DOC_OK;
        e->overwrite_armed = true;
        e->armed_id = doc_file_id(e->path);
        e->armed_version = e->version;
        e->armed_time = GetTime();
        if (copied)
            snprintf(err, errlen, "%s changed on disk. Your version was saved to \"%s\". "
                     "Save again within 30 s to overwrite the file on disk.", e->path, copy);
        else
            snprintf(err, errlen, "%s changed on disk (conflict copy failed: %s). "
                     "Save again within 30 s to overwrite it.", e->path, cerr);
        free(cps);
        return false;
    }
    free(cps);
    e->overwrite_armed = false;
    if (st != DOC_OK) {
        if (!err[0]) snprintf(err, errlen, "Save failed (out of memory)");
        return false;
    }
    e->disk_id = new_id;
    e->saved_id = undo_state_id(&e->undo);
    /* Typing right after a save must not merge into the saved group, or the
     * saved text would be unreachable by undo. */
    undo_seal(&e->undo);
    snprintf(err, errlen, "Saved %s", e->path);
    return true;
}

/* ---- geometry ---------------------------------------------------------------- */

static void update_geometry(Editor *e, Rectangle b)
{
    float side = e->side_pad;
    float w = b.width - 2.0f * side;
    if (w > e->max_col) w = e->max_col;
    if (w < 40.0f) w = 40.0f;
    e->text_w = floorf(w);
    e->text_x = floorf(b.x + (b.width - e->text_w) * 0.5f);
    e->text_y = b.y + e->top_pad;
    e->view_h = b.height - e->top_pad;
    ensure_layout(e);
}

static float row_x(const Editor *e, const WrapRow *row, size_t idx)
{
    float x = 0.0f;
    for (size_t i = row->start; i < idx; i++) x += sfont_advance(e->font, tb_get(&e->buf, i));
    return x;
}

static float max_scroll(const Editor *e)
{
    float content = (float)e->layout.count * e->font->line_height;
    float m = e->single_line ? content - e->view_h : content - e->view_h * 0.5f;
    return m > 0 ? m : 0;
}

static void clamp_scroll(Editor *e)
{
    float m = max_scroll(e);
    if (e->scroll_y > m) e->scroll_y = m;
    if (e->scroll_y < 0) e->scroll_y = 0;
}

static void scroll_to_cursor(Editor *e)
{
    float lh = e->font->line_height;
    size_t r = wrap_row_for_index(&e->layout, (size_t)e->st.cursor);
    float y = (float)r * lh;
    float margin = lh;
    if (y - margin < e->scroll_y) e->scroll_y = y - margin;
    if (y + lh + margin > e->scroll_y + e->view_h) e->scroll_y = y + lh + margin - e->view_h;
    clamp_scroll(e);
}

/* ---- input --------------------------------------------------------------- */

static bool pressed(int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); }

static void select_word_at(Editor *e, int i)
{
    int n = (int)tb_len(&e->buf);
    int lo = i, hi = i;
    if (is_word_at(e, i) || is_word_at(e, i - 1)) {
        while (lo > 0 && is_word_at(e, lo - 1)) lo--;
        while (hi < n && is_word_at(e, hi)) hi++;
    } else if (i < n && tb_get(&e->buf, (size_t)i) != '\n') {
        hi = i + 1;
    }
    set_cursor(e, (UndoCursor){ hi, lo, hi });
}

static void select_paragraph_at(Editor *e, int i)
{
    int n = (int)tb_len(&e->buf);
    int lo = i, hi = i;
    while (lo > 0 && tb_get(&e->buf, (size_t)lo - 1) != '\n') lo--;
    while (hi < n && tb_get(&e->buf, (size_t)hi) != '\n') hi++;
    if (hi < n) hi++; /* include the paragraph break */
    set_cursor(e, (UndoCursor){ hi, lo, hi });
}

static void handle_mouse(Editor *e, Rectangle b)
{
    if (input_mouse_blocked() && !e->modal_owner) {
        e->mouse_selecting = false;
        return;
    }
    Vector2 m = GetMousePosition();
    bool inside = CheckCollisionPointRec(m, b);
    float lx = m.x - e->text_x;
    float ly = m.y - e->text_y + e->scroll_y;

    if (inside) {
        Vector2 wheel = GetMouseWheelMoveV();
        if (wheel.y != 0.0f) {
            e->scroll_y -= wheel.y * e->font->line_height * 1.5f;
            clamp_scroll(e);
        }
    }

    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        double now = GetTime();
        bool near = fabsf(m.x - e->last_click_pos.x) < 4 && fabsf(m.y - e->last_click_pos.y) < 4;
        e->click_count = (now - e->last_click_time < CLICK_INTERVAL_S && near) ? e->click_count % 3 + 1 : 1;
        e->last_click_time = now;
        e->last_click_pos = m;
        e->last_drag_pos = m;

        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (e->click_count == 1) {
            move_to(e, hit_test(e, lx, ly), shift);
            e->st.has_preferred_x = 0;
            e->mouse_selecting = true;
        } else {
            int at = (int)hit_test(e, lx, ly);
            if (e->click_count == 2) select_word_at(e, at);
            else select_paragraph_at(e, at);
            e->mouse_selecting = false;
        }
        e->blink_t0 = now;
    }

    if (e->mouse_selecting) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            e->mouse_selecting = false;
        } else {
            /* Auto-scroll while dragging above or below the view. */
            float top = b.y, bottom = b.y + b.height;
            if (m.y < top) e->scroll_y -= (top - m.y) * 0.5f;
            if (m.y > bottom) e->scroll_y += (m.y - bottom) * 0.5f;
            clamp_scroll(e);
            ly = m.y - e->text_y + e->scroll_y;
            if (m.x != e->last_drag_pos.x || m.y != e->last_drag_pos.y || m.y < top || m.y > bottom) {
                move_to(e, hit_test(e, lx, ly), true);
                e->st.has_preferred_x = 0;
                e->last_drag_pos = m;
                e->blink_t0 = GetTime();
            }
        }
    }
}

static void handle_keyboard(Editor *e)
{
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool super = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#ifdef __APPLE__
    bool primary = super; /* ⌘ */
    bool word = alt;      /* ⌥ */
#else
    bool primary = ctrl;
    bool word = ctrl;
    (void)super;
#endif
    (void)ctrl;
    int S = shift ? ED_K_SHIFT : 0;
    bool rw = !e->read_only;

    /* Navigation */
    if (pressed(KEY_LEFT)) {
        if (word) key(e, STB_TEXTEDIT_K_WORDLEFT | S);
        else if (primary) move_line_edge(e, false, shift);
        else key(e, STB_TEXTEDIT_K_LEFT | S);
    }
    if (pressed(KEY_RIGHT)) {
        if (word) key(e, STB_TEXTEDIT_K_WORDRIGHT | S);
        else if (primary) move_line_edge(e, true, shift);
        else key(e, STB_TEXTEDIT_K_RIGHT | S);
    }
    if (pressed(KEY_UP)) {
        if (primary) key(e, STB_TEXTEDIT_K_TEXTSTART | S);
        else move_vertical(e, -1, shift);
    }
    if (pressed(KEY_DOWN)) {
        if (primary) key(e, STB_TEXTEDIT_K_TEXTEND | S);
        else move_vertical(e, 1, shift);
    }
    if (pressed(KEY_HOME)) {
        if (primary) key(e, STB_TEXTEDIT_K_TEXTSTART | S);
        else move_line_edge(e, false, shift);
    }
    if (pressed(KEY_END)) {
        if (primary) key(e, STB_TEXTEDIT_K_TEXTEND | S);
        else move_line_edge(e, true, shift);
    }
    if (pressed(KEY_PAGE_UP)) move_vertical(e, -page_rows(e), shift);
    if (pressed(KEY_PAGE_DOWN)) move_vertical(e, page_rows(e), shift);

    /* Shortcuts, matched by the letter the key types in the current layout. */
    if (primary) {
        if (shortcut_pressed('a')) select_all(e);
        if (shortcut_pressed('c')) copy(e);
        if (rw && shortcut_pressed('x')) cut(e);
        if (rw && shortcut_pressed('v')) paste(e);
        if (rw && shortcut_pressed('z')) do_undo(e, shift);
        if (rw && shortcut_pressed('y')) do_undo(e, true);
    }

    /* Editing */
    if (rw) {
        DeleteUnit unit = word ? UNIT_WORD : primary ? UNIT_LINE : UNIT_CHAR;
        if (pressed(KEY_BACKSPACE)) erase(e, true, unit);
        if (pressed(KEY_DELETE)) erase(e, false, unit);
        if (pressed(KEY_ENTER) || pressed(KEY_KP_ENTER)) {
            if (e->submit_on_enter && !shift) e->submit_pending = true;
            else if (!primary) type_char(e, '\n'); /* ⌘↩ is a command (accept edit) */
        }
        if (pressed(KEY_TAB)) type_char(e, '\t');
    }

    /* Text input. Characters arrive already composed (dead keys, ⌥-symbols). */
    int cp;
    while ((cp = input_get_char()) > 0) {
        if (getenv("SCRIBODO_INPUT_TRACE")) fprintf(stderr, "trace: char U+%04X primary=%d rw=%d\n", cp, primary, rw);
        if (primary || !rw) continue;
        if (cp < 0x20 || cp == 0x7F) continue;
        if (cp >= 0xF700 && cp <= 0xF8FF) continue; /* macOS function-key codes */
        type_char(e, (uint32_t)cp);
    }
}

void editor_update(Editor *e, Rectangle bounds, bool focused)
{
    update_geometry(e, bounds);
    handle_mouse(e, bounds);
    /* Unfocused editors leave the typed-character queue alone: several text
     * views share it, and the app drains it once per frame when no view has
     * focus (see input_discard_chars), so keystrokes typed while nothing is
     * focused are never inserted later. */
    if (focused) handle_keyboard(e);
    ensure_layout(e);
    if (e->want_cursor_visible) {
        scroll_to_cursor(e);
        e->want_cursor_visible = false;
    }
    clamp_scroll(e);
}

/* ---- drawing --------------------------------------------------------------- */

static const Color COL_TEXT      = { 0xD4, 0xD4, 0xD4, 0xFF };
static const Color COL_SELECTION = { 0x26, 0x4F, 0x78, 0xFF };
static const Color COL_CURSOR    = { 0xAE, 0xAF, 0xAD, 0xFF };
static const Color COL_SCROLL    = { 0x79, 0x79, 0x79, 0x66 };

void editor_draw(Editor *e, Rectangle b, bool focused)
{
    ensure_layout(e);
    const SFont *f = e->font;
    float lh = f->line_height;

    BeginScissorMode((int)b.x, (int)b.y, (int)b.width, (int)b.height);

    size_t first = (size_t)(e->scroll_y / lh);
    size_t last = (size_t)((e->scroll_y + e->view_h + e->top_pad) / lh) + 1;
    if (last > e->layout.count) last = e->layout.count;

    int sel_lo, sel_hi;
    sel_range(e, &sel_lo, &sel_hi);
    bool sel = sel_lo != sel_hi;

    for (size_t r = first; r < last; r++) {
        const WrapRow *row = &e->layout.rows[r];
        float y = e->text_y + (float)r * lh - e->scroll_y;
        size_t end = row->start + row->len;

        bool hl = e->anc_active && !e->anc_touched && e->anc_len;
        if (hl && e->anc_start < end + (row->len == 0) && e->anc_start + e->anc_len > row->start) {
            size_t a = e->anc_start > row->start ? e->anc_start : row->start;
            size_t z = e->anc_start + e->anc_len < end ? e->anc_start + e->anc_len : end;
            float x0 = row_x(e, row, a), x1 = row_x(e, row, z);
            DrawRectangleRec((Rectangle){ e->text_x + x0, y, x1 - x0, lh }, (Color){ 0x3A, 0x34, 0x1A, 0xFF });
        }
        if (sel && (size_t)sel_lo < end + (row->len == 0) && (size_t)sel_hi > row->start) {
            size_t a = (size_t)sel_lo > row->start ? (size_t)sel_lo : row->start;
            size_t z = (size_t)sel_hi < end ? (size_t)sel_hi : end;
            float x0 = row_x(e, row, a);
            float x1 = row_x(e, row, z);
            /* Show selected paragraph breaks as a space-wide highlight. */
            if (row->len && tb_get(&e->buf, end - 1) == '\n' && (size_t)sel_hi >= end)
                x1 += sfont_advance(f, ' ');
            DrawRectangleRec((Rectangle){ e->text_x + x0, y, x1 - x0, lh }, COL_SELECTION);
        }

        float x = e->text_x;
        for (size_t i = row->start; i < end; i++) {
            uint32_t c = tb_get(&e->buf, i);
            sfont_draw(f, c, x, y, COL_TEXT);
            x += sfont_advance(f, c);
        }
    }

    if (tb_len(&e->buf) == 0 && e->placeholder[0]) {
        sfont_draw_utf8(f, e->placeholder, (int)strlen(e->placeholder), e->text_x, e->text_y - e->scroll_y,
                        (Color){ 0x6E, 0x6E, 0x6E, 0xFF });
    }

    if (e->anc_active && !e->anc_touched && e->anc_len == 0 && e->anc_start <= tb_len(&e->buf)) {
        size_t r = wrap_row_for_index(&e->layout, e->anc_start);
        float mx = e->text_x + row_x(e, &e->layout.rows[r], e->anc_start);
        float my = e->text_y + (float)r * lh - e->scroll_y;
        DrawRectangleRec((Rectangle){ floorf(mx) - 2.0f, my + 2, 4.0f, lh - 4 }, (Color){ 0xE6, 0xB4, 0x50, 0xFF });
    }

    if (focused && fmod(GetTime() - e->blink_t0, 1.0) < 0.6) {
        size_t r = wrap_row_for_index(&e->layout, (size_t)e->st.cursor);
        const WrapRow *row = &e->layout.rows[r];
        float cx = e->text_x + row_x(e, row, (size_t)e->st.cursor);
        float cy = e->text_y + (float)r * lh - e->scroll_y;
        DrawRectangleRec((Rectangle){ floorf(cx) - 1.0f, cy + lh * 0.12f, 2.0f, lh * 0.76f }, COL_CURSOR);
    }

    /* Scrollbar */
    float content = (float)e->layout.count * lh + e->view_h * 0.5f;
    if (content > b.height && !e->single_line) {
        float track = b.height;
        float thumb = track * (b.height / content);
        if (thumb < 24) thumb = 24;
        float t = e->scroll_y / (content - b.height);
        if (t > 1) t = 1;
        DrawRectangleRounded((Rectangle){ b.x + b.width - 10, b.y + t * (track - thumb), 6, thumb }, 1.0f, 4, COL_SCROLL);
    }

    EndScissorMode();
}

/* ---- programmatic input (used by tests and future automation) --------------- */

void editor_input_char(Editor *e, uint32_t cp)
{
    if (e->read_only) return;
    if (cp == '\r') cp = '\n';
    if ((cp < 0x20 && cp != '\n' && cp != '\t') || cp == 0x7F || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
        return;
    type_char(e, cp);
}

void editor_input_key(Editor *e, EditorKey k, bool shift)
{
    switch (k) {
    case EDITOR_KEY_UP:         move_vertical(e, -1, shift); return;
    case EDITOR_KEY_DOWN:       move_vertical(e, 1, shift); return;
    case EDITOR_KEY_PAGE_UP:    move_vertical(e, -page_rows(e), shift); return;
    case EDITOR_KEY_PAGE_DOWN:  move_vertical(e, page_rows(e), shift); return;
    case EDITOR_KEY_LINE_START: move_line_edge(e, false, shift); return;
    case EDITOR_KEY_LINE_END:   move_line_edge(e, true, shift); return;
    default: break;
    }
    static const int map[] = {
        [EDITOR_KEY_LEFT] = STB_TEXTEDIT_K_LEFT,           [EDITOR_KEY_RIGHT] = STB_TEXTEDIT_K_RIGHT,
        [EDITOR_KEY_WORD_LEFT] = STB_TEXTEDIT_K_WORDLEFT,  [EDITOR_KEY_WORD_RIGHT] = STB_TEXTEDIT_K_WORDRIGHT,
        [EDITOR_KEY_TEXT_START] = STB_TEXTEDIT_K_TEXTSTART, [EDITOR_KEY_TEXT_END] = STB_TEXTEDIT_K_TEXTEND,
    };
    if (k >= EDITOR_KEY_BACKSPACE) {
        if (e->read_only) return;
        switch (k) {
        case EDITOR_KEY_BACKSPACE:      erase(e, true, UNIT_CHAR); break;
        case EDITOR_KEY_DELETE:         erase(e, false, UNIT_CHAR); break;
        case EDITOR_KEY_BACKSPACE_WORD: erase(e, true, UNIT_WORD); break;
        case EDITOR_KEY_DELETE_WORD:    erase(e, false, UNIT_WORD); break;
        case EDITOR_KEY_BACKSPACE_LINE: erase(e, true, UNIT_LINE); break;
        default: break;
        }
        return;
    }
    key(e, map[k] | (shift ? ED_K_SHIFT : 0));
}

void editor_undo(Editor *e) { if (!e->read_only) do_undo(e, false); }
void editor_redo(Editor *e) { if (!e->read_only) do_undo(e, true); }
void editor_select_all(Editor *e) { select_all(e); }

void editor_set_selection(Editor *e, size_t anchor, size_t cursor)
{
    set_cursor(e, (UndoCursor){ (int)cursor, (int)anchor, (int)cursor });
}

void editor_get_selection(const Editor *e, size_t *anchor, size_t *cursor)
{
    *anchor = (size_t)e->st.select_start;
    *cursor = (size_t)e->st.cursor;
    if (e->st.select_start == e->st.select_end) *anchor = (size_t)e->st.cursor;
}

bool editor_cut_selection(Editor *e, char **utf8_out)
{
    *utf8_out = NULL;
    if (e->read_only || !has_selection(e)) return false;
    *utf8_out = selection_utf8(e);
    action_begin(e, UNDO_MERGE_NONE, false);
    stb_textedit_cut(e, &e->st);
    action_end(e);
    return true;
}

size_t editor_length(const Editor *e) { return tb_len(&e->buf); }

char *editor_text_utf8(const Editor *e)
{
    size_t n = tb_len(&e->buf);
    uint32_t *cps = malloc((n ? n : 1) * sizeof(uint32_t));
    if (!cps) return NULL;
    tb_copy(&e->buf, 0, n, cps);
    size_t bytes = utf8_encoded_len(cps, n);
    char *s = malloc(bytes + 1);
    if (s) {
        utf8_encode(cps, n, (uint8_t *)s);
        s[bytes] = '\0';
    }
    free(cps);
    return s;
}

uint64_t editor_content_id(const Editor *e) { return undo_state_id(&e->undo); }

bool editor_check_invariants(Editor *e)
{
    int n = (int)tb_len(&e->buf);
    if (e->st.cursor < 0 || e->st.cursor > n) return false;
    if (e->st.select_start < 0 || e->st.select_start > n) return false;
    if (e->st.select_end < 0 || e->st.select_end > n) return false;
    ensure_layout(e);
    size_t expect = 0;
    for (size_t r = 0; r < e->layout.count; r++) {
        if (e->layout.rows[r].start != expect) return false;
        expect += e->layout.rows[r].len;
    }
    return expect == (size_t)n;
}

/* ---- status ---------------------------------------------------------------- */

const char *editor_path(const Editor *e) { return e->path; }
bool editor_is_dirty(const Editor *e) { return undo_state_id(&e->undo) != e->saved_id; }
bool editor_overwrite_armed(const Editor *e) { return overwrite_valid(e); }
const char *editor_read_only_reason(const Editor *e) { return e->read_only_reason; }

const char *editor_take_notice(Editor *e)
{
    static char out[256];
    if (!e->notice[0]) return NULL;
    snprintf(out, sizeof(out), "%s", e->notice);
    e->notice[0] = '\0';
    return out;
}

size_t editor_hit_test(Editor *e, float x, float y) { return hit_test(e, x, y); }

bool editor_index_screen_pos(Editor *e, size_t idx, float *x, float *y)
{
    ensure_layout(e);
    if (idx > tb_len(&e->buf)) return false;
    size_t r = wrap_row_for_index(&e->layout, idx);
    *x = e->text_x + row_x(e, &e->layout.rows[r], idx);
    *y = e->text_y + ((float)r + 0.5f) * e->font->line_height - e->scroll_y;
    return true;
}

bool editor_index_visible(Editor *e, size_t idx)
{
    float x, y;
    if (!editor_index_screen_pos(e, idx, &x, &y)) return false;
    return y >= e->text_y - e->top_pad && y <= e->text_y + e->view_h;
}

float editor_scroll_y(const Editor *e) { return e->scroll_y; }
void editor_set_max_chars(Editor *e, size_t max) { e->max_chars = max < EDITOR_MAX_CHARS ? max : EDITOR_MAX_CHARS; }
bool editor_is_read_only(const Editor *e) { return e->read_only; }

EditorStats editor_stats(Editor *e)
{
    if (e->stats_version != e->version) {
        size_t n = tb_len(&e->buf), words = 0;
        bool in_word = false;
        for (size_t i = 0; i < n; i++) {
            bool w = is_word_at(e, (int)i);
            if (w && !in_word) words++;
            in_word = w;
        }
        e->stats.chars = n;
        e->stats.words = words;
        e->stats.crlf = e->crlf;
        e->stats_version = e->version;
        e->stats_cursor = -1;
    }
    if (e->stats_cursor != e->st.cursor) {
        int line = 1, line_start = 0;
        for (int i = 0; i < e->st.cursor; i++) {
            if (tb_get(&e->buf, (size_t)i) == '\n') {
                line++;
                line_start = i + 1;
            }
        }
        e->stats.line = line;
        e->stats.column = e->st.cursor - line_start + 1;
        e->stats_cursor = e->st.cursor;
    }
    int lo, hi;
    sel_range(e, &lo, &hi);
    e->stats.selected = (size_t)(hi - lo);
    return e->stats;
}

/* ---- scratch buffers (chat input) ---------------------------------------- */

void editor_set_scratch(Editor *e, const char *placeholder)
{
    free(e->path);
    e->path = NULL;
    e->scratch = true;
    e->read_only = false;
    e->read_only_reason[0] = '\0';
    snprintf(e->placeholder, sizeof(e->placeholder), "%s", placeholder ? placeholder : "");
    editor_clear(e);
}

void editor_set_placeholder(Editor *e, const char *placeholder)
{
    snprintf(e->placeholder, sizeof(e->placeholder), "%s", placeholder ? placeholder : "");
}

void editor_clear(Editor *e)
{
    tb_set(&e->buf, NULL, 0);
    reset_undo(e);
    stb_textedit_initialize_state(&e->st, 0);
    e->saved_id = undo_state_id(&e->undo);
    e->scroll_y = 0;
    mark_changed(e);
}

static uint32_t *decode_dup(const char *utf8, size_t *n);

bool editor_set_text_utf8(Editor *e, const char *utf8)
{
    size_t n = 0;
    uint32_t *cps = decode_dup(utf8 ? utf8 : "", &n);
    if (!cps) return false;
    n = sanitize_text(cps, n);
    if (n > e->max_chars) n = e->max_chars;
    bool ok = tb_set(&e->buf, cps, n);
    free(cps);
    if (!ok) return false;
    reset_undo(e);
    stb_textedit_initialize_state(&e->st, 0);
    e->saved_id = undo_state_id(&e->undo);
    e->scroll_y = 0;
    mark_changed(e);
    return true;
}

void editor_set_layout(Editor *e, float side_pad, float top_pad, float max_col)
{
    e->side_pad = side_pad;
    e->top_pad = top_pad;
    e->max_col = max_col;
    e->layout_dirty = true;
}

void editor_set_submit_on_enter(Editor *e, bool on) { e->submit_on_enter = on; }
void editor_set_single_line(Editor *e, bool on) { e->single_line = on; }
void editor_set_modal_owner(Editor *e, bool on) { e->modal_owner = on; }

bool editor_take_submit(Editor *e)
{
    bool v = e->submit_pending;
    e->submit_pending = false;
    return v;
}

float editor_content_height(Editor *e)
{
    ensure_layout(e);
    return (float)e->layout.count * e->font->line_height + e->top_pad;
}

/* ---- ranges ---------------------------------------------------------------------- */

static uint32_t *decode_dup(const char *utf8, size_t *n)
{
    size_t len = strlen(utf8);
    uint32_t *cps = malloc((len + 1) * sizeof(uint32_t));
    if (cps) *n = utf8_decode((const uint8_t *)utf8, len, cps, NULL);
    return cps;
}

char *editor_range_utf8(const Editor *e, size_t start, size_t len)
{
    size_t total = tb_len(&e->buf);
    if (start > total || len > total - start) return NULL;
    uint32_t *cps = malloc((len ? len : 1) * sizeof(uint32_t));
    if (!cps) return NULL;
    tb_copy(&e->buf, start, len, cps);
    size_t bytes = utf8_encoded_len(cps, len);
    char *s = malloc(bytes + 1);
    if (s) {
        utf8_encode(cps, len, (uint8_t *)s);
        s[bytes] = '\0';
    }
    free(cps);
    return s;
}

bool editor_replace_range_utf8(Editor *e, size_t start, size_t len, const char *utf8)
{
    size_t n = 0;
    uint32_t *cps = decode_dup(utf8, &n);
    if (!cps) return false;
    n = sanitize_text(cps, n);
    bool ok = editor_replace_range(e, start, len, cps, n);
    free(cps);
    if (ok) e->anc_active = false;
    return ok;
}

size_t editor_find_utf8(const Editor *e, const char *utf8, size_t *first)
{
    size_t n = 0, count = 0, total = tb_len(&e->buf);
    uint32_t *needle = decode_dup(utf8, &n);
    if (!needle || n == 0 || n > total) {
        free(needle);
        return 0;
    }
    for (size_t i = 0; i + n <= total; i++) {
        size_t k = 0;
        while (k < n && tb_get(&e->buf, i + k) == needle[k]) k++;
        if (k == n) {
            if (count == 0 && first) *first = i;
            count++;
        }
    }
    free(needle);
    return count;
}

bool editor_find_near_utf8(const Editor *e, const char *utf8, size_t near, size_t *pos)
{
    size_t n = 0, total = tb_len(&e->buf), best = 0, best_d = SIZE_MAX;
    uint32_t *needle = decode_dup(utf8, &n);
    if (!needle || n == 0 || n > total) {
        free(needle);
        return false;
    }
    for (size_t i = 0; i + n <= total; i++) {
        size_t k = 0;
        while (k < n && tb_get(&e->buf, i + k) == needle[k]) k++;
        size_t d = i > near ? i - near : near - i;
        if (k == n && d < best_d) {
            best = i;
            best_d = d;
        }
    }
    free(needle);
    if (best_d == SIZE_MAX) return false;
    *pos = best;
    return true;
}

bool editor_anchor_set(Editor *e, size_t start, size_t len)
{
    size_t total = tb_len(&e->buf);
    if (start > total || len > total - start) return false;
    e->anc_active = true;
    e->anc_touched = false;
    e->anc_start = start;
    e->anc_len = len;
    return true;
}

bool editor_anchor_get(const Editor *e, size_t *start, size_t *len)
{
    if (!e->anc_active || e->anc_touched) return false;
    *start = e->anc_start;
    *len = e->anc_len;
    return true;
}

bool editor_anchor_active(const Editor *e) { return e->anc_active; }
void editor_anchor_clear(Editor *e) { e->anc_active = false; }

void editor_reveal(Editor *e, size_t index)
{
    size_t n = tb_len(&e->buf);
    e->st.cursor = (int)(index < n ? index : n);
    e->st.select_start = e->st.select_end = e->st.cursor;
    e->want_cursor_visible = true;
}
