/* Real-model experiment driver. No fake backend, test-attach, expected answers,
 * or lexical candidate replacement. Python owns preregistration and scoring.
 * Build via memory_quality_eval.sh; no changes to the application build needed. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "app/chat_store.h"
#include "app/memory_brief_store.h"
#include "llm/context_planner.h"
#include "llm/memory_brief.h"
#include "llm/memory_vectors.h"
#include "llm/http.h"
#include "core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

static StoreManager *store;
static Db *db;
static LlmServer *model, *embed;
static SemanticClient *vectors;
static GenerativeGate *gate;
static MemoryTokenizer tokenizer;
static MemoryScope archive, private_scope;
static char project[64], conversation[40], tokenizer_id[65], brief_model[65];
static FILE *events;
static double began, tokenize_seconds, embedding_seconds, brief_seconds;
static size_t tokenize_calls, tokenize_tokens, index_calls, brief_attempts, brief_published;
static size_t tokenizer_http_calls;
static size_t brief_usage_known, brief_prompt_tokens, brief_completion_tokens, brief_tokenizer_calls;
static double brief_generation_ms;
static bool brief_complete;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static const char *str(const cJSON *j, const char *key)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(x) ? x->valuestring : "";
}
static void emit(cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    if (s) { fprintf(events, "%s\n", s); fflush(events); free(s); }
    cJSON_Delete(j);
}
static bool fail(const char *what, int line)
{
    fprintf(stderr, "memory_quality_eval: line %d: %s; SQL: %s\n", line, what, db ? db_error(db) : "not connected");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", "error");
    cJSON_AddStringToObject(j, "error", what);
    cJSON_AddNumberToObject(j, "line", line);
    emit(j);
    return false;
}
#define NEED(x) do { if (!(x)) return fail(#x, __LINE__); } while (0)
static long counted(const char *text, int kind)
{
    double t = now();
    long n = kind == 0 ? memory_source_tokens(text, &tokenizer) :
             kind == 1 ? memory_reference_tokens(text, &tokenizer) : memory_request_tokens(text, &tokenizer);
    tokenize_seconds += now() - t;
    tokenize_calls++;
    if (n > 0) tokenize_tokens += (size_t)n;
    if (n < 0) {
        fprintf(stderr, "token count failed: kind=%d bytes=%zu state=%d\n", kind, strlen(text), llm_server_state(model, NULL, 0));
    }
    return n;
}
static long source_count(const char *s, void *ctx) { (void)ctx; return counted(s, 0); }
static long reference_count(const char *s, void *ctx) { (void)ctx; return counted(s, 1); }
static long request_count(const char *s, void *ctx) { (void)ctx; return counted(s, 2); }

static bool start_server(LlmServer **out, const char *root, bool embedding)
{
    LlmServerConfig cfg;
    if (embedding) llm_server_embed_config(&cfg, "");
    else llm_server_default_config(&cfg, "");
    const char *label = embedding ? "embed" : "chat";
    /* Production tokenizer admission requires an absolute local Unix socket.
     * Keep a private short directory: the archived run path exceeds sun_path. */
    char cwd[1024];
    NEED(getcwd(cwd, sizeof(cwd)));
    char socket_root[1024];
    snprintf(socket_root, sizeof(socket_root), "%s/build/mq-%s-XXXXXX", cwd, label);
    NEED(mkdtemp(socket_root));
    snprintf(cfg.run_dir, sizeof(cfg.run_dir), "%s", socket_root);
    NEED(strlen(cfg.run_dir) + strlen("/llama.sock") < 104);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s/%s-server.log", root, label);
    snprintf(cfg.cache_path, sizeof(cfg.cache_path), "%s/%s-verified.txt", root, label);
    /* This is a new private supervisor, never the author's application server. */
    if (!embedding) { cfg.slots = 2; cfg.ctx_total = 16384; }
    NEED(strlen(cfg.sha256) == 64 && cfg.server_bin[0] && cfg.model_path[0]);
    NEED(*out = llm_server_create(&cfg));
    llm_server_start(*out);
    double deadline = now() + 600;
    char message[512] = "";
    LlmState state;
    do {
        state = llm_server_state(*out, message, sizeof(message));
        if (state == LLM_READY || state == LLM_FAILED) break;
        struct timespec delay = {.tv_nsec=100000000}; nanosleep(&delay, NULL);
    } while (now() < deadline);
    if (state != LLM_READY) fprintf(stderr, "%s: %s\n", label, message);
    NEED(state == LLM_READY);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", "server");
    cJSON_AddStringToObject(j, "kind", label);
    cJSON_AddStringToObject(j, "model_path", cfg.model_path);
    cJSON_AddStringToObject(j, "verified_model_sha256", cfg.sha256);
    cJSON_AddStringToObject(j, "server_bin", cfg.server_bin);
    cJSON_AddNumberToObject(j, "slot_context", llm_server_slot_ctx(*out));
    cJSON_AddNumberToObject(j, "batch", cfg.batch);
    /* Includes actual running backend build/version data when endpoint provides it. */
    HttpResult r = {0};
    if (http_request(llm_server_socket(*out), "GET", "/props", llm_server_key(*out), NULL,
                     NULL, NULL, NULL, 10000, &r) && r.status == 200 && r.body)
        cJSON_AddStringToObject(j, "runtime_props", r.body);
    http_result_free(&r);
    emit(j);
    return true;
}

