/* The unsaved-draft workflow (app/draft_controller.h), moved out of main.c (AUDIT-004 F3), against the real
 * filesystem in a temp directory. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <pthread.h>
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

/* A gate the write hook blocks on: a write is held in flight until the test opens it, so every in-flight branch
 * runs on purpose, not by timing (red team on remed/main). */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool open, entered;
} Gate;

static Gate g_gate = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, true, false };

static void gate_hook(void *ctx)
{
    Gate *g = ctx;
    pthread_mutex_lock(&g->mu);
    g->entered = true;
    pthread_cond_broadcast(&g->cv);
    while (!g->open) pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
}

static void gate_close(Gate *g)
{
    pthread_mutex_lock(&g->mu);
    g->open = false;
    g->entered = false;
    pthread_mutex_unlock(&g->mu);
}

/* Until the writer is inside the hook: the write is certainly in flight and has not touched the disk. */
static void gate_wait_entered(Gate *g)
{
    pthread_mutex_lock(&g->mu);
    while (!g->entered) pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
}

static void gate_open(Gate *g)
{
    pthread_mutex_lock(&g->mu);
    g->open = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

/* A held write after a first, complete draft of `doc`. */
static void start_held_write(DraftController *d, const char *doc, TextSource *src, char *first, size_t firstlen)
{
    draft_controller_init(d);
    draft_controller_set_write_hook(d, gate_hook, &g_gate);
    CHECK(tick_wait(d, doc, true, 10.0, src));
    snprintf(first, firstlen, "%s", d->path);
    gate_close(&g_gate);
    src->text = "held in flight\n";
    CHECK(draft_controller_tick(d, doc, true, 20.0, source_text, src));
    gate_wait_entered(&g_gate);
    CHECK(d->busy);
}

/* Saved (a clean buffer) while the write is in flight: the old draft goes at once, the new one when it lands. */
static void test_discard_while_writing(void)
{
    char doc[700], first[PATH_MAX];
    make_doc(doc, sizeof(doc), "f.md");
    DraftController d;
    TextSource src = { "first\n", 0 };
    start_held_write(&d, doc, &src, first, sizeof(first));
    int calls = src.calls;
    CHECK(!draft_controller_tick(&d, doc, true, 40.0, source_text, &src) && src.calls == calls); /* one at a time */
    CHECK(!draft_controller_tick(&d, doc, false, 41.0, source_text, &src));
    CHECK(d.busy && d.discard_pending && !d.path[0] && !d.doc[0] && access(first, F_OK) != 0);
    gate_open(&g_gate);
    draft_controller_wait(&d);
    CHECK(!d.busy && !d.discard_pending && !d.path[0] && count_drafts(g_dir, "f.md.scribodo-") == 0);
    draft_controller_destroy(&d);
}

/* Another document opened while the write is in flight: nothing of the first one stays, and the second drafts. */
static void test_switch_while_writing(void)
{
    char doc[700], other[700], first[PATH_MAX];
    make_doc(doc, sizeof(doc), "g.md");
    make_doc(other, sizeof(other), "h.md");
    DraftController d;
    TextSource src = { "first\n", 0 };
    start_held_write(&d, doc, &src, first, sizeof(first));
    CHECK(!draft_controller_tick(&d, other, true, 21.0, source_text, &src));
    CHECK(d.discard_pending && access(first, F_OK) != 0);
    gate_open(&g_gate);
    draft_controller_wait(&d);
    CHECK(!d.path[0] && count_drafts(g_dir, "g.md.scribodo-") == 0);
    src.text = "other\n";
    CHECK(tick_wait(&d, other, true, 31.0, &src) && file_is(d.path, "other\n"));
    draft_controller_discard(&d);
    draft_controller_destroy(&d);
}

/* Quit while the write is in flight: shutdown waits for it, and the draft stays for recovery. */
static void test_quit_while_writing(void)
{
    char doc[700], first[PATH_MAX];
    make_doc(doc, sizeof(doc), "i.md");
    DraftController d;
    TextSource src = { "first\n", 0 };
    start_held_write(&d, doc, &src, first, sizeof(first));
    gate_open(&g_gate); /* the write lands; the controller has not joined it yet */
    CHECK(d.busy);
    draft_controller_destroy(&d);
    CHECK(!d.busy && d.path[0] && file_is(d.path, "held in flight\n") && access(first, F_OK) != 0);
    CHECK(count_drafts(g_dir, "i.md.scribodo-") == 1);
    bool differs = false;
    char recovered[PATH_MAX];
    CHECK(doc_recover_strays(doc, recovered, sizeof(recovered), &differs) == 1 && differs);
}

/* ⌘S, then the same document opened before the next poll: the load must not find a half-written draft. Clean: the
 * draft is gone before recovery looks; dirty: the draft is whole, and recovery keeps it whole. */
static void test_load_while_writing(void)
{
    char doc[700], first[PATH_MAX];
    make_doc(doc, sizeof(doc), "j.md");
    DraftController d;
    TextSource src = { "first\n", 0 };
    start_held_write(&d, doc, &src, first, sizeof(first));
    gate_open(&g_gate);
    draft_controller_before_load(&d, doc, false); /* saved: clean */
    CHECK(!d.busy && !d.path[0] && count_drafts(g_dir, "j.md.scribodo-") == 0);
    bool differs = true;
    char recovered[PATH_MAX];
    CHECK(doc_recover_strays(doc, recovered, sizeof(recovered), &differs) == 0);
    draft_controller_destroy(&d);

    src.text = "first\n";
    start_held_write(&d, doc, &src, first, sizeof(first));
    gate_open(&g_gate);
    draft_controller_before_load(&d, doc, true); /* reloaded with unsaved text: the draft is kept, complete */
    CHECK(!d.busy && d.path[0] && file_is(d.path, "held in flight\n"));
    CHECK(doc_recover_strays(doc, recovered, sizeof(recovered), &differs) == 1 && differs);
    char kept[PATH_MAX + 600];
    snprintf(kept, sizeof(kept), "%s/%s", g_dir, recovered); /* recovery names the copy, beside the document */
    CHECK(file_is(kept, "held in flight\n"));
    unlink(kept);
    draft_controller_destroy(&d);
}

/* A failed write is reported once per run of failures; a success re-arms it. */
static void test_failure_reported_once(void)
{
    char doc[700], missing[700];
    make_doc(doc, sizeof(doc), "k.md");
    snprintf(missing, sizeof(missing), "%s/no-such-dir/k.md", g_dir);
    DraftController d;
    draft_controller_init(&d);
    int error = 0;
    CHECK(!draft_controller_take_failure(&d, &error));
    TextSource src = { "a", 0 };
    CHECK(tick_wait(&d, missing, true, 10.0, &src));
    CHECK(draft_controller_take_failure(&d, &error) && error != 0);
    CHECK(!draft_controller_take_failure(&d, &error)); /* once */
    src.text = "b";
    CHECK(tick_wait(&d, missing, true, 20.0, &src));
    CHECK(!draft_controller_take_failure(&d, &error)); /* the same run of failures */
    CHECK(tick_wait(&d, doc, true, 30.0, &src) && d.path[0]); /* a success */
    draft_controller_discard(&d);
    src.text = "c";
    CHECK(tick_wait(&d, missing, true, 40.0, &src));
    CHECK(draft_controller_take_failure(&d, &error));
    draft_controller_destroy(&d);
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
        { "discard_while_writing", test_discard_while_writing },
        { "switch_while_writing", test_switch_while_writing },
        { "quit_while_writing", test_quit_while_writing },
        { "load_while_writing", test_load_while_writing },
        { "failure_reported_once", test_failure_reported_once },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    rm_rf(g_dir);
    return rc;
}
