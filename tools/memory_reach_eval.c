/* Selection-only reach driver (MEMORY-015 S2). Real pinned tokenizer, real
 * SQL, real Qdrant and the production planner and admission; NO generation.
 * The driver never sees required sequences: Python owns the answer key and
 * the scoring. Only candidate discovery is observed through the recall ops
 * seam, with the production callbacks called unchanged inside the wrappers.
 * Build via memory_reach_eval.sh; never link into the application. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "app/chat_store.h"
#include "app/memory_documents.h"
#include "app/store_backends.h"
#include "app/memory_store.h"
#include "core/json.h"
#include "llm/context_planner.h"
#include "llm/http.h"
#include "llm/memory_tokens.h"
#include "llm/memory_vectors.h"
#include "llm/semantic.h"
#include "llm/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static StoreManager *store;
static Db *db;
static LlmServer *model, *embed;
static SemanticClient *vectors;
static MemoryTokenizer tokenizer;
static MemoryScope archive, private_scope;
static MemoryDocumentSet attached; /* the conversation's attached saved documents */
static int64_t query_before;
static char project[64], conversation[40], tokenizer_id[65];
static FILE *events;
static double tokenize_seconds, embedding_seconds;
static size_t tokenize_calls, index_calls, tokenizer_http_calls;

typedef struct {
    MemoryCandidate lexical[MEMORY_CANDIDATES], semantic[MEMORY_CANDIDATES];
    size_t nl, ns;
    bool lexical_ok, semantic_ok;
    double lexical_seconds, semantic_seconds;
    char why[256];
} Trace;
static Trace trace;

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
    fprintf(stderr, "memory_reach_eval: line %d: %s; SQL: %s\n", line, what, db ? db_error(db) : "not connected");
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
    long n = kind == 0 ? memory_source_tokens(text, &tokenizer) : memory_reference_tokens(text, &tokenizer);
    tokenize_seconds += now() - t;
    tokenize_calls++;
    if (n < 0) fprintf(stderr, "token count failed: kind=%d bytes=%zu\n", kind, strlen(text));
    return n;
}
static long source_count(const char *s, void *ctx) { (void)ctx; return counted(s, 0); }
static long reference_count(const char *s, void *ctx) { (void)ctx; return counted(s, 1); }

static bool start_server(LlmServer **out, const char *root, bool embedding)
{
    LlmServerConfig cfg;
    if (embedding) llm_server_embed_config(&cfg, "");
    else llm_server_default_config(&cfg, "");
    const char *label = embedding ? "embed" : "chat";
    char cwd[1024];
    NEED(getcwd(cwd, sizeof(cwd)));
    char socket_root[1024];
    snprintf(socket_root, sizeof(socket_root), "%s/build/mr-%s-XXXXXX", cwd, label);
    NEED(mkdtemp(socket_root));
    snprintf(cfg.run_dir, sizeof(cfg.run_dir), "%s", socket_root);
    NEED(strlen(cfg.run_dir) + strlen("/llama.sock") < 104);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s/%s-server.log", root, label);
    snprintf(cfg.cache_path, sizeof(cfg.cache_path), "%s/%s-verified.txt", root, label);
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
    cJSON_AddNumberToObject(j, "slot_context", llm_server_slot_ctx(*out));
    cJSON_AddStringToObject(j, "role", embedding ? "embedding only" : "tokenizer only; no generation requested");
    emit(j);
    return true;
}