static bool ingest(MemoryScope *scope)
{
    char why[512] = "";
    char source_conversation[40];
    snprintf(source_conversation, sizeof(source_conversation), "%s", scope->conversation_id);
    /* A bounded production step uses the pinned model tokenizer, <=2000 bytes,
     * <=512 tokens per unit. No synthetic tokens or vector payload hydration. */
    for (;;) {
        bool progress = false;
        bool ingested = memory_ingest_step(db, scope->id, "quality-ingest", source_count, NULL,
                                           tokenizer_id, &progress, why, sizeof(why));
        if (!ingested) fprintf(stderr, "ingest: scope=%s tokenizer=%s transaction=%d why=%s\n",
                               scope->id, tokenizer_id, db_in_transaction(db), why);
        NEED(ingested);
        if (!progress) break;
    }
    NEED(memory_scope_get(db, project, source_conversation, scope));
    NEED(scope->ingested_sequence == scope->pending_sequence);
    NEED(memory_vectors_create(vectors, scope, why, sizeof(why)));
    for (;;) {
        MemoryIndexLease lease = {0}; bool claimed = false;
        NEED(memory_outbox_claim(db, scope->id, "quality-index", &lease, &claimed));
        if (!claimed) {
            bool more = lease.maintenance;
            memory_index_lease_free(&lease);
            if (more) continue;
            break;
        }
        double t = now();
        bool delivered = memory_vectors_upsert(vectors, &lease, why, sizeof(why));
        embedding_seconds += now() - t;
        index_calls++;
        bool acknowledged = memory_outbox_ack(db, &lease, delivered);
        memory_index_lease_free(&lease);
        if (!delivered) fprintf(stderr, "embedding/index: %s\n", why);
        NEED(delivered && acknowledged);
    }
    NEED(memory_scope_get(db, project, source_conversation, scope));
    NEED(!scope->index_pending);
    return true;
}

