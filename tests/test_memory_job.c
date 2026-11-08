#define _DARWIN_C_SOURCE
/* Headless lifecycle and bounded-iteration tests. No SQL/model/service needed.
 * Compile: cc -std=c11 -Wall -Wextra -Werror -pthread -Isrc -Isrc/llm
 *          tests/test_memory_job.c -o /path/to/test_memory_job */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../src/app/memory_job.c"
#include "core/deadline_clock.h"

struct StoreManager { int unused; };
struct Db { int unused; };
static StoreManager manager;
static Db database;
static atomic_uint cleanups;
static atomic_uint identity_generation = 1;
static MemoryScope sql_scope;
static unsigned sets, rebuilds, ingests, claims, upserts, acks, creates, exists_calls;
static bool collection_exists, backend_ok, ack_ok, claim_work, unit_ok = true;
static unsigned unit_checks;
static unsigned project_gc_calls;
static size_t project_gc_rows;
double memory_budget_now_ms(void)
{
    struct timespec now; clock_gettime(SCRIBODO_DEADLINE_CLOCK, &now);
    return (double)now.tv_sec * 1000 + (double)now.tv_nsec / 1e6;
}
void memory_document_cache_invalidate(MemoryDocumentCache *cache) { (void)cache; }
MemoryDocumentCache *memory_document_cache_create(void) { return (MemoryDocumentCache *)calloc(1, 1); }
void memory_document_cache_destroy(MemoryDocumentCache *cache) { free(cache); }
bool memory_documents_capture_cached(MemoryDocumentCache *cache, StoreManager *store, const char *path,
    const StoreProjectIdentity *project, const MemoryDocumentName *names, size_t count, bool remote,
    const atomic_bool *cancel, MemoryDocumentSet *out)
{
    (void)cache; return memory_documents_capture(store, path, project, names, count, remote, cancel, out);
}
bool memory_documents_verify_step(MemoryDocumentCache *cache, StoreManager *store, const char *path,
    const StoreProjectIdentity *project, const MemoryDocumentName *names, size_t count, bool remote,
    const atomic_bool *cancel, bool *progress, bool *pending)
{
    (void)cache; (void)store; (void)path; (void)project; (void)names; (void)count; (void)remote; (void)cancel;
    *progress = false; *pending = false; return true;
}
bool memory_documents_capture_cached_on_db(MemoryDocumentCache *cache, Db *db, StoreManager *store, const char *path,
    const StoreProjectIdentity *project, const MemoryDocumentName *names, size_t count, bool remote,
    const atomic_bool *cancel, double deadline, MemoryDocumentSet *out)
{ (void)db; (void)deadline; return memory_documents_capture_cached(cache, store, path, project, names, count, remote, cancel, out); }
bool memory_documents_verify_step_on_db(MemoryDocumentCache *cache, Db *db, StoreManager *store, const char *path,
    const StoreProjectIdentity *project, const MemoryDocumentName *names, size_t count, bool remote,
    const atomic_bool *cancel, double deadline, bool *progress, bool *pending)
{ (void)db; (void)deadline; return memory_documents_verify_step(cache, store, path, project, names, count, remote, cancel, progress, pending); }
bool memory_gc_step(Db *db, const MemoryScope *scope, size_t limit, MemoryGcStats *stats)
{ (void)db; (void)scope; assert(limit <= 64); *stats = (MemoryGcStats){0}; return true; }
bool memory_gc_project_step(Db *db, const char *database_name, const char *project_name, size_t limit,
    MemoryGcStats *stats, MemoryScope *scope)
{
    (void)db; (void)database_name; (void)project_name; assert(limit <= 64);
    project_gc_calls++;
    *stats = (MemoryGcStats){ .total = project_gc_rows };
    project_gc_rows = 0;
    *scope = (MemoryScope){0}; return true;
}
void store_database(StoreManager *store, char *out, size_t size)
{ (void)store; snprintf(out, size, "origin_database"); }
bool memory_gc_namespace_enqueue(Db *db, const MemoryScope *scope, const char *fingerprint, int version)
{ (void)db; (void)scope; (void)fingerprint; assert(version == 2); return true; }
bool memory_gc_namespace_claim(Db *db, const MemoryScope *scope, const char *owner, MemoryNamespaceLease *lease, bool *claimed)
{ (void)db; (void)scope; (void)owner; *lease = (MemoryNamespaceLease){0}; *claimed = false; return true; }
bool memory_gc_namespace_ack(Db *db, const MemoryNamespaceLease *lease, bool delivered)
{ (void)db; (void)lease; (void)delivered; return true; }
bool memory_gc_namespace_claim_project(Db *db, const char *database_name, const char *project_name,
                                       const char *owner, MemoryNamespaceLease *lease, bool *claimed)
{ (void)database_name; (void)project_name; return memory_gc_namespace_claim(db, NULL, owner, lease, claimed); }
bool memory_vectors_fingerprint(SemanticClient *client, char out[65])
{ (void)client; memset(out, 'a', 64); out[64] = 0; return true; }
bool memory_vectors_retire_namespace(SemanticClient *client, const MemoryNamespaceLease *lease, char *why, size_t size)
{ (void)client; (void)lease; (void)why; (void)size; return true; }
bool memory_vectors_retire_namespace_on_db(SemanticClient *client, Db *db, const MemoryNamespaceLease *lease, char *why, size_t size)
{ (void)db; return memory_vectors_retire_namespace(client, lease, why, size); }
bool memory_vectors_create_on_db(SemanticClient *client, Db *db, const MemoryScope *scope, char *why, size_t size)
{ (void)db; return memory_vectors_create(client, scope, why, size); }
SemanticClient *semantic_client_bounded(SemanticClient *client, atomic_bool *cancel, double deadline)
{ (void)cancel; (void)deadline; return client; }
void semantic_client_destroy(SemanticClient *client) { (void)client; }
bool generative_gate_fence_valid(GenerativeGate *gate, uint64_t fence) { return gate && fence; }
bool memory_brief_claim(Db *db, const MemoryScope *scope, const char *owner, MemoryBriefLease *out, bool *claimed)
{ (void)db; (void)scope; (void)owner; *out = (MemoryBriefLease){0}; *claimed = false; return true; }
bool memory_brief_generate(LlmServer *server, GenerativeGate *gate, const MemoryBriefLease *lease,
    const atomic_bool *cancel, MemoryBriefResult *result)
{ (void)server; (void)gate; (void)lease; (void)cancel; *result = (MemoryBriefResult){ .canceled = true }; return false; }
bool memory_brief_publish(Db *db, const MemoryBriefLease *lease, const char *json, size_t tokens,
    const char *model, const char *protocol, MemoryBriefAdmission admit, void *context, bool *published)
{ (void)db; (void)lease; (void)json; (void)tokens; (void)model; (void)protocol; (void)admit; (void)context; *published = false; return true; }
bool memory_brief_fail(Db *db, const MemoryBriefLease *lease, bool retryable, bool canceled)
{ (void)db; (void)lease; (void)retryable; (void)canceled; return true; }
void memory_brief_lease_free(MemoryBriefLease *lease) { *lease = (MemoryBriefLease){0}; }
static bool tokenizer_ready = true, embed_ready = true;
static bool ingestion_ok = true;
static unsigned captures, document_seeds, document_ingests, document_admissions;
static bool capture_ok = true, document_admit_ok = true;
static bool document_complete, document_complete_ok = true;
static bool document_progress_available = true;
static unsigned document_completions;
static char ingested_document[256];

