#ifndef SCRIBODO_OBSERVER_JOB_INTERNAL_H
#define SCRIBODO_OBSERVER_JOB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "app/store.h"
#include "cJSON.h"

typedef struct {
    size_t chunk_off, src_off, len;
    long message;
} ObserverSegment;

typedef struct {
    bool is_doc;
    char name[256];
    char path[PATH_MAX + 300];
    char *src;
    char **msgs;
    size_t nmsgs;
    size_t msg_to, off_to;
    char *chunk;
    size_t chunk_len;
    ObserverSegment *segs;
    size_t nsegs, cap_segs;
    uint64_t *hashes;
    cJSON *current;
    size_t nhashes, cap_hashes;
    char author[64], assistant[64];
} ObserverJob;

cJSON *observer_job_state_load(StoreManager *store, const char *project);
bool observer_job_state_save(StoreManager *store, const char *project, cJSON *state);
bool observer_job_find(StoreManager *store, const char *project, cJSON *state, ObserverJob *job, bool *error);
void observer_job_free(ObserverJob *job);
bool observer_job_set_chat_progress(cJSON *chats, const char *id, size_t messages, size_t offset);

#endif
