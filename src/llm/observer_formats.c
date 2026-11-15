/* The observer's judge log line and the replay's finding line: the formats of llm/observer_formats.h. */
#include "observer_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

char *observer_judge_log_line(const ObserverJudgeLogEntry *e)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddNumberToObject(j, "version", OBSERVER_JUDGE_LOG_VERSION);
    cJSON_AddStringToObject(j, "kind", e->is_doc ? "doc" : "chat");
    cJSON_AddStringToObject(j, "doc", e->name ? e->name : "");
    cJSON_AddNumberToObject(j, "message", (double)e->message);
    cJSON_AddNumberToObject(j, "start", (double)e->start);
    cJSON_AddNumberToObject(j, "len", (double)e->len);
    cJSON_AddStringToObject(j, "type", e->type ? e->type : "");
    cJSON_AddStringToObject(j, "path", e->sentence ? "sentence" : "statement");
    if (e->sentence) cJSON_AddBoolToObject(j, "attributes", e->attributes);
    if (e->sentence) cJSON_AddBoolToObject(j, "subject", e->subject);
    cJSON_AddStringToObject(j, "canon_id", e->canon_id ? e->canon_id : "");
    cJSON_AddStringToObject(j, "canon", e->canon ? e->canon : "");
    cJSON_AddStringToObject(j, "statement", e->statement ? e->statement : "");
    cJSON *rs = cJSON_AddArrayToObject(j, "replies");
    for (int p = 0; p < 2 && rs; p++) cJSON_AddItemToArray(rs, e->replies[p] ? cJSON_CreateString(e->replies[p]) : cJSON_CreateNull());
    cJSON_AddBoolToObject(j, "conflict", e->conflict);
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return line;
}

/* A text field escaped so the line stays one line of twelve fields: \n \r \t \| and \\ (observer_formats.h). */
static char *escaped(const char *s)
{
    if (!s) s = "";
    char *out = malloc(strlen(s) * 2 + 1), *p = out;
    if (!out) return NULL;
    for (; *s; s++) {
        char c = *s == '\n' ? 'n' : *s == '\r' ? 'r' : *s == '\t' ? 't' : *s == '|' ? '|' : *s == '\\' ? '\\' : 0;
        if (c) { *p++ = '\\'; *p++ = c; } else *p++ = *s;
    }
    *p = 0;
    return out;
}

char *observer_finding_line(const RecordVersion *v)
{
    const SourceRef *s = v->nsources ? &v->sources[0] : NULL;
    const char *src_kind = !s ? "" : s->kind == SRC_DOC ? "doc" : "chat";
    const char *src_name = !s ? "" : s->kind == SRC_DOC ? s->doc : s->chat;
    char *f[9] = { escaped(v->finding.type), escaped(v->finding.subject), escaped(v->finding.relation),
                   escaped(v->finding.object), escaped(v->status), escaped(v->content), escaped(src_name),
                   escaped(s ? s->quote : ""), escaped(v->finding.related_text) };
    size_t cap = 64;
    bool ok = true;
    for (int i = 0; i < 9; i++) { ok = ok && f[i]; cap += f[i] ? strlen(f[i]) : 0; }
    char *line = ok ? malloc(cap) : NULL;
    if (line)
        snprintf(line, cap, "%s|%s|%s|%s|%s|%s|%s|%s|%zu|%zu|%s|%s", f[0], f[1], f[2], f[3], f[4], f[5], src_kind, f[6],
                 s ? s->start : 0, s ? s->len : 0, f[7], f[8]);
    for (int i = 0; i < 9; i++) free(f[i]);
    return line;
}
