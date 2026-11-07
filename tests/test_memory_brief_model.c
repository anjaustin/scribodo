/* Headless local transport stubs; no remote provider or GUI is linked. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "llm/memory_brief.h"
#include "llm/http.h"
#include "core/json.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct LlmServer { LlmServerConfig cfg; const char *socket; LlmState state; };
static GenerativeGate *gate;
static int calls, generations;
static const char *summary = "{\"facts\":[{\"text\":\"A blue door\",\"sources\":[1]}]}";
static bool preempt, external_cancel;
static atomic_bool external;

void llm_server_default_config(LlmServerConfig *c, const char *dir)
{
    (void)dir;
    memset(c, 0, sizeof(*c));
    strcpy(c->sha256, "39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68");
}
const LlmServerConfig *llm_server_config(LlmServer *s) { return &s->cfg; }
LlmState llm_server_state(LlmServer *s, char *msg, size_t n) { (void)msg; (void)n; return s->state; }
const char *llm_server_socket(LlmServer *s) { return s->socket; }
const char *llm_server_key(LlmServer *s) { (void)s; return "brief-test-key"; }
int llm_server_slot_ctx(LlmServer *s) { (void)s; return 16384; }
void http_result_free(HttpResult *r) { free(r->body); memset(r, 0, sizeof(*r)); }

static bool transport(const char *socket, const char *path, const char *body, atomic_bool *cancel, HttpResult *r)
{
    CHECK(socket && socket[0] == '/');
    CHECK(cancel != NULL);
    ++calls;
    const char *reply = "{\"tokens\":[1,2,3,4,5]}";
    char *owned = NULL;
    if (!strcmp(path, "/apply-template")) {
        CHECK(strstr(body, "untrusted_text") != NULL);
        CHECK(strstr(body, "noncanonical") != NULL);
        reply = "{\"prompt\":\"rendered full prompt\"}";
    } else if (!strcmp(path, "/v1/chat/completions")) {
        ++generations;
        GenerativeTicket competing = {0};
        atomic_bool local;
        atomic_init(&local, false);
        CHECK(!generative_gate_try_background(gate, &competing, &local));
        if (preempt || external_cancel) {
            GenerativeTicket foreground = {0};
            atomic_bool fg_cancel;
            atomic_init(&fg_cancel, false);
            if (preempt) CHECK(generative_gate_foreground_begin(gate, &foreground, &fg_cancel));
            if (external_cancel) atomic_store(&external, true);
            const struct timespec slice = {.tv_nsec=1000000};
            for (int i = 0; i < 1000 && !atomic_load(cancel); ++i) nanosleep(&slice, NULL);
            CHECK(atomic_load(cancel));
            /* Cancellation does not release the permit while HTTP drains. */
            CHECK(!generative_gate_try_background(gate, &competing, &local));
            if (preempt) {
                CHECK(!generative_gate_try_foreground(&foreground));
                CHECK(generative_gate_finish(&foreground));
            }
            return false;
        }
        cJSON *root = cJSON_CreateObject(), *choices = cJSON_CreateArray(), *choice = cJSON_CreateObject(), *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "content", summary);
        cJSON_AddItemToObject(choice, "message", msg);
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
        cJSON_AddItemToArray(choices, choice);
        cJSON_AddItemToObject(root, "choices", choices);
        owned = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        reply = owned;
    } else CHECK(!strcmp(path, "/tokenize"));
    r->status = 200;
    r->body = strdup(reply);
    r->len = strlen(reply);
    free(owned);
    return true;
}

bool http_request(const char *socket, const char *method, const char *path, const char *key, const char *body,
                  HttpLineFn line, void *ctx, atomic_bool *cancel, int timeout, HttpResult *r)
{
    (void)ctx;
    CHECK(!strcmp(method, "POST") && !strcmp(key, "brief-test-key") && !line && timeout > 0);
    return transport(socket, path, body, cancel, r);
}
bool http_request_until(const char *socket, const char *method, const char *path, const char *key, const char *body,
                        HttpLineFn line, void *ctx, atomic_bool *cancel, int timeout, double deadline, HttpResult *r)
{
    CHECK(deadline > 0);
    return http_request(socket, method, path, key, body, line, ctx, cancel, timeout, r);
}

