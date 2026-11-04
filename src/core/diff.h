/* Word-level diff for reviewing proposed edits.
 *
 * Text is split into tokens (runs of word characters, runs of whitespace,
 * and single other characters), and the token sequences are compared with
 * Myers' O(ND) algorithm. The result is a list of spans in the old and new
 * text, so a reviewer sees "tightened" replace "made tighter", not a
 * character soup.
 */
#ifndef SCRIBODO_DIFF_H
#define SCRIBODO_DIFF_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { DIFF_EQUAL, DIFF_DELETE, DIFF_INSERT } DiffKind;

typedef struct {
    DiffKind kind;
    size_t pos;  /* byte offset in old text (EQUAL/DELETE) or new text (INSERT) */
    size_t len;  /* bytes */
} DiffOp;

typedef struct {
    DiffOp *ops;
    size_t count;
    size_t deleted_bytes;
    size_t inserted_bytes;
} Diff;

/* Diff two UTF-8 strings. Consecutive ops of the same kind are merged, and a
 * DELETE always precedes the INSERT that replaces it. Returns false on
 * allocation failure or if the inputs are too different to diff cheaply
 * (more than `max_edit` token edits; 0 means a sensible default), in which
 * case the result is a single DELETE + INSERT. */
bool diff_words(const char *old_text, const char *new_text, size_t max_edit, Diff *out);
void diff_free(Diff *d);

#endif
