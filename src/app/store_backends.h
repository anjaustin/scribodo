/* Database implementations behind the backend-neutral module APIs. Routing
 * is moving into project_store.c one operation family at a time; transition
 * code and modules not yet migrated may still resolve store_mode() directly.
 *
 * Sources in the store point at ids. For callers they are presented the
 * same way as the file store's: SourceRef.doc is the document's *current*
 * name (renames are free) and SourceRef.revision is the revision id.
 */
#ifndef SCRIBODO_STORE_BACKENDS_H
#define SCRIBODO_STORE_BACKENDS_H

#include <stdbool.h>
#include <stddef.h>

#include "app/chat_store.h"
#include "app/chat_history.h"
#include "app/records.h"
#include "app/revisions.h"
#include "app/store.h"

/* revisions */
bool sdb_revision_record(Db *db, const char *project_id, const char *doc_path, const char *text, const char *origin,
                         const char *note, bool any_match, bool *recorded, char *id_out, size_t idlen);
size_t sdb_revision_list(Db *db, const char *project_id, const char *doc_path, Revision **out);
bool sdb_revision_list_checked(Db *db, const char *project_id, const char *doc_path, Revision **out, size_t *count);
char *sdb_revision_load(Db *db, const char *project_id, const char *doc_path, const Revision *rev);

/* records (notes, canon, findings) */
bool sdb_record_append(Db *db, const char *project_id, RecordKind kind, const char *id, const char *status,
                       const char *content, const SourceRef *sources, size_t nsources, const FindingInfo *finding,
                       bool fresh, int *ver_out);
size_t sdb_record_list(Db *db, const char *project_id, RecordKind kind, Record **out);
bool sdb_record_list_checked(Db *db, const char *project_id, RecordKind kind, Record **out, size_t *count);

/* chats */
bool sdb_chat_append(Db *db, const char *project_id, const char *id, const char *title, const ChatMessage *m);
bool sdb_chat_append_sequenced(Db *db, const char *project_id, const char *id, const char *title,
                                const ChatMessage *m, int64_t *sequence);
bool sdb_chat_window(Db *db, const char *project_id, const char *id, const ChatWindowRequest *request, ChatWindowPage *out);
size_t sdb_chat_list(Db *db, const char *project_id, ChatSummary **out);
bool sdb_chat_list_checked(Db *db, const char *project_id, ChatSummary **out, size_t *count);
bool sdb_chat_load(Db *db, const char *project_id, const char *id, ChatMessage **msgs, size_t *n);

/* Bounded source traversal for memory ingestion; no full body/result fallback. */
bool sdb_chat_source_page(Db *db, const char *project_id, const char *id,
                          const ChatSourceCursor *cursor, ChatSourcePage *out);
bool sdb_chat_rename(Db *db, const char *project_id, const char *id, const char *title);
bool sdb_chat_set_observer(Db *db, const char *project_id, const char *id, bool read);

/* Move a project's .scribodo/ files into the store (see store_import.c).
 * True if it is in the store afterwards (or already was). */
bool store_import(StoreManager *manager, const char *project_dir, char *why, size_t whylen);
bool store_needs_import(StoreManager *manager, const char *project_dir);
/* A copied folder becomes its own project (the original's data, new ids). */
bool store_fork(StoreManager *manager, const char *project_dir, char *why, size_t whylen);
/* The project's store data written as files into `dir` (its .scribodo/),
 * with original times: what a backup carries for an in-app restore. */
/* Destination must have no structured data (a pre-created import.lock is OK).
 * A failed export may leave partial output; retry in a fresh destination, never
 * append into that output. Calls to the same destination are serialized. */
bool store_export_project(StoreManager *manager, const char *project_id, const char *dir, char *why, size_t whylen);
/* A project whose store record is missing is restored from its newest
 * set-aside files (or starts again on its documents if there are none). */
bool store_restore(StoreManager *manager, const char *project_dir, char *why, size_t whylen);

#endif