static bool ingest(MemoryScope *scope)
{
    char why[512] = "";
    char source_conversation[40];
    snprintf(source_conversation, sizeof(source_conversation), "%s", scope->conversation_id);
    for (;;) {
        bool progress = false;
        bool ingested = memory_ingest_step(db, scope->id, "reach-ingest", source_count, NULL,
                                           tokenizer_id, &progress, why, sizeof(why));
        if (!ingested) fprintf(stderr, "ingest: scope=%s why=%s\n", scope->id, why);
        NEED(ingested);
        if (!progress) break;
    }
    NEED(memory_scope_get(db, project, source_conversation, scope));
    NEED(scope->ingested_sequence == scope->pending_sequence);
    NEED(memory_vectors_create(vectors, scope, why, sizeof(why)));
    for (;;) {
        MemoryIndexLease lease = {0}; bool claimed = false;
        NEED(memory_outbox_claim(db, scope->id, "reach-index", &lease, &claimed));
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

/* ---- recording wrappers around the production callbacks ------------------- */
static bool rec_scope(void *ctx, const char *p, const char *c, MemoryScope *out)
{ (void)ctx; return memory_scope_get(db, p, c, out); }
static bool rec_lexical(void *ctx, const MemoryScope *s, const char *q, size_t limit, MemoryCandidate *out, size_t *n)
{
    (void)ctx;
    double t = now();
    bool ok = memory_lexical_candidates_with_documents_before(db, s, attached.count ? &attached : NULL, q, query_before, limit, out, n);
    trace.lexical_seconds = now() - t;
    trace.lexical_ok = ok;
    trace.nl = ok && *n <= MEMORY_CANDIDATES ? *n : 0;
    if (trace.nl) memcpy(trace.lexical, out, trace.nl * sizeof(*out));
    return ok;
}
static bool rec_semantic(void *ctx, const MemoryScope *s, const char *q, size_t limit, MemoryCandidate *out, size_t *n)
{
    (void)ctx;
    double t = now();
    bool ok = memory_vectors_search_with_documents_before(vectors, s, attached.count ? &attached : NULL, q, query_before, limit, out, n, trace.why, sizeof(trace.why));
    trace.semantic_seconds = now() - t;
    trace.semantic_ok = ok;
    trace.ns = ok && *n <= MEMORY_CANDIDATES ? *n : 0;
    if (trace.ns) memcpy(trace.semantic, out, trace.ns * sizeof(*out));
    return ok;
}
static bool rec_hydrate(void *ctx, const MemoryScope *s, const MemoryCandidate *c, MemoryUnit *out, bool *found)
{ (void)ctx; return memory_candidate_hydrate_with_documents(db, s, attached.count ? &attached : NULL, c, out, found); }
static bool rec_documents_current(void *ctx, const MemoryScope *s, const MemoryDocumentSet *set)
{ (void)ctx; return !set || !set->count || memory_documents_admit(db, s, set); }

static cJSON *candidates_json(const MemoryCandidate *list, size_t n)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "id", list[i].unit_id);
        cJSON_AddNumberToObject(c, "score", list[i].score);
        DbRows rows = {0};
        /* Diagnostic identity lookup for the scorer: which turn a candidate names. */
        if (db_query(db, "SELECT u.sequence, u.source_kind, d.name, u.text FROM memory_units u"
                         " LEFT JOIN documents d ON d.id=u.document_id WHERE u.id=? AND u.scope_id=?",
                     (DbArg[]){ DB_S(list[i].unit_id), DB_S(archive.id) }, 2, &rows) && rows.nrows == 1) {
            int64_t sequence = -1;
            if (db_cell_i64(&rows, 0, 0, &sequence)) cJSON_AddNumberToObject(c, "sequence", (double)sequence);
            else cJSON_AddNullToObject(c, "sequence");
            const char *kind = db_cell(&rows, 0, 1), *name = NULL, *text = NULL;
            if (kind && !strcmp(kind, "document") && db_cell_text(&rows, 0, 2, &name) && db_cell_text(&rows, 0, 3, &text)) {
                cJSON_AddStringToObject(c, "document_name", name);
                cJSON_AddStringToObject(c, "text", text);
            }
        } else cJSON_AddNullToObject(c, "sequence");
        db_rows_free(&rows);
        cJSON_AddItemToArray(array, c);
    }
    return array;
}