bool memory_documents_capture(StoreManager *store, const char *project,
                               const StoreProjectIdentity *identity, const MemoryDocumentName *names,
                               size_t count, bool remote, const atomic_bool *cancel, MemoryDocumentSet *out)
{
    assert(store == &manager && project && identity && !remote && !cancel);
    captures++;
    *out = (MemoryDocumentSet){0};
    for (size_t i = 0; i < count; i++) {
        if (names[i].dirty) continue;
        snprintf(out->refs[out->count++].name, sizeof(out->refs[0].name), "%s", names[i].name);
    }
    return capture_ok;
}

bool memory_documents_seed(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents)
{
    assert(db == &database && scope && documents);
    document_seeds++;
    return true;
}

bool memory_documents_admit(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents)
{
    assert(db == &database && scope->generation == sql_scope.generation && documents->count);
    document_admissions++;
    return document_admit_ok;
}

bool memory_documents_complete(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                bool *complete)
{
    assert(db == &database && scope->generation == sql_scope.generation && documents->count);
    document_completions++;
    *complete = document_complete;
    return document_complete_ok;
}

bool memory_document_ingest_step(Db *db, const MemoryScope *scope, const MemoryDocumentSet *documents,
                                  const char *owner, MemoryTokenCount count, void *context,
                                  const char *fingerprint, bool *progress, char *why, size_t len)
{
    assert(db == &database && scope->generation == sql_scope.generation && documents->count);
    assert(owner && count == memory_source_tokens && context && fingerprint[0] == 'a');
    (void)why; (void)len;
    document_ingests++;
    snprintf(ingested_document, sizeof(ingested_document), "%s", documents->refs[0].name);
    *progress = document_progress_available;
    return true;
}
LlmState llm_server_state(LlmServer *server, char *message, size_t length)
{
    (void)server; (void)message; (void)length;
    return tokenizer_ready ? LLM_READY : LLM_STARTING;
}
bool semantic_embedding_ready(SemanticClient *client) { return client && embed_ready; }

bool store_project_identity(StoreManager *m, const char *path, StoreProjectIdentity *out)
{
    assert(m == &manager);
    if (!strcmp(path, "/invalid")) return false;
    *out = (StoreProjectIdentity){.generation = atomic_load(&identity_generation),
        .device = 2, .inode = 3};
    strcpy(out->project_id, "project");
    return true;
}

void store_thread_done(StoreManager *m)
{
    assert(m == &manager);
    atomic_fetch_add(&cleanups, 1);
}

StoreMode store_mode(StoreManager *m, const char *p, char *id, size_t len, Db **db)
{
    assert(m == &manager && p);
    snprintf(id, len, "project");
    *db = &database;
    return STORE_DB;
}

bool memory_scope_get(Db *db, const char *p, const char *c, MemoryScope *out)
{
    assert(db == &database && !strcmp(p, "project") && c);
    *out = sql_scope;
    return sql_scope.id[0] != 0;
}

bool memory_scope_set(Db *db, const char *p, const char *c, bool enabled, MemoryScope *out)
{
    assert(db == &database && p && c);
    sets++;
    strcpy(sql_scope.id, "scope");
    strcpy(sql_scope.database, "origin_database");
    snprintf(sql_scope.project_id, sizeof(sql_scope.project_id), "%s", p);
    snprintf(sql_scope.conversation_id, sizeof(sql_scope.conversation_id), "%s", c);
    sql_scope.enabled = enabled;
    sql_scope.policy_epoch++;
    *out = sql_scope;
    return true;
}

