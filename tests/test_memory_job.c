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

struct StoreManager { int unused; };
struct Db { int unused; };
static StoreManager manager;
static Db database;
static atomic_uint cleanups;
static atomic_uint identity_generation = 1;
static MemoryScope sql_scope;
static unsigned sets, rebuilds, ingests, claims, upserts, acks, creates, exists_calls;
static bool collection_exists, backend_ok, ack_ok, claim_work;
static bool tokenizer_ready = true, embed_ready = true;
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
    *progress = true;
    return true;
}

bool memory_outbox_claim(Db *db, const char *scope, const char *owner, MemoryIndexLease *out, bool *claimed)
{
    assert(db == &database && scope && owner);
    claims++;
    *claimed = claim_work;
    out->scope = sql_scope;
    return true;
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

    backend_ok = false;
    for (unsigned i = 0; i < 3; i++) {
        state.retry = (struct timespec){0};
        assert(production_step(&job, &request, &state, &status));
        assert(state.failures == i + 1);
    }
    unsigned before = exists_calls;
    assert(production_step(&job, &request, &state, &status));
    assert(exists_calls == before && ingests == 6);
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
    assert(!production_step(&job, &request, &state, &status));
    assert(ingests == cold_ingests && claims == cold_claims && state.failures == 0);
    tokenizer_ready = true; embed_ready = false;
    assert(production_step(&job, &request, &state, &status));
    assert(ingests == cold_ingests + 1 && claims == cold_claims && state.failures == 0);
    embed_ready = true;
    assert(production_step(&job, &request, &state, &status));
    assert(claims == cold_claims + 1 && state.failures == 0);
    job.tokenizer.server = NULL;
    request.action = MEMORY_JOB_DISABLE;
    unsigned prior_ingests = ingests;
    assert(production_control(&job, &request, &status, &invalid) && !invalid);
    assert(status.active && !status.enabled);
    assert(!production_step(&job, &request, &state, &status));
    assert(!status.enabled && ingests == prior_ingests && sets == 2);
    pthread_mutex_destroy(&job.mu);
}

static void test_unchanged_pause_keeps_idle_deadline(void)
{
    Gate g;
    gate_init(&g);
    g.release = true;
    MemoryJob *job = memory_job_create_with_runner(&manager, runner, &g);
    assert(memory_job_select(job, "/p", "idle", MEMORY_JOB_RESUME));
    wait_status(job, "idle");
    for (unsigned i = 0; i < 100; i++) {
        memory_job_pause(job, false);
        struct timespec delay = {0, 2000000};
        nanosleep(&delay, NULL);
    }
    memory_job_destroy(job);
    assert(g.calls <= 2);
    gate_done(&g);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool hold, entered, release;
    unsigned failures, attempts, commits, resources;
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
        assert(first == (g->attempts == 0 || g->commits != 0));
        g->attempts++;
        g->entered = true;
        pthread_cond_broadcast(&g->cv);
        struct timespec until = deadline();
        while (g->hold && !g->release)
            assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
        if (g->failures) g->failures--;
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
    pthread_mutex_lock(&job->mu);
    assert(!strcmp(job->request.conversation, "B"));
    assert(job->control_count == MEMORY_JOB_CONTROL_CAPACITY);
    pthread_mutex_unlock(&job->mu);
    memory_job_clear(job);
    assert(memory_job_status(job, &status) && !status.active && status.controls_pending);
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
    control_release(&g);
    /* Wait for the failed head to enter retry backoff. */
    for (unsigned i = 0; i < 5000; i++) {
        memory_job_status(job, &status);
        if (strstr(status.why, "retrying")) break;
        assert(i != 4999);
        poll_pause();
    }
    for (unsigned i = 0; i < 100; i++) {
        assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
        memory_job_pause(job, true);
        struct timespec delay = {0, 2000000};
        nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&g.mu);
    assert(g.attempts == 1 && !g.commits && !g.resources);
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
    MemoryJob *job = memory_job_create(&manager, (SemanticClient *)&manager, NULL);
    assert(job);
    memory_job_pause(job, true);
    assert(memory_job_select(job, "/p", "A", MEMORY_JOB_DISABLE));
    assert(memory_job_select(job, "/p", "B", MEMORY_JOB_RESUME));
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
    puts("memory_job: all tests passed");
    return 0;
}
