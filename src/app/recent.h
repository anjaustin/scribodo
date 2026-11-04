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

void recent_set_dir(const char *dir);
/* Projects whose folders still exist. */
size_t recent_load(RecentProject *out, size_t max);
/* Move `project` to the front (recording `doc` if non-NULL) and save. */
void recent_touch(const char *project, const char *doc);
/* A document in `project` was renamed: keep "last open" pointing at it. */
void recent_rename_doc(const char *project, const char *from, const char *to);

#endif
