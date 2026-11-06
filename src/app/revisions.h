/* Document revision history: full-text snapshots per document.
 *
 *   <project>/.scribodo/revisions/<doc>/<stamp>-<origin>.md   (snapshot)
 *
 * Origins: save, before-ai-edit, ai-edit, before-restore, restore, and
 * source (the text a note or canon entry quotes).
 *   <project>/.scribodo/revisions/<doc>/index.jsonl           (one line per revision)
 *
 * Revisions are recorded when an AI edit is accepted (the text before and
 * after), when the author restores a revision, and on every save. A revision
 * identical to the newest one is not duplicated. Snapshots are written
 * atomically and never modified, so restoring is always possible. Interim
 * storage until the MariaDB-backed project store (SOW §5).
 */
#ifndef SCRIBODO_REVISIONS_H
#define SCRIBODO_REVISIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "app/store.h"

typedef struct {
    char file[160];   /* snapshot file name within the document's folder */
    char origin[24];  /* "save", "before-ai-edit", "ai-edit", "restore" */
    char note[200];
    char hash[17];
    time_t ts;
    size_t bytes;
} Revision;

/* Record `text` as a revision of the document `doc_path` (its basename names
 * the history folder). Returns false on I/O failure. *recorded tells whether
 * a new revision was written (false if identical to the newest). */
bool revisions_record(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, const char *origin,
                      const char *note, bool *recorded);

/* Make sure `text` is stored as a revision (origin "source", or the newest
 * revision if identical) and return its snapshot file name: the immutable
 * text a note or canon source reference points to. */
bool revisions_snapshot(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, char *file_out, size_t file_len);

/* List revisions, newest first. Caller frees *out. */
size_t revisions_list(StoreManager *manager, const char *project_dir, const char *doc_path, Revision **out);
bool revisions_list_checked(StoreManager *manager, const char *project_dir, const char *doc_path, Revision **out,
                            size_t *count);

/* Load a revision's text (malloc'd), or NULL. */
char *revisions_load(StoreManager *manager, const char *project_dir, const char *doc_path, const Revision *r);

#endif
