/* File-only namespace interruption tests. SIGKILL/process death, not power-loss proof.
 * Every fixture and child uses disposable synthetic text and an unstarted StoreManager. */
#define _DARWIN_C_SOURCE
#include "app/revisions_internal.h"
#include "app/renames.h"
#include "core/file_text.h"
#include "core/json.h"
#include "test_util.h"
#include "test_state_fixture.h"
#include "test_revision_fixture.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char executable[1200];

static void check_text(const char *path, const char *expected)
{
    char *text = NULL;
    CHECK(file_text_read_no_follow(path, &text) == FILE_TEXT_OK && text && !strcmp(text, expected));
    free(text);
}

static size_t operation_lines(const char *project)
{
    char path[1600], *text = NULL;
    snprintf(path, sizeof(path), "%s/.scribodo/renames.jsonl", project);
    CHECK(file_text_read_no_follow(path, &text) == FILE_TEXT_OK);
    size_t n = 0;
    if (text) for (char *p = text; (p = strstr(p, "\"operation\"")) != NULL; p++) n++;
    free(text);
    return n;
}

static void verify_source(StoreManager *store, const char *project, const char *name,
                          const char *token, const char *expected)
{
    char current[256];
    renames_resolve_since(project, name, revisions_file_time(token, 0), current, sizeof(current));
    Revision source = {0};
    snprintf(source.file, sizeof(source.file), "%s", token);
    char *text = revisions_load(store, project, current, &source);
    CHECK(text && !strcmp(text, expected));
    free(text);
}

static void interruption_matrix(const char *root, StoreManager *store)
{
    const char *points[] = { "history_move.prepared", "history_move.file", "history_move.history",
                             "history_move.log", "history_move.done", "atomic.temp_written", "atomic.renamed",
                             "history_move.tag_written", "history_move.tag_published" };
    size_t rows = 0;
    for (size_t mode = 0; mode < 2; mode++) {
        char long_name[256];
        memset(long_name, 'x', 199); strcpy(long_name + 199, "A.md");
        const char *from = mode ? "source.md" : long_name;
        const char *to = mode ? long_name : "destination.md";
        for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
            for (int hit = 1; hit <= 3; hit++) {
                char project[1400], original[1700], target[1700], source_token[160], old_target_token[160];
                snprintf(project, sizeof(project), "%s/case-%zu-%zu-%d", root, mode, p, hit);
                CHECK(mkdir(project, 0755) == 0);
                snprintf(original, sizeof(original), "%s/%s", project, from);
                snprintf(target, sizeof(target), "%s/%s", project, to);
                CHECK(file_text_write_atomic(original, "MANUSCRIPT", 10, 0644));
                CHECK(revisions_snapshot(store, project, from, "SOURCE HISTORY", source_token, sizeof(source_token)));
                CHECK(revisions_snapshot(store, project, to, "OLDER DESTINATION", old_target_token, sizeof(old_target_token)));
                if (mode) {
                    CHECK(test_revision_legacy_source(project, from, source_token, sizeof(source_token)));
                }
                struct stat before;
                CHECK(lstat(original, &before) == 0);
                char spec[160];
                snprintf(spec, sizeof(spec), "%s#%d", points[p], hit);
                pid_t child = fork();
                CHECK(child >= 0);
                if (child == 0) {
                    if (setenv("SCRIBODO_CRASH_POINT", spec, 1)) _exit(3);
                    execl(executable, executable, "--child", project, from, to, (char *)NULL);
                    _exit(3);
                }
                if (child < 0) return;
                int status = 0;
                CHECK(waitpid(child, &status, 0) == child);
                bool died = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
                CHECK(died || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
                char receipt[1700], *pending = NULL;
                snprintf(receipt, sizeof(receipt), "%s/.scribodo/revisions/.pending-move.json", project);
                FileTextStatus pending_status = file_text_read_no_follow(receipt, &pending);
                CHECK(pending_status == FILE_TEXT_OK || pending_status == FILE_TEXT_MISSING);
                bool pending_file = false;
                if (pending_status == FILE_TEXT_OK) {
                    cJSON *j = json_parse_complete(pending, strlen(pending));
                    cJSON *enabled = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "file"), "enabled");
                    CHECK(cJSON_IsBool(enabled));
                    pending_file = cJSON_IsTrue(enabled);
                    cJSON_Delete(j);
                }
                free(pending);
                /* Source resolution and explicit project-open recovery are also first readers,
                 * not just the History panel. */
                if (mode) {
                    char current_name[256];
                    renames_resolve_since(project, from, revisions_file_time(source_token, 0),
                                          current_name, sizeof(current_name));
                    CHECK(current_name[0]);
                } else CHECK(revisions_recover_files(project));
                /* Check this before inventory or a retry can accidentally hide a missing
                 * first-reader recovery hook. The receipt was observed above, not inferred. */
                CHECK(access(receipt, F_OK) != 0);
                struct stat first_read;
                if (pending_file) {
                    CHECK(lstat(original, &first_read) != 0);
                    CHECK(lstat(target, &first_read) == 0 && first_read.st_dev == before.st_dev &&
                          first_read.st_ino == before.st_ino);
                }
                /* The normal reader must publish only a recovered inventory. */
                RevisionHistoryName *names = NULL;
                size_t count = 0;
                CHECK(revisions_history_names_files(project, &names, &count));
                free(names);
                struct stat current;
                if (lstat(original, &current) == 0) {
                    bool moved = false;
                    CHECK(revisions_document_rename_files(project, from, to, &moved) && moved);
                }
                CHECK(lstat(target, &current) == 0 && current.st_dev == before.st_dev && current.st_ino == before.st_ino);
                CHECK(lstat(original, &current) != 0);
                check_text(target, "MANUSCRIPT");
                verify_source(store, project, from, source_token, "SOURCE HISTORY");
                verify_source(store, project, to, old_target_token, "OLDER DESTINATION");
                CHECK(operation_lines(project) == 2); /* archive + rename, never replay duplicates */
                CHECK(access(receipt, F_OK) != 0);
                printf("namespace matrix mode=%zu point=%s hit=%d killed=%d: originals/sources/once verified\n",
                       mode, points[p], hit, died);
                rows++;
                if (!died) break;
            }
        }
    }
    CHECK(rows == 48);
}

