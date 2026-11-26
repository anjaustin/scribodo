/* Build a genuine old filename/index/tag layout from one synthetic source snapshot.
 * Fixture-only, not a migration or recovery implementation. */
#ifndef SCRIBODO_TEST_REVISION_FIXTURE_H
#define SCRIBODO_TEST_REVISION_FIXTURE_H
#include "app/revisions_internal.h"
#include "core/file_text.h"
#include "core/json.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline bool test_revision_legacy_source(const char *project, const char *name, char *token, size_t cap)
{
    char dir[1500], before[1800], after[1800], index[1800], marker[1800], legacy[160];
    if (strlen(name) >= 199 || strlen(token) < 20 ||
        !revisions_history_path_files(project, name, dir, sizeof(dir))) return false;
    int n = snprintf(legacy, sizeof(legacy), "%.*s-source.md", 19, token);
    if (n < 0 || (size_t)n >= sizeof(legacy) || (size_t)n >= cap) return false;
    n = snprintf(before, sizeof(before), "%s/%s", dir, token);
    if (n < 0 || (size_t)n >= sizeof(before)) return false;
    n = snprintf(after, sizeof(after), "%s/%s", dir, legacy);
    if (n < 0 || (size_t)n >= sizeof(after)) return false;
    n = snprintf(index, sizeof(index), "%s/index.jsonl", dir);
    if (n < 0 || (size_t)n >= sizeof(index)) return false;
    n = snprintf(marker, sizeof(marker), "%s/.format", dir);
    if (n < 0 || (size_t)n >= sizeof(marker)) return false;
    char *text = NULL;
    if (file_text_read_no_follow(index, &text) != FILE_TEXT_OK) return false;
    cJSON *j = json_parse(text);
    free(text);
    cJSON *file = cJSON_GetObjectItem(j, "file");
    bool ok = cJSON_IsString(file) && !strcmp(file->valuestring, token);
    if (ok) {
        cJSON_DeleteItemFromObject(j, "file");
        ok = cJSON_AddStringToObject(j, "file", legacy) != NULL;
    }
    char *line = ok ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    ok = line && renamex_np(before, after, RENAME_EXCL) == 0;
    if (ok && !file_text_write_atomic(index, line, strlen(line), 0644)) {
        (void)renamex_np(after, before, RENAME_EXCL);
        ok = false;
    }
    free(line);
    if (ok) ok = unlink(marker) == 0 || errno == ENOENT;
    if (ok) snprintf(token, cap, "%s", legacy);
    return ok;
}
#endif
