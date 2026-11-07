#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "memory_tokens.h"

#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/json.h"
#include "http.h"
#include "llm_text.h"

#define MAX_RESPONSE_BYTES (32u * 1024u * 1024u)
#define MAX_TOKEN_COUNT (MEMORY_TOKENS_MAX_REQUEST_BYTES + 1024u)

bool memory_tokenizer_fingerprint(MemoryTokenizer *t, char out[65])
{
    if (!out) return false;
    out[0] = 0;
    if (!t) return false;
    const char *identity = "fake-neutralized-source-bytes-full-json-bytes-v1";
    if (t->server) {
        LlmServerConfig pinned;
        llm_server_default_config(&pinned, NULL);
        const LlmServerConfig *cfg = llm_server_config(t->server);
        if (!cfg || cfg->embedding || strnlen(cfg->sha256, sizeof(cfg->sha256)) != 64 ||
            strcmp(cfg->sha256, pinned.sha256)) return false;
        identity = cfg->sha256;
    }
    char key[512];
    int n = snprintf(key, sizeof(key), "memory-tokens-v1|%s|neutralize=llm-zwnj-v1|source=tokenize-no-bos|request=full-oaicompat-apply-template-tokenize-bos|parse-special=true", identity);
    if (n < 0 || (size_t)n >= sizeof(key)) return false;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(key, (CC_LONG)n, digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    return true;
}

static bool cancelled(const MemoryTokenizer *t)
{
    return t->cancel && atomic_load(t->cancel);
}

static bool ready(MemoryTokenizer *t)
{
    char fingerprint[65];
    if (!t || cancelled(t) || !memory_tokenizer_fingerprint(t, fingerprint)) return false;
    if (!t->server) return true;
    if (llm_server_state(t->server, NULL, 0) != LLM_READY) return false;
    const char *socket = llm_server_socket(t->server);
    /* http_request also supports loopback TCP; this adapter must not. */
    return socket && socket[0] == '/';
}

static double monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    MemoryTokenizer *tokenizer;
    atomic_bool stop, done;
    pthread_t guard;
    double deadline;
} TokenCall;

static bool call_cancelled(TokenCall *c)
{
    double now = monotonic_ms();
    return cancelled(c->tokenizer) || atomic_load(&c->stop) || now < 0 || now >= c->deadline;
}

/* http_request bounds individual waits, not the whole response. Relay the
 * external cancellation and total deadline without modifying caller state. */
static void *deadline_guard(void *arg)
{
    TokenCall *c = arg;
    const struct timespec slice = { .tv_nsec = 2000000 };
    while (!atomic_load(&c->done)) {
        if (call_cancelled(c)) { atomic_store(&c->stop, true); break; }
        nanosleep(&slice, NULL);
    }
    return NULL;
}

static bool call_begin(TokenCall *c, MemoryTokenizer *t)
{
    double now = monotonic_ms();
    if (now < 0 || !ready(t)) return false;
    c->tokenizer = t;
    c->deadline = now + (t->timeout_ms > 0 ? t->timeout_ms : 10000);
    atomic_init(&c->stop, false);
    atomic_init(&c->done, false);
    return pthread_create(&c->guard, NULL, deadline_guard, c) == 0;
}

static long call_end(TokenCall *c, long result)
{
    bool failed = call_cancelled(c);
    atomic_store(&c->done, true);
    pthread_join(c->guard, NULL);
    return failed || call_cancelled(c) ? -1 : result;
}

static bool shape(const cJSON *j, unsigned depth, size_t *budget)
{
    if (!j || depth > 32 || !*budget) return false;
    --*budget;
    if (!cJSON_IsObject(j) && !cJSON_IsArray(j)) return true;
    size_t fields = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, j) {
        if (cJSON_IsObject(j)) {
            if (++fields > 128 || !x->string) return false;
            for (const cJSON *p = j->child; p != x; p = p->next)
                if (!strcmp(p->string, x->string)) return false;
        }
        if (!shape(x, depth + 1, budget)) return false;
    }
    return true;
}

static cJSON *checked_json(const char *text, size_t n)
{
    cJSON *j = json_parse_complete(text, n);
    size_t budget = MAX_TOKEN_COUNT + 4096u;
    if (!cJSON_IsObject(j) || !shape(j, 0, &budget)) { cJSON_Delete(j); return NULL; }
    return j;
}

