/* Record panel: shows and edits one note or canon entry, docked under the
 * document. It owns a scratch editor for the content, its source line, its
 * buttons and (for canon) the version history. It never writes anything
 * itself: button presses come back as actions for the app to carry out.
 *
 * States and buttons:
 *   note                 Save · To canon · Delete · Close
 *   canon draft          Commit · Discard          (nothing stored yet)
 *   canon committed      Revise · Retract · Close  (Revise stores a new version)
 *   canon retracted      Recommit · Close
 */
#ifndef SCRIBODO_RECORD_VIEW_H
#define SCRIBODO_RECORD_VIEW_H

#include <stdbool.h>

#include "app/records.h"
#include "raylib.h"
#include "ui/font.h"

typedef enum {
    RA_NONE,
    RA_SAVE,      /* note: store the edited content */
    RA_TO_CANON,  /* note: start a canon draft from it */
    RA_DELETE,    /* note */
    RA_CLOSE,     /* close (the app decides about unsaved changes) */
    RA_COMMIT,    /* canon draft: commit as version 1 */
    RA_REVISE,    /* canon: store the edited assertion as a new version */
    RA_RETRACT,
    RA_RECOMMIT,
    RA_SOURCE,    /* open the first source */
} RecordAction;

typedef struct RecordView RecordView;

RecordView *record_view_create(SFont *text_font, SFont *label_font);
void record_view_destroy(RecordView *v);

/* Show a stored record (copied), or a new canon draft (rec NULL, kind
 * REC_CANON) with `content` and `sources`. */
bool record_view_open(RecordView *v, RecordKind kind, const Record *rec, const char *content, const SourceRef *sources,
                      size_t nsources);
void record_view_close(RecordView *v);
bool record_view_active(const RecordView *v);

RecordKind record_view_kind(const RecordView *v);
const char *record_view_id(const RecordView *v);     /* "" for a draft */
const char *record_view_status(const RecordView *v); /* "draft" for a draft */
const SourceRef *record_view_sources(const RecordView *v, size_t *n);
/* Current editor text (malloc'd). */
char *record_view_text(const RecordView *v);
/* Edited since it was opened or last stored. */
bool record_view_dirty(const RecordView *v);
/* After the app stored a version: becomes the record's saved state. */
void record_view_stored(RecordView *v, const char *id, int ver, const char *status, const char *content);

float record_view_height(RecordView *v, float width, float max_h);
/* Mouse and keyboard; `focused` routes typing to the content editor. */
void record_view_update(RecordView *v, Rectangle r, bool focused);
/* Draws, and returns the button pressed this frame. */
RecordAction record_view_draw(RecordView *v, Rectangle r, bool focused);
/* The content editor's rect (clicks there focus the panel). */
Rectangle record_view_editor_rect(const RecordView *v);
/* Named parts for UI scripts: "RecordSave", "RecordHistory0", ... */
Rectangle record_view_element(const RecordView *v, const char *name);
const char *record_view_header(const RecordView *v);

#endif