static bool run_task(const cJSON *task, int64_t boundary)
{
    query_before = boundary;
    memset(&trace, 0, sizeof(trace));
    double started = now(), token_seconds = tokenize_seconds;
    size_t token_calls = tokenize_calls;
    MemoryRecallOps ops = { NULL, rec_scope, rec_lexical, rec_semantic, rec_hydrate, reference_count, tokenizer_id,
                            attached.count ? &attached : NULL, rec_documents_current };
    MemoryPlan plan = {0};
    bool planned = memory_plan_recall(&ops, project, conversation, str(task, "question"), boundary, false, NULL, &plan);
    double planned_at = now();
    bool admitted = planned && memory_plan_admit_with_documents(db, &plan, attached.count ? &attached : NULL, false);
    double finished = now();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", "selection");
    cJSON_AddStringToObject(j, "task", str(task, "id"));
    cJSON_AddBoolToObject(j, "planned", planned);
    cJSON_AddBoolToObject(j, "admitted", admitted);
    cJSON_AddNumberToObject(j, "readiness", plan.readiness);
    cJSON_AddBoolToObject(j, "lexical_ok", plan.lexical_ok);
    cJSON_AddBoolToObject(j, "semantic_ok", plan.semantic_ok);
    cJSON_AddStringToObject(j, "semantic_why", trace.why);
    cJSON_AddBoolToObject(j, "limited", plan.limited);
    cJSON_AddNumberToObject(j, "planner_candidates", plan.candidates);
    cJSON_AddNumberToObject(j, "planning_seconds", planned_at - started);
    cJSON_AddNumberToObject(j, "admission_seconds", finished - planned_at);
    cJSON_AddNumberToObject(j, "lexical_seconds", trace.lexical_seconds);
    cJSON_AddNumberToObject(j, "semantic_seconds", trace.semantic_seconds);
    cJSON_AddNumberToObject(j, "tokenization_seconds", tokenize_seconds - token_seconds);
    cJSON_AddNumberToObject(j, "tokenization_calls", tokenize_calls - token_calls);
    cJSON_AddNumberToObject(j, "reference_tokens", plan.tokens);
    cJSON_AddNumberToObject(j, "reference_bytes", plan.reference ? strlen(plan.reference) : 0);
    cJSON_AddNumberToObject(j, "query_boundary", (double)boundary);
    cJSON_AddItemToObject(j, "lexical_candidates", candidates_json(trace.lexical, trace.nl));
    cJSON_AddItemToObject(j, "semantic_candidates", candidates_json(trace.semantic, trace.ns));
    cJSON *units = cJSON_AddArrayToObject(j, "admitted_units");
    for (size_t i = 0; i < plan.count; i++) {
        const MemoryUnit *u = &plan.units[i];
        for (size_t part = 0; part < memory_plan_excerpt_count(&plan, i); part++) {
            const char *source;
            size_t source_bytes;
            uint64_t source_offset;
            NEED(memory_plan_excerpt(&plan, i, part, &source, &source_bytes, &source_offset));
            char *excerpt = strndup(source, source_bytes);
            NEED(excerpt);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "id", u->id);
            cJSON_AddNumberToObject(r, "sequence", (double)u->sequence);
            cJSON_AddNumberToObject(r, "byte_start", (double)source_offset);
            cJSON_AddNumberToObject(r, "text_bytes", source_bytes);
            cJSON_AddNumberToObject(r, "canonical_text_bytes", u->text_bytes);
            cJSON_AddNumberToObject(r, "canonical_tokens", u->tokens);
            cJSON_AddStringToObject(r, "role", u->role == CHAT_USER ? "user" : "assistant");
            if (u->kind == MEMORY_SOURCE_DOCUMENT) {
                cJSON_AddStringToObject(r, "document_name", u->document_name);
                cJSON_AddStringToObject(r, "revision_id", u->revision_id);
            }
            cJSON_AddStringToObject(r, "text", excerpt);
            free(excerpt);
            cJSON_AddItemToArray(units, r);
        }
    }
    emit(j);
    memory_plan_free(&plan);
    return true; /* an unplanned task is scored, not hidden */
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
    double ingest_started = now();
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(input, "archive")) {
        ChatMessage message = {.role=!strcmp(str(row, "role"), "assistant") ? CHAT_ASSISTANT : CHAT_USER,
                               .content=(char *)str(row, "text")};
        NEED(chat_store_append_sequenced(store, path, conversation, "Reach manuscript", &message, &sequence));
    }
    NEED(memory_scope_set(db, project, conversation, true, &archive));
    NEED(vectors = semantic_client_create(store, embed));
    NEED(semantic_ready(vectors));
    NEED(ingest(&archive));
    /* Attached saved documents: rows, saved revisions, then scope-local
     * ingestion through the production document path. */
    const cJSON *document;
    memset(&attached, 0, sizeof(attached));
    cJSON_ArrayForEach(document, cJSON_GetObjectItem(input, "documents")) {
        NEED(attached.count < MEMORY_DOCUMENTS_MAX);
        const char *name = str(document, "name"), *text = str(document, "text");
        char document_id[40], revision_id[40];
        NEED(store_document_id_db(db, project, name, true, document_id, sizeof(document_id)));
        store_new_id('r', revision_id, sizeof(revision_id));
        NEED(db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES (?,?,?,?,'save',UNIX_TIMESTAMP(NOW(6)))",
                    (DbArg[]){ DB_S(revision_id), DB_S(document_id), DB_S("reach"), DB_S(text) }, 4, NULL));
        NEED(db_run(db, "UPDATE documents SET saved_revision_id=?, current_revision_id=? WHERE id=?",
                    (DbArg[]){ DB_S(revision_id), DB_S(revision_id), DB_S(document_id) }, 3, NULL));
        DbRows r = {0};
        NEED(db_query(db, "SELECT DATABASE(), r.source_hash, OCTET_LENGTH(r.content) FROM revisions r WHERE r.id=?",
                      (DbArg[]){ DB_S(revision_id) }, 1, &r) && r.nrows == 1);
        MemoryDocumentRef *ref = &attached.refs[attached.count++];
        snprintf(ref->database, sizeof(ref->database), "%s", db_cell(&r, 0, 0));
        snprintf(ref->project_id, sizeof(ref->project_id), "%s", project);
        snprintf(ref->document_id, sizeof(ref->document_id), "%s", document_id);
        snprintf(ref->revision_id, sizeof(ref->revision_id), "%s", revision_id);
        snprintf(ref->source_hash, sizeof(ref->source_hash), "%s", db_cell(&r, 0, 1));
        snprintf(ref->name, sizeof(ref->name), "%s", name);
        NEED(db_cell_u64(&r, 0, 2, &ref->bytes));
        db_rows_free(&r);
    }
    if (attached.count) {
        NEED(memory_scope_get(db, project, conversation, &archive));
        NEED(memory_documents_seed(db, &archive, &attached));
        for (;;) {
            bool progress = false;
            char why2[512] = "";
            NEED(memory_document_ingest_step(db, &archive, &attached, "reach-documents", source_count, NULL,
                                             tokenizer_id, &progress, why2, sizeof(why2)));
            if (!progress) break;
        }
        bool complete = false;
        NEED(memory_documents_complete(db, &archive, &attached, &complete) && complete);
        NEED(ingest(&archive)); /* drains the document units through the index outbox */
    }
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
    cJSON_AddNumberToObject(meta, "pending_sequence", (double)archive.pending_sequence);
    cJSON_AddNumberToObject(meta, "ingested_sequence", (double)archive.ingested_sequence);
    cJSON_AddBoolToObject(meta, "index_pending", archive.index_pending);
    cJSON_AddNumberToObject(meta, "ingest_seconds", now() - ingest_started);
    cJSON_AddNumberToObject(meta, "tokenization_calls", tokenize_calls);
    cJSON_AddNumberToObject(meta, "tokenization_seconds", tokenize_seconds);
    cJSON_AddNumberToObject(meta, "embedding_index_calls", index_calls);
    cJSON_AddNumberToObject(meta, "embedding_index_seconds", embedding_seconds);
    cJSON_AddNumberToObject(meta, "attached_documents", (double)attached.count);
    DbRows units = {0};
    if (db_query(db, "SELECT COUNT(*) FROM memory_units WHERE scope_id=? AND generation=?",
                 (DbArg[]){ DB_S(archive.id), DB_I(archive.generation) }, 2, &units) && units.nrows == 1) {
        int64_t n = 0;
        if (db_cell_i64(&units, 0, 0, &n)) cJSON_AddNumberToObject(meta, "archive_units", (double)n);
    }
    db_rows_free(&units);
    emit(meta);
    const cJSON *b = cJSON_GetObjectItem(input, "boundary");
    int64_t boundary = cJSON_IsNumber(b) ? (int64_t)b->valuedouble : sequence - 63;
    NEED(boundary > 0 && boundary <= sequence);
    const cJSON *task;
    cJSON_ArrayForEach(task, cJSON_GetObjectItem(input, "tasks")) NEED(run_task(task, boundary));
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "event", "complete");
    cJSON_AddBoolToObject(done, "driver_ok", true);
    cJSON_AddNumberToObject(done, "tokenization_calls", tokenize_calls);
    cJSON_AddNumberToObject(done, "tokenization_seconds", tokenize_seconds);
    cJSON_AddNumberToObject(done, "tokenizer_http_calls", tokenizer_http_calls);
    emit(done);
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
    if (argc != 5) { fprintf(stderr, "usage: memory_reach_eval OWNED_DB OWNED_ROOT INPUT_JSON EVENTS_JSONL\n"); return 2; }
    const char *prefix = "scribodo_test_reach_", *name = argv[1];
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
    if (bytes < 0 || bytes > 64L * 1024 * 1024) { fclose(f); fclose(events); return 2; }
    char *text = malloc((size_t)bytes + 1);
    if (!text) { fclose(f); fclose(events); return 2; }
    size_t got = fread(text, 1, (size_t)bytes, f); fclose(f); text[got] = 0;
    cJSON *input = got == (size_t)bytes ? json_parse_complete(text, got) : NULL;
    free(text);
    bool ok = input && !strcmp(str(input, "protocol"), "memory-reach-v1") && workload(name, argv[2], input);
    bool cleaned = true;
    if (vectors) {
        if (archive.id[0]) cleaned = drop_owned_namespace(name, &archive) && cleaned;
        if (private_scope.id[0]) cleaned = drop_owned_namespace(name, &private_scope) && cleaned;
    }
    cJSON_Delete(input);
    semantic_client_destroy(vectors);
    store_manager_destroy(store);
    llm_server_destroy(model);
    llm_server_destroy(embed);
    fclose(events);
    return ok && cleaned ? 0 : 1;
}