static void replaced_inode_guards(const char *root, StoreManager *store)
{
    for (int at_target = 0; at_target < 2; at_target++) {
        char project[1400], from[1700], to[1700], parked[1700], foreign[1700], receipt[1700];
        char token[160], old_token[160];
        snprintf(project, sizeof(project), "%s/inode-guard-%d", root, at_target);
        CHECK(mkdir(project, 0755) == 0);
        snprintf(from, sizeof(from), "%s/source.md", project);
        snprintf(to, sizeof(to), "%s/destination.md", project);
        snprintf(parked, sizeof(parked), "%s/parked-original.md", project);
        snprintf(foreign, sizeof(foreign), "%s/preserved-foreign.md", project);
        snprintf(receipt, sizeof(receipt), "%s/.scribodo/revisions/.pending-move.json", project);
        CHECK(file_text_write_atomic(from, "MANUSCRIPT", 10, 0644));
        CHECK(revisions_snapshot(store, project, "source.md", "SOURCE HISTORY", token, sizeof(token)));
        CHECK(revisions_snapshot(store, project, "destination.md", "OLDER DESTINATION", old_token, sizeof(old_token)));
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            if (setenv("SCRIBODO_CRASH_POINT", at_target ? "history_move.file#2" : "history_move.prepared#2", 1)) _exit(3);
            execl(executable, executable, "--child", project, "source.md", "destination.md", (char *)NULL);
            _exit(3);
        }
        if (child < 0) return;
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        const char *changed = at_target ? to : from;
        CHECK(rename(changed, parked) == 0);
        CHECK(file_text_write_atomic(changed, "FOREIGN", 7, 0644));
        char *before = NULL, *after = NULL;
        CHECK(file_text_read_no_follow(receipt, &before) == FILE_TEXT_OK);
        RevisionHistoryName *names = NULL;
        size_t count = 0;
        CHECK(!revisions_history_names_files(project, &names, &count) && !names && !count);
        CHECK(file_text_read_no_follow(receipt, &after) == FILE_TEXT_OK && before && after && !strcmp(before, after));
        free(before); free(after);
        check_text(changed, "FOREIGN");
        check_text(parked, "MANUSCRIPT");
        CHECK(operation_lines(project) == 1); /* blocked replay did not publish the full rename */
        /* Explicit fixture-only repair preserves both objects; the software never guesses by text. */
        CHECK(rename(changed, foreign) == 0 && rename(parked, changed) == 0);
        CHECK(revisions_history_names_files(project, &names, &count));
        free(names);
        check_text(to, "MANUSCRIPT");
        check_text(foreign, "FOREIGN");
        verify_source(store, project, "source.md", token, "SOURCE HISTORY");
        verify_source(store, project, "destination.md", old_token, "OLDER DESTINATION");
        CHECK(operation_lines(project) == 2);
    }
}

