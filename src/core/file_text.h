#include <stdbool.h>
#include <sys/types.h>
#ifndef SCRIBODO_FILE_TEXT_H
#define SCRIBODO_FILE_TEXT_H

typedef enum { FILE_TEXT_OK, FILE_TEXT_MISSING, FILE_TEXT_ERROR } FileTextStatus;

/* Complete regular-file read, caller-owned C string. Missing is distinct from
 * unreadable, special, changing or NUL-containing input; errors return NULL. */
FileTextStatus file_text_read(const char *path, char **out);

/* Replace the file at `path` with `data` atomically: a temp beside it (mkstemp, <path>.tmp-XXXXXX), mode `mode`,
 * every byte written (EINTR retried), fsync, close, rename over the target, fsync of the directory. The target
 * holds the old or the new content whole at any moment; on failure the temp is removed and the target untouched.
 * The one writer for the app's small files (register, "Atomic file write"); doc_io keeps its own because it
 * revalidates the target's identity between the write and the rename. GAP-004 matrix points atomic.temp_written
 * and atomic.renamed. */
bool file_text_write_atomic(const char *path, const void *data, size_t n, mode_t mode);

#endif
