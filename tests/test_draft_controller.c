/* The unsaved-draft workflow (app/draft_controller.h), moved out of main.c (AUDIT-004 F3), against the real
 * filesystem in a temp directory. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/draft_controller.h"
#include "core/doc_io.h"
#include "test_util.h"

static char g_dir[512];

typedef struct {
    const char *text; /* NULL: the buffer cannot be had */
    int calls;
} TextSource;

static char *source_text(void *ctx)
{
    TextSource *src = ctx;
    src->calls++;
    return src->text ? strdup(src->text) : NULL;
}

static size_t count_drafts(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0, len = strlen(prefix);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (!strncmp(ent->d_name, prefix, len)) n++;
    closedir(d);
    return n;
}

static bool file_is(const char *path, const char *want)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n == strlen(want) && !memcmp(buf, want, n);
}

static void make_doc(char *doc, size_t len, const char *name)
{
    snprintf(doc, len, "%s/%s", g_dir, name);
    FILE *f = fopen(doc, "wb");
    if (f) {
        fputs("saved\n", f);
        fclose(f);
    }
}

/* A tick that starts a write, then waits for the worker and adopts it, as later frames would. */
static bool tick_wait(DraftController *d, const char *doc, bool dirty, double now, TextSource *src)
{
    bool started = draft_controller_tick(d, doc, dirty, now, source_text, src);
    draft_controller_wait(d);
    return started;
}

static void test_hash(void)
{
    CHECK(draft_text_hash("") == 1469598103934665603ULL); /* the basis main.c used (not FNV's standard one; kept, behaviour-preserving) */
    CHECK(draft_text_hash("a") == 0x44bd8ad473cd9906ULL);
    CHECK(draft_text_hash("draft one") != draft_text_hash("draft two"));
}

/* Written on the interval, only when the text changed; the previous draft is replaced, never left beside it. */
static void test_interval_and_change(void)
{
    char doc[700];
    make_doc(doc, sizeof(doc), "a.md");
    DraftController d;
    draft_controller_init(&d);
    TextSource src = { "first\n", 0 };
    CHECK(!tick_wait(&d, doc, true, 5.0, &src) && src.calls == 0); /* before the interval */
    CHECK(tick_wait(&d, doc, true, 10.0, &src) && src.calls == 1);
    CHECK(d.path[0] && !strcmp(d.doc, doc) && file_is(d.path, "first\n"));
    CHECK(d.hash == draft_text_hash("first\n") && d.at == 10.0 && !d.busy);
    CHECK(!tick_wait(&d, doc, true, 19.9, &src) && src.calls == 1);
    CHECK(!tick_wait(&d, doc, true, 20.0, &src) && src.calls == 2); /* unchanged: no write */
    char before[PATH_MAX];
    snprintf(before, sizeof(before), "%s", d.path);
    src.text = "second\n";
    CHECK(tick_wait(&d, doc, true, 30.0, &src));
    CHECK(strcmp(before, d.path) != 0 && access(before, F_OK) != 0 && file_is(d.path, "second\n"));
    CHECK(count_drafts(g_dir, "a.md.scribodo-") == 1);
    CHECK(file_is(doc, "saved\n")); /* the document itself is untouched */
    draft_controller_discard(&d);
    CHECK(!d.path[0] && !d.doc[0] && d.hash == 0 && count_drafts(g_dir, "a.md.scribodo-") == 0);
    draft_controller_destroy(&d);
}

/* A clean buffer, a closed document or another document removes the draft. */
static void test_clean_or_switch_discards(void)
{
    char doc[700], other[700];
    make_doc(doc, sizeof(doc), "b.md");
    make_doc(other, sizeof(other), "c.md");
    DraftController d;
    draft_controller_init(&d);
    TextSource src = { "unsaved\n", 0 };
    CHECK(tick_wait(&d, doc, true, 10.0, &src));
    CHECK(!tick_wait(&d, doc, false, 11.0, &src)); /* saved: clean */
    CHECK(!d.path[0] && count_drafts(g_dir, "b.md.scribodo-") == 0);
    CHECK(tick_wait(&d, doc, true, 21.0, &src));
    CHECK(!tick_wait(&d, NULL, false, 22.0, &src)); /* closed */
    CHECK(!d.path[0] && count_drafts(g_dir, "b.md.scribodo-") == 0);
    CHECK(tick_wait(&d, doc, true, 32.0, &src));
    int calls = src.calls;
    CHECK(!tick_wait(&d, other, true, 50.0, &src)); /* another document: dropped first */
    CHECK(!d.path[0] && count_drafts(g_dir, "b.md.scribodo-") == 0 && src.calls == calls);
    CHECK(tick_wait(&d, other, true, 50.0, &src) && !strcmp(d.doc, other));
    draft_controller_discard(&d);
    draft_controller_discard(&d); /* idempotent */
    CHECK(count_drafts(g_dir, "c.md.scribodo-") == 0);
    draft_controller_destroy(&d);
}

