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
    CHECK(!draft_controller_tick(&d, doc, true, 5.0, source_text, &src) && src.calls == 0); /* before the interval */
    CHECK(draft_controller_tick(&d, doc, true, 10.0, source_text, &src) && src.calls == 1);
    CHECK(d.path[0] && !strcmp(d.doc, doc) && file_is(d.path, "first\n"));
    CHECK(d.hash == draft_text_hash("first\n") && d.at == 10.0);
    CHECK(!draft_controller_tick(&d, doc, true, 19.9, source_text, &src) && src.calls == 1);
    CHECK(!draft_controller_tick(&d, doc, true, 20.0, source_text, &src) && src.calls == 2); /* unchanged: no write */
    char before[PATH_MAX];
    snprintf(before, sizeof(before), "%s", d.path);
    src.text = "second\n";
    CHECK(draft_controller_tick(&d, doc, true, 30.0, source_text, &src));
    CHECK(strcmp(before, d.path) != 0 && access(before, F_OK) != 0 && file_is(d.path, "second\n"));
    CHECK(count_drafts(g_dir, "a.md.scribodo-") == 1);
    CHECK(file_is(doc, "saved\n")); /* the document itself is untouched */
    draft_controller_discard(&d);
    CHECK(!d.path[0] && !d.doc[0] && d.hash == 0 && count_drafts(g_dir, "a.md.scribodo-") == 0);
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
    CHECK(draft_controller_tick(&d, doc, true, 10.0, source_text, &src));
    CHECK(!draft_controller_tick(&d, doc, false, 11.0, source_text, &src)); /* saved: clean */
    CHECK(!d.path[0] && count_drafts(g_dir, "b.md.scribodo-") == 0);
    CHECK(draft_controller_tick(&d, doc, true, 21.0, source_text, &src));
    CHECK(!draft_controller_tick(&d, NULL, false, 22.0, source_text, &src)); /* closed */
    CHECK(!d.path[0] && count_drafts(g_dir, "b.md.scribodo-") == 0);
    CHECK(draft_controller_tick(&d, doc, true, 32.0, source_text, &src));
    int calls = src.calls;
    CHECK(!draft_controller_tick(&d, other, true, 50.0, source_text, &src)); /* another document: dropped first */
    CHECK(!d.path[0] && count_drafts(g_dir, "b.md.scribodo-") == 0 && src.calls == calls);
    CHECK(draft_controller_tick(&d, other, true, 50.0, source_text, &src) && !strcmp(d.doc, other));
    draft_controller_discard(&d);
    draft_controller_discard(&d); /* idempotent */
    CHECK(count_drafts(g_dir, "c.md.scribodo-") == 0);
}

/* No text, or a write that cannot happen, leaves no draft and is retried on the next interval. */
static void test_failures(void)
{
    DraftController d;
    draft_controller_init(&d);
    char doc[700];
    make_doc(doc, sizeof(doc), "d.md");
    TextSource none = { NULL, 0 };
    CHECK(!draft_controller_tick(&d, doc, true, 10.0, source_text, &none) && none.calls == 1 && !d.path[0]);
    char missing[700];
    snprintf(missing, sizeof(missing), "%s/no-such-dir/e.md", g_dir);
    TextSource src = { "x", 0 };
    CHECK(!draft_controller_tick(&d, missing, true, 20.0, source_text, &src) && src.calls == 1 && !d.path[0]);
    CHECK(d.hash == 0);
    CHECK(draft_controller_tick(&d, doc, true, 30.0, source_text, &src) && d.path[0]);
    draft_controller_discard(&d);
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
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    rm_rf(g_dir);
    return rc;
}
