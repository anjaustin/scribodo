/* The project store (docs/STORE-001-design.md): MariaDB is the authority
 * for structured project data whenever it is reachable.
 *
 * Each thread that touches the store (the UI, the observer worker) gets its
 * own connection, opened on first use: a MariaDB connection must not be
 * shared between threads.
 *
 * When the database can't be reached the app keeps working on the local
 * files under <project>/.scribodo/, and store_import() merges them into the
 * database once it is back. Every record has a stable id, so the merge never
 * duplicates.
 */
#ifndef SCRIBODO_STORE_H
#define SCRIBODO_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/db.h"

/* Try to connect and migrate (UI thread, at launch). Returns whether the
 * store is online; *why gets a readable reason if not. Tests may pass a
 * database name (NULL: the default "scribodo"). */
bool store_start(const char *database, char *why, size_t whylen);
void store_stop(void);
bool store_online(void);
/* Force the file store (tests, or the author's choice). */
void store_set_offline(bool offline);

/* This thread's connection, or NULL when offline. A connection that fails
 * mid-use is dropped and reopened on the next call. */
Db *store_db(void);
void store_thread_done(void); /* close this thread's connection */

/* Where a project's structured data lives. A project is in the store once
 * it has been imported (<project>/.scribodo/store.json names its id). */
typedef enum {
    STORE_FILES,  /* not in the store yet: local files under .scribodo/ */
    STORE_DB,     /* in the store, and the database is reachable */
    STORE_PAUSED, /* in the store, but the database is down: structured data waits */
} StoreMode;

/* The mode for `project_dir`; for STORE_DB also its project id and this
 * thread's connection. */
StoreMode store_mode(const char *project_dir, char *project_id, size_t idlen, Db **db);
/* Mark a project as moved into the store (after a successful import). */
bool store_mark_imported(const char *project_dir, const char *project_id);

/* A new stable id: "<prefix>-YYYYMMDD-HHMMSS-<16 hex>". */
void store_new_id(char prefix, char *out, size_t outlen);

/* Ids for a project folder (by its real path) and a document in it (by its
 * current file name); created when `create`. */
bool store_project_id(const char *project_dir, bool create, char *out, size_t outlen);
bool store_document_id(const char *project_id, const char *name, bool create, char *out, size_t outlen);
/* A document's current name from its id (renames are just updates). */
bool store_document_name(const char *document_id, char *out, size_t outlen);
/* A rename keeps the document's id, so every source and revision follows. */
bool store_rename_document(const char *project_id, const char *from, const char *to);

#endif
