/* The fonts an author can choose in the Fonts panel: every face of every .ttf, .otf and .ttc file in the bundled
 * fonts folder, /System/Library/Fonts (and its Supplemental folder), /Library/Fonts and ~/Library/Fonts, named by
 * the face's own family and style (core/font_file). Left out: faces without drawable outlines (emoji bitmaps), faces
 * that map no Latin capital A (symbol and CJK-only faces would draw the interface as fallbacks), and hidden system
 * faces (a family beginning with "."), except macOS's own interface faces, listed by Apple's public names: San
 * Francisco, SF Rounded, SF Compact, SF Mono and New York. A family and style seen twice is listed once, the first folder winning, so
 * the bundled Share faces come before an installed copy.
 *
 * Files are mapped, not read: only the pages holding the directory, the names and the cmap are touched, so a scan of
 * a few hundred system fonts reads a few megabytes. Measured by font_catalog_scan's caller (the status line). */
#ifndef SCRIBODO_FONT_CATALOG_H
#define SCRIBODO_FONT_CATALOG_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char path[PATH_MAX];
    int index;
    char family[128];
    char style[64];
    char display[200]; /* "Family" for a Regular face, else "Family Style" */
    bool bundled;
} FontCatalogEntry;

typedef struct {
    FontCatalogEntry *items;
    size_t count;
} FontCatalog;

/* Scans `dirs` in order (one level of subfolders too). Entries are sorted by family, then Regular first, then style.
 * False only when out of memory (the catalog is then empty). `bundled_dir` marks its entries as bundled. */
bool font_catalog_scan(FontCatalog *c, const char *const *dirs, size_t ndirs, const char *bundled_dir);
void font_catalog_free(FontCatalog *c);

/* The entries whose family and style ("Menlo Regular", though a Regular face is displayed by its family alone)
 * contain every space-separated word of `filter` (ASCII case-insensitive), as
 * indexes into c->items written to `out` (at most `cap`). Returns the count. An empty filter matches all. */
size_t font_catalog_filter(const FontCatalog *c, const char *filter, size_t *out, size_t cap);

/* The entry for a path and face, or -1. */
long font_catalog_find(const FontCatalog *c, const char *path, int index);

#endif
