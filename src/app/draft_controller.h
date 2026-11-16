/* Drafts of unsaved text: while the open document has unsaved edits, its whole text is written beside it every
 * DRAFT_INTERVAL_S seconds when it changed, under the save temp's own name, so a process that dies before the author
 * saves leaves the text where doc_recover_strays offers it on the next load (red-team 2026-11-14: a dead battery
 * lost everything since the last save). One draft at a time; a clean buffer removes it.
 *
 * The frame thread takes a snapshot of the buffer and its hash; the write itself (doc_write_draft: a new temp,
 * write, fsync, the previous draft removed, the folder fsync'd) runs on a joinable worker thread, one write at a
 * time, because its fsync tail exceeded the 50 ms frame budget on a 1.2 MB buffer (AUDIT-004 F12, measured
 * 2026-11-15 with build/draft_tick_probe; journal/audit004-main.md). All calls are from one thread. */
#ifndef SCRIBODO_DRAFT_CONTROLLER_H
#define SCRIBODO_DRAFT_CONTROLLER_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRAFT_INTERVAL_S 10.0

typedef struct {
    /* Read by the worker only between its start and its finish. */
    char doc[PATH_MAX];
    char previous[PATH_MAX];
    char *text;
    size_t len;
    uint64_t hash;
    /* Written by the worker; read by the frame thread after the join. */
    bool ok;
    char out[PATH_MAX];
} DraftWrite;

/* Test hook: runs on the writing thread just before each write (tests block it to hold a write in flight). */
typedef void (*DraftWriteHook)(void *ctx);

typedef struct {
    char path[PATH_MAX]; /* the unsaved document's draft beside it, empty when none */
    char doc[PATH_MAX];  /* the document the draft (or the write in flight) belongs to */
    double at;           /* when the draft was last considered */
    uint64_t hash;       /* the text the draft holds */
    bool busy;           /* a write is in flight */
    bool discard_pending; /* discarded while in flight: the write's file is removed when it finishes */
    bool threaded;       /* the write in flight runs on `thread` (else it ran inline) */
    pthread_t thread;
    atomic_bool done;
    DraftWrite write;
    DraftWriteHook write_hook;
    void *write_hook_ctx;
} DraftController;

/* The whole buffer as UTF-8 (malloc'd, the caller frees), or NULL when it cannot be had. */
typedef char *(*DraftTextFn)(void *ctx);

void draft_controller_init(DraftController *draft);
/* Waits for a write in flight, applies its result, and frees it; the draft file itself is left on disk. */
void draft_controller_destroy(DraftController *draft);
/* Removes the draft file, if any, and forgets it; a write in flight is removed when it finishes. */
void draft_controller_discard(DraftController *draft);
/* One frame: `doc` is the open document's path (NULL for none), `dirty` whether its buffer has unsaved edits, `now`
 * the clock in seconds. A finished write is adopted first. The text is asked for at most once per
 * DRAFT_INTERVAL_S, and only while no write is in flight; it is written only when its hash differs from the draft's.
 * Returns true when a write was started this call. */
bool draft_controller_tick(DraftController *draft, const char *doc, bool dirty, double now, DraftTextFn text, void *ctx);
/* Adopts a finished write; true while one is still in flight. Never blocks. */
bool draft_controller_poll(DraftController *draft);
/* Blocks until no write is in flight, and adopts its result (tests, shutdown). */
void draft_controller_wait(DraftController *draft);
/* Tests only. */
void draft_controller_set_write_hook(DraftController *draft, DraftWriteHook hook, void *ctx);
/* 64-bit FNV-1a of a NUL-terminated string (the draft's change check). */
uint64_t draft_text_hash(const char *s);

#endif
