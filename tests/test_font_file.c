/* src/core/font_file.c against the bundled Share fonts and macOS's own collections, then truncated and corrupted
 * copies of them: a malformed file is refused, never read past its end (run under ASan by make sanitize). */
#include <stdlib.h>
#include <string.h>

#include "core/font_file.h"
#include "test_util.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *len = d ? (size_t)n : 0;
    return d;
}

static void expect_name(const char *path, const char *family, const char *style)
{
    size_t n;
    uint8_t *d = slurp(path, &n);
    CHECK(d != NULL);
    if (!d) return;
    FontFaceName fn;
    CHECK(font_file_face_count(d, n) == 1);
    CHECK(font_file_face_name(d, n, 0, &fn));
    CHECK(!strcmp(fn.family, family));
    CHECK(!strcmp(fn.style, style));
    CHECK(!font_file_face_name(d, n, 1, &fn)); /* a single face has no index 1 */
    free(d);
}

static void test_bundled_names(void)
{
    expect_name("assets/fonts/Share-Regular.ttf", "Share", "Regular");
    expect_name("assets/fonts/ShareTech-Regular.ttf", "Share Tech", "Regular");
    expect_name("assets/fonts/ShareTechMono-Regular.ttf", "Share Tech Mono", "Regular");
}

static void test_bundled_coverage(void)
{
    size_t n;
    uint8_t *d = slurp("assets/fonts/Share-Regular.ttf", &n);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(font_file_has_codepoint(d, n, 0, 'A'));
    CHECK(font_file_has_codepoint(d, n, 0, 0x00E9));  /* é */
    CHECK(font_file_has_codepoint(d, n, 0, 0x2019));  /* ’ */
    CHECK(font_file_has_codepoint(d, n, 0, 0x2026));  /* … */
    CHECK(!font_file_has_codepoint(d, n, 0, 0x0159)); /* ř: outside Share, from a fallback (assets/fonts/README.md) */
    CHECK(!font_file_has_codepoint(d, n, 0, 0x2318)); /* ⌘ */
    CHECK(!font_file_has_codepoint(d, n, 1, 'A'));
    CHECK(font_file_has_outlines(d, n, 0));
    free(d);
    d = slurp("/System/Library/Fonts/Apple Color Emoji.ttc", &n);
    if (d) { /* bitmaps only: not offered */
        CHECK(font_file_face_count(d, n) >= 1);
        CHECK(!font_file_has_outlines(d, n, 0));
        free(d);
    }
}

/* Each table of the extracted face is byte-identical to the original's. */
static bool same_tables(const uint8_t *orig, size_t on, int index, const uint8_t *x, size_t xn)
{
    size_t dir = 0;
    if (!memcmp(orig, "ttcf", 4)) dir = (size_t)((orig[12 + 4 * index] << 24) | (orig[13 + 4 * index] << 16) | (orig[14 + 4 * index] << 8) | orig[15 + 4 * index]);
    unsigned nt = (unsigned)((orig[dir + 4] << 8) | orig[dir + 5]);
    if (xn < 12 + 16 * (size_t)nt || ((x[4] << 8) | x[5]) != (int)nt) return false;
    for (unsigned i = 0; i < nt; i++) {
        const uint8_t *a = orig + dir + 12 + 16 * i, *b = x + 12 + 16 * i;
        if (memcmp(a, b, 8) || memcmp(a + 12, b + 12, 4)) return false;
        size_t ao = (size_t)((a[8] << 24) | (a[9] << 16) | (a[10] << 8) | a[11]);
        size_t bo = (size_t)((b[8] << 24) | (b[9] << 16) | (b[10] << 8) | b[11]);
        size_t l = (size_t)((a[12] << 24) | (a[13] << 16) | (a[14] << 8) | a[15]);
        if (bo % 4 || bo + l > xn || ao + l > on || memcmp(orig + ao, x + bo, l)) return false;
    }
    return true;
}