bool memory_scope_rebuild(Db *db, const char *p, const char *c, MemoryScope *out)
{
    assert(db == &database && p && c);
    rebuilds++;
    if (sql_scope.enabled) sql_scope.generation++;
    *out = sql_scope;
    return true;
}

bool memory_vectors_exists(SemanticClient *client, const MemoryScope *scope, bool *exists,
                            char *why, size_t len)
{
    assert(client && !strcmp(scope->database, "origin_database"));
    exists_calls++;
    *exists = collection_exists;
    if (!backend_ok) snprintf(why, len, "offline");
    return backend_ok;
}

bool memory_vectors_create(SemanticClient *client, const MemoryScope *scope, char *why, size_t len)
{
    assert(client && scope->generation == sql_scope.generation && rebuilds);
    creates++;
    if (!backend_ok) snprintf(why, len, "offline");
    if (backend_ok) collection_exists = true;
    return backend_ok;
}

bool memory_tokenizer_fingerprint(MemoryTokenizer *tokenizer, char out[65])
{
    assert(tokenizer);
    memset(out, 'a', 64);
    out[64] = 0;
    return true;
}

long memory_source_tokens(const char *text, void *context)
{
    assert(context);
    return (long)strlen(text);
}

bool memory_ingest_step(Db *db, const char *scope, const char *owner, MemoryTokenCount count,
                         void *context, const char *fingerprint, bool *progress, char *why, size_t len)
{
    assert(db == &database && scope && owner && count == memory_source_tokens && context && fingerprint);
    (void)why; (void)len;
    ingests++;
    *progress = ingestion_ok;
    return ingestion_ok;
}

bool memory_outbox_claim(Db *db, const char *scope, const char *owner, MemoryIndexLease *out, bool *claimed)
{
    assert(db == &database && scope && owner);
    claims++;
    *claimed = claim_work;
    out->scope = sql_scope;
    return true;
}

bool memory_vectors_unit_valid(const MemoryIndexLease *lease, char *why, size_t len)
{
    assert(lease);
    unit_checks++;
    if (!unit_ok && why && len) snprintf(why, len, "Invalid, oversized, or noncanonical memory unit");
    return unit_ok;
}

bool memory_vectors_upsert(SemanticClient *client, const MemoryIndexLease *lease, char *why, size_t len)
{
    assert(client && lease);
    (void)why; (void)len;
    upserts++;
    return backend_ok;
}

bool memory_outbox_ack(Db *db, const MemoryIndexLease *lease, bool delivered)
{
    assert(db == &database && lease);
    (void)delivered;
    acks++;
    return ack_ok;
}

void memory_index_lease_free(MemoryIndexLease *lease)
{
    *lease = (MemoryIndexLease){0};
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool release, entered;
    bool check_idle_spacing;
    struct timespec previous_call;
    unsigned calls;
    char project[PATH_MAX], conversation[40];
    MemoryJobAction action;
    pthread_t thread;
} Gate;

static struct timespec deadline(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    ts.tv_sec += 5;
    return ts;
}

static void poll_pause(void)
{
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}

static bool runner(StoreManager *m, const char *p, const char *c, MemoryJobAction action,
                    bool first, MemoryJobStatus *status, void *context)
{
    assert(m == &manager);
    Gate *g = context;
    pthread_mutex_lock(&g->mu);
    if (!g->calls) g->thread = pthread_self();
    assert(pthread_equal(g->thread, pthread_self()));
    if (g->check_idle_spacing) {
        struct timespec now;
        assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
        if (g->calls && !first) {
            struct timespec due = g->previous_call;
            due.tv_sec++;
            assert(now.tv_sec > due.tv_sec || (now.tv_sec == due.tv_sec && now.tv_nsec >= due.tv_nsec));
        }
        g->previous_call = now;
    }
    g->calls++;
    g->entered = true;
    pthread_cond_broadcast(&g->cv);
    struct timespec until = deadline();
    while (!g->release) assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
    strcpy(g->project, p);
    strcpy(g->conversation, c);
    g->action = action;
    if (first) status->ingested_steps = 0;
    status->ingested_steps++;
    strcpy(status->scope.conversation_id, c);
    status->enabled = true;
    pthread_mutex_unlock(&g->mu);
    return false;
}

static void gate_init(Gate *g)
{
    memset(g, 0, sizeof(*g));
    assert(pthread_mutex_init(&g->mu, NULL) == 0);
    assert(pthread_cond_init(&g->cv, NULL) == 0);
}

static void wait_entered(Gate *g)
{
    pthread_mutex_lock(&g->mu);
    struct timespec until = deadline();
    while (!g->entered) assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
    pthread_mutex_unlock(&g->mu);
}

