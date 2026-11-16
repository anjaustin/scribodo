/* The installed fonts. See font_catalog.h. */
#define _DARWIN_C_SOURCE
#include "app/font_catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/font_file.h"

#define MAX_FILE_BYTES (256u * 1024u * 1024u) /* larger is not a font this offers */

typedef struct {
    FontCatalog *c;
    size_t cap;
    const char *bundled_dir;
    bool oom;
} Scan;

static bool font_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return name[0] != '.' && dot && (!strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") || !strcasecmp(dot, ".ttc"));
}

static bool seen(const FontCatalog *c, const char *family, const char *style)
{
    for (size_t i = 0; i < c->count; i++)
        if (!strcmp(c->items[i].family, family) && !strcmp(c->items[i].style, style)) return true;
    return false;
}

/* A mapped file that shrinks while it is read (a font manager rewriting it) raises SIGBUS on the next page past its new
 * end. The scan runs on the UI thread; the handler, installed only for the scan, abandons that one file. */
static sigjmp_buf g_bus_jump;
static volatile sig_atomic_t g_bus_armed;

static void on_bus(int sig)
{
    (void)sig;
    if (g_bus_armed) siglongjmp(g_bus_jump, 1);
    signal(SIGBUS, SIG_DFL);
    raise(SIGBUS);
}

/* macOS's own interface faces carry an internal family name ("System Font", ".SF NS Mono", hidden by its leading dot),
 * so they are listed by the names Apple uses for them, keeping each file's own style (SF Mono's default weight is
 * Light); other hidden faces stay hidden. */
static const struct { const char *file, *family; } k_apple[] = {
    { "SFNS.ttf", "San Francisco" },  { "SFNSItalic.ttf", "San Francisco" }, { "SFNSRounded.ttf", "SF Rounded" },
    { "SFCompact.ttf", "SF Compact" }, { "SFNSMono.ttf", "SF Mono" },        { "SFNSMonoItalic.ttf", "SF Mono" },
    { "NewYork.ttf", "New York" },     { "NewYorkItalic.ttf", "New York" },
};

static bool apple_name(const char *path, FontFaceName *fn)
{
    static const char sys[] = "/System/Library/Fonts/";
    if (strncmp(path, sys, sizeof(sys) - 1) || strchr(path + sizeof(sys) - 1, '/')) return false;
    for (size_t i = 0; i < sizeof(k_apple) / sizeof(k_apple[0]); i++)
        if (!strcmp(path + sizeof(sys) - 1, k_apple[i].file)) {
            snprintf(fn->family, sizeof(fn->family), "%s", k_apple[i].family);
            return true;
        }
    return false;
}

static void add_faces(Scan *s, const char *path, bool bundled)
{
    /* Non-blocking: a FIFO named *.ttf must not stall the scan; it is refused as not a regular file below. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 12 || (unsigned long long)st.st_size > MAX_FILE_BYTES) {
        close(fd);
        return;
    }
    size_t len = (size_t)st.st_size;
    const uint8_t *d = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (d == MAP_FAILED) return;
    if (sigsetjmp(g_bus_jump, 1)) { /* the file shrank under us: skip it */
        g_bus_armed = 0;
        munmap((void *)d, len);
        return;
    }
    g_bus_armed = 1;
    int faces = font_file_face_count(d, len);
    for (int i = 0; i < faces && !s->oom; i++) {
        FontFaceName fn;
        if (!font_file_face_name(d, len, i, &fn)) continue;
        if (faces == 1) (void)apple_name(path, &fn); /* a renamed system face, else unchanged */
        if (fn.family[0] == '.' || !font_file_has_outlines(d, len, i) ||
            !font_file_has_codepoint(d, len, i, 'A') || seen(s->c, fn.family, fn.style))
            continue;
        if (s->c->count == s->cap) {
            size_t cap = s->cap ? s->cap * 2 : 256;
            FontCatalogEntry *grown = realloc(s->c->items, cap * sizeof(*grown));
            if (!grown) {
                s->oom = true;
                break;
            }
            s->c->items = grown;
            s->cap = cap;
        }
        FontCatalogEntry *e = &s->c->items[s->c->count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->index = i;
        snprintf(e->family, sizeof(e->family), "%s", fn.family);
        snprintf(e->style, sizeof(e->style), "%s", fn.style);
        if (!strcmp(fn.style, "Regular")) snprintf(e->display, sizeof(e->display), "%s", fn.family);
        else snprintf(e->display, sizeof(e->display), "%s %s", fn.family, fn.style);
        e->bundled = bundled;
    }
    g_bus_armed = 0;
    munmap((void *)d, len);
}

