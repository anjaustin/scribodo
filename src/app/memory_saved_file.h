/* Filesystem half of saved-revision eligibility. This does not grant memory
 * consent or resolve SQL ownership/current attachment authorization. */
#ifndef SCRIBODO_MEMORY_SAVED_FILE_H
#define SCRIBODO_MEMORY_SAVED_FILE_H

#include "app/store.h"
#include <stdatomic.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>

#define MEMORY_SAVED_FILE_MAX_BYTES (16u * 1024u * 1024u)
#define MEMORY_SAVED_FILE_STEP_BYTES (128u * 1024u)

/* RAM-only resumable verification. No descriptor survives a call. A stamp is
 * usable for cheap rechecks only after ready was established by a full hash. */
typedef struct {
    CC_SHA256_CTX hash;
    struct stat stamp;
    uint64_t offset;
    bool started, ready;
} MemorySavedFileVerification;

/* budget counts requested bytes (including the final EOF probe), <=128 KiB.
 * Caller pins the project/name/SQL hash/byte count across resumptions (the
 * document cache keys enforce this). Failure clears state. Cancellation is
 * cooperative, not an OS I/O deadline. */
bool memory_saved_file_verify_step(const char *project_dir, const char *name,
                                   const StoreProjectIdentity *project, const char *source_hash,
                                   uint64_t source_bytes, const atomic_bool *cancel,
                                   size_t *budget, MemorySavedFileVerification *state);
/* Descriptor/name/stat checks only: never reads file content or initializes a
 * verification from metadata. Failure clears the previously verified state. */
bool memory_saved_file_recheck(const char *project_dir, const char *name,
                               const StoreProjectIdentity *project, uint64_t source_bytes,
                               const atomic_bool *cancel, MemorySavedFileVerification *state);

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
