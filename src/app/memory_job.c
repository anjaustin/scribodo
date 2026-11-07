#define _POSIX_C_SOURCE 200809L
#include "app/memory_job.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char project[PATH_MAX], conversation[40];
    StoreProjectIdentity identity;
    MemoryJobAction action;
    uint64_t serial;
    MemoryDocumentName names[MEMORY_DOCUMENTS_MAX];
    size_t document_count;
    uint64_t document_version;
} MemoryRequest;

typedef struct {
    bool repair_pending;
    MemoryScope repair;
    unsigned failures;
    struct timespec retry;
    bool scope_known, documents_captured, documents_seeded;
    MemoryScope scope, document_scope;
    MemoryDocumentSet documents;
    MemoryDocumentName names[MEMORY_DOCUMENTS_MAX];
    size_t document_count;
    uint64_t document_version;
} MemoryWorkerState;

struct MemoryJob {
    StoreManager *manager;
    SemanticClient *vectors;
    MemoryTokenizer tokenizer;
    MemoryJobRunner runner;
    void *context;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t wake;
    bool stopping, selected, paused;
    uint64_t serial;
    MemoryRequest request;
    MemoryRequest controls[MEMORY_JOB_CONTROL_CAPACITY];
    size_t control_head, control_count;
    bool control_attempted, control_invalid;
    char control_why[256];
    MemoryJobStatus status;
};

static void reason(MemoryJobStatus *status, const char *why)
{
    snprintf(status->why, sizeof(status->why), "%s", why);
}

static bool identity_matches(MemoryJob *job, const MemoryRequest *request)
{
    StoreProjectIdentity now;
    return store_project_identity(job->manager, request->project, &now) &&
        now.generation == request->identity.generation &&
        now.device == request->identity.device && now.inode == request->identity.inode &&
        !strcmp(now.project_id, request->identity.project_id);
}

static bool admitted(MemoryJob *job, const MemoryRequest *request)
{
    pthread_mutex_lock(&job->mu);
    bool current = !job->stopping && job->selected && !job->paused && !job->control_count &&
        job->request.serial == request->serial;
    pthread_mutex_unlock(&job->mu);
    return current && identity_matches(job, request);
}

static bool same_projection(const MemoryScope *a, const MemoryScope *b)
{
    return !strcmp(a->id, b->id) && !strcmp(a->database, b->database) &&
        a->generation == b->generation && a->history_epoch == b->history_epoch &&
        a->policy_epoch == b->policy_epoch;
}

static bool same_generation(const MemoryScope *a, const MemoryScope *b)
{
    return !strcmp(a->id, b->id) && !strcmp(a->database, b->database) &&
        a->generation == b->generation;
}

static bool fresh_scope(MemoryJob *job, const MemoryRequest *request, Db *db,
                         MemoryJobStatus *status)
{
    MemoryScope fresh = {0};
    if (!admitted(job, request)) return false;
    if (!memory_scope_get(db, request->identity.project_id, request->conversation, &fresh)) {
        status->scope = fresh;
        status->enabled = false;
        reason(status, "Memory disabled or SQL scope unavailable; no automatic consent.");
        return false;
    }
    status->scope = fresh;
    status->enabled = fresh.enabled;
    if (!fresh.enabled) reason(status, "Memory disabled.");
    return fresh.enabled;
}

static void backend_failure(MemoryWorkerState *state, MemoryJobStatus *status, const char *detail)
{
    state->failures++;
    clock_gettime(CLOCK_MONOTONIC, &state->retry);
    state->retry.tv_sec += 1u << (state->failures - 1);
    snprintf(status->why, sizeof(status->why), "Partial coverage: index failure %u/3%s: %.160s",
             state->failures, state->failures >= 3 ? "; explicit rebuild to retry" : "; backing off",
             detail[0] ? detail : "backend unavailable");
}

static bool same_names(const MemoryDocumentName *a, const MemoryDocumentName *b, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (a[i].dirty != b[i].dirty || strcmp(a[i].name, b[i].name)) return false;
    return true;
}

