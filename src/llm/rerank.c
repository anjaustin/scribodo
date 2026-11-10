#define _DARWIN_C_SOURCE
#include "llm/rerank.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/utf8.h"
#include "llm/http.h"
#include "llm/llm_text.h"

#define RERANK_CLIP_BYTES 2000
#define RERANK_N_PROBS 20
#define RERANK_HTTP_TIMEOUT_MS 30000

/* The Qwen3-Reranker model card's prompt and default instruction, byte for byte. */
static const char k_prefix[] =
    "<|im_start|>system\nJudge whether the Document meets the requirements based on the Query and the Instruct "
    "provided. Note that the answer can only be \"yes\" or \"no\".<|im_end|>\n<|im_start|>user\n";
static const char k_instruction[] = "Given a web search query, retrieve relevant passages that answer the query";
static const char k_suffix[] = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";

struct RerankClient {
    LlmServer *server; /* NULL: fixture */
    pthread_mutex_t mu;
    int yes_id, no_id;
    bool tokens_known;
};

RerankClient *rerank_client_create(LlmServer *server)
{
    RerankClient *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->server = server;
    c->yes_id = c->no_id = -1;
    pthread_mutex_init(&c->mu, NULL);
    return c;
}

void rerank_client_destroy(RerankClient *c)
{
    if (!c) return;
    pthread_mutex_destroy(&c->mu);
    free(c);
}

bool rerank_client_fixture(const RerankClient *c) { return c && !c->server; }

char *rerank_prompt(const char *question, const char *text)
{
    if (!question || !text) return NULL;
    char *q = llm_neutralize(question);
    char *clip = malloc(RERANK_CLIP_BYTES + 1);
    char *d = NULL;
    if (clip) {
        utf8_clip(clip, RERANK_CLIP_BYTES + 1, text, RERANK_CLIP_BYTES);
        d = llm_neutralize(clip);
    }
    free(clip);
    char *out = NULL;
    if (q && d) {
        size_t cap = sizeof(k_prefix) + sizeof(k_instruction) + sizeof(k_suffix) + strlen(q) + strlen(d) + 64;
        out = malloc(cap);
        if (out) snprintf(out, cap, "%s<Instruct>: %s\n<Query>: %s\n<Document>: %s%s", k_prefix, k_instruction, q, d, k_suffix);
    }
    free(q);
    free(d);
    return out;
}

char *rerank_request(const char *prompt)
{
    if (!prompt) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *samplers = root ? cJSON_AddArrayToObject(root, "samplers") : NULL;
    cJSON *top_k = cJSON_CreateString("top_k");
    bool ok = root && samplers && top_k && cJSON_AddStringToObject(root, "prompt", prompt) &&
              cJSON_AddNumberToObject(root, "n_predict", 1) && cJSON_AddNumberToObject(root, "n_probs", RERANK_N_PROBS) &&
              cJSON_AddNumberToObject(root, "temperature", 0) && cJSON_AddNumberToObject(root, "top_k", 1) &&
              cJSON_AddBoolToObject(root, "cache_prompt", false) && cJSON_AddBoolToObject(root, "post_sampling_probs", false) &&
              cJSON_AddNumberToObject(root, "id_slot", 0);
    if (ok) { ok = cJSON_AddItemToArray(samplers, top_k); if (ok) top_k = NULL; }
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(top_k);
    cJSON_Delete(root);
    return body;
}

/* P(yes) over {yes, no} from the top probabilities of the first predicted token.
 * Accepts both of llama-server's shapes: top_logprobs (logprob) and top_probs (prob). */
bool rerank_parse(const char *body, int yes_id, int no_id, double *p_yes)
{
    if (!body || !p_yes || yes_id < 0 || no_id < 0) return false;
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    bool ok = false;
    const cJSON *probs = cJSON_GetObjectItemCaseSensitive(root, "completion_probabilities");
    const cJSON *first = cJSON_IsArray(probs) ? cJSON_GetArrayItem(probs, 0) : NULL;
    const cJSON *top = first ? cJSON_GetObjectItemCaseSensitive(first, "top_logprobs") : NULL;
    bool logprob = true;
    if (!cJSON_IsArray(top)) { top = first ? cJSON_GetObjectItemCaseSensitive(first, "top_probs") : NULL; logprob = false; }
    if (cJSON_IsArray(top)) {
        double py = -1, pn = -1;
        const cJSON *row;
        cJSON_ArrayForEach(row, top) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, logprob ? "logprob" : "prob");
            if (!cJSON_IsNumber(id) || !cJSON_IsNumber(v)) continue;
            double p = logprob ? exp(v->valuedouble) : v->valuedouble;
            if (!(p >= 0 && p <= 1.000001)) continue;
            if ((int)id->valuedouble == yes_id) py = p;
            else if ((int)id->valuedouble == no_id) pn = p;
        }
        if (py >= 0 || pn >= 0) {
            if (py < 0) py = 0;
            if (pn < 0) pn = 0;
            if (py + pn > 0) { *p_yes = py / (py + pn); ok = true; }
        }
    }
    cJSON_Delete(root);
    return ok;
}