static cJSON *post(TokenCall *c, const char *path, const char *body)
{
    if (call_cancelled(c) || !ready(c->tokenizer) ||
        strnlen(body, MEMORY_TOKENS_MAX_REQUEST_BYTES + 1u) > MEMORY_TOKENS_MAX_REQUEST_BYTES) return NULL;
    LlmServer *server = c->tokenizer->server;
    HttpResult r = {0};
    double remaining = c->deadline - monotonic_ms();
    int timeout = remaining > INT_MAX ? INT_MAX : remaining >= 1 ? (int)remaining : 1;
    bool ok = http_request(llm_server_socket(server), "POST", path, llm_server_key(server), body,
                           NULL, NULL, &c->stop, timeout, &r);
    cJSON *j = NULL;
    if (ok && !call_cancelled(c) && r.status == 200 && r.body && r.len && r.len <= MAX_RESPONSE_BYTES)
        j = checked_json(r.body, r.len);
    http_result_free(&r);
    return j;
}

static long tokenize(TokenCall *c, const char *text, bool add_special)
{
    if (strnlen(text, MEMORY_TOKENS_MAX_REQUEST_BYTES + 1u) > MEMORY_TOKENS_MAX_REQUEST_BYTES) return -1;
    cJSON *request = cJSON_CreateObject();
    if (!request) return -1;
    bool ok = cJSON_AddStringToObject(request, "content", text) &&
              cJSON_AddBoolToObject(request, "add_special", add_special) &&
              cJSON_AddBoolToObject(request, "parse_special", true);
    char *body = ok ? cJSON_PrintUnformatted(request) : NULL;
    cJSON_Delete(request);
    if (!body) return -1;
    cJSON *response = post(c, "/tokenize", body);
    free(body);
    const cJSON *tokens = cJSON_GetObjectItemCaseSensitive(response, "tokens");
    long count = 0;
    if (!cJSON_IsArray(tokens)) count = -1;
    else {
        const cJSON *token;
        cJSON_ArrayForEach(token, tokens) {
            if (!cJSON_IsNumber(token) || !isfinite(token->valuedouble) ||
                token->valuedouble < 0 || token->valuedouble > INT_MAX ||
                floor(token->valuedouble) != token->valuedouble || count >= (long)MAX_TOKEN_COUNT || count == LONG_MAX) {
                count = -1;
                break;
            }
            count++;
        }
    }
    cJSON_Delete(response);
    return count;
}

long memory_source_tokens(const char *source, void *ctx)
{
    MemoryTokenizer *t = ctx;
    if (!source || strnlen(source, MEMORY_TOKENS_MAX_SOURCE_BYTES + 1u) > MEMORY_TOKENS_MAX_SOURCE_BYTES || !ready(t)) return -1;
    char *clean = llm_neutralize(source);
    if (!clean) return -1;
    long count = -1;
    if (!t->server) count = (long)strlen(clean);
    else {
        TokenCall call;
        if (call_begin(&call, t)) count = call_end(&call, tokenize(&call, clean, false));
    }
    free(clean);
    return cancelled(t) ? -1 : count;
}

long memory_request_tokens(const char *complete_json, void *ctx)
{
    MemoryTokenizer *t = ctx;
    if (!complete_json || !ready(t)) return -1;
    size_t n = strnlen(complete_json, MEMORY_TOKENS_MAX_REQUEST_BYTES + 1u);
    if (n > MEMORY_TOKENS_MAX_REQUEST_BYTES) return -1;
    cJSON *request = checked_json(complete_json, n);
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(request, "messages");
    bool valid = cJSON_IsArray(messages) && messages->child;
    const cJSON *message;
    cJSON_ArrayForEach(message, messages) {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (!cJSON_IsObject(message) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(message, "role")) ||
            !(cJSON_IsString(content) || cJSON_IsArray(content) || cJSON_IsNull(content))) valid = false;
    }
    cJSON_Delete(request);
    if (!valid) return -1;
    if (!t->server) return cancelled(t) ? -1 : (long)n;
    TokenCall call;
    if (!call_begin(&call, t)) return -1;
    /* Preserve every chat option, including template kwargs and generation
     * prefix controls: /apply-template uses the chat-completions parser. */
    cJSON *response = post(&call, "/apply-template", complete_json);
    const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(response, "prompt");
    long count = cJSON_IsString(prompt) ? tokenize(&call, prompt->valuestring, true) : -1;
    cJSON_Delete(response);
    return call_end(&call, count);
}
