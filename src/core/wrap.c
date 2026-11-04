#include "wrap.h"

#include <stdlib.h>

void wrap_free(WrapLayout *layout)
{
    free(layout->rows);
    layout->rows = NULL;
    layout->count = layout->cap = 0;
}

static int push_row(WrapLayout *l, size_t start, size_t len, float width)
{
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        WrapRow *rows = realloc(l->rows, cap * sizeof(WrapRow));
        if (!rows) return -1;
        l->rows = rows;
        l->cap = cap;
    }
    l->rows[l->count++] = (WrapRow){ start, len, width };
    return 0;
}

int wrap_compute(WrapLayout *l, const TextBuffer *tb, float max_width,
                 WrapAdvanceFn advance, void *ctx)
{
    l->count = 0;
    size_t n = tb_len(tb);
    size_t i = 0;

    while (i < n) {
        size_t row_start = i;
        float x = 0.0f;
        size_t brk = 0;       /* index just after the last break opportunity */
        float brk_width = 0.0f;
        int have_brk = 0;

        for (;;) {
            if (i >= n) {
                if (push_row(l, row_start, i - row_start, x)) return -1;
                break;
            }
            uint32_t c = tb_get(tb, i);
            if (c == '\n') {
                i++;
                if (push_row(l, row_start, i - row_start, x)) return -1;
                break;
            }
            float w = advance(c, ctx);
            /* Whitespace may hang past the edge; anything else wraps. */
            if (x + w > max_width && i > row_start && !wrap_is_space(c)) {
                if (have_brk) {
                    i = brk;
                    if (push_row(l, row_start, i - row_start, brk_width)) return -1;
                } else {
                    /* A single word wider than the line: hard break. */
                    if (push_row(l, row_start, i - row_start, x)) return -1;
                }
                break;
            }
            x += w;
            i++;
            if (wrap_is_space(c)) {
                brk = i;
                brk_width = x;
                have_brk = 1;
            }
        }
    }

    if (n == 0 || tb_get(tb, n - 1) == '\n') {
        if (push_row(l, n, 0, 0.0f)) return -1;
    }
    return 0;
}

size_t wrap_row_for_index(const WrapLayout *l, size_t idx)
{
    if (l->count == 0) return 0;
    /* Last row whose start <= idx. */
    size_t lo = 0, hi = l->count;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (l->rows[mid].start <= idx) lo = mid;
        else hi = mid;
    }
    return lo;
}
