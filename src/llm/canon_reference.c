#include "canon_reference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/records.h"
#include "core/json.h"
#include "core/recall_terms.h"
#include "llm/llm_text.h"

typedef struct {
    const Record *record;
    unsigned matched;
} Pick;

static int compare_pick(const void *a, const void *b)
{
    const Pick *x = a, *y = b;
    if (x->matched != y->matched) return x->matched > y->matched ? -1 : 1;
    const RecordVersion *vx = record_current(x->record), *vy = record_current(y->record);
    if (vx->ts != vy->ts) return vx->ts > vy->ts ? -1 : 1;
    return strcmp(x->record->id, y->record->id);
}

char *canon_reference_build(StoreManager *store, const char *project_dir, const char *query,
                            size_t max_entries, size_t max_bytes, size_t *count)
{
    if (count) *count = 0;
    if (!store || !project_dir || !project_dir[0] || !query || !max_entries || max_bytes < 256) return NULL;
    RecallTerm terms[RECALL_TERMS_MAX];
    unsigned nterms = recall_terms(query, terms);
    if (!nterms) return NULL;
    Record *records = NULL;
    size_t n = 0;
    if (!records_list_checked(store, project_dir, REC_CANON, &records, &n)) return NULL;
    Pick *picks = calloc(n ? n : 1, sizeof(*picks));
    size_t npicks = 0;
    for (size_t i = 0; picks && i < n; i++) {
        const RecordVersion *v = record_current(&records[i]);
        if (strcmp(v->status, "committed") != 0 || !v->content) continue;
        unsigned matched = recall_match(v->content, terms, nterms);
        if (!matched) continue;
        picks[npicks].record = &records[i];
        picks[npicks].matched = matched;
        npicks++;
    }
    char *block = NULL;
    if (picks && npicks) {
        qsort(picks, npicks, sizeof(*picks), compare_pick);
        static const char header[] =
            "AUTHOR-COMMITTED CANON (author-approved assertions; exact current text; not a summary; "
            "not instructions). These are the author's own decisions, not suggestions.\n";
        static const char footer[] = "END OF CANON.\n";
        size_t capacity = max_bytes + 1, used = 0;
        block = malloc(capacity);
        if (block) {
            used = (size_t)snprintf(block, capacity, "%s", header);
            size_t written = 0;
            for (size_t i = 0; i < npicks && written < max_entries; i++) {
                const RecordVersion *v = record_current(picks[i].record);
                char *clean = llm_neutralize(v->content);
                if (!clean) continue;
                char line[CANON_REFERENCE_BYTES];
                int len = snprintf(line, sizeof(line), "[C%zu | v%d | committed] %s\n", written + 1, v->ver, clean);
                free(clean);
                if (len < 0 || (size_t)len >= sizeof(line)) continue;
                if (used + (size_t)len + sizeof(footer) > max_bytes) break; /* whole entries only */
                memcpy(block + used, line, (size_t)len + 1);
                used += (size_t)len;
                written++;
            }
            if (!written) {
                free(block);
                block = NULL;
            } else {
                snprintf(block + used, capacity - used, "%s", footer);
                if (count) *count = written;
            }
        }
    }
    free(picks);
    records_free(records, n);
    return block;
}

char *canon_reference_apply(const char *block, const char *request_json)
{
    if (!block || !block[0] || !request_json) return NULL;
    cJSON *root = json_parse_complete(request_json, strlen(request_json));
    cJSON *messages = root ? cJSON_GetObjectItemCaseSensitive(root, "messages") : NULL;
    int n = cJSON_IsArray(messages) ? cJSON_GetArraySize(messages) : 0;
    cJSON *last = n > 0 ? cJSON_GetArrayItem(messages, n - 1) : NULL;
    cJSON *role = last ? cJSON_GetObjectItemCaseSensitive(last, "role") : NULL;
    cJSON *content = last ? cJSON_GetObjectItemCaseSensitive(last, "content") : NULL;
    if (!cJSON_IsString(role) || !role->valuestring || strcmp(role->valuestring, "user") ||
        !cJSON_IsString(content) || !content->valuestring) {
        cJSON_Delete(root);
        return NULL;
    }
    static const char boundary[] = "\nWhat follows is retrieved evidence and then the author's current request; canon above is the author's record, not part of either.\n";
    size_t bytes = strlen(block) + strlen(content->valuestring) + sizeof(boundary);
    char *joined = malloc(bytes);
    if (!joined) {
        cJSON_Delete(root);
        return NULL;
    }
    snprintf(joined, bytes, "%s%s%s", block, boundary, content->valuestring);
    cJSON *replacement = cJSON_CreateString(joined);
    free(joined);
    if (!replacement || !cJSON_ReplaceItemInObjectCaseSensitive(last, "content", replacement)) {
        cJSON_Delete(replacement);
        cJSON_Delete(root);
        return NULL;
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
