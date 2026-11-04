/* Soft word-wrap layout over a TextBuffer.
 *
 * Produces visual rows. Each row includes its trailing '\n' if it ends a
 * paragraph. Every buffer index belongs to exactly one row, and an index on
 * a soft-wrap boundary belongs to the following row. If the text is empty or
 * ends in '\n', a final empty row (len 0) represents the last line.
 *
 * Glyph measurement goes through a callback, so layout can be tested without
 * loading fonts.
 */
#ifndef SCRIBODO_WRAP_H
#define SCRIBODO_WRAP_H

#include <stddef.h>
#include <stdint.h>

#include "text_buffer.h"

typedef float (*WrapAdvanceFn)(uint32_t cp, void *ctx);

typedef struct {
    size_t start;
    size_t len;
    float width; /* sum of advances, excluding any trailing newline */
} WrapRow;

typedef struct {
    WrapRow *rows;
    size_t count;
    size_t cap;
} WrapLayout;

void wrap_free(WrapLayout *layout);

/* Recompute all rows for `max_width`. Returns 0 on success, -1 on OOM. */
int wrap_compute(WrapLayout *layout, const TextBuffer *tb, float max_width,
                 WrapAdvanceFn advance, void *ctx);

/* Index of the row containing buffer position `idx` (0 <= idx <= len). */
size_t wrap_row_for_index(const WrapLayout *layout, size_t idx);

/* True for characters at which a line may break after. */
static inline int wrap_is_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A);
}

#endif
