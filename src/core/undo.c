#include "undo.h"

#include <stdlib.h>
#include <string.h>

#define UNDO_MERGE_WINDOW_S 2.0
#define UNDO_OPEN_NONE    (-1L) /* no action in progress */
#define UNDO_OPEN_PENDING (-2L) /* action in progress; group created on first op */

static void group_free(UndoGroup *g)
{
    free(g->ops);
    free(g->text);
    memset(g, 0, sizeof(*g));
}

void undo_init(UndoLog *log)
{
    memset(log, 0, sizeof(*log));
    log->open = UNDO_OPEN_NONE;
    log->next_id = 1;
}

void undo_free(UndoLog *log)
{
    for (size_t i = 0; i < log->count; i++) group_free(&log->groups[i]);
    free(log->groups);
    memset(log, 0, sizeof(*log));
    log->open = UNDO_OPEN_NONE;
}

static void discard_redo(UndoLog *log)
{
    for (size_t i = log->top; i < log->count; i++) group_free(&log->groups[i]);
    log->count = log->top;
}

void undo_begin(UndoLog *log, UndoCursor before, UndoMergeKind kind, bool allow_merge, double now)
{
    if (allow_merge && kind != UNDO_MERGE_NONE && log->top > 0 && log->top == log->count) {
        UndoGroup *prev = &log->groups[log->top - 1];
        bool no_selection = before.sel_start == before.sel_end;
        if (!prev->sealed && prev->kind == kind && no_selection &&
            prev->after.cursor == before.cursor &&
            now - prev->last_time <= UNDO_MERGE_WINDOW_S) {
            log->open = (long)(log->top - 1);
            return;
        }
    }
    log->open = UNDO_OPEN_PENDING;
    log->pending_before = before;
    log->pending_kind = kind;
}

static UndoGroup *open_group(UndoLog *log)
{
    if (log->open == UNDO_OPEN_NONE) return NULL;
    if (log->open >= 0) return &log->groups[log->open];

    /* First op of a new action: create its group now. */
    discard_redo(log);
    if (log->count == log->cap) {
        size_t cap = log->cap ? log->cap * 2 : 64;
        UndoGroup *g = realloc(log->groups, cap * sizeof(UndoGroup));
        if (!g) return NULL;
        log->groups = g;
        log->cap = cap;
    }
    UndoGroup *g = &log->groups[log->count];
    memset(g, 0, sizeof(*g));
    g->before = log->pending_before;
    g->after = log->pending_before;
    g->kind = log->pending_kind;
    log->open = (long)log->count;
    log->count++;
    log->top = log->count;
    return g;
}

static bool reserve_text(UndoGroup *g, size_t extra)
{
    if (g->ntext + extra <= g->cap_text) return true;
    size_t cap = g->cap_text ? g->cap_text : 64;
    while (cap < g->ntext + extra) cap *= 2;
    uint32_t *t = realloc(g->text, cap * sizeof(uint32_t));
    if (!t) return false;
    g->text = t;
    g->cap_text = cap;
    return true;
}

static UndoOp *push_op(UndoGroup *g)
{
    if (g->nops == g->cap_ops) {
        size_t cap = g->cap_ops ? g->cap_ops * 2 : 4;
        UndoOp *ops = realloc(g->ops, cap * sizeof(UndoOp));
        if (!ops) return NULL;
        g->ops = ops;
        g->cap_ops = cap;
    }
    return &g->ops[g->nops++];
}

