/* The author's fonts. See appearance.h. */
#define _DARWIN_C_SOURCE
#include "app/appearance.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/file_text.h"
#include "core/json.h"

static const AppearanceRoleInfo k_roles[APPEARANCE_ROLE_COUNT] = {
    { "interface", "Interface", "menus, lists, buttons and labels", "ShareTech-Regular.ttf", "Share Tech", 14.0f, 11.0f, 16.0f, 1.45f },
    { "document", "Documents", "the text you write", "Share-Regular.ttf", "Share", 18.0f, 12.0f, 36.0f, 1.6f },
    { "chat", "Chat", "messages and replies", "Share-Regular.ttf", "Share", 15.0f, 11.0f, 28.0f, 1.5f },
    { "monospace", "Figures", "the status bar, counts and positions", "ShareTechMono-Regular.ttf", "Share Tech Mono", 13.0f, 10.0f, 16.0f, 1.45f },
};

/* Letters the chosen and bundled faces lack (ř, Greek, Cyrillic, ⌘) come from these, in order. */
static const char *const k_fallbacks[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
};

const AppearanceRoleInfo *appearance_role_info(int role)
{
    return role >= 0 && role < APPEARANCE_ROLE_COUNT ? &k_roles[role] : NULL;
}

void appearance_defaults(Appearance *a)
{
    memset(a, 0, sizeof(*a));
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) a->role[r].size = k_roles[r].default_size;
}

void appearance_set_size(Appearance *a, int role, float size)
{
    const AppearanceRoleInfo *in = appearance_role_info(role);
    if (!in) return;
    if (isnan(size)) size = in->default_size;
    size = roundf(size * 2.0f) / 2.0f;
    a->role[role].size = size < in->min_size ? in->min_size : size > in->max_size ? in->max_size : size;
}

static void path_of(char *out, size_t outlen)
{
    const char *env = getenv("SCRIBODO_APPEARANCE");
    if (env && env[0]) snprintf(out, outlen, "%s", env);
    else snprintf(out, outlen, "%s/Library/Application Support/Scribodo/appearance.json", getenv("HOME") ? getenv("HOME") : "");
}

void appearance_load(Appearance *a)
{
    appearance_defaults(a);
    char path[PATH_MAX];
    path_of(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = json_parse(buf);
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) {
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(j, k_roles[r].key);
        if (!cJSON_IsObject(o)) continue;
        const cJSON *font = cJSON_GetObjectItemCaseSensitive(o, "font");
        const cJSON *face = cJSON_GetObjectItemCaseSensitive(o, "face");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(o, "size");
        /* A face is a file path, absolute and short enough to keep whole; anything else is the default. */
        if (cJSON_IsString(font) && font->valuestring[0] == '/' && strlen(font->valuestring) < sizeof(a->role[r].path)) {
            snprintf(a->role[r].path, sizeof(a->role[r].path), "%s", font->valuestring);
            if (cJSON_IsNumber(face) && face->valuedouble >= 0 && face->valuedouble < 256)
                a->role[r].index = (int)face->valuedouble;
        }
        if (cJSON_IsNumber(size)) appearance_set_size(a, r, (float)size->valuedouble);
    }
    cJSON_Delete(j);
}

bool appearance_save(const Appearance *a)
{
    char path[PATH_MAX], dir[PATH_MAX];
    path_of(path, sizeof(path));
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = '\0';
        mkdir(dir, 0700);
    }
    cJSON *j = cJSON_CreateObject();
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) {
        Appearance c = *a;
        appearance_set_size(&c, r, a->role[r].size);
        cJSON *o = cJSON_AddObjectToObject(j, k_roles[r].key);
        if (!o) continue;
        if (c.role[r].path[0]) {
            cJSON_AddStringToObject(o, "font", c.role[r].path);
            cJSON_AddNumberToObject(o, "face", c.role[r].index);
        }
        cJSON_AddNumberToObject(o, "size", c.role[r].size);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return false;
    bool ok = file_text_write_atomic(path, s, strlen(s), 0644);
    free(s);
    return ok;
}

/* macOS's own fonts sit on the read-only, signed system volume; the bundled ones are pinned by hash. Anything else
 * (/Library/Fonts, ~/Library/Fonts, a path in appearance.json) is probed before it is loaded (ui/font.h). */
static bool trusted_path(const char *path, const char *assets_dir)
{
    static const char sys[] = "/System/Library/Fonts/";
    if (!strncmp(path, sys, sizeof(sys) - 1) && !strstr(path, "/../")) return true;
    if (!assets_dir || !assets_dir[0]) return false;
    size_t n = strlen(assets_dir);
    return !strncmp(path, assets_dir, n) && !strncmp(path + n, "/fonts/", 7) && !strstr(path + n, "/../");
}

bool appearance_has_choice(const Appearance *a, int role)
{
    return appearance_role_info(role) && a->role[role].path[0];
}

int appearance_sources(const Appearance *a, int role, bool with_choice, const char *assets_dir,
                       SFontSource out[SFONT_MAX_FACES], char paths[SFONT_MAX_FACES][PATH_MAX])
{
    const AppearanceRoleInfo *in = appearance_role_info(role);
    if (!in) return 0;
    int n = 0;
    if (with_choice && a->role[role].path[0]) {
        snprintf(paths[n], PATH_MAX, "%s", a->role[role].path);
        out[n] = (SFontSource){ paths[n], a->role[role].index, trusted_path(paths[n], assets_dir) };
        n++;
    } else if (assets_dir && assets_dir[0]) {
        int w = snprintf(paths[n], PATH_MAX, "%s/fonts/%s", assets_dir, in->default_file);
        if (w > 0 && w < PATH_MAX) {
            out[n] = (SFontSource){ paths[n], 0, true };
            n++;
        }
    }
    for (size_t i = 0; i < sizeof(k_fallbacks) / sizeof(k_fallbacks[0]) && n < SFONT_MAX_FACES; i++) {
        snprintf(paths[n], PATH_MAX, "%s", k_fallbacks[i]);
        out[n] = (SFontSource){ paths[n], 0, true };
        n++;
    }
    return n;
}

static bool has_default_fonts(const char *dir)
{
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) {
        char p[PATH_MAX];
        int n = snprintf(p, sizeof(p), "%s/fonts/%s", dir, k_roles[r].default_file);
        if (n <= 0 || n >= (int)sizeof(p) || access(p, R_OK) != 0) return false;
    }
    return true;
}

bool assets_dir_resolve(const char *app_dir, char *out, size_t outlen)
{
    if (!out || !outlen) return false;
    out[0] = '\0';
    if (!app_dir) app_dir = "";
    const char *sep = app_dir[0] && app_dir[strlen(app_dir) - 1] != '/' ? "/" : "";
    static const char *const rel[] = { "../Resources/assets", "../assets" };
    for (size_t i = 0; i < 2; i++) {
        int n = snprintf(out, outlen, "%s%s%s", app_dir, sep, rel[i]);
        if (n > 0 && (size_t)n < outlen && has_default_fonts(out)) return true;
    }
    out[0] = '\0';
    return false;
}