static void release_gate(Gate *g)
{
    pthread_mutex_lock(&g->mu);
    g->release = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static MemoryJobStatus wait_status(MemoryJob *job, const char *conversation)
{
    MemoryJobStatus status;
    for (unsigned i = 0; i < 5000; i++) {
        assert(memory_job_status(job, &status));
        if (!strcmp(status.scope.conversation_id, conversation)) return status;
        poll_pause();
    }
    assert(!"status timed out");
    return status;
}

static void gate_done(Gate *g)
{
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
}

static void test_latest_and_pause(void)
{
    Gate g;
    gate_init(&g);
    MemoryJob *job = memory_job_create_with_runner(&manager, runner, &g);
    assert(job && memory_job_select(job, "/original", "A", MEMORY_JOB_RESUME));
    wait_entered(&g);
    memory_job_pause(job, true);
    for (unsigned i = 0; i < 10000; i++)
        assert(memory_job_select(job, "/pending", "B", MEMORY_JOB_RESUME));
    char project[] = "/copied", conversation[] = "C";
    assert(memory_job_select(job, project, conversation, MEMORY_JOB_RESUME));
    strcpy(project, "/mutate");
    conversation[0] = 'X';
    release_gate(&g);
    /* Wait for A to finish while C remains paused. */
    pthread_mutex_lock(&job->mu);
    assert(job->paused);
    pthread_mutex_unlock(&job->mu);
    MemoryJobStatus status;
    assert(memory_job_status(job, &status) && !status.enabled && !status.scope.id[0]);
    memory_job_pause(job, false);
    status = wait_status(job, "C");
    assert(status.ingested_steps == 1);
    memory_job_destroy(job);
    assert(g.calls == 2 && !strcmp(g.project, "/copied") && !strcmp(g.conversation, "C"));
    assert(g.action == MEMORY_JOB_RESUME);
    gate_done(&g);
}

static void *destroy_worker(void *arg)
{
    memory_job_destroy(arg);
    return NULL;
}

static void test_clear_and_shutdown(void)
{
    Gate g;
    gate_init(&g);
    unsigned before = atomic_load(&cleanups);
    MemoryJob *job = memory_job_create_with_runner(&manager, runner, &g);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_RESUME));
    wait_entered(&g);
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    memory_job_clear(job);
    MemoryJobStatus status;
    assert(memory_job_status(job, &status) && !status.active && !status.enabled);
    pthread_t destroyer;
    assert(pthread_create(&destroyer, NULL, destroy_worker, job) == 0);
    for (;;) {
        pthread_mutex_lock(&job->mu);
        bool stopping = job->stopping;
        pthread_mutex_unlock(&job->mu);
        if (stopping) break;
        poll_pause();
    }
    assert(atomic_load(&cleanups) == before);
    release_gate(&g);
    assert(pthread_join(destroyer, NULL) == 0);
    assert(atomic_load(&cleanups) == before + 1 && g.calls == 1);
    gate_done(&g);
}

