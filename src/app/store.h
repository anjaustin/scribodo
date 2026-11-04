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
/* The database in use ("scribodo", or a test database). */
const char *store_database(void);
/* Force the file store (tests, or the author's choice). */
void store_set_offline(bool offline);

/* This thread's connection, or NULL when offline. A connection that fails
 * mid-use is dropped and reopened on the next call. */
Db *store_db(void);
void store_thread_done(void); /* close this thread's connection */
/* Can the database be reached? (a short, separate connection: safe on any
 * thread; the UI thread then calls store_start). */
bool store_probe(void);

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
/* Mark a project as moved into the store (after a successful import): the
 * marker records the folder's identity (device, inode). */
bool store_mark_imported(const char *project_dir, const char *project_id);

/* Why a project with a marker is paused although the store is up:
 *   STORE_PROBLEM_COPY     this folder isn't the one the project belongs to
 *                          (a copy, or a replacement); *original gets that path
 *   STORE_PROBLEM_MISSING  the store has no such project (reset, another
 *                          database, a restored folder)
 * 0 when there is no problem. A moved folder is not a problem: the project's
 * path follows it. */
enum { STORE_PROBLEM_COPY = 1, STORE_PROBLEM_MISSING = 2 };
int store_problem(const char *project_dir, char *original, size_t len);
void store_forget_verification(void);
/* Internal (store_import): this thread is importing `project_dir` (NULL:
 * done), so it alone still sees the project's files. */
void store_set_importing(const char *project_dir);
/* Internal (exports): file writers on this thread stamp what they write
 * with `t` (epoch seconds) instead of now; 0 restores now. Keeps History,
 * notes and chats dated as they were when a project is exported. */
void store_set_write_time(double t);
double store_write_time(void); /* the override, or the current time */

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
