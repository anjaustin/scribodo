#include "reviewed_edit_controller.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void reviewed_edit_controller_init(ReviewedEditController *controller)
{
    if (controller) memset(controller, 0, sizeof(*controller));
}

bool reviewed_edit_controller_adopt(ReviewedEditController *controller, Editor *editor, ReviewView *review)
{
    if (!controller || controller->editor || controller->review) return false;
    controller->editor = editor;
    controller->review = review;
    return true;
}

void reviewed_edit_controller_clear_proposal(ReviewedEditController *controller)
{
    if (!controller) return;
    free(controller->proposal.original);
    free(controller->proposal.replacement);
    free(controller->proposal.note);
    memset(&controller->proposal, 0, sizeof(controller->proposal));
    if (controller->review) review_clear(controller->review);
    if (controller->editor) editor_anchor_clear(controller->editor);
}

void reviewed_edit_controller_clear_pending(ReviewedEditController *controller)
{
    if (!controller) return;
    free(controller->pending.original);
    free(controller->pending.instruction);
    memset(&controller->pending, 0, sizeof(controller->pending));
}

void reviewed_edit_controller_leave_mode(ReviewedEditController *controller)
{
    if (!controller) return;
    controller->edit_mode = false;
    free(controller->edit_text);
    controller->edit_text = NULL;
}

ReviewedEditResult reviewed_edit_controller_propose(ReviewedEditController *controller, size_t start, size_t len,
                                                     const char *original, const char *replacement, const char *note,
                                                     bool truncated)
{
    reviewed_edit_controller_clear_proposal(controller);
    if (!controller || !controller->editor || !editor_path(controller->editor)) return REVIEWED_EDIT_NO_DOCUMENT;
    if (!editor_anchor_set(controller->editor, start, len)) return REVIEWED_EDIT_CHANGED;
    if (!replacement || !replacement[0]) {
        editor_anchor_clear(controller->editor);
        return REVIEWED_EDIT_EMPTY_REPLY;
    }
    controller->proposal.original = strdup(original ? original : "");
    controller->proposal.replacement = strdup(replacement);
    controller->proposal.note = strdup(note ? note : "");
    snprintf(controller->proposal.doc, sizeof(controller->proposal.doc), "%s", editor_path(controller->editor));
    if (!controller->proposal.original || !controller->proposal.replacement || !controller->proposal.note ||
        !controller->review || !review_set(controller->review, original ? original : "", replacement, note, truncated)) {
        reviewed_edit_controller_clear_proposal(controller);
        return REVIEWED_EDIT_OUT_OF_MEMORY;
    }
    controller->proposal.active = true;
    editor_reveal(controller->editor, start);
    return REVIEWED_EDIT_OK;
}

