/* Document renames: <project>/.scribodo/renames.jsonl, one line per rename
 * ({"from":"a.md","to":"b.md","ts":…}). Sources recorded under an older
 * name (findings, notes, canon) resolve to the current one, so provenance
 * survives a rename without rewriting immutable records.
 */
#ifndef SCRIBODO_RENAMES_H
#define SCRIBODO_RENAMES_H

#include <stdbool.h>
#include <stddef.h>

bool renames_record(const char *project_dir, const char *from, const char *to);
/* Follow renames from `name` to the document's current name. */
void renames_resolve(const char *project_dir, const char *name, char *out, size_t outlen);

#endif
