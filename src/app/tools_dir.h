/* Where the app's helper scripts live (tools/services.sh, backup.sh, restore.sh). */
#ifndef SCRIBODO_TOOLS_DIR_H
#define SCRIBODO_TOOLS_DIR_H

#include <stdbool.h>
#include <stddef.h>

/* In order: $SCRIBODO_TOOLS_DIR when set and not empty; the bundle's Contents/Resources/tools when the binary runs
 * from Scribodo.app/Contents/MacOS; the source tree's tools/ beside build/ (make run). `app_dir` is the binary's folder
 * (raylib's GetApplicationDirectory(), with or without a trailing slash). True, with the folder in `out`, only when
 * services.sh is there and executable; otherwise false and `out` is empty (never a partial path).
 *
 * What a bundle must hold, beside the binary in Contents/MacOS (the scripts resolve everything from the folder above
 * tools/, here Contents/Resources; tests/test_services_docker.sh builds this layout and runs it):
 *   Contents/Resources/tools/services.sh, services_lib.sh, services.defaults, backup.sh, restore.sh
 *   Contents/Resources/docker/compose.yml      (docker mode)
 *   Contents/Resources/bin/qdrant              (native mode; the pinned release, as `make qdrant` fetches it)
 * MariaDB is not bundled: native mode uses Homebrew's mariadb@11.4 (SCRIBODO_MARIADB_BIN). */
bool tools_dir_resolve(const char *app_dir, char *out, size_t outlen);

#endif
