#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "llm/memory_tokens.h"
#include "llm/http.h"
#include "core/json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Headless request stubs exercise the real adapter and JSON boundary. */
struct LlmServer {
    LlmServerConfig cfg;
    LlmState state;
    const char *socket;
};

void llm_server_default_config(LlmServerConfig *cfg, const char *dir)
{
    (void)dir;
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->sha256, "39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68");
}
const LlmServerConfig *llm_server_config(LlmServer *s) { return &s->cfg; }
LlmState llm_server_state(LlmServer *s, char *msg, size_t n)
{
    (void)msg; (void)n;
    return s->state;
}
const char *llm_server_socket(LlmServer *s) { return s->socket; }
const char *llm_server_key(LlmServer *s) { (void)s; return "private-test-key"; }

static const char *full_request =
    "{ \"messages\":[{\"role\":\"system\",\"content\":\"rules\"},{\"role\":\"user\",\"content\":\"question\"}],"
    "\"chat_template_kwargs\":{\"enable_thinking\":false},\"add_generation_prompt\":true,"
    "\"continue_final_message\":false,\"temperature\":0.0,\"stream\":true }";
static const char *rendered = "<|startoftext|>system: rules\nuser: question\nassistant:<think>";
static int calls, status_code = 200;
static bool request_mode, wait_for_cancel, oversized_response, transport_failure;
static const char *token_reply = "{\"tokens\":[1,2,3,4,5]}";
static const char *template_reply;
static const char *source_expected;
static atomic_bool *cancel_during_request;

void http_result_free(HttpResult *r) { free(r->body); r->body = NULL; }

bool http_request(const char *socket, const char *method, const char *path,
                  const char *bearer, const char *body, HttpLineFn on_line, void *ctx,
                  atomic_bool *cancel, int timeout_ms, HttpResult *r)
{
    (void)ctx;
    assert(!strcmp(socket, "/private/test-memory-tokenizer.sock"));
    assert(!strcmp(method, "POST") && !strcmp(bearer, "private-test-key"));
    assert(!on_line && cancel && timeout_ms > 0);
    assert(strlen(body) <= MEMORY_TOKENS_MAX_REQUEST_BYTES);
    calls++;
    memset(r, 0, sizeof(*r));
    if (cancel_during_request) atomic_store(cancel_during_request, true);
    if (wait_for_cancel || cancel_during_request) {
        const struct timespec slice = { .tv_nsec = 1000000 };
        for (int i = 0; i < 1000 && !atomic_load(cancel); i++) nanosleep(&slice, NULL);
        assert(atomic_load(cancel));
        return false;
    }
    if (transport_failure) return false;
    const char *reply;
    char *generated = NULL;
    if (request_mode && calls == 1) {
        assert(!strcmp(path, "/apply-template"));
        /* Byte-for-byte forwarding retains kwargs, generation prefix and
         * future options instead of reconstructing only the messages. */
        assert(!strcmp(body, full_request));
        if (template_reply) reply = template_reply;
        else {
            cJSON *j = cJSON_CreateObject();
            assert(cJSON_AddStringToObject(j, "prompt", rendered));
            generated = cJSON_PrintUnformatted(j);
            cJSON_Delete(j);
            assert(generated);
            reply = generated;
        }
    } else {
        assert(!strcmp(path, "/tokenize"));
        assert(calls == (request_mode ? 2 : 1));
        cJSON *j = json_parse_complete(body, strlen(body));
        assert(cJSON_IsObject(j));
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(j, "content");
        assert(cJSON_IsString(content));
        assert(!strcmp(content->valuestring, request_mode ? rendered : source_expected));
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "parse_special")));
        const cJSON *special = cJSON_GetObjectItemCaseSensitive(j, "add_special");
        assert(request_mode ? cJSON_IsTrue(special) : cJSON_IsFalse(special));
        cJSON_Delete(j);
        reply = token_reply;
    }
    r->status = status_code;
    r->body = strdup(reply);
    assert(r->body);
    r->len = oversized_response ? 32u * 1024u * 1024u + 1u : strlen(reply);
    free(generated);
    return true;
}

