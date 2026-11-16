/* src/app/appearance.c: defaults, the saved file round trip, clamping, malformed files, the face order per role and
 * where the bundled fonts are found. No window: the sources are checked as data, not loaded. */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/appearance.h"
#include "ui/theme.h"
#include "test_util.h"

static char g_dir[PATH_MAX];
static char g_file[PATH_MAX];

static void write_file(const char *s)
{
    FILE *f = fopen(g_file, "wb");
    fputs(s, f);
    fclose(f);
}

static void test_defaults(void)
{
    Appearance a;
    appearance_defaults(&a);
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) {
        CHECK(a.role[r].path[0] == '\0');
        CHECK(a.role[r].size == appearance_role_info(r)->default_size);
    }
    CHECK(!strcmp(appearance_role_info(APPEARANCE_INTERFACE)->default_file, "ShareTech-Regular.ttf"));
    CHECK(!strcmp(appearance_role_info(APPEARANCE_DOCUMENT)->default_file, "Share-Regular.ttf"));
    CHECK(!strcmp(appearance_role_info(APPEARANCE_CHAT)->default_file, "Share-Regular.ttf"));
    CHECK(!strcmp(appearance_role_info(APPEARANCE_MONO)->default_file, "ShareTechMono-Regular.ttf"));
    CHECK(appearance_role_info(APPEARANCE_ROLE_COUNT) == NULL && appearance_role_info(-1) == NULL);
    CHECK(a.theme == THEME_DARK && !a.typewriter && !a.dim_paragraphs);
}

static void test_theme_and_writing_aids(void)
{
    Appearance a;
    appearance_defaults(&a);
    a.theme = THEME_PAPER;
    a.typewriter = true;
    CHECK(appearance_save(&a));
    Appearance b;
    appearance_load(&b);
    CHECK(b.theme == THEME_PAPER && b.typewriter && !b.dim_paragraphs);
    write_file("{\"theme\":\"neon\",\"typewriter\":1,\"dim_paragraphs\":true}");
    appearance_load(&b);
    CHECK(b.theme == THEME_DARK);   /* an unknown palette: the default */
    CHECK(!b.typewriter);           /* only JSON true turns an aid on */
    CHECK(b.dim_paragraphs);
}

static void test_missing_file_is_the_defaults(void)
{
    unlink(g_file);
    Appearance a, d;
    appearance_load(&a);
    appearance_defaults(&d);
    CHECK(!memcmp(&a, &d, sizeof(a)));
}

static void test_round_trip(void)
{
    Appearance a;
    appearance_defaults(&a);
    snprintf(a.role[APPEARANCE_DOCUMENT].path, PATH_MAX, "/System/Library/Fonts/Supplemental/Georgia.ttf");
    snprintf(a.role[APPEARANCE_MONO].path, PATH_MAX, "/System/Library/Fonts/Menlo.ttc");
    a.role[APPEARANCE_MONO].index = 2;
    appearance_set_size(&a, APPEARANCE_DOCUMENT, 21.0f);
    CHECK(appearance_save(&a));
    Appearance b;
    appearance_load(&b);
    CHECK(!strcmp(b.role[APPEARANCE_DOCUMENT].path, "/System/Library/Fonts/Supplemental/Georgia.ttf"));
    CHECK(b.role[APPEARANCE_DOCUMENT].size == 21.0f);
    CHECK(!strcmp(b.role[APPEARANCE_MONO].path, "/System/Library/Fonts/Menlo.ttc") && b.role[APPEARANCE_MONO].index == 2);
    CHECK(!b.role[APPEARANCE_INTERFACE].path[0] && !appearance_has_choice(&b, APPEARANCE_INTERFACE));
    CHECK(appearance_has_choice(&b, APPEARANCE_DOCUMENT));
    struct stat st;
    CHECK(stat(g_file, &st) == 0 && (st.st_mode & 0777) == 0644);
}

