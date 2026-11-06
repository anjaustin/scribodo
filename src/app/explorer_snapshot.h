#ifndef SCRIBODO_EXPLORER_SNAPSHOT_H
#define SCRIBODO_EXPLORER_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/chat_store.h"

#define EXPLORER_MAX_DOCS 200

typedef struct {
    char *docs[EXPLORER_MAX_DOCS];
    size_t ndocs;
    ChatSummary *chats;
    char (*chat_labels)[200];
    size_t nchats;
    uint64_t generation;
    uint64_t project_generation;
} ExplorerSnapshot;

void explorer_snapshot_init(ExplorerSnapshot *snapshot);
bool explorer_snapshot_build(ExplorerSnapshot *next, StoreManager *store, const char *project_dir, uint64_t generation,
                             uint64_t project_generation);
void explorer_snapshot_destroy(ExplorerSnapshot *snapshot);
const char *explorer_snapshot_doc(const ExplorerSnapshot *snapshot, uint64_t generation, uint64_t project_generation,
                                  size_t index);
const ChatSummary *explorer_snapshot_chat(const ExplorerSnapshot *snapshot, uint64_t generation,
                                          uint64_t project_generation, size_t index);

#endif