static void test_validation_and_identity(void)
{
    Gate g;
    gate_init(&g);
    assert(!memory_job_create(NULL, NULL, NULL));
    assert(!memory_job_create(&manager, NULL, NULL));
    assert(!memory_job_create_with_runner(&manager, NULL, NULL));
    MemoryJob *job = memory_job_create_with_runner(&manager, runner, &g);
    assert(!memory_job_select(job, NULL, "A", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/p", NULL, MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/invalid", "A", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "", "A", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/p\n", "A", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/p", "../A", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/p", "A", (MemoryJobAction)99));
    char path[PATH_MAX + 1], id[41];
    memset(path, 'p', sizeof(path)); path[PATH_MAX] = 0;
    memset(id, 'c', sizeof(id)); id[40] = 0;
    assert(!memory_job_select(job, path, "A", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/p", id, MEMORY_JOB_RESUME));
    assert(!memory_job_status(job, NULL));
    assert(!memory_job_status(NULL, &(MemoryJobStatus){0}));
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_RESUME));
    wait_entered(&g);
    atomic_fetch_add(&identity_generation, 1);
    release_gate(&g);
    for (unsigned i = 0; i < 5000; i++) {
        MemoryJobStatus status;
        memory_job_status(job, &status);
        if (strstr(status.why, "identity changed")) {
            assert(!status.enabled);
            break;
        }
        assert(i != 4999);
        poll_pause();
    }
    memory_job_destroy(job);
    assert(g.calls == 1);
    gate_done(&g);
    memory_job_clear(NULL);
    memory_job_pause(NULL, true);
    memory_job_destroy(NULL);
}

static void test_bounded_production(void)
{
    /* Invoke one production iteration directly to control clock/backoff and
     * assert exact call counts without racing a live worker. */
    MemoryJob job = {.manager = &manager, .selected = true};
    job.vectors = (SemanticClient *)&job;
    assert(pthread_mutex_init(&job.mu, NULL) == 0);
    MemoryRequest request = {.action = MEMORY_JOB_RESUME, .serial = 1};
    strcpy(request.project, "/p");
    strcpy(request.conversation, "A");
    store_project_identity(&manager, "/p", &request.identity);
    job.request = request;
    MemoryWorkerState state = {0};
    MemoryJobStatus status = {.active = true};
    sql_scope = (MemoryScope){0};
    assert(!production_step(&job, &request, &state, &status));
    assert(!sets && !rebuilds && !ingests && !status.enabled);
    assert(project_gc_calls > 0); /* Default-off selection still services project cleanup. */

    state = (MemoryWorkerState){0};
    request.action = MEMORY_JOB_ENABLE;
    backend_ok = true;
    ack_ok = false; /* Delivered but expired/stale acknowledgment is partial. */
    claim_work = true;
    bool invalid = false;
    assert(production_control(&job, &request, &status, &invalid) && !invalid);
    assert(status.active && status.enabled);
    assert(production_step(&job, &request, &state, &status));
    assert(sets == 1 && rebuilds == 1 && creates == 1 && ingests == 1);
    assert(claims == 1 && upserts == 1 && acks == 1 && status.indexed == 0);
    assert(strstr(status.why, "acknowledgment"));
    ack_ok = true;
    assert(production_step(&job, &request, &state, &status));
    assert(sets == 1 && rebuilds == 1 && creates == 1 && ingests == 2);
    assert(status.indexed == 1);

    /* A malformed unit is the unit's failure: acknowledged as not delivered
     * (the outbox backs it off and fails it after three attempts), counted
     * as rejected, never as a backend failure, and the drain goes on. */
    unit_ok = false;
    unsigned upserts_before = upserts, acks_before = acks;
    for (unsigned i = 0; i < 5; i++) assert(production_step(&job, &request, &state, &status));
    assert(state.failures == 0 && status.rejected == 5 && upserts == upserts_before && acks == acks_before + 5);
    assert(strstr(status.why, "rejected by the index"));
    unit_ok = true;
    unsigned ingests_after_rejections = ingests;

    backend_ok = false;
    for (unsigned i = 0; i < 3; i++) {
        state.retry = (struct timespec){0};
        assert(production_step(&job, &request, &state, &status));
        assert(state.failures == i + 1);
    }
    unsigned before = exists_calls;
    assert(production_step(&job, &request, &state, &status));
    assert(exists_calls == before && ingests == ingests_after_rejections + 4);
    assert(strstr(status.why, "explicit rebuild"));

    /* A pending create must not trigger another generation bump on retry. */
    state = (MemoryWorkerState){.repair_pending = true, .repair = sql_scope};
    collection_exists = false;
    unsigned prior_rebuilds = rebuilds;
    assert(production_step(&job, &request, &state, &status));
    state.retry = (struct timespec){0};
    assert(production_step(&job, &request, &state, &status));
    assert(rebuilds == prior_rebuilds);

    backend_ok = true;
    state = (MemoryWorkerState){0};
    collection_exists = true;
    job.tokenizer.server = (LlmServer *)&job;
    tokenizer_ready = false;
    unsigned cold_ingests = ingests, cold_claims = claims;
    unsigned cold_gc = project_gc_calls;
    state.next_maintenance_ms = 0;
    project_gc_rows = 64;
    assert(production_step(&job, &request, &state, &status));
    assert(project_gc_calls == cold_gc + 1 && status.gc_rows >= 64);
    assert(ingests == cold_ingests && claims == cold_claims && state.failures == 0);
    tokenizer_ready = true; embed_ready = false;
    assert(production_step(&job, &request, &state, &status));
    assert(ingests == cold_ingests + 1 && claims == cold_claims && state.failures == 0);
    embed_ready = true;
    assert(production_step(&job, &request, &state, &status));
    assert(claims == cold_claims + 1 && state.failures == 0);
    ingestion_ok = false;
    state.next_maintenance_ms = 0;
    project_gc_rows = 64;
    unsigned failed_gc = project_gc_calls;
    assert(production_step(&job, &request, &state, &status));
    assert(project_gc_calls == failed_gc + 1 && !project_gc_rows);
    ingestion_ok = true;
    job.tokenizer.server = NULL;
    request.action = MEMORY_JOB_DISABLE;
    unsigned prior_ingests = ingests;
    assert(production_control(&job, &request, &status, &invalid) && !invalid);
    assert(status.active && !status.enabled);
    unsigned prior_gc = project_gc_calls;
    state.next_maintenance_ms = 0;
    project_gc_rows = 64;
    assert(production_step(&job, &request, &state, &status));
    assert(project_gc_calls == prior_gc + 1 && status.gc_rows >= 64);
    assert(!status.enabled && ingests == prior_ingests && sets == 2);
    pthread_mutex_destroy(&job.mu);
}

static void test_unchanged_pause_keeps_idle_deadline(void)
{
    Gate g;
    gate_init(&g);
    g.release = true;
    g.check_idle_spacing = true;
    MemoryJob *job = memory_job_create_with_runner(&manager, runner, &g);
    assert(memory_job_select(job, "/p", "idle", MEMORY_JOB_RESUME));
    MemoryDocumentName name = {.name = "idle.txt"};
    assert(memory_job_documents(job, "/p", "idle", &name, 1, 1));
    wait_status(job, "idle");
    pthread_mutex_lock(&job->mu);
    uint64_t serial = job->request.serial;
    pthread_mutex_unlock(&job->mu);
    for (unsigned i = 0; i < 100; i++) {
        memory_job_pause(job, false);
        assert(memory_job_documents(job, "/p", "idle", &name, 1, 1));
        struct timespec delay = {0, 2000000};
        nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&job->mu);
    assert(job->request.serial == serial);
    pthread_mutex_unlock(&job->mu);
    memory_job_destroy(job);
    /* Hosted TSan can stretch the sleeps across multiple legitimate one-second
     * ticks. The runner checks spacing, not a scheduler-dependent total count. */
    assert(g.calls >= 1);
    gate_done(&g);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool hold, entered, release;
    unsigned failures, attempts, commits, resources;
    bool retry_waiting;
    struct timespec retry_due;
    char conversations[MEMORY_JOB_CONTROL_CAPACITY][40];
    MemoryJobAction actions[MEMORY_JOB_CONTROL_CAPACITY];
} ControlGate;

static bool control_runner(StoreManager *m, const char *p, const char *c, MemoryJobAction action,
                            bool first, MemoryJobStatus *status, void *context)
{
    assert(m == &manager && p);
    ControlGate *g = context;
    pthread_mutex_lock(&g->mu);
    bool committed = false;
    if (action == MEMORY_JOB_RESUME) {
        g->resources++;
        strcpy(status->scope.conversation_id, c);
    } else {
        if (g->retry_waiting) {
            struct timespec now;
            assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
            assert(now.tv_sec > g->retry_due.tv_sec ||
                   (now.tv_sec == g->retry_due.tv_sec && now.tv_nsec >= g->retry_due.tv_nsec));
            g->retry_waiting = false;
        }
        assert(first == (g->attempts == 0 || g->commits != 0));
        g->attempts++;
        g->entered = true;
        pthread_cond_broadcast(&g->cv);
        struct timespec until = deadline();
        while (g->hold && !g->release)
            assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
        if (g->failures) {
            g->failures--;
            assert(clock_gettime(CLOCK_REALTIME, &g->retry_due) == 0);
            g->retry_due.tv_sec++;
            g->retry_waiting = true;
        }
        else {
            assert(g->commits < MEMORY_JOB_CONTROL_CAPACITY);
            strcpy(g->conversations[g->commits], c);
            g->actions[g->commits++] = action;
            strcpy(status->scope.conversation_id, c); /* Must not bind to NAV. */
            committed = true;
        }
    }
    pthread_mutex_unlock(&g->mu);
    return committed;
}

static void control_gate_init(ControlGate *g)
{
    memset(g, 0, sizeof(*g));
    assert(pthread_mutex_init(&g->mu, NULL) == 0);
    assert(pthread_cond_init(&g->cv, NULL) == 0);
    g->hold = true;
}

static void control_wait_entered(ControlGate *g)
{
    pthread_mutex_lock(&g->mu);
    struct timespec until = deadline();
    while (!g->entered) assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
    pthread_mutex_unlock(&g->mu);
}

static void control_release(ControlGate *g)
{
    pthread_mutex_lock(&g->mu);
    g->release = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static void test_consent_fifo_while_paused(void)
{
    ControlGate g;
    control_gate_init(&g);
    g.failures = 1;
    MemoryJob *job = memory_job_create_with_runner(&manager, control_runner, &g);
    memory_job_pause(job, true);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_DISABLE));
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    MemoryJobStatus status;
    assert(memory_job_status(job, &status) && status.controls_pending);
    control_wait_entered(&g); /* Consent runs even though resource work is paused. */
    const char *names[] = {"A", "C", "D", "E", "F", "G", "H", "I"};
    for (unsigned i = 1; i < MEMORY_JOB_CONTROL_CAPACITY; i++)
        assert(memory_job_select(job, "/p", names[i], i % 2 ? MEMORY_JOB_ENABLE : MEMORY_JOB_REBUILD));
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    assert(!memory_job_select(job, "/p", "rejected", MEMORY_JOB_DISABLE));
    MemoryDocumentName document = {.name = "bound.txt"};
    for (uint64_t version = 1; version < 100; version++)
        assert(memory_job_documents(job, "/p", "B", &document, 1, version));
    pthread_mutex_lock(&job->mu);
    assert(!strcmp(job->request.conversation, "B"));
    assert(job->control_count == MEMORY_JOB_CONTROL_CAPACITY);
    assert(job->request.document_count == 1 && job->request.document_version == 99);
    pthread_mutex_unlock(&job->mu);
    memory_job_clear(job);
    assert(!memory_job_documents(job, "/p", "B", &document, 1, 100));
    assert(memory_job_status(job, &status) && !status.active && status.controls_pending);
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    control_release(&g);
    /* Wakeups must preserve the retry deadline. A loaded host may legitimately
     * take over a second to execute this loop; check admission time in the
     * callback, not an assumed upper bound on scheduler/sleep latency. */
    for (unsigned i = 0; i < 100; i++) {
        assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
        memory_job_pause(job, true);
        struct timespec delay = {0, 2000000};
        nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&g.mu);
    assert(g.attempts >= 1 && !g.resources);
    pthread_mutex_unlock(&g.mu);
    for (unsigned i = 0; i < 5000; i++) {
        memory_job_status(job, &status);
        if (!status.controls_pending) break;
        assert(i != 4999);
        poll_pause();
    }
    assert(!status.scope.conversation_id[0]); /* A's result never labels B. */
    pthread_mutex_lock(&g.mu);
    assert(g.attempts == MEMORY_JOB_CONTROL_CAPACITY + 1 && g.commits == MEMORY_JOB_CONTROL_CAPACITY);
    assert(!g.resources);
    for (unsigned i = 0; i < MEMORY_JOB_CONTROL_CAPACITY; i++) {
        assert(!strcmp(g.conversations[i], names[i]));
        assert(g.actions[i] == (i == 0 ? MEMORY_JOB_DISABLE : i % 2 ? MEMORY_JOB_ENABLE : MEMORY_JOB_REBUILD));
    }
    pthread_mutex_unlock(&g.mu);
    memory_job_pause(job, false);
    wait_status(job, "B");
    memory_job_destroy(job);
    pthread_cond_destroy(&g.cv);
    pthread_mutex_destroy(&g.mu);
}

static void test_changed_control_identity_and_destroy(void)
{
    ControlGate g;
    control_gate_init(&g);
    g.failures = 1;
    unsigned before = atomic_load(&cleanups);
    MemoryJob *job = memory_job_create_with_runner(&manager, control_runner, &g);
    memory_job_pause(job, true);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_DISABLE));
    control_wait_entered(&g);
    unsigned original_generation = atomic_load(&identity_generation);
    atomic_fetch_add(&identity_generation, 1);
    memory_job_clear(job);
    control_release(&g);
    for (unsigned i = 0; i < 5000; i++) {
        MemoryJobStatus status;
        memory_job_status(job, &status);
        assert(status.controls_pending && !status.active);
        if (strstr(status.why, "identity changed")) break;
        assert(i != 4999);
        poll_pause();
    }
    /* Once a control observes a mismatch it stays fail-closed, even if a
     * filesystem/test double later restores the earlier identity. */
    atomic_store(&identity_generation, original_generation);
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    struct timespec retry_delay = {1, 100000000};
    nanosleep(&retry_delay, NULL);
    MemoryJobStatus pending;
    memory_job_status(job, &pending);
    assert(pending.controls_pending && strstr(pending.why, "identity changed"));
    struct timespec start = after_ms(0);
    memory_job_destroy(job); /* Explicit discard: no forever join waiting on SQL. */
    struct timespec finish = after_ms(0);
    assert(finish.tv_sec - start.tv_sec <= 1);
    assert(atomic_load(&cleanups) == before + 1);
    assert(g.attempts == 1 && !g.commits && !g.resources);
    pthread_cond_destroy(&g.cv);
    pthread_mutex_destroy(&g.mu);
}

