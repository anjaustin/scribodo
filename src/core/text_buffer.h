/* Gap buffer of Unicode codepoints.
 *
 * Positions are codepoint indices. Storing UTF-32 keeps cursor arithmetic
 * trivial and makes every index a valid character boundary; files are
 * converted to/from UTF-8 at load and save time.
 */
#ifndef SCRIBODO_TEXT_BUFFER_H
#define SCRIBODO_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TextBuffer {
    uint32_t *data;
    size_t cap;
    size_t gap_start;
    size_t gap_end;
} TextBuffer;

void tb_init(TextBuffer *tb);
void tb_free(TextBuffer *tb);

static inline size_t tb_len(const TextBuffer *tb)
{
    return tb->cap - (tb->gap_end - tb->gap_start);
}

static inline uint32_t tb_get(const TextBuffer *tb, size_t i)
{
    return i < tb->gap_start ? tb->data[i] : tb->data[i + (tb->gap_end - tb->gap_start)];
}

/* Insert `n` codepoints at `pos` (clamped to the length). Returns false on
 * allocation failure, leaving the buffer unchanged. */
bool tb_insert(TextBuffer *tb, size_t pos, const uint32_t *cps, size_t n);

/* Ensure `n` more codepoints can be inserted without allocating. */
bool tb_reserve(TextBuffer *tb, size_t n);

/* Delete `n` codepoints starting at `pos` (clamped to the buffer). */
void tb_delete(TextBuffer *tb, size_t pos, size_t n);

/* Copy `n` codepoints starting at `pos` into `out`. Range must be valid. */
void tb_copy(const TextBuffer *tb, size_t pos, size_t n, uint32_t *out);

/* Replace the entire contents. Returns false on allocation failure. */
bool tb_set(TextBuffer *tb, const uint32_t *cps, size_t n);

#endif