static void test_clamping(void)
{
    Appearance a;
    appearance_defaults(&a);
    const AppearanceRoleInfo *in = appearance_role_info(APPEARANCE_INTERFACE);
    appearance_set_size(&a, APPEARANCE_INTERFACE, 500.0f);
    CHECK(a.role[APPEARANCE_INTERFACE].size == in->max_size);
    appearance_set_size(&a, APPEARANCE_INTERFACE, -3.0f);
    CHECK(a.role[APPEARANCE_INTERFACE].size == in->min_size);
    appearance_set_size(&a, APPEARANCE_INTERFACE, 14.26f);
    CHECK(a.role[APPEARANCE_INTERFACE].size == 14.5f); /* the 0.5 px grid */
    appearance_set_size(&a, APPEARANCE_INTERFACE, 0.0f / 0.0f);
    CHECK(a.role[APPEARANCE_INTERFACE].size == in->default_size);
    appearance_set_size(&a, APPEARANCE_ROLE_COUNT, 12.0f); /* no such role: nothing written */
}

/* A malformed role falls back alone; a relative path, a non-string font or an absurd face is not taken. */
static void test_malformed(void)
{
    write_file("{\"interface\":{\"font\":\"relative.ttf\",\"size\":99},\"document\":{\"font\":7,\"size\":\"big\"},"
               "\"chat\":{\"font\":\"/a/b.ttf\",\"face\":-1,\"size\":16},\"monospace\":[1,2]}");
    Appearance a;
    appearance_load(&a);
    CHECK(!a.role[APPEARANCE_INTERFACE].path[0]);
    CHECK(a.role[APPEARANCE_INTERFACE].size == appearance_role_info(APPEARANCE_INTERFACE)->max_size);
    CHECK(!a.role[APPEARANCE_DOCUMENT].path[0]);
    CHECK(a.role[APPEARANCE_DOCUMENT].size == appearance_role_info(APPEARANCE_DOCUMENT)->default_size);
    CHECK(!strcmp(a.role[APPEARANCE_CHAT].path, "/a/b.ttf") && a.role[APPEARANCE_CHAT].index == 0);
    CHECK(a.role[APPEARANCE_CHAT].size == 16.0f);
    CHECK(a.role[APPEARANCE_MONO].size == appearance_role_info(APPEARANCE_MONO)->default_size);
    write_file("not json at all");
    appearance_load(&a);
    CHECK(!a.role[APPEARANCE_CHAT].path[0]);
    char big[PATH_MAX + 64];
    memset(big, 'x', sizeof(big));
    big[0] = '/';
    big[sizeof(big) - 1] = '\0';
    char json[PATH_MAX + 128];
    snprintf(json, sizeof(json), "{\"chat\":{\"font\":\"%s\"}}", big);
    write_file(json);
    appearance_load(&a);
    CHECK(!a.role[APPEARANCE_CHAT].path[0]); /* too long to keep whole: the default, never a cut path */
}

