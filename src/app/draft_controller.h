/* Drafts of unsaved text: while the open document has unsaved edits, its whole text is written beside it every
 * DRAFT_INTERVAL_S seconds when it changed, under the save temp's own name, so a process that dies before the author
 * saves leaves the text where doc_recover_strays offers it on the next load (red-team 2026-11-14: a dead battery
 * lost everything since the last save). One draft at a time; a clean buffer removes it. */
#ifndef SCRIBODO_DRAFT_CONTROLLER_H
#define SCRIBODO_DRAFT_CONTROLLER_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#define DRAFT_INTERVAL_S 10.0

typedef struct {
    char path[PATH_MAX]; /* the unsaved document's draft beside it, empty when none */
    char doc[PATH_MAX];  /* the document the draft belongs to */
    double at;           /* when the draft was last considered */
    uint64_t hash;       /* the text the draft holds */
} DraftController;

/* The whole buffer as UTF-8 (malloc'd, the caller frees), or NULL when it cannot be had. */
typedef char *(*DraftTextFn)(void *ctx);

void draft_controller_init(DraftController *draft);
/* Removes the draft file, if any, and forgets it. */
void draft_controller_discard(DraftController *draft);
/* One frame: `doc` is the open document's path (NULL for none), `dirty` whether its buffer has unsaved edits, `now`
 * the clock in seconds. The text is asked for at most once per DRAFT_INTERVAL_S and written only when its hash
 * differs from the draft's. Returns true when a draft was written this call. */
bool draft_controller_tick(DraftController *draft, const char *doc, bool dirty, double now, DraftTextFn text, void *ctx);
/* 64-bit FNV-1a of a NUL-terminated string (the draft's change check). */
uint64_t draft_text_hash(const char *s);

#endif
