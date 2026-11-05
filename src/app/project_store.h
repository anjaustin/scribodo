/* Internal one-shot routing for project-store operations. The resolver and
 * its borrowed thread-local database connection never escape this API. */
#ifndef SCRIBODO_PROJECT_STORE_H
#define SCRIBODO_PROJECT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/records.h"
size_t project_store_records_list(const char *project_dir, RecordKind kind, Record **out);
bool project_store_records_append(const char *project_dir, RecordKind kind, const char *id, const char *status,
                                  const char *content, const SourceRef *sources, size_t nsources,
                                  const FindingInfo *finding, bool fresh, int *ver_out);

#endif