static int by_name(const void *a, const void *b)
{
    const char *const *x = a, *const *y = b;
    return strcmp(*x, *y);
}

static void scan_dir(Scan *s, const char *dir, int depth, bool bundled)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    /* Names sorted, so the first of two copies of a face is the same on every run. */
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            char **g = realloc(names, nc * sizeof(*g));
            if (!g) break;
            names = g, cap = nc;
        }
        if (!(names[n] = strdup(de->d_name))) break;
        n++;
    }
    closedir(dp);
    if (n) qsort(names, n, sizeof(*names), by_name);
    for (size_t i = 0; i < n; i++) {
        char path[PATH_MAX];
        int w = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (w > 0 && (size_t)w < sizeof(path) && !s->oom) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (depth > 0) scan_dir(s, path, depth - 1, bundled);
            } else if (font_name(names[i])) {
                add_faces(s, path, bundled);
            }
        }
        free(names[i]);
    }
    free(names);
}

/* Bold, Light, Italic: Regular first, then by style name. */
static int by_family(const void *a, const void *b)
{
    const FontCatalogEntry *x = a, *y = b;
    int c = strcasecmp(x->family, y->family);
    if (c) return c;
    bool xr = !strcmp(x->style, "Regular"), yr = !strcmp(y->style, "Regular");
    if (xr != yr) return xr ? -1 : 1;
    return strcasecmp(x->style, y->style);
}

bool font_catalog_scan(FontCatalog *c, const char *const *dirs, size_t ndirs, const char *bundled_dir)
{
    memset(c, 0, sizeof(*c));
    Scan s = { c, 0, bundled_dir, false };
    struct sigaction bus = { 0 }, old;
    bus.sa_handler = on_bus;
    sigemptyset(&bus.sa_mask);
    sigaction(SIGBUS, &bus, &old);
    for (size_t i = 0; i < ndirs && !s.oom; i++) {
        if (!dirs[i] || !dirs[i][0]) continue;
        bool bundled = bundled_dir && !strcmp(dirs[i], bundled_dir);
        scan_dir(&s, dirs[i], 1, bundled);
    }
    sigaction(SIGBUS, &old, NULL);
    if (s.oom) {
        font_catalog_free(c);
        return false;
    }
    if (c->count) qsort(c->items, c->count, sizeof(*c->items), by_family);
    return true;
}

void font_catalog_free(FontCatalog *c)
{
    free(c->items);
    memset(c, 0, sizeof(*c));
}

static bool contains_ci(const char *hay, const char *word, size_t wlen)
{
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        while (k < wlen && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)word[k])) k++;
        if (k == wlen) return true;
    }
    return false;
}

size_t font_catalog_filter(const FontCatalog *c, const char *filter, size_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < c->count && n < cap; i++) {
        bool all = true;
        const char *f = filter ? filter : "";
        char full[sizeof(c->items[i].family) + sizeof(c->items[i].style) + 1];
        snprintf(full, sizeof(full), "%s %s", c->items[i].family, c->items[i].style);
        while (*f && all) {
            while (*f == ' ') f++;
            size_t w = strcspn(f, " ");
            if (w && !contains_ci(full, f, w)) all = false;
            f += w;
        }
        if (all) out[n++] = i;
    }
    return n;
}

long font_catalog_find(const FontCatalog *c, const char *path, int index)
{
    for (size_t i = 0; i < c->count; i++)
        if (c->items[i].index == index && !strcmp(c->items[i].path, path)) return (long)i;
    return -1;
}