/* No text, or a write that cannot happen, leaves no draft and is retried on the next interval. */
static void test_failures(void)
{
    DraftController d;
    draft_controller_init(&d);
    char doc[700];
    make_doc(doc, sizeof(doc), "d.md");
    TextSource none = { NULL, 0 };
    CHECK(!tick_wait(&d, doc, true, 10.0, &none) && none.calls == 1 && !d.path[0]);
    char missing[700];
    snprintf(missing, sizeof(missing), "%s/no-such-dir/e.md", g_dir);
    TextSource src = { "x", 0 };
    CHECK(tick_wait(&d, missing, true, 20.0, &src) && src.calls == 1); /* started, then failed */
    CHECK(!d.path[0] && !d.doc[0] && d.hash == 0); /* owns nothing: another document may draft */
    CHECK(tick_wait(&d, doc, true, 30.0, &src) && d.path[0]);
    /* A failed rewrite keeps the draft it had. */
    char kept[PATH_MAX];
    snprintf(kept, sizeof(kept), "%s", d.path);
    chmod(g_dir, 0555);
    src.text = "y";
    CHECK(tick_wait(&d, doc, true, 40.0, &src));
    chmod(g_dir, 0755);
    CHECK(!strcmp(d.path, kept) && !strcmp(d.doc, doc) && d.hash == draft_text_hash("x") && file_is(kept, "x"));
    draft_controller_discard(&d);
    draft_controller_destroy(&d);
}

/* The frame thread never waits for the write: a discard or a switch while it is in flight removes what it writes,
 * and no second write starts until it has finished. */
static void test_in_flight(void)
{
    char doc[700], other[700];
    make_doc(doc, sizeof(doc), "f.md");
    make_doc(other, sizeof(other), "g.md");
    DraftController d;
    draft_controller_init(&d);
    TextSource src = { "in flight\n", 0 };
    CHECK(tick_wait(&d, doc, true, 10.0, &src));
    char first[PATH_MAX];
    snprintf(first, sizeof(first), "%s", d.path);
    src.text = "in flight, changed\n";
    CHECK(draft_controller_tick(&d, doc, true, 20.0, source_text, &src)); /* not waited for */
    CHECK(!draft_controller_tick(&d, doc, true, 40.0, source_text, &src) || !d.busy); /* one at a time */
    CHECK(!draft_controller_tick(&d, doc, false, 41.0, source_text, &src)); /* saved while (maybe) writing */
    CHECK(!d.path[0] && !d.doc[0] && access(first, F_OK) != 0);
    draft_controller_wait(&d);
    CHECK(!d.busy && !d.discard_pending && !d.path[0] && count_drafts(g_dir, "f.md.scribodo-") == 0);

    src.text = "switch\n";
    CHECK(draft_controller_tick(&d, doc, true, 60.0, source_text, &src));
    CHECK(!draft_controller_tick(&d, other, true, 61.0, source_text, &src)); /* another document */
    draft_controller_wait(&d);
    CHECK(!d.path[0] && count_drafts(g_dir, "f.md.scribodo-") == 0);
    CHECK(tick_wait(&d, other, true, 72.0, &src) && file_is(d.path, "switch\n"));
    /* Shutdown with a write in flight: it finishes, and the draft stays for recovery. */
    src.text = "at exit\n";
    CHECK(draft_controller_tick(&d, other, true, 90.0, source_text, &src));
    draft_controller_destroy(&d);
    CHECK(!d.busy && d.path[0] && file_is(d.path, "at exit\n") && count_drafts(g_dir, "g.md.scribodo-") == 1);
    bool differs = false;
    char recovered[PATH_MAX];
    CHECK(doc_recover_strays(other, recovered, sizeof(recovered), &differs) == 1 && differs);
}

static void rm_rf(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(path);
        else unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/scribodo-draft-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    TestCase tests[] = {
        { "hash", test_hash },
        { "interval_and_change", test_interval_and_change },
        { "clean_or_switch_discards", test_clean_or_switch_discards },
        { "failures", test_failures },
        { "in_flight", test_in_flight },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    rm_rf(g_dir);
    return rc;
}
