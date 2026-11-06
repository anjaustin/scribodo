#ifndef SCRIBODO_REVIEWED_EDIT_CONTROLLER_H
#define SCRIBODO_REVIEWED_EDIT_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

#include "ui/editor.h"
#include "ui/review_view.h"

typedef struct {
    bool active;
    size_t index;
    char *original;
    char *instruction;
} PendingReviewedEdit;

typedef struct {
    bool active;
    char *original;
    char *replacement;
    char *note;
    char doc[PATH_MAX];
} ReviewedEditProposal;

typedef struct {
    ReviewView *review;
    Editor *editor; /* borrowed; must outlive destroy */
    bool edit_mode;
    char *edit_text;
    char edit_banner[240];
    PendingReviewedEdit pending;
    ReviewedEditProposal proposal;
} ReviewedEditController;

void reviewed_edit_controller_init(ReviewedEditController *controller);
/* Adopts `review`; borrows `editor`. May be called only on an empty controller. */
bool reviewed_edit_controller_adopt(ReviewedEditController *controller, Editor *editor, ReviewView *review);
void reviewed_edit_controller_clear_proposal(ReviewedEditController *controller);
void reviewed_edit_controller_clear_pending(ReviewedEditController *controller);
void reviewed_edit_controller_leave_mode(ReviewedEditController *controller);

typedef enum {
    REVIEWED_EDIT_OK,
    REVIEWED_EDIT_NO_DOCUMENT,
    REVIEWED_EDIT_EMPTY_REPLY,
    REVIEWED_EDIT_OUT_OF_MEMORY,
    REVIEWED_EDIT_NOT_EDITABLE,
    REVIEWED_EDIT_NO_SELECTION,
    REVIEWED_EDIT_BUSY,
    REVIEWED_EDIT_WRONG_DOCUMENT,
    REVIEWED_EDIT_CHANGED,
    REVIEWED_EDIT_HISTORY_BEFORE_FAILED,
    REVIEWED_EDIT_REPLACE_FAILED,
    REVIEWED_EDIT_HISTORY_AFTER_FAILED
} ReviewedEditResult;

typedef bool (*ReviewedEditRecordRevision)(void *context, const char *origin, const char *note);

ReviewedEditResult reviewed_edit_controller_propose(ReviewedEditController *controller, size_t start, size_t len,
                                                     const char *original, const char *replacement, const char *note,
                                                     bool truncated);
ReviewedEditResult reviewed_edit_controller_begin(ReviewedEditController *controller);
ReviewedEditResult reviewed_edit_controller_track_request(ReviewedEditController *controller, size_t message_index,
                                                           const char *original, const char *instruction,
                                                           size_t anchor_start, size_t anchor_length);
bool reviewed_edit_controller_take_finished(ReviewedEditController *controller, size_t message_index,
                                             char **original, char **instruction);
ReviewedEditResult reviewed_edit_controller_accept(ReviewedEditController *controller,
                                                    ReviewedEditRecordRevision record_revision, void *context);
void reviewed_edit_controller_destroy(ReviewedEditController *controller);

#endif
