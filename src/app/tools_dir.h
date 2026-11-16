/* Where the app's helper scripts live (tools/services.sh, backup.sh, restore.sh). */
#ifndef SCRIBODO_TOOLS_DIR_H
#define SCRIBODO_TOOLS_DIR_H

#include <stdbool.h>
#include <stddef.h>

/* In order: $SCRIBODO_TOOLS_DIR when set and not empty (used as given); the bundle's Contents/Resources/tools when
 * the binary runs from Scribodo.app/Contents/MacOS; the source tree's tools/ beside build/ (make run). `app_dir` is
 * the binary's folder with a trailing slash, as raylib's GetApplicationDirectory() gives it. The path is always
 * written (the source-tree one when nothing is found, so a failure names a place); true when services.sh is there
 * and executable. */
bool tools_dir_resolve(const char *app_dir, char *out, size_t outlen);

#endif
