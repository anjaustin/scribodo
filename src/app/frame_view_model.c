#include "frame_view_model.h"

#include <stdlib.h>
#include <string.h>

#define FRAME_STRING_BLOCK_SIZE 4096

struct FrameStringBlock {
    FrameStringBlock *next;
    size_t used;
    size_t capacity;
    char data[];
};

void frame_view_model_init(FrameViewModel *frame)
{
    if (frame) memset(frame, 0, sizeof(*frame));
}

void frame_view_model_begin(FrameViewModel *frame)
{
    if (!frame) return;
    for (FrameStringBlock *block = frame->blocks; block; block = block->next) block->used = 0;
    frame->current = frame->blocks;
    frame->generation++;
    frame->sealed = false;
}

static FrameStringBlock *frame_block(FrameViewModel *frame, size_t needed)
{
    FrameStringBlock *block = frame->current;
    while (block && block->capacity - block->used < needed) block = block->next;
    if (block) {
        frame->current = block;
        return block;
    }
    size_t capacity = needed > FRAME_STRING_BLOCK_SIZE ? needed : FRAME_STRING_BLOCK_SIZE;
    block = malloc(sizeof(*block) + capacity);
    if (!block) return NULL;
    *block = (FrameStringBlock){ .capacity = capacity };
    if (!frame->blocks) frame->blocks = block;
    else {
        FrameStringBlock *tail = frame->blocks;
        while (tail->next) tail = tail->next;
        tail->next = block;
    }
    frame->current = block;
    return block;
}

const char *frame_view_model_copy(FrameViewModel *frame, const char *text)
{
    static const char unavailable[] = "[frame text unavailable]";
    if (!frame || frame->sealed) return unavailable;
    if (!text) text = "";
    size_t length = strlen(text) + 1;
    FrameStringBlock *block = frame_block(frame, length);
    if (!block) return unavailable;
    char *copy = block->data + block->used;
    memcpy(copy, text, length);
    block->used += length;
    return copy;
}

void frame_view_model_seal(FrameViewModel *frame)
{
    if (frame) frame->sealed = true;
}

void frame_view_model_destroy(FrameViewModel *frame)
{
    if (!frame) return;
    FrameStringBlock *block = frame->blocks;
    while (block) {
        FrameStringBlock *next = block->next;
        free(block);
        block = next;
    }
    memset(frame, 0, sizeof(*frame));
}
