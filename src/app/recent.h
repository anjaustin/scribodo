/* Recently opened projects (most recent first), with the document last open
 * in each, kept in <state dir>/recent.json. UI scripts point the state dir
 * at their own folder so tests never touch the author's list.
 */
#ifndef SCRIBODO_RECENT_H
#define SCRIBODO_RECENT_H

#include <limits.h>
#include <stddef.h>

#define RECENT_MAX 10

typedef struct {
    char path[PATH_MAX];
    char doc[256]; /* last open document's file name, or "" */
} RecentProject;

typedef struct RecentStore RecentStore;

RecentStore *recent_store_create(const char *dir);
void recent_store_destroy(RecentStore *store);
/* Projects whose folders still exist. */
size_t recent_store_load(RecentStore *store, RecentProject *out, size_t max);
/* Move `project` to the front (recording `doc` if non-NULL) and save. */
void recent_store_touch(RecentStore *store, const char *project, const char *doc);
/* A document in `project` was renamed: keep "last open" pointing at it. */
void recent_store_rename_doc(RecentStore *store, const char *project, const char *from, const char *to);

#endif
