#ifndef SCRIBODO_FRAME_VIEW_MODEL_H
#define SCRIBODO_FRAME_VIEW_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FrameStringBlock FrameStringBlock;

typedef struct {
    FrameStringBlock *blocks;
    FrameStringBlock *current;
    uint64_t generation;
    bool sealed;
} FrameViewModel;

void frame_view_model_init(FrameViewModel *frame);
void frame_view_model_begin(FrameViewModel *frame);
const char *frame_view_model_copy(FrameViewModel *frame, const char *text);
void frame_view_model_seal(FrameViewModel *frame);
void frame_view_model_destroy(FrameViewModel *frame);

#endif