static bool admission(void *ctx)
{
    const MemoryBriefResult *r = ctx;
    return generative_gate_fence_valid(gate, r->gate_fence);
}
static bool generate_briefs(size_t limit)
{
    NEED(gate = generative_gate_create());
    NEED(memory_brief_model_fingerprint(&tokenizer, brief_model));
    double start = now();
    size_t input_units = 0;
    for (;;) {
        MemoryBriefLease lease = {0}; bool claimed = false;
        NEED(memory_brief_claim(db, &archive, "quality-brief", &lease, &claimed));
        if (!claimed) { memory_brief_lease_free(&lease); break; }
        if (limit && brief_attempts >= limit) {
            NEED(memory_brief_fail(db, &lease, true, true));
            memory_brief_lease_free(&lease);
            break;
        }
        MemoryBriefResult result = {0};
        double t = now();
        bool generated = memory_brief_generate(model, gate, &lease, NULL, &result);
        brief_attempts++;
        if (result.usage_known) {
            brief_usage_known++;
            brief_prompt_tokens += result.prompt_tokens;
            brief_completion_tokens += result.completion_tokens;
        }
        brief_tokenizer_calls += result.tokenizer_http_calls;
        brief_generation_ms += result.generation_ms;
        bool published = false;
        if (generated) {
            NEED(memory_brief_publish(db, &lease, result.json, result.tokens, result.model_fingerprint,
                                      MEMORY_BRIEF_PROTOCOL, admission, &result, &published));
        } else NEED(memory_brief_fail(db, &lease, false, result.canceled));
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "event", "brief_generation");
        cJSON_AddStringToObject(j, "input_digest", lease.input_digest);
        cJSON_AddBoolToObject(j, "generated", generated);
        cJSON_AddBoolToObject(j, "published", published);
        cJSON_AddNumberToObject(j, "seconds", now() - t);
        cJSON_AddNumberToObject(j, "measured_output_tokens", result.tokens);
        cJSON_AddNumberToObject(j, "source_units", lease.count);
        cJSON_AddBoolToObject(j, "usage_known", result.usage_known);
        if (result.usage_known) {
            cJSON_AddNumberToObject(j, "prompt_tokens", result.prompt_tokens);
            cJSON_AddNumberToObject(j, "completion_tokens", result.completion_tokens);
        }
        cJSON_AddNumberToObject(j, "tokenizer_http_calls", result.tokenizer_http_calls);
        cJSON_AddNumberToObject(j, "generation_ms", result.generation_ms);
        if (generated) cJSON_AddStringToObject(j, "json", result.json);
        emit(j);
        if (published) { brief_published++; input_units += lease.count; }
        memory_brief_lease_free(&lease);
        /* Genuine failure is retained, not retried/tuned to green. */
        if (!generated || !published) break;
    }
    brief_seconds = now() - start;
    DbRows rows = {0};
    NEED(db_query(db, "SELECT COUNT(*) FROM memory_units WHERE scope_id=? AND generation=? AND source_kind='chat'",
                  (DbArg[]){DB_S(archive.id), DB_I(archive.generation)}, 2, &rows));
    uint64_t total = 0;
    bool decoded = db_cell_u64(&rows, 0, 0, &total);
    db_rows_free(&rows);
    NEED(decoded);
    brief_complete = input_units == total;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", "background");
    cJSON_AddNumberToObject(j, "brief_attempts", brief_attempts);
    cJSON_AddNumberToObject(j, "brief_published", brief_published);
    cJSON_AddNumberToObject(j, "brief_input_units", input_units);
    cJSON_AddNumberToObject(j, "archive_units", (double)total);
    cJSON_AddBoolToObject(j, "brief_frontier_complete", brief_complete);
    cJSON_AddNumberToObject(j, "brief_seconds", brief_seconds);
    cJSON_AddNumberToObject(j, "brief_usage_known_attempts", brief_usage_known);
    cJSON_AddBoolToObject(j, "brief_full_usage_known", brief_usage_known == brief_attempts);
    cJSON_AddNumberToObject(j, "brief_prompt_tokens_known", brief_prompt_tokens);
    cJSON_AddNumberToObject(j, "brief_completion_tokens_known", brief_completion_tokens);
    cJSON_AddNumberToObject(j, "brief_tokenizer_http_calls", brief_tokenizer_calls);
    cJSON_AddNumberToObject(j, "brief_generation_ms", brief_generation_ms);
    cJSON_AddStringToObject(j, "summary_cost_charge_arm", "t1_retrieval");
    cJSON_AddNumberToObject(j, "embedding_index_calls", index_calls);
    cJSON_AddNumberToObject(j, "embedding_index_seconds", embedding_seconds);
    cJSON_AddStringToObject(j, "embedding_token_usage", "excluded: real embedding client exposes no usage counters");
    cJSON_AddNumberToObject(j, "backlog_sequences", archive.pending_sequence - archive.ingested_sequence);
    cJSON_AddBoolToObject(j, "index_pending", archive.index_pending);
    cJSON_AddNumberToObject(j, "retries", 0);
    emit(j);
    return true;
}