static void test_production_control_does_not_run_resources(void)
{
    unsigned prior_sets = sets, prior_ingests = ingests, prior_claims = claims;
    unsigned prior_exists = exists_calls, prior_upserts = upserts, prior_creates = creates;
    unsigned prior_captures = captures, prior_documents = document_ingests;
    MemoryJob *job = memory_job_create(&manager, (SemanticClient *)&manager, NULL);
    assert(job);
    memory_job_pause(job, true);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_DISABLE));
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    MemoryDocumentName document = {.name = "paused.txt"};
    assert(memory_job_documents(job, "/p", "B", &document, 1, 1));
    for (unsigned i = 0; i < 5000; i++) {
        MemoryJobStatus status;
        memory_job_status(job, &status);
        if (!status.controls_pending) {
            assert(!status.scope.conversation_id[0]);
            break;
        }
        assert(i != 4999);
        poll_pause();
    }
    memory_job_destroy(job);
    assert(sets == prior_sets + 1);
    assert(ingests == prior_ingests && claims == prior_claims);
    assert(exists_calls == prior_exists && upserts == prior_upserts && creates == prior_creates);
    assert(captures == prior_captures && document_ingests == prior_documents);
}

static void test_document_binding_iterations(void)
{
    MemoryJob job = {.manager = &manager, .selected = true, .serial = 1};
    job.vectors = (SemanticClient *)&job;
    assert(pthread_mutex_init(&job.mu, NULL) == 0);
    assert(pthread_cond_init(&job.wake, NULL) == 0);
    MemoryRequest request = {.action = MEMORY_JOB_RESUME, .serial = 1};
    strcpy(request.project, "/p");
    strcpy(request.conversation, "A");
    store_project_identity(&manager, "/p", &request.identity);
    job.request = request;
    MemoryWorkerState state = {0};
    MemoryJobStatus status = {.active = true};
    sql_scope.enabled = true;
    backend_ok = true; collection_exists = true; claim_work = false;
    MemoryDocumentName names[] = {{.name = "A.txt"}, {.name = "B.txt"}};
    assert(!memory_job_documents(NULL, "/p", "A", names, 1, 1));
    assert(!memory_job_documents(&job, "/wrong", "A", names, 1, 1));
    assert(!memory_job_documents(&job, "/p", "wrong", names, 1, 1));
    assert(!memory_job_documents(&job, "/p", "A", NULL, 1, 1));
    assert(!memory_job_documents(&job, "/p", "A", names, 9, 1));
    assert(memory_job_documents(&job, "/p", "A", names, 1, 1));
    names[0].name[0] = 'X'; /* Snapshot owns its names. */
    request = job.request;
    unsigned before = captures, seeded = document_seeds, steps = document_ingests;
    unsigned chat_before = ingests, claims_before = claims;
    assert(production_step(&job, &request, &state, &status));
    assert(captures == before + 1 && document_seeds == seeded + 1);
    assert(document_ingests == steps + 1 && !strcmp(ingested_document, "A.txt"));
    assert(status.document_sources == 1 && status.document_steps == 1);
    assert(status.documents_pending);
    assert(ingests == chat_before + 1 && claims == claims_before + 1);
    for (unsigned i = 0; i < 5; i++) assert(production_step(&job, &request, &state, &status));
    assert(captures == before + 1 && document_seeds == seeded + 1);
    assert(status.documents_pending); /* Progress is not proof of completion. */
    document_progress_available = false;
    assert(production_step(&job, &request, &state, &status));
    assert(status.documents_pending); /* Leased/failed work may make no progress. */
    document_complete = true;
    assert(production_step(&job, &request, &state, &status));
    assert(!status.documents_pending && status.document_sources == 1);
    document_complete_ok = false;
    assert(production_step(&job, &request, &state, &status));
    assert(status.documents_pending); /* An unavailable SQL check fails closed. */
    document_complete_ok = true; document_complete = false;
    document_progress_available = true;
    assert(memory_job_select(&job, "/p", "A", MEMORY_JOB_ENABLE));
    assert(job.request.document_count == 1 && job.request.document_version == 1);
    assert(job.controls[0].document_count == 0); /* NAV never rewrites consent. */
    job.control_count = 0; /* Direct iteration seam; no worker owns this job. */

    /* Detaching stops retained SQL receipts from being continued next tick. */
    assert(memory_job_documents(&job, "/p", "A", NULL, 0, 2));
    request = job.request; steps = document_ingests;
    unsigned completions_before = document_completions;
    assert(production_step(&job, &request, &state, &status));
    assert(!state.documents.count && !status.document_sources && document_ingests == steps);
    assert(!status.documents_pending && document_completions == completions_before);
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 3));
    request = job.request;
    assert(production_step(&job, &request, &state, &status));
    assert(!strcmp(ingested_document, "B.txt"));
    names[1].dirty = true;
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 4));
    request = job.request; steps = document_ingests;
    assert(production_step(&job, &request, &state, &status));
    assert(!state.documents.count && document_ingests == steps);

    names[1].dirty = false;
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 5));
    request = job.request;
    assert(production_step(&job, &request, &state, &status));
    capture_ok = false;
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 6));
    request = job.request; steps = document_ingests;
    assert(production_step(&job, &request, &state, &status));
    assert(!state.documents.count && document_ingests == steps);
    capture_ok = true;
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 7));
    request = job.request;
    assert(production_step(&job, &request, &state, &status));
    document_admit_ok = false; before = captures; steps = document_ingests;
    assert(production_step(&job, &request, &state, &status));
    assert(!state.documents.count && document_ingests == steps);
    document_admit_ok = true;
    assert(production_step(&job, &request, &state, &status));
    assert(captures == before && document_ingests == steps);

    state.failures = 3; /* Binding refresh must not restart backend retries. */
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 8));
    request = job.request;
    assert(production_step(&job, &request, &state, &status));
    assert(state.failures == 3 && document_ingests == steps + 1);
    before = captures;
    sql_scope.generation++;
    assert(production_step(&job, &request, &state, &status));
    assert(captures == before + 1 && state.failures == 0);
    job.paused = true; before = captures;
    assert(memory_job_documents(&job, "/p", "A", names + 1, 1, 9));
    request = job.request;
    assert(!production_step(&job, &request, &state, &status));
    assert(captures == before);
    assert(memory_job_select(&job, "/p", "B", MEMORY_JOB_RESUME));
    assert(!job.request.document_count && !job.request.document_version);
    memory_job_clear(&job);
    assert(!job.request.document_count && !job.request.document_version);
    pthread_cond_destroy(&job.wake);
    pthread_mutex_destroy(&job.mu);
}

