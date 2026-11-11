/* Diagnostic: the production anchor gate (answer_gate) over recorded claims.
 * stdin: a JSON array of {"answer", "question", "spans": [...], "turns": [...]}.
 * stdout: one JSON line per item, {"verdict": "..."}. No model; the gate's own
 * code, linked from the same objects as the app. Driven by
 * tools/answer_gate_replay.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "llm/answer_pipeline.h"

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static size_t strings(const cJSON *o, const char *key, const char **out, size_t cap)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
    size_t n = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, a) if (cJSON_IsString(v) && n < cap) out[n++] = v->valuestring;
    return n;
}

int main(void)
{
    size_t cap = 1u << 20, len = 0;
    char *buf = malloc(cap);
    if (!buf) return 2;
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len - 1, stdin);
        len += got;
        if (got == 0) break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); return 2; }
            buf = grown;
        }
    }
    buf[len] = '\0';
    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!root || !cJSON_IsArray(root)) { fprintf(stderr, "stdin is not a JSON array\n"); cJSON_Delete(root); return 1; }
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        const char *spans[8], *turns[8];
        size_t nspans = strings(item, "spans", spans, 8), nturns = strings(item, "turns", turns, 8);
        const char *verdict = answer_gate(str(item, "answer"), str(item, "question"), spans, nspans, turns, nturns);
        char *out = cJSON_PrintUnformatted(cJSON_CreateString(verdict)); /* a JSON string literal */
        printf("{\"verdict\":%s}\n", out ? out : "\"\"");
        free(out);
    }
    cJSON_Delete(root);
    return 0;
}
