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
#include <stdint.h>

#include "app/chat_store.h"
#include "app/chat_history.h"
#include "app/records.h"
#include "app/revisions.h"
#include "app/store.h"

/* revisions */
/* A successful normal record (origin="save", any_match=false) is an explicit
 * saved-write receipt: call ONLY after a successful filesystem save. It advances
 * saved_revision_id even when recorded=false; imports must not use this contract.
 * Other origins and any_match snapshots never authorize saved content.
 * id_out is optional; provide at least 40 bytes when a complete ID is required
 * (the legacy output-buffer API otherwise truncates to idlen). */
bool sdb_revision_record(Db *db, const char *project_id, const char *doc_path, const char *text, const char *origin,
                         const char *note, bool any_match, bool *recorded, char *id_out, size_t idlen);
size_t sdb_revision_list(Db *db, const char *project_id, const char *doc_path, Revision **out);
bool sdb_revision_list_checked(Db *db, const char *project_id, const char *doc_path, Revision **out, size_t *count);
char *sdb_revision_load(Db *db, const char *project_id, const char *doc_path, const Revision *rev);

typedef struct {
    char database[64], project_id[40], document_id[40], revision_id[40], source_hash[65];
    uint64_t bytes;
} SavedRevisionIdentity;

/* SQL-only checked metadata for an existing document's saved receipt; no content
 * hydration, creation, snapshot, or automatic memory consent. doc_name must be a
 * strict basename. False (including absent/malformed data) zeroes out.
 * A receipt does NOT prove the filesystem still matches: an attachment resolver
 * must independently verify disk existence/hash/path containment and dirty-buffer
 * eligibility before using this identity. The saved FK restricts deletion of a
 * referenced revision; explicitly clear receipts before deleting their owners. */
bool sdb_saved_revision_identity(Db *db, const char *project_id, const char *doc_name, SavedRevisionIdentity *out);
/* The 16-hex content hash stored in revisions.hash (FNV-1a over the text). */
void sdb_content_hash16(const char *s, char out[17]);

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
/* The document file `name` in `project_dir` gets a saved revision holding its
 * on-disk text (reusing a revision with that content, else an "import" one).
 * import=true runs inside an import's transaction and makes both pointers
 * follow the file; import=false runs in its own transaction and only fills a
 * missing saved pointer (the lazy repair of projects moved before imports
 * recorded saved files). Missing, special or unreadable files and names that
 * are not document files return true without adopting. *adopted reports
 * whether a pointer was set. */
bool sdb_saved_file_adopt(Db *db, const char *project_id, const char *project_dir, const char *name, bool import, bool *adopted);
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