static bool prepare_documents(MemoryJob *job, const MemoryRequest *request, Db *db,
                              MemoryWorkerState *state, MemoryJobStatus *status)
{
    bool changed = !state->documents_captured ||
        !same_generation(&state->document_scope, &status->scope) ||
        state->document_version != request->document_version ||
        state->document_count != request->document_count ||
        !same_names(state->names, request->names, request->document_count);
    if (changed) {
        /* Discard first: a failed capture must never retain old authorization. */
        state->documents = (MemoryDocumentSet){0};
        state->documents_seeded = false;
        state->documents_captured = false;
        state->document_scope = status->scope;
        state->document_version = request->document_version;
        state->document_count = request->document_count;
        memcpy(state->names, request->names, sizeof(state->names));
        if (!admitted(job, request)) return false;
        MemoryDocumentSet captured = {0};
        if (memory_documents_capture(job->manager, request->project, &request->identity,
                                      request->names, request->document_count, false, NULL, &captured))
            state->documents = captured;
        state->documents_captured = true;
    }
    status->document_sources = state->documents.count;
    status->documents_pending = state->documents.count != 0;
    if (!admitted(job, request)) return false;
    if (!state->documents_seeded) {
        if (!memory_documents_seed(db, &status->scope, &state->documents)) {
            reason(status, "Partial coverage: document seeding unavailable.");
            return true; /* Do not make chat durability depend on document work. */
        }
        state->documents_seeded = true;
    }
    return true;
}