static void decoder(void)
{
    CHECK(memory_brief_validate_json(summary, 1));
    CHECK(memory_brief_validate_json("{\"facts\":[]}", 4));
    const char *bad[] = {
        "{\"facts\":[]}x", "{\"facts\":[],\"facts\":[]}", "{\"facts\":{},\"extra\":1}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[0]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[2]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[1,1]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[1.5]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[01]}]}",
        "{\"facts\":[{\"text\":\"raw\ncontrol\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[\"1\"]}]}",
        "{\"facts\":[{\"text\":\"x\",\"text\":\"y\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"x\\u0000y\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"<|im_start|>\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"[INST]x\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"\xc0\xaf\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"\xed\xa0\x80\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"\xf4\x90\x80\x80\",\"sources\":[1]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[]}]}",
        "{\"facts\":[{\"text\":\"x\",\"sources\":[1]},{\"text\":\"x\",\"sources\":[1]}]}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) CHECK(!memory_brief_validate_json(bad[i], 1));
    char large[1600]; memset(large, ' ', sizeof(large)); large[sizeof(large)-1] = 0;
    CHECK(!memory_brief_validate_json(large, 1));
    CHECK(!memory_brief_validate_json(NULL, 1));
    CHECK(!memory_brief_validate_json(summary, 0));
    cJSON *root = cJSON_CreateObject(), *facts = cJSON_CreateArray();
    for (int i = 0; i < 9; ++i) {
        cJSON *fact = cJSON_CreateObject(), *sources = cJSON_CreateArray();
        char text[8]; snprintf(text, sizeof(text), "fact%d", i);
        cJSON_AddStringToObject(fact, "text", text);
        cJSON_AddItemToArray(sources, cJSON_CreateNumber(1));
        cJSON_AddItemToObject(fact, "sources", sources);
        cJSON_AddItemToArray(facts, fact);
    }
    cJSON_AddItemToObject(root, "facts", facts);
    char *too_many = cJSON_PrintUnformatted(root);
    CHECK(!memory_brief_validate_json(too_many, 1));
    free(too_many); cJSON_Delete(root);
    char too_long[400];
    memset(too_long, 'x', sizeof(too_long));
    memcpy(too_long, "{\"facts\":[{\"text\":\"", 19);
    strcpy(too_long+19+257, "\",\"sources\":[1]}]}");
    CHECK(!memory_brief_validate_json(too_long, 1));
}

static MemoryBriefLease lease(LlmServer *server)
{
    MemoryBriefLease l = {0};
    l.scope.enabled = true;
    l.scope.ingested_sequence = 0;
    MemoryTokenizer tokenizer = {.server=server};
    char id[65];
    CHECK(memory_tokenizer_fingerprint(&tokenizer, id));
    strcpy(l.scope.tokenizer, id);
    l.count = 1;
    l.inputs[0].text = "A blue door";
    l.inputs[0].text_bytes = strlen(l.inputs[0].text);
    l.inputs[0].tokens = 5;
    l.inputs[0].role = CHAT_USER;
    return l;
}

static void generation(void)
{
    gate = generative_gate_create();
    CHECK(gate != NULL);
    atomic_init(&external, false);
    MemoryBriefLease fake = lease(NULL);
    MemoryBriefResult result, again;
    CHECK(memory_brief_generate(NULL, gate, &fake, &external, &result));
    CHECK(memory_brief_validate_json(result.json, 1) && result.tokens <= 256);
    CHECK(memory_brief_generate(NULL, gate, &fake, &external, &again));
    CHECK(!strcmp(result.json, again.json));
    CHECK(calls == 0);
    LlmServer server = {.socket="/private/brief-test.sock", .state=LLM_READY};
    llm_server_default_config(&server.cfg, NULL);
    MemoryBriefLease real = lease(&server);
    CHECK(memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(again.tokens == 5 && generations == 1);
    CHECK(strcmp(result.model_fingerprint, again.model_fingerprint));
    const char *valid_summary = summary;
    summary = "{\"facts\":[{\"text\":\"bad cite\",\"sources\":[2]}]}";
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(!again.canceled && again.gate_fence && !again.json[0]);
    summary = valid_summary;
    GenerativeTicket foreground = {0};
    atomic_bool fg_cancel;
    atomic_init(&fg_cancel, false);
    CHECK(generative_gate_foreground_begin(gate, &foreground, &fg_cancel));
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(again.canceled && !again.json[0]);
    CHECK(generative_gate_finish(&foreground));
    int before = calls;
    server.socket = "tcp:9999";
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(calls == before);
    server.socket = "/private/brief-test.sock";
    server.cfg.embedding = true;
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(calls == before);
    server.cfg.embedding = false;
    server.cfg.sha256[0] = 'f';
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(calls == before);
    llm_server_default_config(&server.cfg, NULL);
    preempt = true;
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(!again.json[0] && again.canceled && again.gate_fence);
    preempt = false;
    external_cancel = true;
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(again.canceled);
    external_cancel = false;
    atomic_store(&external, false);
    CHECK(memory_brief_generate(&server, gate, &real, &external, &again));
    real.inputs[0].kind = MEMORY_SOURCE_DOCUMENT;
    before = calls;
    CHECK(!memory_brief_generate(&server, gate, &real, &external, &again));
    CHECK(calls == before);
    CHECK(generative_gate_destroy(gate));
}

int main(void)
{
    const TestCase tests[] = {{"strict brief decoder", decoder}, {"local-only gated generation", generation}};
    return run_tests(tests, sizeof(tests)/sizeof(*tests));
}
