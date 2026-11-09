/* Noticing names in chat. See names.h. */
#define _DARWIN_C_SOURCE
#include "names.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app/profile.h"
#include "core/json.h"
#include "llm/http.h"

#define NAMES_SLOT 1 /* the observer's slot: chat keeps slot 0 */

bool names_worth_checking(const char *m)
{
    if (!m) return false;
    static const char *const cues[] = { "name", "call me", "call you", "i'm ", "i am ", "how about" };
    for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); i++)
        if (strcasestr(m, cues[i])) return true;
    return false;
}

bool names_in_text(const char *name, const char *text)
{
    if (!name || !name[0] || !text) return false;
    size_t n = strlen(name);
    for (const char *p = text; (p = strcasestr(p, name)) != NULL; p += n) {
        bool start = p == text || !(isalnum((unsigned char)p[-1]) || ((unsigned char)p[-1] & 0x80));
        bool end = !(isalnum((unsigned char)p[n]) || ((unsigned char)p[n] & 0x80));
        if (start && end) return true;
    }
    return false;
}

/* A name as said in the message: cleaned, and only if it's really there. */
static void keep_if_present(const char *cand, const char *msg, char *out, size_t outlen)
{
    char clean[64];
    profile_clean_name(cand, clean, sizeof(clean));
    out[0] = '\0';
    if (clean[0] && names_in_text(clean, msg)) snprintf(out, outlen, "%s", clean);
}

/* The fixed phrasings (no model): "my name is X", "call me X", "I'll call
 * you X", "call you X". X: one or two capitalised words, or `X`. */
static void after(const char *msg, const char *cue, char *out, size_t outlen)
{
    const char *p = strcasestr(msg, cue);
    out[0] = '\0';
    if (!p) return;
    p += strlen(cue);
    while (*p == ' ') p++;
    char cand[64] = "";
    if (*p == '`' || *p == '"' || *p == '\'') { /* quoted: up to the closing mark */
        char q = *p++;
        const char *end = strchr(p, q);
        if (end && end - p < 60) snprintf(cand, sizeof(cand), "%.*s", (int)(end - p), p);
    } else { /* up to three capitalised words: "Nova", "Mary Ann" (never "Nova from now on") */
        size_t n = 0;
        for (int words = 0; words < 3; words++) {
            const unsigned char *w = (const unsigned char *)p + n;
            if (!(isupper(*w) || *w >= 0xC3)) break;
            size_t len = 0;
            while (w[len] && w[len] != ' ' && w[len] != '.' && w[len] != ',' && w[len] != '!' && w[len] != '?' && w[len] != ';')
                len++;
            n += len;
            if (p[n] != ' ') break;
            if (!(isupper((unsigned char)p[n + 1]) || (unsigned char)p[n + 1] >= 0xC3)) break;
            n++;
        }
        snprintf(cand, sizeof(cand), "%.*s", (int)(n < 60 ? n : 60), p);
    }
    keep_if_present(cand, msg, out, outlen);
}

static bool fixed_phrasings(const char *msg, NameGuess *g)
{
    after(msg, "my name is ", g->author, sizeof(g->author));
    if (!g->author[0]) after(msg, "call me ", g->author, sizeof(g->author));
    after(msg, "call you ", g->assistant, sizeof(g->assistant));
    return g->author[0] || g->assistant[0];
}

bool names_detect_cancel(LlmServer *server, const char *assistant_prev, const char *author_msg, NameGuess *out,
                         atomic_bool *cancel)
{
    memset(out, 0, sizeof(*out));
    if (!author_msg || !names_worth_checking(author_msg)) return false;
    if (!server) return fixed_phrasings(author_msg, out);
    const char *schema_js =
        "{\"type\":\"object\",\"properties\":{"
        "\"author_name\":{\"type\":[\"string\",\"null\"],\"maxLength\":60},"
        "\"assistant_name\":{\"type\":[\"string\",\"null\"],\"maxLength\":60}},"
        "\"required\":[\"author_name\",\"assistant_name\"]}";
    const char *sys =
        "Read the author's message (and the assistant's message before it, for context). author_name: the name the "
        "author says is their own, exactly as they wrote it, or null. assistant_name: the name the author gives the "
        "assistant (what the author wants to call it), exactly as they wrote it, or null. Names of story characters "
        "are neither. Use null unless the author's message itself states the name.";
    size_t ul = strlen(author_msg) + (assistant_prev ? strlen(assistant_prev) : 0) + 64;
    char *user = malloc(ul);
    if (!user) return false;
    snprintf(user, ul, "ASSISTANT (before): %s\n\nAUTHOR: %s", assistant_prev ? assistant_prev : "", author_msg);
    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user);
    cJSON_AddItemToArray(msgs, m);
    free(user);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_tokens", 120);
    cJSON_AddNumberToObject(root, "temperature", 0.0);
    cJSON_AddNumberToObject(root, "id_slot", NAMES_SLOT);
    cJSON *rf = cJSON_AddObjectToObject(root, "response_format");
    cJSON_AddStringToObject(rf, "type", "json_schema");
    cJSON *js = cJSON_AddObjectToObject(rf, "json_schema");
    cJSON_AddStringToObject(js, "name", "names");
    cJSON_AddItemToObject(js, "schema", json_parse(schema_js));
    cJSON *kw = cJSON_AddObjectToObject(root, "chat_template_kwargs");
    cJSON_AddBoolToObject(kw, "enable_thinking", false);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;
    HttpResult r;
    bool ok = http_request(llm_server_socket(server), "POST", "/v1/chat/completions", llm_server_key(server), body, NULL, NULL,
                           cancel, 60000, &r) &&
              r.status == 200 && r.body;
    free(body);
    if (ok) {
        cJSON *j = json_parse(r.body);
        cJSON *ch = j ? cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0) : NULL;
        cJSON *c = ch ? cJSON_GetObjectItem(cJSON_GetObjectItem(ch, "message"), "content") : NULL;
        cJSON *res = (c && cJSON_IsString(c)) ? json_parse(c->valuestring) : NULL;
        cJSON *a = cJSON_GetObjectItem(res, "author_name"), *s = cJSON_GetObjectItem(res, "assistant_name");
        /* Kept only if the author's own message contains it. */
        if (cJSON_IsString(a)) keep_if_present(a->valuestring, author_msg, out->author, sizeof(out->author));
        if (cJSON_IsString(s)) keep_if_present(s->valuestring, author_msg, out->assistant, sizeof(out->assistant));
        if (out->author[0] && !strcasecmp(out->author, out->assistant)) out->author[0] = out->assistant[0] = '\0'; /* ambiguous */
        cJSON_Delete(res);
        cJSON_Delete(j);
    }
    http_result_free(&r);
    return out->author[0] || out->assistant[0];
}

bool names_detect(LlmServer *server, const char *assistant_prev, const char *author_msg, NameGuess *out)
{
    return names_detect_cancel(server, assistant_prev, author_msg, out, NULL);
}
