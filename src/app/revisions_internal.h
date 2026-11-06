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

#endif
