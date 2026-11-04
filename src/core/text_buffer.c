#include "text_buffer.h"

#include <stdlib.h>
#include <string.h>

#define TB_MIN_GAP 1024

void tb_init(TextBuffer *tb)
{
    memset(tb, 0, sizeof(*tb));
}

void tb_free(TextBuffer *tb)
{
    free(tb->data);
    memset(tb, 0, sizeof(*tb));
}

static void move_gap(TextBuffer *tb, size_t pos)
{
    if (pos == tb->gap_start) return;
    size_t gap = tb->gap_end - tb->gap_start;
    if (pos < tb->gap_start) {
        size_t n = tb->gap_start - pos;
        memmove(tb->data + pos + gap, tb->data + pos, n * sizeof(uint32_t));
    } else {
        size_t n = pos - tb->gap_start;
        memmove(tb->data + tb->gap_start, tb->data + tb->gap_end, n * sizeof(uint32_t));
    }
    tb->gap_start = pos;
    tb->gap_end = pos + gap;
}

static bool ensure_gap(TextBuffer *tb, size_t needed)
{
    size_t gap = tb->gap_end - tb->gap_start;
    if (gap >= needed) return true;

    size_t len = tb_len(tb);
    const size_t max_cap = SIZE_MAX / sizeof(uint32_t) / 2;
    if (needed > max_cap - len - TB_MIN_GAP) return false;
    size_t new_cap = tb->cap ? tb->cap : TB_MIN_GAP;
    while (new_cap - len < needed + TB_MIN_GAP) {
        if (new_cap > max_cap) return false;
        new_cap *= 2;
    }

    uint32_t *data = realloc(tb->data, new_cap * sizeof(uint32_t));
    if (!data) return false;

    /* Shift the post-gap tail to the end of the enlarged allocation. */
    size_t tail = tb->cap - tb->gap_end;
    memmove(data + new_cap - tail, data + tb->gap_end, tail * sizeof(uint32_t));
    tb->data = data;
    tb->gap_end = new_cap - tail;
    tb->cap = new_cap;
    return true;
}

bool tb_reserve(TextBuffer *tb, size_t n)
{
    return ensure_gap(tb, n);
}

bool tb_insert(TextBuffer *tb, size_t pos, const uint32_t *cps, size_t n)
{
    if (n == 0) return true;
    size_t len = tb_len(tb);
    if (pos > len) pos = len;
    if (!ensure_gap(tb, n)) return false;
    move_gap(tb, pos);
    memcpy(tb->data + tb->gap_start, cps, n * sizeof(uint32_t));
    tb->gap_start += n;
    return true;
}

void tb_delete(TextBuffer *tb, size_t pos, size_t n)
{
    size_t len = tb_len(tb);
    if (pos >= len || n == 0) return;
    if (n > len - pos) n = len - pos;
    move_gap(tb, pos);
    tb->gap_end += n;
}

void tb_copy(const TextBuffer *tb, size_t pos, size_t n, uint32_t *out)
{
    size_t end = pos + n;
    if (end <= tb->gap_start) {
        memcpy(out, tb->data + pos, n * sizeof(uint32_t));
    } else if (pos >= tb->gap_start) {
        size_t gap = tb->gap_end - tb->gap_start;
        memcpy(out, tb->data + pos + gap, n * sizeof(uint32_t));
    } else {
        size_t first = tb->gap_start - pos;
        memcpy(out, tb->data + pos, first * sizeof(uint32_t));
        memcpy(out + first, tb->data + tb->gap_end, (n - first) * sizeof(uint32_t));
    }
}

bool tb_set(TextBuffer *tb, const uint32_t *cps, size_t n)
{
    if (n > SIZE_MAX / sizeof(uint32_t) - TB_MIN_GAP) return false;
    size_t cap = n + TB_MIN_GAP;
    uint32_t *data = malloc(cap * sizeof(uint32_t));
    if (!data) return false;
    if (n) memcpy(data, cps, n * sizeof(uint32_t));
    free(tb->data);
    tb->data = data;
    tb->cap = cap;
    tb->gap_start = n;
    tb->gap_end = cap;
    return true;
}