static cJSON *original(const char *id, int64_t sequence, const char *text)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddNumberToObject(j, "sequence", (double)sequence);
    cJSON_AddStringToObject(j, "text", text);
    return j;
}
static bool oracle_plan(MemoryPlan *plan, const cJSON *task, int64_t boundary)
{
    plan->scope = archive;
    plan->readiness = MEMORY_PARTIAL;
    const cJSON *seq;
    cJSON_ArrayForEach(seq, cJSON_GetObjectItemCaseSensitive(task, "oracle_sequences")) {
        NEED(cJSON_IsNumber(seq) && seq->valuedouble < (double)boundary);
        DbRows rows = {0};
        /* This dataset-citation lookup is used ONLY by the oracle control. */
        NEED(db_query(db, "SELECT id,text_hash FROM memory_units WHERE scope_id=? AND generation=?"
                         " AND source_kind='chat' AND sequence=? ORDER BY byte_start LIMIT 8",
                      (DbArg[]){DB_S(archive.id), DB_I(archive.generation), DB_I((int64_t)seq->valuedouble)}, 3, &rows));
        for (size_t i = 0; i < rows.nrows; i++) {
            MemoryCandidate candidate = {.history_epoch=archive.history_epoch, .generation=archive.generation};
            snprintf(candidate.unit_id, sizeof(candidate.unit_id), "%s", db_cell(&rows, i, 0));
            snprintf(candidate.text_hash, sizeof(candidate.text_hash), "%s", db_cell(&rows, i, 1));
            MemoryUnit u = {0}; bool found = false;
            bool ok = memory_candidate_hydrate(db, &archive, &candidate, &u, &found);
            if (ok && found && plan->count < MEMORY_PLAN_UNITS &&
                plan->text_bytes + u.text_bytes <= MEMORY_PLAN_BYTES) {
                plan->text_bytes += u.text_bytes;
                plan->units[plan->count++] = u;
                memset(&u, 0, sizeof(u));
            }
            memory_unit_free(&u);
            if (!ok || !found) { db_rows_free(&rows); return fail("oracle hydration", __LINE__); }
        }
        db_rows_free(&rows);
    }
    return true;
}

