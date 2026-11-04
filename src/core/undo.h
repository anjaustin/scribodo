/* Grouped, unbounded undo log.
 *
 * Each user action (a keystroke, paste, AI edit, ...) is one group of
 * primitive insert/delete operations. Consecutive typing or deleting at the
 * same cursor position is merged into one group so undo works per word
 * rather than per character.
 *
 * The log records edits; it does not perform them. Callers mutate the
 * TextBuffer and report each change via undo_record_*. undo_undo/redo apply
 * inverse/forward operations directly to the buffer.
 */
#ifndef SCRIBODO_UNDO_H
#define SCRIBODO_UNDO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "text_buffer.h"

typedef enum {
    UNDO_MERGE_NONE = 0,
    UNDO_MERGE_TYPING,
    UNDO_MERGE_BACKSPACE,
    UNDO_MERGE_DELETE,
} UndoMergeKind;

typedef struct {
    int cursor;
    int sel_start;
    int sel_end;
} UndoCursor;

typedef struct {
    bool insert; /* true: text was inserted; false: text was deleted */
    size_t pos;
    size_t len;
    size_t text_off; /* offset into the owning group's text arena */
} UndoOp;

typedef struct {
    UndoOp *ops;
    size_t nops, cap_ops;
    uint32_t *text;
    size_t ntext, cap_text;
    UndoCursor before, after;
    UndoMergeKind kind;
    double last_time;
    uint64_t id;
    bool sealed;
} UndoGroup;

typedef struct {
    UndoGroup *groups;
    size_t count; /* groups stored (applied + redoable) */
    size_t top;   /* groups currently applied */
    size_t cap;
    long open;    /* index of the group being recorded; UNDO_OPEN_NONE/PENDING */
    UndoCursor pending_before;
    UndoMergeKind pending_kind;
    uint64_t base_id; /* state id when no group is applied */
    uint64_t next_id;
} UndoLog;

void undo_init(UndoLog *log);
void undo_free(UndoLog *log);

/* Start recording an action. If `allow_merge` and the previous group has the
 * same non-NONE kind, was not sealed, ended where this action begins, and is
 * recent (within 2 s of `now`), the action is appended to it. Otherwise a new
 * group is started and any redo history is discarded once it records an op. */
void undo_begin(UndoLog *log, UndoCursor before, UndoMergeKind kind, bool allow_merge, double now);

/* Record a primitive change. Ignored if no action is open. For deletes,
 * `cps` is the text that was removed (captured before deleting). */
bool undo_record_insert(UndoLog *log, size_t pos, const uint32_t *cps, size_t n);
bool undo_record_delete(UndoLog *log, size_t pos, const uint32_t *cps, size_t n);

/* Finish the open action. A new group with no operations is dropped. */
void undo_end(UndoLog *log, UndoCursor after, double now);

/* Drop all history (e.g. after a recording failure made it unreliable). */
void undo_clear(UndoLog *log);

/* Prevent further merging into the most recent group. */
void undo_seal(UndoLog *log);

bool undo_can_undo(const UndoLog *log);
bool undo_can_redo(const UndoLog *log);

/* Revert / reapply one group on `tb`. On success `*cursor` receives the
 * cursor state to restore. Buffer capacity is reserved up front, so a group
 * is applied completely or not at all. */
bool undo_undo(UndoLog *log, TextBuffer *tb, UndoCursor *cursor);
bool undo_redo(UndoLog *log, TextBuffer *tb, UndoCursor *cursor);

/* Identifier of the current content state; equal ids mean equal content.
 * A fresh log starts at 0; undo_clear() moves to a new, never-used id. */
uint64_t undo_state_id(const UndoLog *log);

#endif