static void test_sources(void)
{
    Appearance a;
    appearance_defaults(&a);
    SFontSource src[SFONT_MAX_FACES];
    char paths[SFONT_MAX_FACES][PATH_MAX];
    int n = appearance_sources(&a, APPEARANCE_DOCUMENT, true, "/assets", src, paths);
    CHECK(n == 3);
    CHECK(!strcmp(src[0].path, "/assets/fonts/Share-Regular.ttf") && src[0].index == 0);
    CHECK(strstr(src[1].path, "/Arial.ttf") && strstr(src[2].path, "/Arial Unicode.ttf"));
    CHECK(src[0].trusted && src[1].trusted && src[2].trusted); /* bundled and macOS's own: no probe */
    snprintf(a.role[APPEARANCE_DOCUMENT].path, PATH_MAX, "/fonts/Charter.ttc");
    a.role[APPEARANCE_DOCUMENT].index = 3;
    n = appearance_sources(&a, APPEARANCE_DOCUMENT, true, "/assets", src, paths);
    CHECK(n == 3 && !strcmp(src[0].path, "/fonts/Charter.ttc") && src[0].index == 3);
    CHECK(!src[0].trusted); /* outside the system volume and the bundle: probed before loading */
    const char *trust[][2] = { { "/System/Library/Fonts/Supplemental/Georgia.ttf", "1" }, { "/Library/Fonts/X.ttf", "0" },
                               { "/assets/fonts/Share-Regular.ttf", "1" }, { "/assets/fontsX/a.ttf", "0" },
                               { "/System/Library/Fonts/../../../Users/x/evil.ttf", "0" }, { "/assets/fonts/../../x.ttf", "0" },
                               { "/Users/me/Library/Fonts/Share-Regular.ttf", "0" } };
    for (size_t i = 0; i < sizeof(trust) / sizeof(trust[0]); i++) {
        snprintf(a.role[APPEARANCE_CHAT].path, PATH_MAX, "%s", trust[i][0]);
        appearance_sources(&a, APPEARANCE_CHAT, true, "/assets", src, paths);
        CHECK(src[0].trusted == (trust[i][1][0] == '1'));
    }
    a.role[APPEARANCE_CHAT].path[0] = '\0';
    CHECK(strstr(src[1].path, "/Arial.ttf") != NULL); /* a chosen face's missing letters: Arial, not Share */
    n = appearance_sources(&a, APPEARANCE_DOCUMENT, false, "/assets", src, paths); /* the choice failed to load */
    CHECK(n == 3 && !strcmp(src[0].path, "/assets/fonts/Share-Regular.ttf"));
    n = appearance_sources(&a, APPEARANCE_INTERFACE, true, "", src, paths); /* no bundled fonts found */
    CHECK(n == 2 && strstr(src[0].path, "/Arial.ttf"));
    CHECK(appearance_sources(&a, 9, true, "/assets", src, paths) == 0);
}

static void test_assets_dir(void)
{
    char out[PATH_MAX], app[PATH_MAX];
    char cwd[PATH_MAX];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    snprintf(app, sizeof(app), "%s/build", cwd); /* make run: build/scribodo beside assets/ */
    CHECK(assets_dir_resolve(app, out, sizeof(out)));
    CHECK(strstr(out, "/build/../assets") != NULL);
    CHECK(!assets_dir_resolve(g_dir, out, sizeof(out)) && !out[0]); /* nothing there */
    char tiny[8];
    CHECK(!assets_dir_resolve(app, tiny, sizeof(tiny)) && !tiny[0]); /* never a partial path */
    /* A bundle: Contents/MacOS/scribodo with Contents/Resources/assets/fonts. */
    char b[PATH_MAX];
    snprintf(b, sizeof(b), "%s/Scribodo.app/Contents/Resources/assets/fonts", g_dir);
    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s/Scribodo.app/Contents/MacOS' && cp assets/fonts/*.ttf '%s'", b, g_dir, b);
    CHECK(system(cmd) == 0);
    snprintf(app, sizeof(app), "%s/Scribodo.app/Contents/MacOS/", g_dir);
    CHECK(assets_dir_resolve(app, out, sizeof(out)) && strstr(out, "Contents/MacOS/../Resources/assets"));
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/scribodo-test-appearance-%d", (int)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_file, sizeof(g_file), "%s/appearance.json", g_dir);
    setenv("SCRIBODO_APPEARANCE", g_file, 1);
    static const TestCase tests[] = {
        { "defaults", test_defaults },
        { "missing_file_is_the_defaults", test_missing_file_is_the_defaults },
        { "round_trip", test_round_trip },
        { "clamping", test_clamping },
        { "theme_and_writing_aids", test_theme_and_writing_aids },
        { "malformed", test_malformed },
        { "sources", test_sources },
        { "assets_dir", test_assets_dir },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) rc = 1;
    return rc;
}