static char *request_json(const cJSON *input, const char *question)
{
    cJSON *request = cJSON_CreateObject(), *messages = cJSON_AddArrayToObject(request, "messages");
    cJSON *system = cJSON_CreateObject(), *user = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", str(input, "instructions"));
    cJSON_AddItemToArray(messages, system);
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(input, "recent")) {
        cJSON *turn = cJSON_CreateObject();
        cJSON_AddStringToObject(turn, "role", "user");
        cJSON_AddStringToObject(turn, "content", str(row, "text"));
        cJSON_AddItemToArray(messages, turn);
    }
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", question);
    cJSON_AddItemToArray(messages, user);
    cJSON_AddNumberToObject(request, "max_tokens", 128);
    cJSON_AddNumberToObject(request, "temperature", 0);
    cJSON_AddNumberToObject(request, "seed", 73190427);
    cJSON_AddNumberToObject(request, "id_slot", 0);
    cJSON_AddBoolToObject(request, "cache_prompt", false);
    cJSON_AddBoolToObject(request, "stream", true);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(request, "stream_options"), "include_usage", true);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(request, "chat_template_kwargs"), "enable_thinking", false);
    char *json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    return json;
}
typedef struct {
    double started, ttft;
    char *text;
    size_t bytes;
    cJSON *usage;
    char finish[64];
    bool failed, done;
} Stream;
static bool stream_line(const char *line, size_t len, void *ctx)
{
    (void)len;
    Stream *s = ctx;
    if (strncmp(line, "data: ", 6)) return true;
    if (!strcmp(line + 6, "[DONE]")) { s->done = true; return true; }
    cJSON *root = json_parse(line + 6);
    if (!root || cJSON_GetObjectItem(root, "error")) s->failed = true;
    const cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (cJSON_IsObject(usage)) { cJSON_Delete(s->usage); s->usage = cJSON_Duplicate(usage, true); }
    const cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
    const cJSON *delta = cJSON_GetObjectItem(choice, "delta");
    const char *content = str(delta, "content");
    if (*content) {
        if (s->ttft < 0) s->ttft = now() - s->started;
        size_t n = strlen(content);
        if (s->bytes + n > 65536) s->failed = true;
        else {
            char *text = realloc(s->text, s->bytes + n + 1);
            if (!text) s->failed = true;
            else { s->text = text; memcpy(text + s->bytes, content, n + 1); s->bytes += n; }
        }
    }
    if (*str(choice, "finish_reason")) snprintf(s->finish, sizeof(s->finish), "%s", str(choice, "finish_reason"));
    cJSON_Delete(root);
    return !s->failed;
}
static bool run_task(const cJSON *input, const cJSON *task, const char *arm, int64_t boundary)
{
    double start = now(), token_start = tokenize_seconds;
    size_t call_start = tokenize_calls, count_start = tokenize_tokens;
    size_t http_start = tokenizer_http_calls;
    MemoryPlan plan = {0};
    bool planned = !strcmp(arm, "retrieval_only") || !strcmp(arm, "t1_retrieval");
    bool oracle = !strcmp(arm, "oracle_source");
    if (planned) {
        NEED(memory_plan_build(db, vectors, project, conversation, str(task, "question"), boundary,
                               false, reference_count, NULL, tokenizer_id, NULL, &plan));
        if (!strcmp(arm, "t1_retrieval"))
            NEED(memory_plan_add_briefs(db, &plan, boundary, brief_model, reference_count, NULL));
        NEED(memory_plan_admit(db, &plan, false));
    }
    if (oracle) NEED(oracle_plan(&plan, task, boundary));
    char *base = request_json(input, str(task, "question"));
    NEED(base);
    char *request = NULL;
    long source_tokens = 0;
    for (;;) {
        NEED(memory_plan_apply(&plan, base, false, 8192 - 128, request_count, NULL, &request));
        source_tokens = plan.reference ? counted(plan.reference, 1) : 0;
        NEED(source_tokens >= 0);
        if (source_tokens <= MEMORY_PLAN_TOKENS) break;
        /* Native apply enforces bytes/final-window. Enforce the common optional
         * 4096-token cap too, always dropping whole items, briefs first. */
        free(request); request = NULL;
        if (plan.brief_count) {
            memory_brief_free(&plan.briefs[--plan.brief_count]);
            if (!plan.brief_count) plan.brief_model_fingerprint[0] = 0;
        }
        else if (plan.count) {
            MemoryUnit *last = &plan.units[--plan.count];
            plan.text_bytes -= last->text_bytes; memory_unit_free(last);
        } else NEED(false);
        plan.limited = true;
    }
    free(base);
    /* These are offline receipts, NEVER another prompt renderer. Raw originals
     * are distinct from brief provenance pointers; pointer bodies stay absent. */
    cJSON *refs = cJSON_CreateObject();
    cJSON *raw = cJSON_AddArrayToObject(refs, "originals");
    cJSON *pointers = cJSON_AddArrayToObject(refs, "brief_input_pointers");
    cJSON *briefs = cJSON_AddArrayToObject(refs, "briefs");
    size_t pointer_validation_bytes = 0;
    for (size_t i = 0; i < plan.count; i++) {
        const MemoryUnit *u = &plan.units[i];
        cJSON *r = original(u->id, u->sequence, u->text);
        cJSON_AddNumberToObject(r, "byte_start", (double)u->byte_start);
        cJSON_AddNumberToObject(r, "text_bytes", u->text_bytes);
        cJSON_AddNumberToObject(r, "source_tokens", u->tokens);
        cJSON_AddItemToArray(raw, r);
    }
    for (size_t i = 0; i < plan.brief_count; i++) {
        const MemoryBrief *b = &plan.briefs[i];
        cJSON *receipt = cJSON_CreateObject();
        cJSON_AddStringToObject(receipt, "id", b->id);
        cJSON_AddStringToObject(receipt, "summary_hash", b->summary_hash);
        cJSON_AddNumberToObject(receipt, "tokens", b->tokens);
        cJSON_AddItemToArray(briefs, receipt);
        for (size_t k = 0; k < b->count; k++) {
            const MemoryUnit *u = &b->inputs[k];
            pointer_validation_bytes += u->text_bytes;
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "id", u->id);
            cJSON_AddStringToObject(p, "brief_id", b->id);
            cJSON_AddStringToObject(p, "text_hash", u->text_hash);
            cJSON_AddNumberToObject(p, "sequence", (double)u->sequence);
            cJSON_AddNumberToObject(p, "byte_start", (double)u->byte_start);
            cJSON_AddNumberToObject(p, "text_bytes", u->text_bytes);
            cJSON_AddItemToArray(pointers, p);
        }
    }
    long final_tokens = counted(request, 2);
    NEED(final_tokens > 0 && final_tokens + 128 <= llm_server_slot_ctx(model));
    if (planned || oracle) NEED(memory_plan_admit(db, &plan, false));
    MemoryScope current;
    NEED(memory_scope_get(db, project, conversation, &current) && memory_scope_same(&archive, &current));
    double plan_seconds = now() - start;
    Stream stream = {.started=now(), .ttft=-1};
    HttpResult result = {0};
    bool ok = http_request_until(llm_server_socket(model), "POST", "/v1/chat/completions", llm_server_key(model),
                                 request, stream_line, &stream, NULL, 120000, (now() + 180) * 1000, &result);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", "answer");
    cJSON_AddStringToObject(j, "task", str(task, "id"));
    cJSON_AddStringToObject(j, "arm", arm);
    cJSON_AddBoolToObject(j, "transport_ok", ok && result.status == 200 && !stream.failed && stream.done);
    cJSON_AddNumberToObject(j, "http_status", result.status);
    cJSON_AddStringToObject(j, "error", result.error);
    cJSON_AddStringToObject(j, "response", stream.text ? stream.text : "");
    cJSON_AddStringToObject(j, "finish_reason", stream.finish);
    if (stream.usage) { cJSON_AddItemToObject(j, "generator_usage", stream.usage); stream.usage = NULL; }
    else cJSON_AddNullToObject(j, "generator_usage");
    cJSON_AddNumberToObject(j, "ttft_seconds", stream.ttft);
    cJSON_AddNumberToObject(j, "generation_seconds", now() - stream.started);
    cJSON_AddNumberToObject(j, "planning_seconds", plan_seconds);
    cJSON_AddNumberToObject(j, "final_prompt_tokens", final_tokens);
    cJSON_AddNumberToObject(j, "source_tokens", source_tokens);
    cJSON_AddNumberToObject(j, "tokenization_seconds", tokenize_seconds - token_start);
    cJSON_AddNumberToObject(j, "tokenization_calls", tokenize_calls - call_start);
    cJSON_AddNumberToObject(j, "tokenizer_http_calls", tokenizer_http_calls - http_start);
    cJSON_AddNumberToObject(j, "tokenization_processed_tokens", tokenize_tokens - count_start);
    cJSON_AddNumberToObject(j, "query_boundary", (double)boundary);
    cJSON_AddStringToObject(j, "archive_scope", archive.id);
    cJSON_AddBoolToObject(j, "lexical_ok", plan.lexical_ok);
    cJSON_AddBoolToObject(j, "semantic_ok", plan.semantic_ok);
    cJSON_AddNumberToObject(j, "planner_candidates", plan.candidates);
    cJSON_AddNumberToObject(j, "planner_selected_units", plan.count);
    cJSON_AddNumberToObject(j, "planner_selected_briefs", plan.brief_count);
    cJSON_AddNumberToObject(j, "prompt_raw_original_units", plan.count);
    cJSON_AddNumberToObject(j, "prompt_brief_original_pointers", cJSON_GetArraySize(pointers));
    cJSON_AddNumberToObject(j, "offline_brief_provenance_hydrated_bytes", pointer_validation_bytes);
    cJSON_AddStringToObject(j, "brief_pointer_body_dispatch", "not sent; production renderer only");
    cJSON_AddNumberToObject(j, "backlog_sequences", current.pending_sequence - current.ingested_sequence);
    cJSON_AddNumberToObject(j, "retries", 0);
    cJSON_AddItemToObject(j, "admitted", refs);
    cJSON_AddStringToObject(j, "production_reference", plan.reference ? plan.reference : "");
    cJSON_AddStringToObject(j, "final_request", request);
    emit(j);
    free(request); free(stream.text); http_result_free(&result); memory_plan_free(&plan);
    return true; /* Individual failed answers are scored, not hidden by early exit. */
}