static bool production_step(MemoryJob *job, const MemoryRequest *request,
                            MemoryWorkerState *state, MemoryJobStatus *status)
{
    char project_id[64] = {0}, why[256] = {0};
    Db *db = NULL;
    if (!admitted(job, request)) return false;
    if (store_mode(job->manager, request->project, project_id, sizeof(project_id), &db) != STORE_DB || !db) {
        reason(status, "Memory paused: SQL project unavailable; no file-history fallback.");
        return false;
    }
    if (strcmp(project_id, request->identity.project_id) || !admitted(job, request)) {
        reason(status, "Memory paused: project/store identity changed; select again.");
        return false;
    }
    if (!fresh_scope(job, request, db, status)) return false;

    if (state->scope_known && !same_generation(&state->scope, &status->scope)) {
        state->failures = 0;
        state->retry = (struct timespec){0};
        state->repair_pending = false;
    }
    state->scope_known = true;
    state->scope = status->scope;

    if (job->tokenizer.server && llm_server_state(job->tokenizer.server, NULL, 0) != LLM_READY) {
        reason(status, "Partial coverage: local tokenizer is not ready; no attempt consumed.");
        return false;
    }

    bool progress = false;
    bool index_ready = false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool retry_due = now.tv_sec > state->retry.tv_sec ||
        (now.tv_sec == state->retry.tv_sec && now.tv_nsec >= state->retry.tv_nsec);
    if (state->failures < 3 && retry_due) {
        bool exists = false;
        bool repairing = state->repair_pending && same_projection(&state->repair, &status->scope);
        if (repairing || memory_vectors_exists(job->vectors, &status->scope, &exists, why, sizeof(why))) {
            if (!exists) {
                /* Re-seed SQL BEFORE creating the replacement collection. A failed
                 * create retries this generation, not another endless rebuild. */
                if (!repairing) {
                    if (!admitted(job, request) ||
                        !memory_scope_rebuild(db, project_id, request->conversation, &status->scope)) {
                        reason(status, "Partial coverage: SQL collection-loss reseed failed.");
                        return false;
                    }
                    state->repair = status->scope;
                    state->repair_pending = true;
                }
                if (!fresh_scope(job, request, db, status)) return false;
                if (!same_projection(&state->repair, &status->scope)) return false;
                if (memory_vectors_create(job->vectors, &status->scope, why, sizeof(why))) {
                    state->repair_pending = false;
                    index_ready = true;
                } else backend_failure(state, status, why);
            } else index_ready = true;
        } else backend_failure(state, status, why);
    }

    /* SQL ingestion remains available during index outages. Never make author
     * message durability depend on embeddings or Qdrant. */
    if (!fresh_scope(job, request, db, status)) return false;
    state->scope = status->scope;
    MemoryScope pinned = status->scope;
    if (!prepare_documents(job, request, db, state, status)) return false;
    char fingerprint[65];
    if (!memory_tokenizer_fingerprint(&job->tokenizer, fingerprint)) {
        reason(status, "Partial coverage: local tokenizer identity unavailable.");
        return false;
    }
    if (!fresh_scope(job, request, db, status) || !same_projection(&pinned, &status->scope))
        return false;
    if (!memory_ingest_step(db, status->scope.id, "memory-worker", memory_source_tokens,
                            &job->tokenizer, fingerprint, &progress, why, sizeof(why))) {
        snprintf(status->why, sizeof(status->why), "Partial coverage: ingestion failed: %.190s", why);
        return false;
    }
    if (progress) status->ingested_steps++;
    bool document_progress = false;
    if (!fresh_scope(job, request, db, status) || !same_projection(&pinned, &status->scope))
        return progress;
    pinned = status->scope; /* the first chat claim may have just pinned the tokenizer */
    if (state->documents_seeded && state->documents.count) {
        if (!memory_documents_admit(db, &pinned, &state->documents)) {
            /* A saved receipt changed without a new UI snapshot. Wait for the
             * next binding version, rather than recapturing files every tick. */
            state->documents = (MemoryDocumentSet){0};
            status->document_sources = 0;
            status->documents_pending = false;
            reason(status, "Partial coverage: document binding changed; waiting for refresh.");
        } else if (admitted(job, request) &&
                   !memory_document_ingest_step(db, &pinned, &state->documents, "memory-worker",
                       memory_source_tokens, &job->tokenizer, fingerprint, &document_progress, why, sizeof(why))) {
            snprintf(status->why, sizeof(status->why), "Partial coverage: document ingestion failed: %.175s", why);
        }
    }
    if (document_progress) status->document_steps++;
    progress = progress || document_progress;
    if (state->documents.count) {
        if (!fresh_scope(job, request, db, status) || !same_projection(&pinned, &status->scope))
            return progress;
        bool complete = false;
        if (state->documents_seeded &&
            memory_documents_complete(db, &pinned, &state->documents, &complete))
            status->documents_pending = !complete;
        else status->documents_pending = true;
    }
    bool embedding_wait = index_ready && !semantic_embedding_ready(job->vectors);
    if (embedding_wait) {
        index_ready = false;
        reason(status, "Partial coverage: embedding model is not ready; no index attempt consumed.");
    }
    if (index_ready && fresh_scope(job, request, db, status) && same_projection(&pinned, &status->scope)) {
        MemoryIndexLease lease = {0};
        bool claimed = false;
        if (!memory_outbox_claim(db, status->scope.id, "memory-worker", &lease, &claimed)) {
            reason(status, "Partial coverage: unable to claim index work.");
        } else if (claimed) {
            bool delivered = false;
            bool attempted = fresh_scope(job, request, db, status) &&
                same_projection(&lease.scope, &status->scope);
            if (attempted)
                delivered = memory_vectors_upsert(job->vectors, &lease, why, sizeof(why));
            bool acknowledged = memory_outbox_ack(db, &lease, delivered);
            if (delivered && acknowledged) {
                status->indexed++;
                progress = true;
                reason(status, "Partial coverage: indexing in progress.");
            } else if (!attempted)
                reason(status, "Partial coverage: scope changed or paused; index lease deferred.");
            else if (!delivered) backend_failure(state, status, why);
            else reason(status, "Partial coverage: stale/expired index acknowledgment; not counted.");
        } else reason(status, lease.maintenance ? "Partial coverage: retiring stale derived index jobs."
                                               : "Partial coverage / idle: no claimable work (not proof of completeness).");
        progress = progress || lease.maintenance;
        memory_index_lease_free(&lease);
    }
    if (!index_ready && !state->failures && !embedding_wait)
        reason(status, "Partial coverage: index unavailable; SQL ingestion only.");
    return progress;
}

