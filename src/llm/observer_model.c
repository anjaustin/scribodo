#define _DARWIN_C_SOURCE
#include "observer_model_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/experiments.h"
#include "core/json.h"
#include "llm/http.h"
#include "llm/llm_text.h"
#include "llm/observer.h"

/* ---- model backend ------------------------------------------------------------------ */

static const char *k_extract_system =
    "You are a careful continuity assistant for a novelist. Read the passage and list what it establishes about the "
    "story. Use only what the passage states; never invent. For each finding give its type: entity (a named "
    "character, place or object), event (something that happens), relationship (how two named entities relate), or "
    "question (something the text leaves open). subject: the name of the main entity it is about. description: one "
    "short factual sentence that uses names, not pronouns. Relationships must link two named entities. evidence: copy the exact words from the passage that support it (a short phrase or sentence, "
    "verbatim). For relationships also give relation and object. At most 8 findings; fewer is fine.";

static const char *k_judge_system =
    "You check a novel for continuity errors. CANON is a fact the author has established. NEW is from the latest "
    "text. First, in check, say what NEW states or implies about the same person, place or thing as CANON (details "
    "such as appearance, dates, family ties, injuries, or whether something happened before count). Then verdict: "
    "conflict if CANON and NEW cannot both be true; consistent if they agree; unrelated if NEW says nothing about "
    "what CANON establishes.";

/* The conflict-gate experiment's judge (SCRIBODO_OBSERVER_CONFLICT_GATE=1): the verdict carries the two values it says
 * disagree, which observer_model_judge locates in the texts. Measured on both planted novels 2026-11-14 (journal/
 * observer-cycle2_quality.md, step 5): off-fault conflicts rose from 44 to 61 (Mars) and 41 to 83 (P&P); kept here
 * gated by the author's rule for failed experiments. */
static const char *k_judge_system_values =
    "You check a novel for continuity errors. CANON is a fact the author has established. NEW is from the latest "
    "text. First, in check, say what NEW states or implies about the same person, place or thing as CANON (details "
    "such as appearance, dates, family ties, injuries, or whether something happened before count). Then canon_value: "
    "the words of CANON that NEW contradicts, copied exactly from CANON, and new_value: the words of NEW that "
    "contradict them, copied exactly from NEW; both empty unless the two cannot both be true. Then verdict: "
    "conflict if CANON and NEW cannot both be true; consistent if they agree; unrelated if NEW says nothing about "
    "what CANON establishes. Something NEW adds that CANON does not mention is not a conflict.";

static cJSON *extract_schema(void)
{
    const char *js =
        "{\"type\":\"object\",\"properties\":{\"findings\":{\"type\":\"array\",\"maxItems\":8,\"items\":{"
        "\"type\":\"object\",\"properties\":{"
        "\"type\":{\"type\":\"string\",\"enum\":[\"entity\",\"event\",\"relationship\",\"question\"]},"
        "\"subject\":{\"type\":\"string\",\"maxLength\":60},"
        "\"description\":{\"type\":\"string\",\"maxLength\":200},"
        "\"evidence\":{\"type\":\"string\",\"maxLength\":300},"
        "\"relation\":{\"type\":\"string\",\"maxLength\":40},"
        "\"object\":{\"type\":\"string\",\"maxLength\":60}},"
        "\"required\":[\"type\",\"subject\",\"description\",\"evidence\"]}}},\"required\":[\"findings\"]}";
    return json_parse(js);
}

static cJSON *judge_schema(bool values)
{
    /* "check" comes first: the model explains before it decides (measured:
     * conflict recall 4/6 -> 6/6). */
    if (values)
        return json_parse("{\"type\":\"object\",\"properties\":{\"check\":{\"type\":\"string\",\"maxLength\":300},"
                          "\"canon_value\":{\"type\":\"string\",\"maxLength\":80},"
                          "\"new_value\":{\"type\":\"string\",\"maxLength\":80},"
                          "\"verdict\":{\"type\":\"string\",\"enum\":[\"conflict\",\"consistent\",\"unrelated\"]}},"
                          "\"required\":[\"check\",\"canon_value\",\"new_value\",\"verdict\"]}");
    return json_parse("{\"type\":\"object\",\"properties\":{\"check\":{\"type\":\"string\",\"maxLength\":300},"
                       "\"verdict\":{\"type\":\"string\",\"enum\":[\"conflict\",\"consistent\",\"unrelated\"]}},"
                       "\"required\":[\"check\",\"verdict\"]}");
}

