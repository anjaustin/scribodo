/* Backups from inside the app (SOW §7; tools/backup.sh does the work).
 *
 * backup_project: a backup of one project. In the store, the project's own
 * data is also exported as files (original times kept) into the backup's
 * export/ folder, so it can be restored without touching the database.
 *
 * backup_restore_copy: never overwrites anything. The backed-up project is
 * put in a new folder next to `parent_dir` ("Name (restored <time>)"):
 * its documents, and its data from export/ (or, for a project kept on
 * files, its .scribodo/). Opening that folder imports it as a project of
 * its own. Restoring a whole database after a disaster is
 * tools/restore.sh.
 */
#ifndef SCRIBODO_BACKUP_H
#define SCRIBODO_BACKUP_H

#include <stdbool.h>
#include <stddef.h>
#include "app/store.h"

/* `tools_dir` holds backup.sh. Blocking (run it off the UI thread). On
 * success, `dest` is the backup folder. */
bool backup_project(StoreManager *manager, const char *tools_dir, const char *project_dir, char *dest, size_t destlen, char *why, size_t whylen);

/* Blocking. On success, `restored` is the new project folder. */
bool backup_restore_copy(const char *tools_dir, const char *backup_dir, const char *parent_dir, char *restored,
                         size_t restoredlen, char *why, size_t whylen);

/* The default folder backups go to. */
void backup_default_dir(char *out, size_t outlen);

#endif