static bool production_control(MemoryJob *job, const MemoryRequest *request,
                                MemoryJobStatus *status, bool *invalid)
{
    char project_id[64] = {0};
    Db *db = NULL;
    if (!identity_matches(job, request)) {
        *invalid = true;
        reason(status, "Consent control pending: project/store identity changed; explicit close/discard required.");
        return false;
    }
    if (store_mode(job->manager, request->project, project_id, sizeof(project_id), &db) != STORE_DB || !db) {
        reason(status, "Consent control pending: waiting for SQL.");
        return false;
    }
    if (strcmp(project_id, request->identity.project_id) || !identity_matches(job, request)) {
        *invalid = true;
        reason(status, "Consent control pending: project/store identity changed; explicit close/discard required.");
        return false;
    }
    bool ok;
    if (request->action == MEMORY_JOB_REBUILD)
        ok = memory_scope_rebuild(db, project_id, request->conversation, &status->scope);
    else
        ok = memory_scope_set(db, project_id, request->conversation,
                              request->action == MEMORY_JOB_ENABLE, &status->scope);
    if (!ok) reason(status, "Consent control pending: SQL failed; retrying in one second.");
    else {
        status->active = true;
        status->enabled = status->scope.enabled;
        reason(status, status->enabled ? "Memory enabled; coverage remains partial while processing is paused/warming." : "Memory disabled.");
    }
    return ok;
}

static struct timespec after_ms(long ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    return until;
}

static bool time_due(struct timespec deadline, struct timespec now)
{
    return deadline.tv_sec < now.tv_sec ||
        (deadline.tv_sec == now.tv_sec && deadline.tv_nsec <= now.tv_nsec);
}

static void *memory_main(void *arg)
{
    MemoryJob *job = arg;
    uint64_t serial = 0;
    MemoryWorkerState state = {0};
    MemoryJobStatus status = {0};
    MemoryRequest previous = {0};
    struct timespec navigation_due = {0}, control_due = {0};
    pthread_mutex_lock(&job->mu);
    for (;;) {
        if (job->stopping) break;
        struct timespec now = after_ms(0);
        if (job->control_count) {
            /* Retain the head (including in-flight work) until SQL commits.
             * Navigation, clear and pause cannot drop or bypass this FIFO. */
            if (!time_due(control_due, now)) {
                pthread_cond_timedwait(&job->wake, &job->mu, &control_due);
                continue;
            }
            MemoryRequest control = job->controls[job->control_head];
            bool first = !job->control_attempted;
            bool invalid = job->control_invalid;
            job->control_attempted = true;
            pthread_mutex_unlock(&job->mu);
            MemoryJobStatus result = {0};
            bool committed = false;
            if (invalid || !identity_matches(job, &control)) {
                invalid = true;
                reason(&result, "Consent control pending: project/store identity changed; explicit close/discard required.");
            } else if (job->runner) {
                committed = job->runner(job->manager, control.project, control.conversation,
                    control.action, first, &result, job->context);
            } else committed = production_control(job, &control, &result, &invalid);
            pthread_mutex_lock(&job->mu);
            job->control_invalid = invalid;
            if (committed) {
                job->control_head = (job->control_head + 1) % MEMORY_JOB_CONTROL_CAPACITY;
                job->control_count--;
                job->control_attempted = false;
                job->control_invalid = false;
                job->control_why[0] = 0;
                control_due = (struct timespec){0};
                /* Re-read SQL on the selected scope, never publish control A's
                 * scope/status as navigation B. Rebuild also resets index retry. */
                if (job->selected && !strcmp(job->request.project, control.project) &&
                    !strcmp(job->request.conversation, control.conversation)) {
                    if (result.scope.id[0] && job->request.identity.generation == control.identity.generation &&
                        job->request.identity.device == control.identity.device &&
                        job->request.identity.inode == control.identity.inode &&
                        !strcmp(job->request.identity.project_id, control.identity.project_id)) {
                         result.ingested_steps = job->status.ingested_steps;
                         result.indexed = job->status.indexed;
                         result.document_steps = job->status.document_steps;
                         result.document_sources = job->status.document_sources;
                         result.documents_pending = job->status.documents_pending;
                        job->status = result; /* committed consent is visible even while resource work is paused */
                    }
                    serial = 0;
                    if (control.action == MEMORY_JOB_REBUILD) state = (MemoryWorkerState){0};
                }
                navigation_due = (struct timespec){0};
            } else {
                snprintf(job->control_why, sizeof(job->control_why), "%s",
                         result.why[0] ? result.why : "Consent control pending: SQL failed; retrying in one second.");
                control_due = after_ms(1000);
            }
            continue;
        }
        if (!job->selected || job->paused) {
            pthread_cond_wait(&job->wake, &job->mu);
            continue;
        }
        if (job->request.serial == serial && !time_due(navigation_due, now)) {
            /* Spurious/unrelated wakeups preserve the original pacing deadline. */
            pthread_cond_timedwait(&job->wake, &job->mu, &navigation_due);
            continue;
        }
        MemoryRequest request = job->request;
        bool first = request.serial != serial;
        if (first) {
            serial = request.serial;
            bool same = !strcmp(previous.project, request.project) &&
                !strcmp(previous.conversation, request.conversation) &&
                previous.identity.generation == request.identity.generation &&
                previous.identity.device == request.identity.device &&
                previous.identity.inode == request.identity.inode &&
                !strcmp(previous.identity.project_id, request.identity.project_id);
            if (!same) state = (MemoryWorkerState){0};
            previous = request;
            status = (MemoryJobStatus){.active = true};
        }
        pthread_mutex_unlock(&job->mu);
        bool progress = false;
        if (!identity_matches(job, &request)) {
            status.enabled = false;
            reason(&status, "Memory paused: project/store identity changed; select again.");
        } else if (admitted(job, &request)) {
            progress = job->runner ? job->runner(job->manager, request.project, request.conversation,
                request.action, first, &status, job->context) :
                production_step(job, &request, &state, &status);
        }
        bool identity_ok = identity_matches(job, &request);
        pthread_mutex_lock(&job->mu);
        if (job->stopping) break;
        if (!job->selected || job->request.serial != serial) continue;
        if (!identity_ok) {
            status.enabled = false;
            reason(&status, "Memory paused: project/store identity changed; select again.");
        }
        job->status = status;
        navigation_due = after_ms(progress ? 10 : 1000);
    }
    pthread_mutex_unlock(&job->mu);
    store_thread_done(job->manager);
    return NULL;
}

