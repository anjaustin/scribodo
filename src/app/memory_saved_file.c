#define _DARWIN_C_SOURCE
#include "memory_saved_file.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool stopped(const atomic_bool *cancel)
{
    return cancel && atomic_load(cancel);
}

static bool valid_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.' || strnlen(name, 256) >= 256) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p == '/' || *p < 32 || *p == 127) return false;
    return true;
}

static bool same_file(const struct stat *before, const struct stat *after)
{
    return S_ISREG(after->st_mode) && before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_size == after->st_size && before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec &&
           before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec &&
           before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
}

static bool directory_matches(const struct stat *st, const StoreProjectIdentity *project)
{
    return S_ISDIR(st->st_mode) && (uint64_t)st->st_dev == project->device &&
           (uint64_t)st->st_ino == project->inode;
}

/* Recheck the path as well as the opened directory: a renamed directory must
 * not turn a receipt for this path into one for an unlinked/other directory. */
static bool stable_path(int dir, int fd, const char *path, const char *name,
                         const StoreProjectIdentity *project, const struct stat *stamp)
{
    struct stat directory, named_directory, file, named;
    return fstat(dir, &directory) == 0 && directory_matches(&directory, project) &&
           lstat(path, &named_directory) == 0 && directory_matches(&named_directory, project) &&
           fstat(fd, &file) == 0 && same_file(stamp, &file) &&
           fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW) == 0 && same_file(stamp, &named);
}

static bool verification_io(const char *path, const char *name, const StoreProjectIdentity *project,
                             const char *source_hash, uint64_t bytes, const atomic_bool *cancel,
                             size_t *budget, MemorySavedFileVerification *state, bool recheck)
{
    if (!state) return false;
    bool ok = !stopped(cancel) && path && path[0] && strnlen(path, PATH_MAX) < PATH_MAX && project &&
              valid_name(name) && bytes <= MEMORY_SAVED_FILE_MAX_BYTES;
    if (recheck) ok = ok && state->started && state->ready;
    else ok = ok && budget && *budget <= MEMORY_SAVED_FILE_STEP_BYTES && source_hash &&
              strnlen(source_hash, 65) == 64 && strspn(source_hash, "0123456789abcdef") == 64;
    int dir = -1, fd = -1;
    struct stat directory, before;
    if (ok) {
        dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ok = dir >= 0 && fstat(dir, &directory) == 0 && directory_matches(&directory, project);
    }
    if (ok && !stopped(cancel)) fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    ok = ok && fd >= 0 && fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 &&
         (uint64_t)before.st_size == bytes && state->offset <= bytes;
    if (ok && state->started) ok = same_file(&state->stamp, &before);
    if (ok && !state->started) {
        state->stamp = before;
        state->started = CC_SHA256_Init(&state->hash) == 1;
        ok = state->started;
    }
    if (ok) ok = stable_path(dir, fd, path, name, project, &state->stamp);
    unsigned char chunk[16384];
    while (ok && !recheck && !state->ready && state->offset < bytes && *budget) {
        if (stopped(cancel)) { ok = false; break; }
        size_t wanted = bytes - state->offset < sizeof(chunk) ? (size_t)(bytes - state->offset) : sizeof(chunk);
        if (wanted > *budget) wanted = *budget;
        *budget -= wanted;
        ssize_t n = pread(fd, chunk, wanted, (off_t)state->offset);
        ok = n > 0 && CC_SHA256_Update(&state->hash, chunk, (CC_LONG)n) == 1;
        if (ok) state->offset += (uint64_t)n;
    }
    if (ok && !recheck && !state->ready && state->offset == bytes && *budget) {
        (*budget)--;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        ok = !stopped(cancel) && pread(fd, chunk, 1, (off_t)state->offset) == 0 &&
             CC_SHA256_Final(digest, &state->hash) == 1;
        char actual[65];
        if (ok) {
            for (size_t i = 0; i < sizeof(digest); i++) snprintf(actual + i * 2, 3, "%02x", digest[i]);
            ok = !strcmp(actual, source_hash);
            state->ready = ok;
        }
    }
    if (ok) ok = !stopped(cancel) && stable_path(dir, fd, path, name, project, &state->stamp);
    if (fd >= 0) close(fd);
    if (dir >= 0) close(dir);
    if (!ok) *state = (MemorySavedFileVerification){0};
    return ok;
}

bool memory_saved_file_verify_step(const char *path, const char *name,
                                   const StoreProjectIdentity *project, const char *source_hash,
                                   uint64_t bytes, const atomic_bool *cancel, size_t *budget,
                                   MemorySavedFileVerification *state)
{
    return verification_io(path, name, project, source_hash, bytes, cancel, budget, state, false);
}

bool memory_saved_file_recheck(const char *path, const char *name,
                               const StoreProjectIdentity *project, uint64_t bytes,
                               const atomic_bool *cancel, MemorySavedFileVerification *state)
{
    return verification_io(path, name, project, NULL, bytes, cancel, NULL, state, true);
}

bool memory_saved_file_matches(const char *project_dir, const char *name,
                                const StoreProjectIdentity *project, const char *source_hash,
                                uint64_t source_bytes, bool buffer_dirty, bool remote,
                                const atomic_bool *cancel)
{
    if (remote || buffer_dirty || stopped(cancel)) return false;
    if (!project_dir || !project_dir[0] || strnlen(project_dir, PATH_MAX) >= PATH_MAX || !project ||
        !valid_name(name) || !source_hash || strnlen(source_hash, 65) != 64 ||
        strspn(source_hash, "0123456789abcdef") != 64 || source_bytes > MEMORY_SAVED_FILE_MAX_BYTES) return false;
    int dir = open(project_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return false;
    struct stat directory;
    bool ok = fstat(dir, &directory) == 0 && S_ISDIR(directory.st_mode) &&
              (uint64_t)directory.st_dev == project->device && (uint64_t)directory.st_ino == project->inode;
    int fd = ok && !stopped(cancel) ? openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1;
    struct stat before, after;
    ok = fd >= 0 && fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 &&
         (uint64_t)before.st_size == source_bytes;
    CC_SHA256_CTX hash;
    if (ok) ok = CC_SHA256_Init(&hash) == 1;
    unsigned char chunk[16384], digest[CC_SHA256_DIGEST_LENGTH];
    uint64_t read_bytes = 0;
    while (ok && read_bytes < source_bytes) {
        if (stopped(cancel)) { ok = false; break; }
        size_t wanted = source_bytes - read_bytes < sizeof(chunk) ? (size_t)(source_bytes - read_bytes) : sizeof(chunk);
        ssize_t n = read(fd, chunk, wanted);
        if (n <= 0) { ok = false; break; }
        read_bytes += (uint64_t)n;
        ok = CC_SHA256_Update(&hash, chunk, (CC_LONG)n) == 1;
    }
    /* EOF and descriptor/name stability prevent a growing/replaced file from
     * being accepted as the saved revision merely because its prefix matched. */
    struct stat named;
    if (ok) ok = !stopped(cancel) && read(fd, chunk, 1) == 0 && fstat(fd, &after) == 0 &&
                 same_file(&before, &after) && fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                 same_file(&after, &named) && CC_SHA256_Final(digest, &hash) == 1;
    char actual[65];
    if (ok) {
        for (size_t i = 0; i < sizeof(digest); i++) snprintf(actual + i * 2, 3, "%02x", digest[i]);
        ok = !strcmp(actual, source_hash) && !stopped(cancel);
    }
    if (fd >= 0) close(fd);
    close(dir);
    return ok;
}
