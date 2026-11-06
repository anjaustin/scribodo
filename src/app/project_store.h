/* Internal one-shot routing for project-store operations. The resolver and
 * its borrowed thread-local database connection never escape this API. */
#ifndef SCRIBODO_PROJECT_STORE_H
#define SCRIBODO_PROJECT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/records.h"
#include "app/revisions.h"
#include "app/chat_store.h"
#include "app/store_entities.h"
#include "app/store.h"

typedef struct cJSON cJSON;

typedef enum {
    PROJECT_STORE_BLOCK_NONE,
    PROJECT_STORE_BLOCK_OFFLINE,
    PROJECT_STORE_BLOCK_TRANSITION,
    PROJECT_STORE_BLOCK_COPY,
    PROJECT_STORE_BLOCK_MISSING
} ProjectStoreBlock;

enum {
    PROJECT_STORE_CAP_STRUCTURED_WRITE = 1u << 0,
    PROJECT_STORE_CAP_LINKED_DOCUMENTS = 1u << 1,
    PROJECT_STORE_CAP_ENTITIES = 1u << 2,
    PROJECT_STORE_CAP_SEMANTIC = 1u << 3,
    PROJECT_STORE_CAP_SLOW_REFRESH = 1u << 4
};

typedef struct {
    unsigned capabilities;
    ProjectStoreBlock blocked_by;
} ProjectStoreInspection;

typedef struct {
    int file_lock;
    unsigned capabilities;
} ProjectStoreGuard;

ProjectStoreInspection project_store_inspect(StoreManager *manager, const char *project_dir);
bool project_store_guard_begin(StoreManager *manager, const char *project_dir, ProjectStoreGuard *guard);
void project_store_guard_end(ProjectStoreGuard *guard);
bool project_store_feature_key(StoreManager *manager, const char *project_dir, unsigned capability, char *key, size_t keylen);

size_t project_store_records_list(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out);
bool project_store_records_append(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status,
                                  const char *content, const SourceRef *sources, size_t nsources,
                                   const FindingInfo *finding, bool fresh, int *ver_out);

size_t project_store_revisions_list(StoreManager *manager, const char *project_dir, const char *doc_path, Revision **out);
bool project_store_revisions_record(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                                    const char *origin, const char *note, bool *recorded);
bool project_store_revisions_snapshot(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                                      char *file_out, size_t file_len);
char *project_store_revisions_load(StoreManager *manager, const char *project_dir, const char *doc_path, const Revision *revision);

bool project_store_chat_append(StoreManager *manager, const char *project_dir, const char *id, const char *title, const ChatMessage *message);
bool project_store_chat_rename(StoreManager *manager, const char *project_dir, const char *id, const char *title);
bool project_store_chat_set_observer(StoreManager *manager, const char *project_dir, const char *id, bool read);
size_t project_store_chat_list(StoreManager *manager, const char *project_dir, ChatSummary **out);
bool project_store_chat_list_checked(StoreManager *manager, const char *project_dir, ChatSummary **out, size_t *count);
bool project_store_chat_load(StoreManager *manager, const char *project_dir, const char *id, ChatMessage **messages, size_t *count);

cJSON *project_store_observer_state_load(StoreManager *manager, const char *project_dir);
bool project_store_observer_state_save(StoreManager *manager, const char *project_dir, cJSON *state);
bool project_store_observer_rules_get(StoreManager *manager, const char *project_dir, char signature[17]);
bool project_store_observer_rules_set(StoreManager *manager, const char *project_dir, const char *signature);

bool project_store_document_set_aside(StoreManager *manager, const char *project_dir, const char *name, const char *aside);
bool project_store_document_rename(StoreManager *manager, const char *project_dir, const char *from, const char *to);

bool project_store_entities_available(StoreManager *manager, const char *project_dir);
bool project_store_entities_built(StoreManager *manager, const char *project_dir);
bool project_store_entities_backfill(StoreManager *manager, const char *project_dir);
size_t project_store_entity_list(StoreManager *manager, const char *project_dir, EntityRow **out);
bool project_store_entity_detail(StoreManager *manager, const char *project_dir, const char *entity_id, EntityDetail *out);

#endif