static MemoryJob *create_job(StoreManager *manager, SemanticClient *vectors, LlmServer *tokenizer,
                             MemoryJobRunner runner, void *context)
{
    if (!manager || (!vectors && !runner)) return NULL;
    MemoryJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->manager = manager;
    job->vectors = vectors;
    job->tokenizer.server = tokenizer;
    job->runner = runner;
    job->context = context;
    if (pthread_mutex_init(&job->mu, NULL) != 0) { free(job); return NULL; }
    if (pthread_cond_init(&job->wake, NULL) != 0) {
        pthread_mutex_destroy(&job->mu);
        free(job);
        return NULL;
    }
    if (pthread_create(&job->thread, NULL, memory_main, job) != 0) {
        pthread_cond_destroy(&job->wake);
        pthread_mutex_destroy(&job->mu);
        free(job);
        return NULL;
    }
    return job;
}

MemoryJob *memory_job_create(StoreManager *manager, SemanticClient *vectors, LlmServer *tokenizer)
{
    return create_job(manager, vectors, tokenizer, NULL, NULL);
}

MemoryJob *memory_job_create_with_runner(StoreManager *manager, MemoryJobRunner runner, void *context)
{
    if (!runner) return NULL;
    return create_job(manager, NULL, NULL, runner, context);
}

