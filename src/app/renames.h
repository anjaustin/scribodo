/* Document renames: <project>/.scribodo/renames.jsonl, one line per rename
 * ({"from":"a.md","to":"b.md","ts":…}). Sources recorded under an older
 * name (findings, notes, canon) resolve to the current one, so provenance
 * survives a rename without rewriting immutable records.
 */
#ifndef SCRIBODO_RENAMES_H
#define SCRIBODO_RENAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

bool renames_record(const char *project_dir, const char *from, const char *to);
/* Durable receipt replay. Caller serializes namespace transitions; operation identifies
 * exactly one from/to/time tuple. Existing acknowledged lines are fsynced again, not appended. */
bool renames_record_once(const char *project_dir, const char *from, const char *to, double when, const char *operation);
/* Follow renames from `name` to the document's current name. */
void renames_resolve(const char *project_dir, const char *name, char *out, size_t outlen);
/* Only renames recorded at or after `since` (when the source was made,
 * seconds with sub-second precision). Replays pending namespace transitions first;
 * an empty output signals unavailable/unverified resolution, not the original name. */
void renames_resolve_since(const char *project_dir, const char *name, double since, char *out, size_t outlen);

#endif
