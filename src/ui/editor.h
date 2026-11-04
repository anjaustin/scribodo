/* Native prose editor surface.
 *
 * Cursor, selection, and navigation logic come from stb_textedit; storage is
 * a codepoint gap buffer; undo is our own grouped log (stb_textedit's undo is
 * fixed-size and per-character). Text is soft-wrapped in a centered column.
 */
#ifndef SCRIBODO_EDITOR_H
#define SCRIBODO_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raylib.h"
#include "ui/font.h"

typedef struct Editor Editor;

typedef struct {
    int line;          /* 1-based paragraph line */
    int column;        /* 1-based, in codepoints */
    size_t chars;
    size_t words;
    size_t selected;   /* codepoints selected */
    bool crlf;
} EditorStats;

Editor *editor_create(SFont *font);
void editor_destroy(Editor *e);

/* Load a UTF-8 text file. A missing file opens as an empty document that is
 * created on first save. Invalid UTF-8 opens read-only so that saving can
 * never destroy bytes we could not decode. */
bool editor_load_file(Editor *e, const char *path, char *err, size_t errlen);

/* Save via doc_save(): atomic, symlink- and permission-preserving, keeps BOM
 * and line endings. If the file changed on disk since load/save, nothing is
 * overwritten: the author's text goes to a sibling conflict copy and the
 * overwrite is armed (see editor_overwrite_armed). */
bool editor_save(Editor *e, char *err, size_t errlen);

/* Process input. `bounds` is the editor's on-screen rect this frame. */
void editor_update(Editor *e, Rectangle bounds, bool focused);
void editor_draw(Editor *e, Rectangle bounds, bool focused);

/* Replace [pos, pos+len) with `cps` as a single undoable action. This is the
 * entry point for applying reviewed AI edits. */
bool editor_replace_range(Editor *e, size_t pos, size_t len, const uint32_t *cps, size_t n);

/* Programmatic input, mirroring what the keyboard handler does. Used by
 * tests now, and later by reviewed-edit application and automation. */
typedef enum {
    EDITOR_KEY_LEFT, EDITOR_KEY_RIGHT, EDITOR_KEY_UP, EDITOR_KEY_DOWN,
    EDITOR_KEY_WORD_LEFT, EDITOR_KEY_WORD_RIGHT,
    EDITOR_KEY_LINE_START, EDITOR_KEY_LINE_END,
    EDITOR_KEY_TEXT_START, EDITOR_KEY_TEXT_END,
    EDITOR_KEY_PAGE_UP, EDITOR_KEY_PAGE_DOWN,
    /* Editing keys (ignored when read-only) */
    EDITOR_KEY_BACKSPACE, EDITOR_KEY_DELETE,
    EDITOR_KEY_BACKSPACE_WORD, EDITOR_KEY_DELETE_WORD, EDITOR_KEY_BACKSPACE_LINE,
    EDITOR_KEY_COUNT
} EditorKey;

void editor_input_char(Editor *e, uint32_t cp);
void editor_input_key(Editor *e, EditorKey key, bool shift);
bool editor_paste_utf8(Editor *e, const char *utf8);
bool editor_cut_selection(Editor *e, char **utf8_out); /* caller frees */
void editor_undo(Editor *e);
void editor_redo(Editor *e);
void editor_select_all(Editor *e);
void editor_set_selection(Editor *e, size_t anchor, size_t cursor);
void editor_get_selection(const Editor *e, size_t *anchor, size_t *cursor);

size_t editor_length(const Editor *e);
char *editor_text_utf8(const Editor *e); /* caller frees */
/* Changes whenever content changes; equal ids imply identical content. */
uint64_t editor_content_id(const Editor *e);
/* Cursor/selection in range and layout rows tile the buffer exactly. */
bool editor_check_invariants(Editor *e);

const char *editor_path(const Editor *e);
/* True for a short window after a save was refused because the file changed
 * on disk (the author's text went to a conflict copy). Saving again in that
 * window, without further edits, overwrites the on-disk version. */
bool editor_overwrite_armed(const Editor *e);
const char *editor_read_only_reason(const Editor *e);
/* One-shot message for the status bar (e.g. size limit hit), or NULL. */
const char *editor_take_notice(Editor *e);
/* Buffer position at text-column coordinates (y includes scroll). */
size_t editor_hit_test(Editor *e, float x, float y);
/* Screen position (caret x, row middle y) of buffer index `idx`, using the
 * geometry from the latest editor_update(). */
bool editor_index_screen_pos(Editor *e, size_t idx, float *x, float *y);
bool editor_index_visible(Editor *e, size_t idx);
float editor_scroll_y(const Editor *e);
/* ---- ranges (for reviewed AI edits) --------------------------------------
 * Positions are codepoint indices, as returned by editor_get_selection. */

/* Text of [start, start+len) as UTF-8 (malloc'd), or NULL if out of range. */
char *editor_range_utf8(const Editor *e, size_t start, size_t len);
/* Replace [start, start+len) with UTF-8 text as one undoable action. */
bool editor_replace_range_utf8(Editor *e, size_t start, size_t len, const char *utf8);
/* Number of occurrences of `utf8` in the document; *first gets the first. */
size_t editor_find_utf8(const Editor *e, const char *utf8, size_t *first);
/* Highlight a range (e.g. the passage a proposal targets); len 0 clears. */
void editor_set_highlight(Editor *e, size_t start, size_t len);
/* Scroll so that `index` is visible. */
void editor_reveal(Editor *e, size_t index);

/* Scratch mode: an editable buffer with no file (e.g. the chat input).
 * Clears the buffer. `placeholder` is shown while it is empty. */
void editor_set_scratch(Editor *e, const char *placeholder);
void editor_clear(Editor *e);
/* Padding and maximum text-column width (defaults suit the prose view). */
void editor_set_layout(Editor *e, float side_pad, float top_pad, float max_col);
/* Enter submits (see editor_take_submit) and Shift+Enter inserts a newline. */
void editor_set_submit_on_enter(Editor *e, bool on);
bool editor_take_submit(Editor *e);
/* Height of the laid-out text, for auto-sizing an input box. */
float editor_content_height(Editor *e);

/* Lower the size limit (tests). Never raises it above the built-in cap. */
void editor_set_max_chars(Editor *e, size_t max);
bool editor_is_dirty(const Editor *e);
bool editor_is_read_only(const Editor *e);
EditorStats editor_stats(Editor *e);

#endif