bool memory_job_select(MemoryJob *job, const char *project, const char *conversation, MemoryJobAction action)
{
    if (!job || !project || !conversation || action < MEMORY_JOB_RESUME || action > MEMORY_JOB_REBUILD)
        return false;
    size_t plen = strnlen(project, PATH_MAX), clen = strnlen(conversation, 40);
    if (!plen || plen >= PATH_MAX || !clen || clen >= 40) return false;
    for (size_t i = 0; i < plen; i++)
        if ((unsigned char)project[i] < 32 || project[i] == 127) return false;
    for (size_t i = 0; i < clen; i++) {
        unsigned char c = (unsigned char)conversation[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    MemoryRequest request = {.action = action};
    memcpy(request.project, project, plen + 1);
    memcpy(request.conversation, conversation, clen + 1);
    if (!store_project_identity(job->manager, project, &request.identity)) return false;
    pthread_mutex_lock(&job->mu);
    if (job->stopping || job->serial == UINT64_MAX ||
        (action != MEMORY_JOB_RESUME && job->control_count == MEMORY_JOB_CONTROL_CAPACITY)) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    request.serial = ++job->serial;
    if (action != MEMORY_JOB_RESUME) {
        size_t tail = (job->control_head + job->control_count) % MEMORY_JOB_CONTROL_CAPACITY;
        job->controls[tail] = request;
        job->control_count++;
    }
    request.action = MEMORY_JOB_RESUME;
    if (job->selected && !strcmp(job->request.project, project) &&
        !strcmp(job->request.conversation, conversation)) {
        memcpy(request.names, job->request.names, sizeof(request.names));
        request.document_count = job->request.document_count;
        request.document_version = job->request.document_version;
    }
    job->request = request;
    job->selected = true;
    job->status = (MemoryJobStatus){.active = true};
    reason(&job->status, "Memory selection pending; consent not yet resolved.");
    pthread_cond_signal(&job->wake);
    pthread_mutex_unlock(&job->mu);
    return true;
}

bool memory_job_documents(MemoryJob *job, const char *project, const char *conversation,
                          const MemoryDocumentName *names, size_t count, uint64_t version)
{
    if (!job || !project || !conversation || count > MEMORY_DOCUMENTS_MAX || (count && !names))
        return false;
    for (size_t i = 0; i < count; i++)
        if (!names[i].name[0] || strnlen(names[i].name, sizeof(names[i].name)) == sizeof(names[i].name))
            return false;
    pthread_mutex_lock(&job->mu);
    if (job->stopping || !job->selected || strcmp(job->request.project, project) ||
        strcmp(job->request.conversation, conversation)) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    if (job->request.document_version == version && job->request.document_count == count &&
        same_names(job->request.names, names, count)) {
        pthread_mutex_unlock(&job->mu);
        return true;
    }
    if (job->serial == UINT64_MAX) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    memset(job->request.names, 0, sizeof(job->request.names));
    for (size_t i = 0; i < count; i++) {
        snprintf(job->request.names[i].name, sizeof(job->request.names[i].name), "%s", names[i].name);
        job->request.names[i].dirty = names[i].dirty;
    }
    job->request.document_count = count;
    job->request.document_version = version;
    job->request.serial = ++job->serial;
    job->status.document_sources = 0;
    job->status.documents_pending = count != 0;
    pthread_cond_signal(&job->wake);
    pthread_mutex_unlock(&job->mu);
    return true;
}

void memory_job_clear(MemoryJob *job)
{
    if (!job) return;
    pthread_mutex_lock(&job->mu);
    job->selected = false;
    job->request = (MemoryRequest){0};
    job->status = (MemoryJobStatus){0};
    pthread_cond_signal(&job->wake);
    pthread_mutex_unlock(&job->mu);
}

void memory_job_pause(MemoryJob *job, bool paused)
{
    if (!job) return;
    pthread_mutex_lock(&job->mu);
    if (job->paused != paused) {
        job->paused = paused;
        pthread_cond_signal(&job->wake);
    }
    pthread_mutex_unlock(&job->mu);
}

bool memory_job_status(MemoryJob *job, MemoryJobStatus *out)
{
    if (!job || !out) return false;
    pthread_mutex_lock(&job->mu);
    *out = job->status;
    out->controls_pending = job->control_count != 0;
    if (out->controls_pending)
        reason(out, job->control_why[0] ? job->control_why : "Consent controls pending SQL commit.");
    pthread_mutex_unlock(&job->mu);
    return true;
}

void memory_job_destroy(MemoryJob *job)
{
    if (!job) return;
    pthread_mutex_lock(&job->mu);
    job->stopping = true;
    job->selected = false;
    pthread_cond_signal(&job->wake);
    pthread_mutex_unlock(&job->mu);
    pthread_join(job->thread, NULL);
    pthread_cond_destroy(&job->wake);
    pthread_mutex_destroy(&job->mu);
    free(job);
}