/* One constrained completion; returns the content (malloc'd) or NULL. */
static char *complete(ObserverModel *model, const char *system, const char *user, cJSON *schema, int max_tokens)
{
    if (!model->server || llm_server_state(model->server, NULL, 0) != LLM_READY) {
        cJSON_Delete(schema);
        return NULL;
    }
    char *sys_n = llm_neutralize(system), *user_n = llm_neutralize(user);
    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m1 = cJSON_CreateObject(), *m2 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "system");
    cJSON_AddStringToObject(m1, "content", sys_n ? sys_n : "");
    cJSON_AddStringToObject(m2, "role", "user");
    cJSON_AddStringToObject(m2, "content", user_n ? user_n : "");
    cJSON_AddItemToArray(msgs, m1);
    cJSON_AddItemToArray(msgs, m2);
    free(sys_n);
    free(user_n);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    cJSON_AddNumberToObject(root, "temperature", 0.0);
    cJSON *rf = cJSON_AddObjectToObject(root, "response_format");
    cJSON_AddStringToObject(rf, "type", "json_schema");
    cJSON *js = cJSON_AddObjectToObject(rf, "json_schema");
    cJSON_AddStringToObject(js, "name", "observer");
    cJSON_AddItemToObject(js, "schema", schema);
    cJSON *kw = cJSON_AddObjectToObject(root, "chat_template_kwargs");
    cJSON_AddBoolToObject(kw, "enable_thinking", false);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return NULL;
    HttpResult r;
    char *content = NULL;
    if (http_request(llm_server_socket(model->server), "POST", "/v1/chat/completions", llm_server_key(model->server), body, NULL,
                     NULL, model->cancel, 180000, &r) &&
        r.status == 200 && r.body) {
        cJSON *j = json_parse(r.body);
        cJSON *ch = j ? cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0) : NULL;
        cJSON *c = ch ? cJSON_GetObjectItem(cJSON_GetObjectItem(ch, "message"), "content") : NULL;
        if (c && cJSON_IsString(c)) content = strdup(c->valuestring);
        cJSON_Delete(j);
    }
    http_result_free(&r);
    free(body);
    return content;
}

/* A chat is a conversation about the story, not the story: only the
 * story-world facts in it are findings (the author's decision, user
 * testing 2026-11-04). */
static const char *k_extract_chat =
    "\nThis passage is a conversation between the author (AUTHOR) and their writing assistant (ASSISTANT). Extract "
    "only facts about the author's story world: its characters, places, objects, events and open questions. Ignore "
    "the conversation itself: greetings, the names the author and the assistant use for themselves or each other, "
    "what the assistant offers, asks or suggests, and talk about the writing process. If the conversation says "
    "nothing about the story world, return no findings.";

char *observer_model_extract(ObserverModel *model, const char *chunk, bool is_chat)
{
    if (!model->server) {
        usleep(20000);
        return atomic_load(model->cancel) ? NULL : obs_fake_extract(chunk);
    }
    size_t n = strlen(chunk) + 16;
    char *user = malloc(n);
    if (!user) return NULL;
    snprintf(user, n, "PASSAGE:\n%s", chunk);
    char sys[2400];
    snprintf(sys, sizeof(sys), "%s%s", k_extract_system, is_chat ? k_extract_chat : "");
    char *out = complete(model, sys, user, extract_schema(), 1400);
    free(user);
    return out;
}

/* Two votes, CANON/NEW in both orders; a conflict only if both agree (measured: removes most false alarms without
 * losing real conflicts). Under the conflict-gate experiment each vote must also name the canon value and the new
 * value it says disagree, located in their texts by obs_values_in_texts (2026-11-14: 85 of 92 conflicts over two
 * novels were additions the judge called conflicts; measured, the requirement did not reduce them; journal/
 * observer-cycle2_quality.md, step 5). */
bool observer_model_judge(ObserverModel *model, const char *canon, const char *statement, char *check, size_t checklen)
{
    const bool gate = experiments()->observer_conflict_gate;
    bool votes = true;
    check[0] = '\0';
    for (int pass = 0; pass < 2 && votes; pass++) {
        char *out = NULL;
        if (!model->server) {
            out = obs_fake_judge(canon, statement);
        } else {
            size_t n = strlen(canon) + strlen(statement) + 32;
            char *user = malloc(n);
            if (!user) return false;
            if (pass == 0) snprintf(user, n, "CANON: %s\nNEW: %s", canon, statement);
            else snprintf(user, n, "NEW: %s\nCANON: %s", statement, canon);
            out = complete(model, gate ? k_judge_system_values : k_judge_system, user, judge_schema(gate), gate ? 320 : 260);
            free(user);
        }
        cJSON *j = out ? json_parse(out) : NULL;
        cJSON *v = j ? cJSON_GetObjectItem(j, "verdict") : NULL, *c = j ? cJSON_GetObjectItem(j, "check") : NULL;
        cJSON *cv = j ? cJSON_GetObjectItem(j, "canon_value") : NULL, *nv = j ? cJSON_GetObjectItem(j, "new_value") : NULL;
        votes = v && cJSON_IsString(v) && !strcmp(v->valuestring, "conflict");
        if (votes && gate)
            votes = cv && cJSON_IsString(cv) && nv && cJSON_IsString(nv) &&
                    obs_values_in_texts(canon, statement, cv->valuestring, nv->valuestring);
        if (votes && pass == 0 && c && cJSON_IsString(c)) snprintf(check, checklen, "%s", c->valuestring);
        cJSON_Delete(j);
        free(out);
        if (atomic_load(model->cancel)) return false;
    }
    return votes;
}
