/* Database implementations behind the file-based modules (revisions.c,
 * records.c, chat_store.c, the observer's state). Each module asks
 * store_mode() first and calls these for projects in the store; the module
 * APIs (and their callers) stay the same.
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
#include "app/records.h"
#include "app/revisions.h"
#include "app/store.h"

/* revisions */
bool sdb_revision_record(Db *db, const char *project_id, const char *doc_path, const char *text, const char *origin,
                         const char *note, bool any_match, bool *recorded, char *id_out, size_t idlen);
size_t sdb_revision_list(Db *db, const char *project_id, const char *doc_path, Revision **out);
char *sdb_revision_load(Db *db, const char *project_id, const char *doc_path, const Revision *rev);

/* records (notes, canon, findings) */
bool sdb_record_append(Db *db, const char *project_id, RecordKind kind, const char *id, const char *status,
                       const char *content, const SourceRef *sources, size_t nsources, const FindingInfo *finding,
                       bool fresh, int *ver_out);
size_t sdb_record_list(Db *db, const char *project_id, RecordKind kind, Record **out);

/* chats */
bool sdb_chat_append(Db *db, const char *project_id, const char *id, const char *title, const ChatMessage *m);
size_t sdb_chat_list(Db *db, const char *project_id, ChatSummary **out);
bool sdb_chat_load(Db *db, const char *project_id, const char *id, ChatMessage **msgs, size_t *n);
bool sdb_chat_rename(Db *db, const char *project_id, const char *id, const char *title);

/* Move a project's .scribodo/ files into the store (see store_import.c).
 * True if it is in the store afterwards (or already was). */
bool store_import(const char *project_dir, char *why, size_t whylen);

#endif
