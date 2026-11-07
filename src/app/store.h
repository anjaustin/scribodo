/* The project store (docs/STORE-001-design.md): MariaDB is the authority
 * for structured project data whenever it is reachable.
 *
 * Each thread that touches the store (the UI, the observer worker) gets its
 * own connection, opened on first use: a MariaDB connection must not be
 * shared between threads.
 *
 * A project not yet imported keeps using local files under
 * <project>/.scribodo/. Once imported, MariaDB is its only structured-data
 * authority: if the database is unreachable, chats, records, revisions and
 * observer work pause rather than writing a second history to merge later.
 * Manuscript files remain directly editable in either state.
 */
#ifndef SCRIBODO_STORE_H
#define SCRIBODO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/db.h"

typedef struct StoreManager StoreManager;

StoreManager *store_manager_create(void);
/* Stops the manager and closes this thread's connection. All borrowed workers
 * must be joined (and therefore have closed their connections) first. */
void store_manager_destroy(StoreManager *manager);

/* Try to connect and migrate (UI thread, at launch). Returns whether the
 * store is online; *why gets a readable reason if not. Tests may pass a
 * database name (NULL: the default "scribodo"). */
bool store_start(StoreManager *manager, const char *database, char *why, size_t whylen);
void store_stop(StoreManager *manager);
bool store_online(const StoreManager *manager);
/* The database in use ("scribodo", or a test database). */
void store_database(StoreManager *manager, char *out, size_t outlen);
/* Force the manager offline. Marked projects pause; no file-history fallback. */
void store_set_offline(StoreManager *manager, bool offline);

/* This thread's connection, or NULL when offline. A connection that fails
 * mid-use is dropped and reopened on the next call. */
Db *store_db(StoreManager *manager);
void store_thread_done(StoreManager *manager); /* close this thread's connection */
/* Can the database be reached? (a short, separate connection: safe on any
 * thread; the UI thread then calls store_start). */
bool store_probe(StoreManager *manager);

/* Where a project's structured data lives. A project is in the store once
 * it has been imported (<project>/.scribodo/store.json names its id). */
typedef enum {
    STORE_FILES,  /* not in the store yet: local files under .scribodo/ */
    STORE_DB,     /* in the store, and the database is reachable */
    STORE_PAUSED, /* offline/unverified/malformed marker: structured data waits */
} StoreMode;

/* The mode for `project_dir`; for STORE_DB also its project id and this
 * thread's connection. */
StoreMode store_mode(StoreManager *manager, const char *project_dir, char *project_id, size_t idlen, Db **db);
/* Mark a project as moved into the store (after a successful import): the
 * marker records the folder's identity (device, inode). */
bool store_mark_imported(const char *project_dir, const char *project_id);

typedef struct {
    uint64_t generation;
    char project_id[64];
    unsigned long long device, inode;
} StoreProjectIdentity;

/* Local-only snapshot for async submission/publication checks: never opens a
 * connection or queries SQL. Uses the actual directory identity, not marker
 * inode metadata. False (and zeroed output) means no valid stored identity;
 * it does not establish that falling back to file-backed history is safe.
 * Valid snapshots are available even while the store is offline. */
bool store_project_identity(StoreManager *manager, const char *project_dir, StoreProjectIdentity *out);

/* Why a project with a marker is paused although the store is up:
 *   STORE_PROBLEM_COPY     this folder isn't the one the project belongs to
 *                          (a copy, or a replacement); *original gets that path
 *   STORE_PROBLEM_MISSING  the store has no such project (reset, another
 *                          database, a restored folder)
 * 0 when there is no problem. A moved folder is not a problem: the project's
 * path follows it. */
enum { STORE_PROBLEM_COPY = 1, STORE_PROBLEM_MISSING = 2 };
int store_problem(StoreManager *manager, const char *project_dir, char *original, size_t len);
void store_forget_verification(StoreManager *manager);
/* Internal (store_import): this thread is importing `project_dir` (NULL:
 * done), so it alone still sees the project's files. */
void store_set_importing(StoreManager *manager, const char *project_dir);
/* Internal (exports): file writers on this thread stamp what they write
 * with `t` (epoch seconds) instead of now; 0 restores now. Keeps History,
 * notes and chats dated as they were when a project is exported. */
void store_set_write_time(StoreManager *manager, double t);
double store_write_time(StoreManager *manager); /* the override, or the current time */

/* A new stable id: "<prefix>-YYYYMMDD-HHMMSS-<16 hex>". */
void store_new_id(char prefix, char *out, size_t outlen);

/* Ids for a project folder (by its real path) and a document in it (by its
 * current file name); created when `create`. */
bool store_project_id(StoreManager *manager, const char *project_dir, bool create, char *out, size_t outlen);
bool store_project_id_db(Db *db, const char *project_dir, bool create, char *out, size_t outlen);
bool store_document_id(StoreManager *manager, const char *project_id, const char *name, bool create, char *out, size_t outlen);
/* Backend helper for code that already owns a borrowed manager connection. */
bool store_document_id_db(Db *db, const char *project_id, const char *name, bool create, char *out, size_t outlen);
/* A document's current name from its id (renames are just updates). */
bool store_document_name(StoreManager *manager, const char *document_id, char *out, size_t outlen);
/* A rename keeps the document's id, so every source and revision follows. */
bool store_rename_document(StoreManager *manager, const char *project_id, const char *from, const char *to);

#endif