static void test_collection(void)
{
    const char *path = "/System/Library/Fonts/Helvetica.ttc";
    size_t n;
    uint8_t *d = slurp(path, &n);
    if (!d) {
        printf("     (no %s: collection checks skipped)\n", path);
        return;
    }
    int faces = font_file_face_count(d, n);
    CHECK(faces > 1);
    bool saw_bold = false;
    for (int i = 0; i < faces; i++) {
        FontFaceName a, b;
        CHECK(font_file_face_name(d, n, i, &a));
        CHECK(!strcmp(a.family, "Helvetica"));
        saw_bold = saw_bold || !strcmp(a.style, "Bold");
        size_t xn = 0;
        uint8_t *x = font_file_extract_face(d, n, i, &xn);
        CHECK(x != NULL);
        if (!x) continue;
        CHECK(font_file_face_count(x, xn) == 1);
        CHECK(font_file_face_name(x, xn, 0, &b) && !strcmp(a.family, b.family) && !strcmp(a.style, b.style));
        CHECK(font_file_has_codepoint(x, xn, 0, 'H') == font_file_has_codepoint(d, n, i, 'H'));
        CHECK(font_file_has_codepoint(x, xn, 0, 0x0159) == font_file_has_codepoint(d, n, i, 0x0159));
        CHECK(same_tables(d, n, i, x, xn));
        free(x);
    }
    CHECK(saw_bold);
    CHECK(font_file_extract_face(d, n, faces, &(size_t){ 0 }) == NULL);
    free(d);
}




/* Every entry point on one buffer; ASan reports any read outside it. */
static void exercise(const uint8_t *d, size_t n)
{
    int faces = font_file_face_count(d, n);
    for (int i = 0; i < (faces > 3 ? 3 : faces) + 1; i++) {
        FontFaceName fn;
        (void)font_file_face_name(d, n, i, &fn);
        (void)font_file_has_codepoint(d, n, i, 'A');
        (void)font_file_has_codepoint(d, n, i, 0x1F600);
        (void)font_file_has_outlines(d, n, i);
        size_t xn;
        free(font_file_extract_face(d, n, i, &xn));
    }
}

/* Copies of exactly the prefix length, so a read past it is outside the allocation. */
static void test_truncated(void)
{
    const char *paths[] = { "assets/fonts/ShareTechMono-Regular.ttf", "/System/Library/Fonts/Helvetica.ttc" };
    for (size_t p = 0; p < 2; p++) {
        size_t n;
        uint8_t *d = slurp(paths[p], &n);
        if (!d) continue;
        size_t limit = n < 300000 ? n : 300000;
        for (size_t cut = 0; cut <= limit; cut += cut < 4096 ? 1 : 251) {
            uint8_t *c = malloc(cut ? cut : 1);
            memcpy(c, d, cut);
            exercise(c, cut);
            free(c);
        }
        free(d);
    }
    CHECK(font_file_face_count(NULL, 0) == 0);
    CHECK(!font_file_face_name(NULL, 0, 0, &(FontFaceName){ 0 }));
}

static void test_corrupted(void)
{
    size_t n;
    uint8_t *d = slurp("assets/fonts/ShareTech-Regular.ttf", &n);
    CHECK(d != NULL);
    if (!d) return;
    uint8_t *c = malloc(n);
    size_t head = 1024; /* the directory and the start of the tables: where a flipped byte changes an offset */
    for (int round = 0; round < 3000; round++) {
        memcpy(c, d, n);
        int flips = 1 + (int)(rnd() % 8);
        for (int k = 0; k < flips; k++) c[rnd() % (round % 2 ? n : head)] = (uint8_t)rnd();
        exercise(c, n);
    }
    /* A collection header that claims more faces than it has offsets for. */
    uint8_t ttc[16] = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 200, 0, 0, 0, 16 };
    exercise(ttc, sizeof(ttc));
    CHECK(font_file_face_count(ttc, sizeof(ttc)) == 200);
    CHECK(!font_file_face_name(ttc, sizeof(ttc), 0, &(FontFaceName){ 0 }));
    free(c);
    free(d);
}

int main(void)
{
    static const TestCase tests[] = {
        { "bundled_names", test_bundled_names },
        { "bundled_coverage", test_bundled_coverage },
        { "collection", test_collection },
        { "truncated", test_truncated },
        { "corrupted", test_corrupted },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