static void test_destroy_owns_inflight_control(void)
{
    ControlGate g;
    control_gate_init(&g);
    g.failures = 1;
    unsigned before = atomic_load(&cleanups);
    MemoryJob *job = memory_job_create_with_runner(&manager, control_runner, &g);
    memory_job_pause(job, true);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_DISABLE));
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_ENABLE));
    control_wait_entered(&g);
    MemoryJobStatus status;
    assert(memory_job_status(job, &status) && status.controls_pending);
    pthread_t destroyer;
    assert(pthread_create(&destroyer, NULL, destroy_worker, job) == 0);
    for (;;) {
        pthread_mutex_lock(&job->mu);
        bool stopping = job->stopping;
        pthread_mutex_unlock(&job->mu);
        if (stopping) break;
        poll_pause();
    }
    assert(atomic_load(&cleanups) == before);
    control_release(&g);
    assert(pthread_join(destroyer, NULL) == 0);
    assert(atomic_load(&cleanups) == before + 1);
    assert(g.attempts == 1 && !g.commits && !g.resources);
    pthread_cond_destroy(&g.cv);
    pthread_mutex_destroy(&g.mu);
}

static void test_paused_committed_consent_snapshot(void)
{
    unsigned before_ingests = ingests, before_claims = claims;
    MemoryJob *job = memory_job_create(&manager, (SemanticClient *)&manager, NULL);
    assert(job); memory_job_pause(job, true);
    for (unsigned action = 0; action < 2; action++) {
        assert(memory_job_select(job, "/p", "A", action ? MEMORY_JOB_DISABLE : MEMORY_JOB_ENABLE));
        for (unsigned i = 0; i < 5000; i++) {
            MemoryJobStatus status; assert(memory_job_status(job, &status));
            if (!status.controls_pending) {
                assert(status.active && status.enabled == !action && !strcmp(status.scope.conversation_id, "A"));
                break;
            }
            assert(i != 4999); poll_pause();
        }
    }
    memory_job_destroy(job);
    assert(ingests == before_ingests && claims == before_claims);
}