static void namespace_controls(const char *root, StoreManager *store)
{
    char project[1400], a[1700], b[1700], receipt[1700];
    snprintf(project, sizeof(project), "%s/namespace-controls", root);
    CHECK(mkdir(project, 0755) == 0);
    snprintf(a, sizeof(a), "%s/a.md", project);
    snprintf(b, sizeof(b), "%s/b.md", project);
    CHECK(file_text_write_atomic(a, "HARDLINK", 8, 0644) && link(a, b) == 0);
    CHECK(revisions_record(store, project, "a.md", "A HISTORY", "save", NULL, NULL));
    CHECK(revisions_record(store, project, "b.md", "B HISTORY", "save", NULL, NULL));
    bool moved = true;
    CHECK(!revisions_document_rename_files(project, "a.md", "b.md", &moved) && !moved);
    check_text(a, "HARDLINK");
    check_text(b, "HARDLINK");
    RevisionHistoryName *names = NULL;
    size_t count = 0;
    CHECK(revisions_history_names_files(project, &names, &count) && count == 2);
    free(names);
    /* Parsing must not silently discard the end of a damaged replay receipt. */
    snprintf(receipt, sizeof(receipt), "%s/.scribodo/revisions/.pending-move.json", project);
    const char invalid[] = "{\"version\":1}garbage";
    CHECK(file_text_write_atomic(receipt, invalid, strlen(invalid), 0600));
    CHECK(!revisions_recover_files(project));
    check_text(receipt, invalid);
    CHECK(unlink(receipt) == 0); /* only this fixture's deliberately malformed receipt */
    /* Two complete basenames can require more than 2048 JSON bytes when escaped. */
    char from[256], to[256], resolved[256];
    memset(from, 1, 250); strcpy(from + 250, ".md");
    memset(to, 2, 250); strcpy(to + 250, ".md");
    CHECK(renames_record_once(project, from, to, 1700000000.125, "00000000000000000000000000000001"));
    renames_resolve_since(project, from, 1700000000, resolved, sizeof(resolved));
    CHECK(!strcmp(resolved, to));
    CHECK(renames_record_once(project, from, to, 1700000000.125, "00000000000000000000000000000001"));
    CHECK(operation_lines(project) == 1);
    CHECK(!renames_record_once(project, to, from, 1700000000.125, "00000000000000000000000000000001"));
    CHECK(operation_lines(project) == 1);
}