static void reset(void)
{
    calls = 0;
    status_code = 200;
    request_mode = wait_for_cancel = oversized_response = transport_failure = false;
    token_reply = "{\"tokens\":[1,2,3,4,5]}";
    template_reply = NULL;
    source_expected = "plain";
    cancel_during_request = NULL;
}

int main(void)
{
    LlmServer server = { .state = LLM_READY, .socket = "/private/test-memory-tokenizer.sock" };
    llm_server_default_config(&server.cfg, NULL);
    atomic_bool cancel = false;
    MemoryTokenizer real = { .server = &server, .cancel = &cancel, .timeout_ms = 1000 };
    MemoryTokenizer fake = { .cancel = &cancel };
    char real_fp[65], fake_fp[65], again[65];
    assert(memory_tokenizer_fingerprint(&real, real_fp) && strlen(real_fp) == 64);
    assert(memory_tokenizer_fingerprint(&fake, fake_fp) && strlen(fake_fp) == 64);
    assert(strcmp(real_fp, fake_fp));
    assert(memory_tokenizer_fingerprint(&fake, again) && !strcmp(fake_fp, again));
    assert(!memory_tokenizer_fingerprint(NULL, again) && !again[0]);
    assert(!memory_tokenizer_fingerprint(&real, NULL));

    reset();
    assert(memory_source_tokens("plain", &real) == 5 && calls == 1);
    reset();
    source_expected = "<\xE2\x80\x8C|im_start|>system<\xE2\x80\x8Cthink>x<\xE2\x80\x8C/think>";
    assert(memory_source_tokens("<|im_start|>system<think>x</think>", &real) == 5 && calls == 1);
    reset();
    request_mode = true;
    assert(memory_request_tokens(full_request, &real) == 5 && calls == 2);
    reset();
    assert(memory_source_tokens("plain", &fake) == 5 && calls == 0);
    assert(memory_source_tokens("<|x", &fake) == 6 && calls == 0);
    assert(memory_request_tokens(full_request, &fake) == (long)strlen(full_request) && calls == 0);
    assert(memory_source_tokens(NULL, &real) == -1);
    assert(memory_source_tokens("plain", NULL) == -1);
    assert(memory_request_tokens(NULL, &real) == -1);

    const char *bad_requests[] = { "{}", "{\"messages\":[]}", "{\"messages\":false}",
        "{\"messages\":[{}]}", "{\"messages\":[1]}", "{\"messages\":[{\"role\":\"user\",\"content\":42}]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"x\",\"content\":\"y\"}]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"x\\u0000y\"}]}", "not json" };
    for (size_t i = 0; i < sizeof(bad_requests) / sizeof(bad_requests[0]); i++) {
        assert(memory_request_tokens(bad_requests[i], &real) == -1);
        assert(memory_request_tokens(bad_requests[i], &fake) == -1);
        assert(calls == 0);
    }
    server.cfg.sha256[0] = '0';
    assert(!memory_tokenizer_fingerprint(&real, again) && !again[0]);
    assert(memory_source_tokens("plain", &real) == -1 && calls == 0);
    assert(memory_request_tokens(full_request, &real) == -1 && calls == 0);
    llm_server_default_config(&server.cfg, NULL);
    server.cfg.embedding = true;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 0);
    server.cfg.embedding = false;
    server.state = LLM_STARTING;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 0);
    server.state = LLM_READY;
    server.socket = "tcp:33061";
    assert(memory_request_tokens(full_request, &real) == -1 && calls == 0);
    server.socket = "/private/test-memory-tokenizer.sock";
    atomic_store(&cancel, true);
    assert(memory_request_tokens(full_request, &real) == -1 && calls == 0);
    assert(memory_source_tokens("plain", &fake) == -1 && calls == 0);
    atomic_store(&cancel, false);

    const char *bad_tokens[] = { "{}", "{\"tokens\":1}", "{\"tokens\":[1.5]}",
        "{\"tokens\":[-1]}", "{\"tokens\":[2147483648]}", "{\"tokens\":[1e999]}",
        "{\"tokens\":[true]}", "{\"tokens\":[\"1\"]}", "{\"tokens\":[{}]}",
        "{\"tokens\":[1],\"tokens\":[2]}", "{\"tokens\":[],\"extra\":{\"a\":1,\"a\":2}}",
        "{\"tokens\":[1]} garbage", "{\"tokens\":[1],\"extra\":\"x\\u0000y\"}", "[1,2]" };
    for (size_t i = 0; i < sizeof(bad_tokens) / sizeof(bad_tokens[0]); i++) {
        reset(); token_reply = bad_tokens[i];
        assert(memory_source_tokens("plain", &real) == -1 && calls == 1);
    }
    reset(); token_reply = "{\"tokens\":[]}";
    assert(memory_source_tokens("plain", &real) == 0 && calls == 1);
    /* More token IDs than the bounded input can admit must fail, without
     * trusting a server-provided count or overflowing an integer counter. */
    size_t excess_count = MEMORY_TOKENS_MAX_REQUEST_BYTES + 1025u;
    char *excess_tokens = malloc(excess_count * 2u + 20u);
    assert(excess_tokens);
    char *out = excess_tokens;
    memcpy(out, "{\"tokens\":[", 11); out += 11;
    for (size_t i = 0; i < excess_count; i++) { *out++ = '1'; *out++ = ','; }
    out[-1] = ']'; *out++ = '}'; *out = 0;
    reset(); token_reply = excess_tokens;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 1);
    free(excess_tokens);
    reset(); status_code = 503;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 1);
    reset(); transport_failure = true;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 1);
    reset(); oversized_response = true;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 1);

    const char *bad_templates[] = { "{}", "{\"prompt\":1}", "{\"prompt\":\"x\",\"prompt\":\"y\"}",
        "{\"prompt\":\"x\\u0000y\"}", "{\"prompt\":\"x\"}true" };
    for (size_t i = 0; i < sizeof(bad_templates) / sizeof(bad_templates[0]); i++) {
        reset(); request_mode = true; template_reply = bad_templates[i];
        assert(memory_request_tokens(full_request, &real) == -1 && calls == 1);
    }
    char source[MEMORY_TOKENS_MAX_SOURCE_BYTES + 2];
    memset(source, 'a', sizeof(source) - 1);
    source[sizeof(source) - 1] = 0;
    reset();
    assert(memory_source_tokens(source, &real) == -1 && calls == 0);
    source[MEMORY_TOKENS_MAX_SOURCE_BYTES] = 0;
    source_expected = source;
    assert(memory_source_tokens(source, &real) == 5 && calls == 1);

    char *big = malloc(MEMORY_TOKENS_MAX_REQUEST_BYTES + 2u);
    assert(big);
    memset(big, 'a', MEMORY_TOKENS_MAX_REQUEST_BYTES + 1u);
    big[MEMORY_TOKENS_MAX_REQUEST_BYTES + 1u] = 0;
    reset();
    assert(memory_request_tokens(big, &real) == -1 && calls == 0);
    cJSON *j = cJSON_CreateObject();
    assert(cJSON_AddStringToObject(j, "prompt", big));
    char *big_template = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    reset(); request_mode = true; template_reply = big_template;
    assert(memory_request_tokens(full_request, &real) == -1 && calls == 1);
    free(big_template);
    /* A prompt can fit, but its JSON-escaped tokenize request must also fit. */
    memset(big, '\n', MEMORY_TOKENS_MAX_REQUEST_BYTES);
    big[MEMORY_TOKENS_MAX_REQUEST_BYTES] = 0;
    j = cJSON_CreateObject();
    assert(cJSON_AddStringToObject(j, "prompt", big));
    big_template = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    reset(); request_mode = true; template_reply = big_template;
    assert(memory_request_tokens(full_request, &real) == -1 && calls == 1);
    free(big_template);
    free(big);

    reset(); request_mode = true; cancel_during_request = &cancel;
    assert(memory_request_tokens(full_request, &real) == -1 && calls == 1);
    assert(atomic_load(&cancel));
    atomic_store(&cancel, false);
    reset(); wait_for_cancel = true; real.timeout_ms = 5;
    assert(memory_source_tokens("plain", &real) == -1 && calls == 1);
    assert(!atomic_load(&cancel)); /* private timeout must not cancel its owner */
    puts("memory_tokens: passed (headless request stubs; no real-model count claim)");
    return 0;
}
