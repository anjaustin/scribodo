/* The author's fonts: one face and size per role, chosen in the Fonts panel (⌘,). App-wide:
 * ~/Library/Application Support/Scribodo/appearance.json (or $SCRIBODO_APPEARANCE; a UI test keeps its own,
 * startup_env). A role with no chosen face uses the bundled default (assets/fonts: Share Tech for the interface,
 * Share for documents and chat, Share Tech Mono for figures); sizes are clamped to the role's range on load and when
 * changed. The defaults are the author's choice of 2026-11-16. */
#ifndef SCRIBODO_APPEARANCE_H
#define SCRIBODO_APPEARANCE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui/font.h"

enum { APPEARANCE_INTERFACE, APPEARANCE_DOCUMENT, APPEARANCE_CHAT, APPEARANCE_MONO, APPEARANCE_ROLE_COUNT };

typedef struct {
    char path[PATH_MAX]; /* "" = the bundled default */
    int index;           /* the face within a collection */
    float size;          /* logical px */
} AppearanceFont;

typedef struct {
    AppearanceFont role[APPEARANCE_ROLE_COUNT];
} Appearance;

typedef struct {
    const char *key;          /* in appearance.json */
    const char *label;        /* in the Fonts panel */
    const char *hint;
    const char *default_file; /* under assets/fonts */
    const char *default_name; /* its family, as the Fonts panel lists it */
    float default_size, min_size, max_size;
    float line_height;        /* a multiple of the size */
} AppearanceRoleInfo;

const AppearanceRoleInfo *appearance_role_info(int role);
void appearance_defaults(Appearance *a);
/* Missing, unreadable or malformed: the defaults (a malformed role alone falls back alone). */
void appearance_load(Appearance *a);
bool appearance_save(const Appearance *a);
void appearance_set_size(Appearance *a, int role, float size); /* clamped, on a 0.5 px grid */

/* The faces to try for `role`, in order: the chosen face when `with_choice` and one is chosen, else the bundled
 * default under `assets_dir`; then Arial and Arial Unicode for the letters it lacks. `paths` holds the strings the
 * sources point into. Returns the count (at most SFONT_MAX_FACES). A chosen face that fails to load is not covered
 * for by the bundled one here: the caller sees sfont's primary_source and loads again without the choice. */
int appearance_sources(const Appearance *a, int role, bool with_choice, const char *assets_dir,
                       SFontSource out[SFONT_MAX_FACES], char paths[SFONT_MAX_FACES][PATH_MAX]);
/* True when the role has a chosen face. */
bool appearance_has_choice(const Appearance *a, int role);

/* The folder holding fonts/ (the bundle's Contents/Resources/assets, else assets/ beside build/); false when neither
 * holds the bundled default fonts. `app_dir` as for tools_dir_resolve. */
bool assets_dir_resolve(const char *app_dir, char *out, size_t outlen);

#endif
