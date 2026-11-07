/* Filesystem half of saved-revision eligibility. This does not grant memory
 * consent or resolve SQL ownership/current attachment authorization. */
#ifndef SCRIBODO_MEMORY_SAVED_FILE_H
#define SCRIBODO_MEMORY_SAVED_FILE_H

#include "app/store.h"
#include <stdatomic.h>

#define MEMORY_SAVED_FILE_MAX_BYTES (16u * 1024u * 1024u)

/* Worker-only, bounded streaming read, no whole-file allocation. The caller
 * supplies the SQL saved revision's SHA-256/byte count and a captured project
 * directory identity. Dirty open buffers and remote requests fail before I/O.
 * Rejects symlinks, nonregular/missing/changed files and mismatched disk bytes.
 * Success is an as-of file receipt, not a permanent filesystem lock. Callers
 * must separately freeze/revalidate attachment, SQL and store identities.
 * Cancellation is checked between reads; this is not an OS I/O deadline. */
bool memory_saved_file_matches(const char *project_dir, const char *name,
                                const StoreProjectIdentity *project, const char *source_hash,
                                uint64_t source_bytes, bool buffer_dirty, bool remote,
                                const atomic_bool *cancel);

#endif