static bool record(UndoLog *log, bool insert, size_t pos, const uint32_t *cps, size_t n)
{
    if (n == 0) return true;
    UndoGroup *g = open_group(log);
    if (!g) return log->open == UNDO_OPEN_NONE;
    if (!reserve_text(g, n)) return false;

    UndoOp *last = g->nops ? &g->ops[g->nops - 1] : NULL;
    /* The last op's text always sits at the end of the arena, so it can be
     * extended in place. */
    if (last && last->insert && insert && last->pos + last->len == pos) {
        memcpy(g->text + g->ntext, cps, n * sizeof(uint32_t));
        g->ntext += n;
        last->len += n;
    } else if (last && !last->insert && !insert && pos == last->pos) {
        /* Forward delete: the removed text follows what was removed before. */
        memcpy(g->text + g->ntext, cps, n * sizeof(uint32_t));
        g->ntext += n;
        last->len += n;
    } else if (last && !last->insert && !insert && pos + n == last->pos) {
        /* Backspace: the removed text precedes what was removed before. */
        uint32_t *t = g->text + last->text_off;
        memmove(t + n, t, last->len * sizeof(uint32_t));
        memcpy(t, cps, n * sizeof(uint32_t));
        g->ntext += n;
        last->pos = pos;
        last->len += n;
    } else {
        UndoOp *op = push_op(g);
        if (!op) return false;
        op->insert = insert;
        op->pos = pos;
        op->len = n;
        op->text_off = g->ntext;
        memcpy(g->text + g->ntext, cps, n * sizeof(uint32_t));
        g->ntext += n;
    }
    /* Content changed: this group now represents a new state. */
    g->id = log->next_id++;
    return true;
}

bool undo_record_insert(UndoLog *log, size_t pos, const uint32_t *cps, size_t n)
{
    return record(log, true, pos, cps, n);
}

bool undo_record_delete(UndoLog *log, size_t pos, const uint32_t *cps, size_t n)
{
    return record(log, false, pos, cps, n);
}

void undo_end(UndoLog *log, UndoCursor after, double now)
{
    if (log->open >= 0) {
        UndoGroup *g = &log->groups[log->open];
        g->after = after;
        g->last_time = now;
    }
    log->open = UNDO_OPEN_NONE;
}

void undo_clear(UndoLog *log)
{
    uint64_t next = log->next_id;
    undo_free(log);
    undo_init(log);
    /* Never reuse ids: the cleared log describes content that is not the
     * same as any earlier state, including the original base state. */
    log->base_id = next;
    log->next_id = next + 1;
}

static bool reserve_for(TextBuffer *tb, const UndoGroup *g, bool inserting_inserts)
{
    size_t total = 0;
    for (size_t i = 0; i < g->nops; i++)
        if (g->ops[i].insert == inserting_inserts) total += g->ops[i].len;
    return tb_reserve(tb, total);
}

void undo_seal(UndoLog *log)
{
    if (log->top > 0) log->groups[log->top - 1].sealed = true;
}

bool undo_can_undo(const UndoLog *log) { return log->top > 0; }
bool undo_can_redo(const UndoLog *log) { return log->top < log->count; }

bool undo_undo(UndoLog *log, TextBuffer *tb, UndoCursor *cursor)
{
    if (log->top == 0) return false;
    UndoGroup *g = &log->groups[log->top - 1];
    if (!reserve_for(tb, g, false)) return false; /* undo re-inserts deletions */
    for (size_t i = g->nops; i-- > 0;) {
        UndoOp *op = &g->ops[i];
        if (op->insert) tb_delete(tb, op->pos, op->len);
        else tb_insert(tb, op->pos, g->text + op->text_off, op->len);
    }
    g->sealed = true;
    log->top--;
    if (cursor) *cursor = g->before;
    return true;
}

bool undo_redo(UndoLog *log, TextBuffer *tb, UndoCursor *cursor)
{
    if (log->top == log->count) return false;
    UndoGroup *g = &log->groups[log->top];
    if (!reserve_for(tb, g, true)) return false;
    for (size_t i = 0; i < g->nops; i++) {
        UndoOp *op = &g->ops[i];
        if (!op->insert) tb_delete(tb, op->pos, op->len);
        else tb_insert(tb, op->pos, g->text + op->text_off, op->len);
    }
    g->sealed = true;
    log->top++;
    if (cursor) *cursor = g->after;
    return true;
}

uint64_t undo_state_id(const UndoLog *log)
{
    return log->top > 0 ? log->groups[log->top - 1].id : log->base_id;
}
