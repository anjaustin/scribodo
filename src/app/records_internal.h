/* File-backend operations used by the project-store router and importer. */
#ifndef SCRIBODO_RECORDS_INTERNAL_H
#define SCRIBODO_RECORDS_INTERNAL_H

#include "app/records.h"

bool records_append_files(const char *project_dir, RecordKind kind, const char *id, const char *status,
                          const char *content, const SourceRef *sources, size_t nsources,
                          const FindingInfo *finding, bool fresh, int *ver_out);

#endif