static void test_disabled_rebuild_fifo_drains(void)
{
    unsigned before_ingests = ingests, before_claims = claims, before_sets = sets;
    sql_scope.enabled = true;
    uint64_t generation = sql_scope.generation;
    MemoryJob *job = memory_job_create(&manager, (SemanticClient *)&manager, NULL);
    assert(job); memory_job_pause(job, true);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_DISABLE));
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_REBUILD));
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_ENABLE));
    for (unsigned i = 0; i < 5000; i++) {
        MemoryJobStatus status; assert(memory_job_status(job, &status));
        if (!status.controls_pending) {
            assert(status.active && status.enabled && !strcmp(status.scope.conversation_id, "A"));
            break;
        }
        assert(i != 4999); poll_pause();
    }
    memory_job_destroy(job);
    assert(sets == before_sets + 2 && sql_scope.generation == generation);
    assert(ingests == before_ingests && claims == before_claims);
}

int main(void)
{
    test_unchanged_pause_keeps_idle_deadline();
    test_consent_fifo_while_paused();
    test_changed_control_identity_and_destroy();
    test_destroy_owns_inflight_control();
    test_latest_and_pause();
    test_clear_and_shutdown();
    test_validation_and_identity();
    test_bounded_production();
    test_production_control_does_not_run_resources();
    test_paused_committed_consent_snapshot();
    test_disabled_rebuild_fifo_drains();
    test_document_binding_iterations();
    puts("memory_job: all tests passed");
    return 0;
}
