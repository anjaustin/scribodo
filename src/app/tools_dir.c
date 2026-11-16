#include "tools_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool has_services(const char *dir)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/services.sh", dir);
    return n > 0 && (size_t)n < sizeof(path) && access(path, X_OK) == 0;
}

static bool write_path(char *out, size_t outlen, const char *a, const char *b)
{
    int n = snprintf(out, outlen, "%s%s", a, b);
    return n > 0 && (size_t)n < outlen;
}

bool tools_dir_resolve(const char *app_dir, char *out, size_t outlen)
{
    if (!out || !outlen) return false;
    out[0] = '\0';
    const char *override = getenv("SCRIBODO_TOOLS_DIR");
    if (override && override[0]) return write_path(out, outlen, override, "") && has_services(out);
    if (!app_dir) app_dir = "";
    const char *sep = app_dir[0] && app_dir[strlen(app_dir) - 1] != '/' ? "/" : "";
    char base[4096];
    if (!write_path(base, sizeof(base), app_dir, sep)) return false;
    /* A bundle: Scribodo.app/Contents/MacOS/scribodo, its scripts in Contents/Resources/tools. */
    if (write_path(out, outlen, base, "../Resources/tools") && has_services(out)) return true;
    /* The source tree: build/scribodo, the scripts in tools/. */
    return write_path(out, outlen, base, "../tools") && has_services(out);
}
