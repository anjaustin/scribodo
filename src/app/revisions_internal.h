/* File-backend revision operations used by routing and transitions. */
#ifndef SCRIBODO_REVISIONS_INTERNAL_H
#define SCRIBODO_REVISIONS_INTERNAL_H

#include "app/revisions.h"

size_t revisions_list_files(const char *project_dir, const char *doc_path, Revision **out);
bool revisions_list_files_checked(const char *project_dir, const char *doc_path, Revision **out, size_t *count);
bool revisions_record_files(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text, const char *origin,
                            const char *note, bool *recorded);
bool revisions_snapshot_files(StoreManager *manager, const char *project_dir, const char *doc_path, const char *text,
                              char *file_out, size_t file_len);
char *revisions_load_files(const char *project_dir, const char *doc_path, const Revision *revision);

/* Shared file-history identity boundary. Names are full logical document basenames;
 * callers must never derive history paths themselves. False means I/O/identity ambiguity. */
bool revisions_history_path_files(const char *project_dir, const char *doc_path, char *out, size_t cap);
/* Filesystem equivalence only, never case-folding, Unicode normalization or text matching. */
bool revisions_history_same_files(const char *project_dir, const char *a, const char *b);
typedef struct { char name[256]; } RevisionHistoryName;
bool revisions_history_names_files(const char *project_dir, RevisionHistoryName **out, size_t *count);
/* Complete a pending file namespace transition before opening manuscripts or resolving sources.
 * Missing history roots are a successful no-op; malformed/conflicting receipts are preserved. */
bool revisions_recover_files(const char *project_dir);
bool revisions_history_set_aside_files(const char *project_dir, const char *name);
bool revisions_history_move_files(const char *project_dir, const char *from, const char *to);
/* One replayable file+history+rename-log transition. On uncertain failure file_moved
 * tells the UI whether its editor must follow the file already at the new name. */
bool revisions_document_rename_files(const char *project_dir, const char *from, const char *to, bool *file_moved);

#endif