ReviewedEditResult reviewed_edit_controller_begin(ReviewedEditController *controller)
{
    if (!controller || !controller->editor || !editor_path(controller->editor) || editor_is_read_only(controller->editor))
        return REVIEWED_EDIT_NOT_EDITABLE;
    size_t a, c;
    editor_get_selection(controller->editor, &a, &c);
    size_t lo = a < c ? a : c, hi = a < c ? c : a;
    if (hi == lo) return REVIEWED_EDIT_NO_SELECTION;
    if (controller->pending.active) return REVIEWED_EDIT_BUSY;
    reviewed_edit_controller_leave_mode(controller);
    reviewed_edit_controller_clear_proposal(controller);
    controller->edit_text = editor_range_utf8(controller->editor, lo, hi - lo);
    if (!controller->edit_text) return REVIEWED_EDIT_OUT_OF_MEMORY;
    controller->edit_mode = true;
    char first[80];
    snprintf(first, sizeof(first), "%.60s", controller->edit_text);
    for (char *p = first; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    size_t length = strlen(first);
    while (length > 0 && ((unsigned char)first[length - 1] & 0xC0) == 0x80) length--;
    if (length > 0 && ((unsigned char)first[length - 1] & 0xC0) == 0xC0) length--;
    first[length] = '\0';
    snprintf(controller->edit_banner, sizeof(controller->edit_banner),
             "\xE2\x9C\x8E Editing \xE2\x80\x9C%s%s\xE2\x80\x9D \xE2\x80\x94 describe the change", first,
             strlen(controller->edit_text) > length ? "\xE2\x80\xA6" : "");
    if (!editor_anchor_set(controller->editor, lo, hi - lo)) {
        reviewed_edit_controller_leave_mode(controller);
        return REVIEWED_EDIT_CHANGED;
    }
    return REVIEWED_EDIT_OK;
}

ReviewedEditResult reviewed_edit_controller_track_request(ReviewedEditController *controller, size_t message_index,
                                                           const char *original, const char *instruction,
                                                           size_t anchor_start, size_t anchor_length)
{
    if (!controller || !controller->editor) return REVIEWED_EDIT_NO_DOCUMENT;
    char *original_copy = strdup(original ? original : "");
    char *instruction_copy = strdup(instruction ? instruction : "");
    if (!original_copy || !instruction_copy) {
        free(original_copy);
        free(instruction_copy);
        return REVIEWED_EDIT_OUT_OF_MEMORY;
    }
    reviewed_edit_controller_clear_proposal(controller);
    reviewed_edit_controller_clear_pending(controller);
    if (!editor_anchor_set(controller->editor, anchor_start, anchor_length)) {
        free(original_copy);
        free(instruction_copy);
        return REVIEWED_EDIT_CHANGED;
    }
    controller->pending.active = true;
    controller->pending.index = message_index;
    controller->pending.original = original_copy;
    controller->pending.instruction = instruction_copy;
    reviewed_edit_controller_leave_mode(controller);
    return REVIEWED_EDIT_OK;
}

bool reviewed_edit_controller_take_finished(ReviewedEditController *controller, size_t message_index,
                                             char **original, char **instruction)
{
    if (original) *original = NULL;
    if (instruction) *instruction = NULL;
    if (!controller || !controller->pending.active || controller->pending.index != message_index) return false;
    if (original) *original = controller->pending.original;
    else free(controller->pending.original);
    if (instruction) *instruction = controller->pending.instruction;
    else free(controller->pending.instruction);
    controller->pending.original = NULL;
    controller->pending.instruction = NULL;
    controller->pending.active = false;
    controller->pending.index = 0;
    return true;
}

ReviewedEditResult reviewed_edit_controller_accept(ReviewedEditController *controller,
                                                    ReviewedEditRecordRevision record_revision, void *context)
{
    if (!controller || !controller->proposal.active) return REVIEWED_EDIT_NO_DOCUMENT;
    Editor *editor = controller->editor;
    if (!editor || !editor_path(editor) || strcmp(controller->proposal.doc, editor_path(editor))) {
        reviewed_edit_controller_clear_proposal(controller);
        return REVIEWED_EDIT_WRONG_DOCUMENT;
    }
    if (editor_is_read_only(editor)) return REVIEWED_EDIT_NOT_EDITABLE;
    size_t start = 0, len = 0;
    bool same = editor_anchor_get(editor, &start, &len);
    if (same) {
        char *now = editor_range_utf8(editor, start, len);
        same = now && !strcmp(now, controller->proposal.original);
        free(now);
    }
    if (!same) {
        reviewed_edit_controller_clear_proposal(controller);
        return REVIEWED_EDIT_CHANGED;
    }
    if (record_revision && !record_revision(context, "before-ai-edit", controller->proposal.note))
        return REVIEWED_EDIT_HISTORY_BEFORE_FAILED;
    if (!editor_replace_range_utf8(editor, start, len, controller->proposal.replacement))
        return REVIEWED_EDIT_REPLACE_FAILED;
    bool after = !record_revision || record_revision(context, "ai-edit", controller->proposal.note);
    reviewed_edit_controller_clear_proposal(controller);
    return after ? REVIEWED_EDIT_OK : REVIEWED_EDIT_HISTORY_AFTER_FAILED;
}

void reviewed_edit_controller_destroy(ReviewedEditController *controller)
{
    if (!controller) return;
    reviewed_edit_controller_clear_proposal(controller);
    reviewed_edit_controller_clear_pending(controller);
    reviewed_edit_controller_leave_mode(controller);
    review_destroy(controller->review);
    memset(controller, 0, sizeof(*controller));
}
