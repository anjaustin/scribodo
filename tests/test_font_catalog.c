/* src/app/font_catalog.c: the bundled faces, a folder of decoys, de-duplication, the filter, and a timed scan of the
 * system's own fonts (printed; the Fonts panel scans when it opens). */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app/font_catalog.h"
#include "test_util.h"

static char g_dir[PATH_MAX];

static void put(const char *name, const void *data, size_t n)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb");
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}

static void copy_font(const char *from, const char *to)
{
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s/%s'", from, g_dir, to);
    CHECK(system(cmd) == 0);
}

static void test_bundled(void)
{
    const char *dirs[] = { "assets/fonts" };
    FontCatalog c;
    CHECK(font_catalog_scan(&c, dirs, 1, "assets/fonts"));
    CHECK(c.count == 3);
    if (c.count == 3) {
        CHECK(!strcmp(c.items[0].display, "Share") && !strcmp(c.items[1].display, "Share Tech") &&
              !strcmp(c.items[2].display, "Share Tech Mono"));
        CHECK(c.items[0].bundled && c.items[2].bundled && c.items[0].index == 0);
        CHECK(!strcmp(c.items[1].path, "assets/fonts/ShareTech-Regular.ttf"));
    }
    CHECK(font_catalog_find(&c, "assets/fonts/ShareTechMono-Regular.ttf", 0) == 2);
    CHECK(font_catalog_find(&c, "assets/fonts/ShareTechMono-Regular.ttf", 1) == -1);
    font_catalog_free(&c);
    CHECK(c.items == NULL && c.count == 0);
}

/* Decoys are skipped; a copy of a bundled face is listed once, as the bundled one; a subfolder is read. */
static void test_decoys_and_duplicates(void)
{
    put("empty.ttf", "", 0);
    put("text.otf", "this is not a font at all", 25);
    put("README.txt", "x", 1);
    put(".hidden.ttf", "x", 1);
    char fifo[PATH_MAX];
    snprintf(fifo, sizeof(fifo), "%s/pipe.ttf", g_dir);
    CHECK(mkfifo(fifo, 0600) == 0); /* no writer: a blocking open would never return (red team, 2026-11-16) */
    copy_font("assets/fonts/Share-Regular.ttf", "Copy-of-Share.ttf");
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/more", g_dir);
    mkdir(sub, 0700);
    copy_font("assets/fonts/ShareTechMono-Regular.ttf", "more/Mono.ttf");
    const char *dirs[] = { "assets/fonts", g_dir };
    FontCatalog c;
    CHECK(font_catalog_scan(&c, dirs, 2, "assets/fonts"));
    CHECK(c.count == 3);
    for (size_t i = 0; i < c.count; i++) CHECK(c.items[i].bundled);
    font_catalog_free(&c);
    const char *only[] = { g_dir };
    CHECK(font_catalog_scan(&c, only, 1, NULL));
    CHECK(c.count == 2); /* Share from the copy, Share Tech Mono from the subfolder */
    CHECK(c.count == 2 && !c.items[0].bundled && strstr(c.items[0].path, "Copy-of-Share.ttf"));
    font_catalog_free(&c);
    const char *none[] = { "/nonexistent-scribodo-fonts", "" , NULL };
    CHECK(font_catalog_scan(&c, none, 3, NULL) && c.count == 0);
}

static void test_filter(void)
{
    const char *dirs[] = { "assets/fonts" };
    FontCatalog c;
    CHECK(font_catalog_scan(&c, dirs, 1, NULL));
    size_t idx[8];
    CHECK(font_catalog_filter(&c, "", idx, 8) == 3);
    CHECK(font_catalog_filter(&c, NULL, idx, 8) == 3);
    CHECK(font_catalog_filter(&c, "tech", idx, 8) == 2);
    CHECK(font_catalog_filter(&c, "  MONO  tech ", idx, 8) == 1 && idx[0] == 2);
    CHECK(font_catalog_filter(&c, "mono serif", idx, 8) == 0);
    CHECK(font_catalog_filter(&c, "share", idx, 2) == 2); /* capped */
    CHECK(font_catalog_filter(&c, "tech regular", idx, 8) == 2); /* the style, though "Regular" is not displayed */
    font_catalog_free(&c);
}

static void test_system_scan(void)
{
    const char *dirs[] = { "assets/fonts", "/System/Library/Fonts", "/Library/Fonts" };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    FontCatalog c;
    CHECK(font_catalog_scan(&c, dirs, 3, "assets/fonts"));
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    printf("     system scan: %zu faces in %.0f ms\n", c.count, ms);
    long georgia = -1, helvetica_bold = -1;
    for (size_t i = 0; i < c.count; i++) {
        CHECK(c.items[i].family[0] != '.');
        if (!strcmp(c.items[i].display, "Georgia")) georgia = (long)i;
        if (!strcmp(c.items[i].display, "Helvetica Bold")) helvetica_bold = (long)i;
        if (i) CHECK(strcasecmp(c.items[i - 1].family, c.items[i].family) <= 0);
    }
    if (access("/System/Library/Fonts/Supplemental/Georgia.ttf", R_OK) == 0) CHECK(georgia >= 0);
    if (access("/System/Library/Fonts/Helvetica.ttc", R_OK) == 0) CHECK(helvetica_bold >= 0 && c.items[helvetica_bold].index > 0);
    size_t idx[4];
    CHECK(font_catalog_filter(&c, "apple color emoji", idx, 4) == 0); /* bitmaps: not offered */
    font_catalog_free(&c);
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/scribodo-test-fontcat-%d", (int)getpid());
    mkdir(g_dir, 0700);
    static const TestCase tests[] = {
        { "bundled", test_bundled },
        { "decoys_and_duplicates", test_decoys_and_duplicates },
        { "filter", test_filter },
        { "system_scan", test_system_scan },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) rc = 1;
    return rc;
}
