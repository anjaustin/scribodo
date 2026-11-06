#ifndef SCRIBODO_OBSERVER_MODEL_INTERNAL_H
#define SCRIBODO_OBSERVER_MODEL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include "llm/server.h"

typedef struct {
    LlmServer *server;       /* borrowed */
    atomic_bool *cancel;     /* borrowed from the worker owner */
} ObserverModel;

char *observer_model_extract(ObserverModel *model, const char *chunk, bool is_chat);
bool observer_model_judge(ObserverModel *model, const char *canon, const char *statement,
                          char *check, size_t checklen);

#endif