static bool single_token(RerankClient *c, const char *word, atomic_bool *cancel, double deadline_ms, int *id)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root && cJSON_AddStringToObject(root, "content", word) && cJSON_AddBoolToObject(root, "add_special", false);
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!body) return false;
    HttpResult r;
    ok = http_request_until(llm_server_socket(c->server), "POST", "/tokenize", llm_server_key(c->server), body,
                            NULL, NULL, cancel, RERANK_HTTP_TIMEOUT_MS, deadline_ms, &r) && r.status == 200 && r.body;
    free(body);
    if (ok) {
        cJSON *j = cJSON_Parse(r.body);
        const cJSON *tokens = j ? cJSON_GetObjectItemCaseSensitive(j, "tokens") : NULL;
        ok = cJSON_IsArray(tokens) && cJSON_GetArraySize(tokens) == 1 && cJSON_IsNumber(cJSON_GetArrayItem(tokens, 0));
        if (ok) *id = (int)cJSON_GetArrayItem(tokens, 0)->valuedouble;
        cJSON_Delete(j);
    }
    http_result_free(&r);
    return ok;
}

bool rerank_client_ready(RerankClient *c, atomic_bool *cancel, double deadline_ms)
{
    if (!c) return false;
    if (!c->server) return true;
    if (llm_server_state(c->server, NULL, 0) != LLM_READY) return false;
    pthread_mutex_lock(&c->mu);
    bool known = c->tokens_known;
    pthread_mutex_unlock(&c->mu);
    if (known) return true;
    int yes = -1, no = -1;
    if (!single_token(c, "yes", cancel, deadline_ms, &yes) || !single_token(c, "no", cancel, deadline_ms, &no) || yes == no) return false;
    pthread_mutex_lock(&c->mu);
    c->yes_id = yes; c->no_id = no; c->tokens_known = true;
    pthread_mutex_unlock(&c->mu);
    return true;
}

/* Deterministic stand-in: the share of the question's distinct content words
 * present in the text. The stopword list is tools/answer_rerank_arm.py's, so
 * the fixture is the lexical control measured in build/rk2. */
static bool stopword(const char *w)
{
    static const char *const k[] = { "a", "an", "the", "and", "or", "but", "of", "in", "on", "at", "to", "for",
        "from", "by", "with", "about", "as", "into", "like", "through", "after", "over", "between", "out", "against",
        "during", "without", "before", "under", "around", "among", "is", "are", "was", "were", "be", "been", "being",
        "am", "do", "does", "did", "doing", "have", "has", "had", "having", "i", "me", "my", "mine", "you", "your",
        "yours", "he", "him", "his", "she", "her", "hers", "it", "its", "they", "them", "their", "theirs", "we", "us",
        "our", "ours", "this", "that", "these", "those", "who", "whom", "whose", "which", "what", "when", "where",
        "why", "how", "there", "here", "then", "than", "too", "very", "can", "could", "will", "would", "shall",
        "should", "may", "might", "must", "not", "no", "nor", "so", "if", "because", "while", "until", "up", "down",
        "off", "again", "further", "once", "all", "any", "both", "each", "few", "more", "most", "other", "some",
        "such", "only", "own", "same", "s", "t", "just", "don", "now", NULL };
    for (size_t i = 0; k[i]; i++) if (!strcmp(w, k[i])) return true;
    return false;
}

static size_t words(const char *s, char out[][32], size_t cap)
{
    size_t n = 0;
    while (*s && n < cap) {
        while (*s && !isalpha((unsigned char)*s)) s++;
        size_t len = 0;
        char w[32];
        while (*s && (isalpha((unsigned char)*s) || *s == '\'')) {
            if (len < sizeof(w) - 1) w[len++] = (char)tolower((unsigned char)*s);
            s++;
        }
        w[len] = 0;
        if (len > 1 && !stopword(w)) {
            bool dup = false;
            for (size_t i = 0; i < n && !dup; i++) dup = !strcmp(out[i], w);
            if (!dup) snprintf(out[n++], 32, "%s", w);
        }
    }
    return n;
}

double rerank_fixture_score(const char *question, const char *text)
{
    if (!question || !text) return 0;
    char q[64][32], t[512][32];
    size_t nq = words(question, q, 64), nt = words(text, t, 512);
    if (!nq) return 0;
    size_t hit = 0;
    for (size_t i = 0; i < nq; i++)
        for (size_t j = 0; j < nt; j++)
            if (!strcmp(q[i], t[j])) { hit++; break; }
    return (double)hit / (double)nq;
}

bool rerank_score(RerankClient *c, const char *question, const char *text, atomic_bool *cancel,
                  double deadline_ms, double *p_yes, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    if (!c || !question || !text || !p_yes) { if (err) snprintf(err, errlen, "Invalid reranker call"); return false; }
    if (!c->server) { *p_yes = rerank_fixture_score(question, text); return true; }
    if (!rerank_client_ready(c, cancel, deadline_ms)) { if (err) snprintf(err, errlen, "The reranker isn't ready"); return false; }
    char *prompt = rerank_prompt(question, text);
    char *body = rerank_request(prompt);
    free(prompt);
    if (!body) { if (err) snprintf(err, errlen, "Not enough memory to build the rerank request"); return false; }
    HttpResult r;
    bool ok = http_request_until(llm_server_socket(c->server), "POST", "/completion", llm_server_key(c->server), body,
                                 NULL, NULL, cancel, RERANK_HTTP_TIMEOUT_MS, deadline_ms, &r);
    free(body);
    if (!ok) { if (err) snprintf(err, errlen, "Rerank request failed"); return false; }
    pthread_mutex_lock(&c->mu);
    int yes = c->yes_id, no = c->no_id;
    pthread_mutex_unlock(&c->mu);
    ok = r.status == 200 && r.body && rerank_parse(r.body, yes, no, p_yes);
    if (!ok && err) snprintf(err, errlen, r.status == 200 ? "Rerank reply lacked yes/no probabilities" : "Rerank HTTP %d", r.status);
    http_result_free(&r);
    return ok;
}
