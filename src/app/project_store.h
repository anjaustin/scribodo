/* Internal one-shot routing for project-store operations. The resolver and
 * its borrowed thread-local database connection never escape this API. */
#ifndef SCRIBODO_PROJECT_STORE_H
#define SCRIBODO_PROJECT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/records.h"
#include "app/revisions.h"
#include "app/chat_store.h"
size_t project_store_records_list(const char *project_dir, RecordKind kind, Record **out);
bool project_store_records_append(const char *project_dir, RecordKind kind, const char *id, const char *status,
                                  const char *content, const SourceRef *sources, size_t nsources,
                                   const FindingInfo *finding, bool fresh, int *ver_out);

size_t project_store_revisions_list(const char *project_dir, const char *doc_path, Revision **out);
bool project_store_revisions_record(const char *project_dir, const char *doc_path, const char *text,
                                    const char *origin, const char *note, bool *recorded);
bool project_store_revisions_snapshot(const char *project_dir, const char *doc_path, const char *text,
                                      char *file_out, size_t file_len);
char *project_store_revisions_load(const char *project_dir, const char *doc_path, const Revision *revision);

bool project_store_chat_append(const char *project_dir, const char *id, const char *title, const ChatMessage *message);
bool project_store_chat_rename(const char *project_dir, const char *id, const char *title);
bool project_store_chat_set_observer(const char *project_dir, const char *id, bool read);
size_t project_store_chat_list(const char *project_dir, ChatSummary **out);
bool project_store_chat_load(const char *project_dir, const char *id, ChatMessage **messages, size_t *count);

#endif