static void refused_log_replay(const char *root, StoreManager *store)
{
    char project[1400], file[1700], log[1700], parked[1700], receipt[1700], token[160];
    snprintf(project, sizeof(project), "%s/log-refusal", root);
    CHECK(mkdir(project, 0755) == 0);
    snprintf(file, sizeof(file), "%s/source.md", project);
    CHECK(file_text_write_atomic(file, "MANUSCRIPT", 10, 0644));
    CHECK(revisions_snapshot(store, project, "source.md", "SOURCE HISTORY", token, sizeof(token)));
    snprintf(log, sizeof(log), "%s/.scribodo/renames.jsonl", project);
    snprintf(parked, sizeof(parked), "%s/.scribodo/preserved-log.jsonl", project);
    snprintf(receipt, sizeof(receipt), "%s/.scribodo/revisions/.pending-move.json", project);
    CHECK(renames_record(project, "unrelated.md", "also-unrelated.md"));
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (setenv("SCRIBODO_CRASH_POINT", "history_move.prepared", 1)) _exit(3);
        execl(executable, executable, "--child", project, "source.md", "destination.md", (char *)NULL);
        _exit(3);
    }
    if (child < 0) return;
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(rename(log, parked) == 0 && mkdir(log, 0755) == 0);
    bool moved = false;
    /* This retry itself moves the file, then hits the planted log refusal. */
    CHECK(!revisions_document_rename_files(project, "source.md", "destination.md", &moved) && moved);
    CHECK(!revisions_recover_files(project)); /* log is unreadable as a regular file */
    CHECK(access(receipt, F_OK) == 0);
    snprintf(file, sizeof(file), "%s/destination.md", project);
    check_text(file, "MANUSCRIPT");
    char current[256];
    renames_resolve_since(project, "source.md", 0, current, sizeof(current));
    CHECK(!current[0]); /* never pretend the old name is authoritative */
    CHECK(rmdir(log) == 0 && rename(parked, log) == 0); /* only the fixture's planted empty directory */
    CHECK(revisions_recover_files(project));
    verify_source(store, project, "source.md", token, "SOURCE HISTORY");
    CHECK(operation_lines(project) == 1);
}

static void trailing_receipt_guard(const char *root, StoreManager *store)
{
    char project[1400], source[1700], receipt[1700], token[160];
    snprintf(project, sizeof(project), "%s/trailing-receipt", root);
    CHECK(mkdir(project, 0755) == 0);
    snprintf(source, sizeof(source), "%s/source.md", project);
    CHECK(file_text_write_atomic(source, "MANUSCRIPT", 10, 0644));
    CHECK(revisions_snapshot(store, project, "source.md", "SOURCE HISTORY", token, sizeof(token)));
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (setenv("SCRIBODO_CRASH_POINT", "history_move.prepared", 1)) _exit(3);
        execl(executable, executable, "--child", project, "source.md", "destination.md", (char *)NULL);
        _exit(3);
    }
    if (child < 0) return;
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    snprintf(receipt, sizeof(receipt), "%s/.scribodo/revisions/.pending-move.json", project);
    char *original = NULL;
    CHECK(file_text_read_no_follow(receipt, &original) == FILE_TEXT_OK);
    if (!original) return; /* setup failure must not turn into a test-harness crash */
    size_t size = strlen(original);
    char *damaged = malloc(size + 8);
    CHECK(damaged);
    if (!damaged) { free(original); return; }
    memcpy(damaged, original, size);
    memcpy(damaged + size, "garbage", 8);
    CHECK(file_text_write_atomic(receipt, damaged, size + 7, 0600));
    CHECK(!revisions_recover_files(project));
    check_text(receipt, damaged);
    check_text(source, "MANUSCRIPT");
    CHECK(file_text_write_atomic(receipt, original, size, 0600)); /* restore only our deliberately corrupted fixture */
    CHECK(revisions_recover_files(project));
    verify_source(store, project, "source.md", token, "SOURCE HISTORY");
    free(original); free(damaged);
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--child")) {
        bool moved = false;
        return revisions_document_rename_files(argv[2], argv[3], argv[4], &moved) && moved ? 0 : 2;
    }
    if (!realpath(argv[0], executable)) return 2;
    char root[512];
    StoreManager *store = NULL;
    if (!test_state_fixture_init(root, sizeof(root), &store)) return 2;
    interruption_matrix(root, store);
    replaced_inode_guards(root, store);
    namespace_controls(root, store);
    refused_log_replay(root, store);
    trailing_receipt_guard(root, store);
    int result = g_failures ? 1 : 0;
    printf("%d checks, %d failures\n", g_checks, g_failures);
    test_state_fixture_destroy(root, store);
    return result;
}
