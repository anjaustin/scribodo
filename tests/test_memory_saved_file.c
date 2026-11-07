#define _DARWIN_C_SOURCE
#include "app/memory_saved_file.h"
#include "test_util.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Interpose only this test's streaming reads: exercise cancellation and file
 * replacement at a deterministic point, without a scheduler-sensitive race. */
static ssize_t saved_test_read(int fd, void *buffer, size_t bytes);
#define read saved_test_read
#include "../src/app/memory_saved_file.c"
#undef read

static unsigned read_calls, read_action;
static const char *mutate_path, *replace_path;
static atomic_bool *cancel_after_read;

static ssize_t saved_test_read(int fd, void *buffer, size_t bytes)
{
    ssize_t n = read(fd, buffer, bytes);
    read_calls++;
    unsigned action = read_action;
    read_action = 0;
    if (n > 0 && action == 1) atomic_store(cancel_after_read, true);
    if (n > 0 && action == 2) {
        int writer = open(mutate_path, O_WRONLY);
        CHECK(writer >= 0);
        if (writer >= 0) { CHECK(write(writer, "!", 1) == 1); close(writer); }
    }
    if (n > 0 && action == 3) CHECK(rename(replace_path, mutate_path) == 0);
    return n;
}

static void digest(const char *text, char out[65])
{
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(text, (CC_LONG)strlen(text), bytes);
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

static void saved_files(void)
{
    char dir[PATH_MAX], path[PATH_MAX], other[PATH_MAX], hash[65];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/memory-file-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    struct stat st; CHECK(stat(dir, &st) == 0);
    StoreProjectIdentity identity = { .device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino };
    snprintf(path, sizeof(path), "%s/one.md", dir);
    FILE *f = fopen(path, "w"); CHECK(f != NULL);
    const char *body = "Zoë holds the amber key. 東京。\n";
    if (f) { fputs(body, f); fclose(f); }
    digest(body, hash);
    atomic_bool cancel; atomic_init(&cancel, false);
    CHECK(memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), false, false, &cancel));
    MemorySavedFileVerification verification = {0};
    CHECK(!memory_saved_file_recheck(dir, "one.md", &identity, strlen(body), &cancel, &verification));
    CHECK(!verification.started && !verification.ready);
    size_t budget = 1;
    CHECK(memory_saved_file_verify_step(dir, "one.md", &identity, hash, strlen(body), &cancel, &budget, &verification));
    CHECK(verification.started && !verification.ready && verification.offset == 1 && budget == 0);
    budget = MEMORY_SAVED_FILE_STEP_BYTES;
    CHECK(memory_saved_file_verify_step(dir, "one.md", &identity, hash, strlen(body), &cancel, &budget, &verification));
    CHECK(verification.ready && verification.offset == strlen(body));
    CHECK(memory_saved_file_recheck(dir, "one.md", &identity, strlen(body), &cancel, &verification));
    /* A same-content replacement must not inherit the opened file identity. */
    snprintf(other, sizeof(other), "%s/replacement.md", dir);
    f = fopen(other, "w"); CHECK(f != NULL); if (f) { fputs(body, f); fclose(f); }
    mutate_path = path; replace_path = other; read_action = 3;
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), false, false, &cancel));
    CHECK(memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), false, false, &cancel));
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), true, false, &cancel));
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), false, true, &cancel));
    CHECK(!memory_saved_file_matches(NULL, NULL, NULL, NULL, 0, false, true, NULL));
    atomic_store(&cancel, true);
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), false, false, &cancel));
    atomic_store(&cancel, false);
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body) - 1, false, false, NULL));
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, MEMORY_SAVED_FILE_MAX_BYTES + 1, false, false, NULL));
    StoreProjectIdentity wrong = identity; wrong.inode++;
    CHECK(!memory_saved_file_matches(dir, "one.md", &wrong, hash, strlen(body), false, false, NULL));
    char wrong_hash[65]; digest("different body", wrong_hash);
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, wrong_hash, strlen(body), false, false, NULL));
    CHECK(!memory_saved_file_matches(dir, "missing.md", &identity, hash, strlen(body), false, false, NULL));
    const char *bad[] = { "", ".", "..", "../one.md", "/one.md", ".hidden", "a\n.md", "a\x7f.md" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        CHECK(!memory_saved_file_matches(dir, bad[i], &identity, hash, strlen(body), false, false, NULL));
    snprintf(other, sizeof(other), "%s/link.md", dir); CHECK(symlink(path, other) == 0);
    CHECK(!memory_saved_file_matches(dir, "link.md", &identity, hash, strlen(body), false, false, NULL));
    unlink(other);
    snprintf(other, sizeof(other), "%s/directory.md", dir); CHECK(mkdir(other, 0700) == 0);
    CHECK(!memory_saved_file_matches(dir, "directory.md", &identity, hash, strlen(body), false, false, NULL));
    rmdir(other);
    snprintf(other, sizeof(other), "%s/pipe.md", dir); CHECK(mkfifo(other, 0600) == 0);
    CHECK(!memory_saved_file_matches(dir, "pipe.md", &identity, hash, strlen(body), false, false, NULL));
    unlink(other);
    /* Externally changed bytes cannot inherit an otherwise valid SQL receipt. */
    f = fopen(path, "w"); CHECK(f != NULL);
    if (f) { fputs("A changed saved file.\n", f); fclose(f); }
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, strlen(body), false, false, NULL));
    unlink(path);
    /* Empty saved revisions are valid identities, though they yield no units. */
    f = fopen(path, "w"); CHECK(f != NULL); if (f) fclose(f);
    digest("", hash);
    CHECK(memory_saved_file_matches(dir, "one.md", &identity, hash, 0, false, false, NULL));
    /* Exact cap, bounded streaming, and a one-byte oversized sparse file. */
    int fd = open(path, O_WRONLY | O_TRUNC); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(ftruncate(fd, MEMORY_SAVED_FILE_MAX_BYTES) == 0); close(fd); }
    unsigned char zeros[16384] = {0}, sum[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX state; CC_SHA256_Init(&state);
    for (size_t i = 0; i < MEMORY_SAVED_FILE_MAX_BYTES / sizeof(zeros); i++)
        CC_SHA256_Update(&state, zeros, sizeof(zeros));
    CC_SHA256_Final(sum, &state);
    for (size_t i = 0; i < sizeof(sum); i++) snprintf(hash + i * 2, 3, "%02x", sum[i]);
    CHECK(memory_saved_file_matches(dir, "one.md", &identity, hash, MEMORY_SAVED_FILE_MAX_BYTES, false, false, NULL));
    read_calls = 0; read_action = 1; cancel_after_read = &cancel;
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, MEMORY_SAVED_FILE_MAX_BYTES, false, false, &cancel));
    CHECK(read_calls == 1); atomic_store(&cancel, false);
    read_action = 2;
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, MEMORY_SAVED_FILE_MAX_BYTES, false, false, NULL));
    fd = open(path, O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(ftruncate(fd, MEMORY_SAVED_FILE_MAX_BYTES + 1) == 0); close(fd); }
    CHECK(!memory_saved_file_matches(dir, "one.md", &identity, hash, MEMORY_SAVED_FILE_MAX_BYTES, false, false, NULL));
    unlink(path); rmdir(dir);
}

int main(void)
{
    TestCase tests[] = { { "saved file authority", saved_files } };
    return run_tests(tests, sizeof(tests) / sizeof(*tests));
}