static bool workload(const char *database, const char *root, const cJSON *input)
{
    NEED(start_server(&model, root, false));
    NEED(start_server(&embed, root, true));
    tokenizer = (MemoryTokenizer){.server=model, .timeout_ms=10000, .http_calls=&tokenizer_http_calls};
    NEED(memory_tokenizer_fingerprint(&tokenizer, tokenizer_id));
    NEED(store = store_manager_create());
    char why[512], path[1024];
    NEED(store_start(store, database, why, sizeof(why)));
    char active[64]; store_database(store, active, sizeof(active));
    NEED(!strcmp(active, database));
    snprintf(path, sizeof(path), "%s/project", root);
    NEED(mkdir(path, 0700) == 0);
    NEED(store_project_id(store, path, true, project, sizeof(project)));
    NEED(store_mark_imported(path, project));
    char admitted_id[64];
    NEED(store_mode(store, path, admitted_id, sizeof(admitted_id), &db) == STORE_DB && db);
    chat_store_new_id(conversation, sizeof(conversation));
    const cJSON *row;
    int64_t sequence = -1;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(input, "archive")) {
        ChatMessage message = {.role=CHAT_USER, .content=(char *)str(row, "text")};
        NEED(chat_store_append_sequenced(store, path, conversation, "Quality manuscript", &message, &sequence));
    }
    NEED(memory_scope_set(db, project, conversation, true, &archive));
    NEED(vectors = semantic_client_create(store, embed));
    NEED(semantic_ready(vectors));
    NEED(ingest(&archive));
    /* A real indexed other-conversation canary, not prompt-only privacy theater. */
    char private_conversation[40]; chat_store_new_id(private_conversation, sizeof(private_conversation));
    ChatMessage secret = {.role=CHAT_USER, .content=(char *)str(input, "private_canary")};
    int64_t private_seq = -1;
    NEED(chat_store_append_sequenced(store, path, private_conversation, "Out of scope", &secret, &private_seq));
    NEED(memory_scope_set(db, project, private_conversation, true, &private_scope));
    NEED(ingest(&private_scope));
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "event", "archive");
    cJSON_AddStringToObject(meta, "scope", archive.id);
    cJSON_AddStringToObject(meta, "private_scope", private_scope.id);
    cJSON_AddStringToObject(meta, "tokenizer_fingerprint", tokenizer_id);
    cJSON_AddNumberToObject(meta, "schema_version", db_schema_version(db));
    DbRows version = {0};
    if (db_query(db, "SELECT VERSION()", NULL, 0, &version) && version.nrows == 1)
        cJSON_AddStringToObject(meta, "mariadb_runtime_version", db_cell(&version, 0, 0));
    db_rows_free(&version);
    cJSON_AddStringToObject(meta, "qdrant_runtime_version", "excluded: not exposed by production memory client");
    cJSON_AddNumberToObject(meta, "pending_sequence", (double)archive.pending_sequence);
    cJSON_AddNumberToObject(meta, "ingested_sequence", (double)archive.ingested_sequence);
    cJSON_AddNumberToObject(meta, "tokenization_calls", tokenize_calls);
    cJSON_AddNumberToObject(meta, "tokenization_seconds", tokenize_seconds);
    emit(meta);
    const cJSON *limit = cJSON_GetObjectItem(input, "brief_limit");
    NEED(generate_briefs(cJSON_IsNumber(limit) ? (size_t)limit->valuedouble : 0));
    int64_t boundary = sequence - 63;
    NEED(boundary > 0);
    const cJSON *task;
    const char *arms[] = {"recent_window", "retrieval_only", "t1_retrieval", "oracle_source"};
    /* Rotate arm order per task to reduce fixed thermal/cache order bias. No
     * model answers are ever appended; each case is an independent conversation. */
    size_t task_index = 0;
    cJSON_ArrayForEach(task, cJSON_GetObjectItem(input, "tasks")) {
        for (size_t a = 0; a < 4; a++) NEED(run_task(input, task, arms[(a + task_index) % 4], boundary));
        task_index++;
    }
    return true;
}
static bool drop_owned_namespace(const char *database, const MemoryScope *scope)
{
    NEED(scope && !strcmp(scope->database, database));
    char fingerprint[65], path[400], secret_path[1200], key[256], socket[32];
    NEED(memory_vectors_fingerprint(vectors, fingerprint));
    int length = snprintf(path, sizeof(path), "/collections/%s__%s__memory_v2__%s__%llu",
                          database, project, fingerprint, (unsigned long long)scope->generation);
    NEED(length > 0 && length < (int)sizeof(path));
    const char *secrets = getenv("SCRIBODO_SECRETS"), *home = getenv("HOME");
    if (secrets) snprintf(secret_path, sizeof(secret_path), "%s/qdrant_api_key", secrets);
    else snprintf(secret_path, sizeof(secret_path), "%s/Library/Application Support/Scribodo/services/qdrant_api_key", home ? home : "");
    FILE *f = fopen(secret_path, "rb");
    NEED(f);
    size_t n = fread(key, 1, sizeof(key)-1, f);
    bool read_ok = !ferror(f) && feof(f);
    fclose(f); key[n] = 0; key[strcspn(key, "\r\n")] = 0;
    NEED(read_ok && key[0]);
    const char *port = getenv("SCRIBODO_QDRANT_PORT");
    snprintf(socket, sizeof(socket), "tcp:%s", port ? port : "6333");
    HttpResult result = {0};
    bool ok = http_request(socket, "DELETE", path, key, NULL, NULL, NULL, NULL, 15000, &result) &&
              (result.status == 200 || result.status == 404);
    memset(key, 0, sizeof(key));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", "namespace_cleanup");
    cJSON_AddStringToObject(j, "exact_generated_namespace", path);
    cJSON_AddBoolToObject(j, "ok", ok);
    cJSON_AddNumberToObject(j, "http_status", result.status);
    emit(j); http_result_free(&result);
    return ok;
}
int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: memory_quality_eval OWNED_DB OWNED_ROOT INPUT_JSON EVENTS_JSONL\n"); return 2; }
    const char *prefix = "scribodo_test_quality_", *name = argv[1];
    const char *suffix = name + (strlen(name) >= strlen(prefix) ? strlen(prefix) : strlen(name));
    size_t digits = strspn(suffix, "0123456789");
    if (strncmp(name, prefix, strlen(prefix)) || !digits || suffix[digits] != '_' ||
        strlen(suffix + digits + 1) != 16 || strspn(suffix + digits + 1, "0123456789abcdef") != 16) return 2;
    struct stat st;
    if (lstat(argv[2], &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return 2;
    char marker[1200], marked[100] = "";
    snprintf(marker, sizeof(marker), "%s/owned-database", argv[2]);
    FILE *f = fopen(marker, "rb");
    if (!f) return 2;
    size_t n = fread(marked, 1, sizeof(marked)-1, f); fclose(f); marked[n] = 0;
    if (strcmp(marked, name)) return 2;
    events = fopen(argv[4], "ab");
    if (!events) return 2;
    f = fopen(argv[3], "rb");
    if (!f) { fclose(events); return 2; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); rewind(f);
    if (bytes < 0 || bytes > 32 * 1024 * 1024) { fclose(f); fclose(events); return 2; }
    char *text = malloc((size_t)bytes + 1);
    if (!text) { fclose(f); fclose(events); return 2; }
    size_t got = fread(text, 1, (size_t)bytes, f); fclose(f); text[got] = 0;
    cJSON *input = got == (size_t)bytes ? json_parse_complete(text, got) : NULL;
    free(text);
    began = now();
    bool ok = input && !strcmp(str(input, "protocol"), "memory-quality-v2") && workload(name, argv[2], input);
    /* Exact current namespaces in the generated database only; no global reset. */
    char why[512] = "";
    if (vectors && archive.id[0]) ok = memory_vectors_drop(vectors, &archive, why, sizeof(why)) && ok;
    if (vectors && private_scope.id[0]) ok = memory_vectors_drop(vectors, &private_scope, why, sizeof(why)) && ok;
    if (vectors && archive.id[0]) ok = drop_owned_namespace(name, &archive) && ok;
    if (vectors && private_scope.id[0] && private_scope.generation != archive.generation)
        ok = drop_owned_namespace(name, &private_scope) && ok;
    semantic_client_destroy(vectors);
    if (gate) { generative_gate_close(gate); generative_gate_destroy(gate); }
    if (store) { store_thread_done(store); store_manager_destroy(store); }
    db = NULL;
    llm_server_destroy(embed); llm_server_destroy(model);
    cJSON *end = cJSON_CreateObject();
    cJSON_AddStringToObject(end, "event", "complete");
    cJSON_AddBoolToObject(end, "driver_ok", ok);
    cJSON_AddNumberToObject(end, "actual_runtime_seconds", now() - began);
    cJSON_AddNumberToObject(end, "tokenization_calls", tokenize_calls);
    cJSON_AddNumberToObject(end, "tokenizer_http_calls", tokenizer_http_calls);
    cJSON_AddNumberToObject(end, "tokenization_processed_tokens", tokenize_tokens);
    cJSON_AddNumberToObject(end, "tokenization_seconds", tokenize_seconds);
    struct rusage ru;
    if (!getrusage(RUSAGE_SELF, &ru)) {
#ifdef __APPLE__
        cJSON_AddNumberToObject(end, "harness_peak_rss_bytes", (double)ru.ru_maxrss);
#else
        cJSON_AddNumberToObject(end, "harness_peak_rss_bytes", (double)ru.ru_maxrss * 1024);
#endif
    }
    cJSON_AddStringToObject(end, "app_service_rss", "excluded: headless harness; service RSS not sampled");
    emit(end); fclose(events); cJSON_Delete(input);
    return ok ? 0 : 1;
}
