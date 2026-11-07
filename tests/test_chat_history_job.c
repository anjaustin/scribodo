/* Headless, deterministic worker tests. Compile the implementation here so
 * allocation/thread startup failures can be injected without production hooks. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app/chat_history_job.h"

static bool fail_alloc, fail_thread, fail_mutex, fail_cond;
static unsigned allocations, threads;
static void *job_calloc(size_t n, size_t size)
{
    allocations++;
    return fail_alloc ? NULL : calloc(n, size);
}
static int job_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    threads++;
    return fail_thread ? EAGAIN : pthread_create(t, a, fn, arg);
}
static int job_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    return fail_mutex ? EAGAIN : pthread_mutex_init(m, a);
}
static int job_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    return fail_cond ? EAGAIN : pthread_cond_init(c, a);
}
#define calloc job_calloc
#define pthread_create job_thread_create
#define pthread_mutex_init job_mutex_init
#define pthread_cond_init job_cond_init
#include "../src/app/chat_history_job.c"
#undef calloc
#undef pthread_create
#undef pthread_mutex_init
#undef pthread_cond_init

struct StoreManager { int unused; };
static struct StoreManager manager;
static atomic_int pages, cleanups;

/* Only these three store-facing symbols are substituted. The reader creates
 * owned strings (including partial failed output) and the free stub counts
 * their lifetime. No UI, database, model, or service is involved. */
void chat_window_page_free(ChatWindowPage *page)
{
    if (page->latest_user_docs) {
        free(page->latest_user_docs);
        atomic_fetch_sub(&pages, 1);
    }
    assert(!page->messages && !page->count);
    *page = (ChatWindowPage){0};
}
void store_thread_done(StoreManager *m)
{
    assert(m == &manager);
    atomic_fetch_add(&cleanups, 1);
}
bool chat_store_window(StoreManager *m, const char *p, const char *c,
                        const ChatWindowRequest *r, ChatWindowPage *out)
{
    assert(m == &manager);
    (void)p; (void)c; (void)r; (void)out;
    return false;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool hold, released, fail;
    bool hold_second, release_second;
    unsigned calls, active, max_active;
    pthread_t reader_thread;
    char conversations[8][40];
    char project[PATH_MAX];
    ChatWindowRequest request;
} Gate;

static struct timespec deadline(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    ts.tv_sec += 5;
    return ts;
}
static void pause_poll(void)
{
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}
static void gate_init(Gate *g, bool hold)
{
    memset(g, 0, sizeof(*g));
    g->hold = hold;
    assert(pthread_mutex_init(&g->mu, NULL) == 0);
    assert(pthread_cond_init(&g->cv, NULL) == 0);
}
static void gate_done(Gate *g)
{
    assert(g->active == 0 && g->max_active <= 1);
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
    assert(atomic_load(&pages) == 0);
}
static bool gate_read(StoreManager *m, const char *project, const char *conversation,
                       const ChatWindowRequest *request, ChatWindowPage *page, void *ctx)
{
    assert(m == &manager);
    Gate *g = ctx;
    pthread_mutex_lock(&g->mu);
    if (!g->calls) g->reader_thread = pthread_self();
    assert(pthread_equal(g->reader_thread, pthread_self()));
    assert(g->calls < 8);
    unsigned call = g->calls++;
    g->active++;
    if (g->active > g->max_active) g->max_active = g->active;
    pthread_cond_broadcast(&g->cv);
    struct timespec until = deadline();
    while (g->hold && !g->released)
        assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
    while (call == 1 && g->hold_second && !g->release_second)
        assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
    /* Access only after the caller has mutated its original input buffers. */
    strcpy(g->conversations[call], conversation);
    strcpy(g->project, project);
    g->request = *request;
    page->latest_user_docs = strdup(conversation);
    assert(page->latest_user_docs);
    atomic_fetch_add(&pages, 1);
    bool ok = !g->fail;
    g->active--;
    pthread_mutex_unlock(&g->mu);
    return ok;
}
static void wait_calls(Gate *g, unsigned calls)
{
    struct timespec until = deadline();
    pthread_mutex_lock(&g->mu);
    while (g->calls < calls) assert(pthread_cond_timedwait(&g->cv, &g->mu, &until) == 0);
    pthread_mutex_unlock(&g->mu);
}
static void release_gate(Gate *g)
{
    pthread_mutex_lock(&g->mu);
    g->released = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}
static ChatHistoryResult wait_result(ChatHistoryJob *job)
{
    ChatHistoryResult result = {0};
    for (int i = 0; i < 5000; i++) {
        if (chat_history_job_poll(job, &result)) return result;
        pause_poll();
    }
    assert(!"timed out waiting for result");
    return result;
}
static void wait_idle(ChatHistoryJob *job)
{
    for (int i = 0; i < 5000; i++) {
        if (!chat_history_job_busy(job)) return;
        pause_poll();
    }
    assert(!"timed out waiting for idle");
}
static void wait_ready(ChatHistoryJob *job)
{
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&job->mu);
        bool ready = job->has_result;
        pthread_mutex_unlock(&job->mu);
        if (ready) return;
        pause_poll();
    }
    assert(!"timed out waiting for unconsumed result");
}
static const ChatWindowRequest latest = {.direction = CHAT_WINDOW_LATEST};

static void test_latest_bounded(void)
{
    Gate g;
    gate_init(&g, true);
    unsigned alloc_before = allocations, threads_before = threads;
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(job && !chat_history_job_busy(job));
    assert(chat_history_job_start(job, "/original", "A", &latest, 7));
    wait_calls(&g, 1);
    /* All replacements have the SAME public ticket. Only the final request
     * may execute; A's slow result must never be observable. */
    for (unsigned i = 0; i < 10000; i++)
        assert(chat_history_job_start(job, "/pending", "B", &latest, 7));
    char project[] = "/copied", conversation[] = "C";
    ChatWindowRequest focus = {CHAT_WINDOW_FOCUS, 19, 32};
    assert(chat_history_job_start(job, project, conversation, &focus, 7));
    strcpy(project, "/mutate");
    conversation[0] = 'X';
    focus.sequence = 99;
    assert(allocations == alloc_before + 1 && threads == threads_before + 1);
    ChatHistoryResult result = {.ticket = 123};
    assert(!chat_history_job_poll(job, &result) && result.ticket == 123);
    release_gate(&g);
    result = wait_result(job);
    assert(result.ok && result.ticket == 7 && !result.why[0]);
    assert(!strcmp(result.page.latest_user_docs, "C"));
    assert(!chat_history_job_busy(job));
    chat_history_job_destroy(job);
    assert(g.calls == 2 && !strcmp(g.conversations[0], "A") && !strcmp(g.conversations[1], "C"));
    assert(!strcmp(g.project, "/copied") && g.request.sequence == 19 && g.request.start == 32);
    /* Ownership survives job destruction. */
    assert(atomic_load(&pages) == 1);
    chat_window_page_free(&result.page);
    gate_done(&g);
}

static void test_cancel(void)
{
    Gate g;
    gate_init(&g, true);
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(chat_history_job_start(job, "/p", "A", &latest, UINT64_MAX));
    wait_calls(&g, 1);
    assert(chat_history_job_start(job, "/p", "B", &latest, UINT64_MAX));
    chat_history_job_cancel(job);
    assert(chat_history_job_busy(job)); /* blocking reader is still alive */
    release_gate(&g);
    wait_idle(job);
    ChatHistoryResult result;
    assert(!chat_history_job_poll(job, &result));
    assert(chat_history_job_start(job, "/p", "C", &latest, UINT64_MAX));
    result = wait_result(job);
    assert(result.ok && !strcmp(result.page.latest_user_docs, "C"));
    chat_window_page_free(&result.page);
    assert(chat_history_job_start(job, "/p", "D", &latest, 0));
    /* Wait for an unpolled result, then cancel it. */
    wait_ready(job);
    chat_history_job_cancel(job);
    assert(!chat_history_job_poll(job, &result) && !chat_history_job_busy(job));
    chat_history_job_destroy(job);
    assert(g.calls == 3);
    gate_done(&g);
}

static void test_no_transient_stale_result(void)
{
    Gate g;
    gate_init(&g, true);
    g.hold_second = true;
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(job && chat_history_job_start(job, "/p", "A", &latest, 1));
    wait_calls(&g, 1);
    assert(chat_history_job_start(job, "/p", "B", &latest, 1));
    release_gate(&g);
    wait_calls(&g, 2); /* A completed; B cannot yet produce a replacement result. */
    ChatHistoryResult result;
    assert(!chat_history_job_poll(job, &result));
    pthread_mutex_lock(&g.mu);
    g.release_second = true;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    result = wait_result(job);
    assert(result.ok && !strcmp(result.page.latest_user_docs, "B"));
    chat_window_page_free(&result.page);
    chat_history_job_destroy(job);
    gate_done(&g);
}

static void test_cancel_only_inflight(void)
{
    Gate g;
    gate_init(&g, true);
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(job && chat_history_job_start(job, "/p", "A", &latest, 1));
    wait_calls(&g, 1);
    chat_history_job_cancel(job); /* no newer request/generation to mask this guard */
    release_gate(&g);
    wait_idle(job);
    ChatHistoryResult result;
    assert(!chat_history_job_poll(job, &result));
    chat_history_job_destroy(job);
    gate_done(&g);
}

static void test_cancel_restart_and_replace_result(void)
{
    Gate g;
    gate_init(&g, true);
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(chat_history_job_start(job, "/p", "A", &latest, 1));
    wait_calls(&g, 1);
    chat_history_job_cancel(job);
    /* Restart before A returns, again using the same public ticket. */
    assert(chat_history_job_start(job, "/p", "B", &latest, 1));
    release_gate(&g);
    wait_ready(job);
    assert(atomic_load(&pages) == 1 && chat_history_job_busy(job));
    /* Supersede a completed but unconsumed result, not just pending work. */
    char path[PATH_MAX], id[40];
    memset(path, 'p', sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    memset(id, 'c', sizeof(id) - 1);
    id[sizeof(id) - 1] = 0;
    assert(chat_history_job_start(job, path, id, &latest, 1));
    assert(!chat_history_job_poll(job, NULL));
    ChatHistoryResult result = wait_result(job);
    assert(result.ok && !strcmp(result.page.latest_user_docs, id));
    chat_window_page_free(&result.page);
    assert(g.calls == 3);
    /* Destroy also frees a completed unconsumed output. */
    assert(chat_history_job_start(job, "/p", "D", &latest, 0));
    wait_ready(job);
    chat_history_job_destroy(job);
    gate_done(&g);
}

static void *destroy_job(void *arg)
{
    chat_history_job_destroy(arg);
    return NULL;
}
static void test_shutdown(void)
{
    Gate g;
    gate_init(&g, true);
    int before = atomic_load(&cleanups);
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(chat_history_job_start(job, "/p", "A", &latest, 1));
    wait_calls(&g, 1);
    assert(chat_history_job_start(job, "/p", "B", &latest, 2));
    pthread_t destroyer;
    assert(pthread_create(&destroyer, NULL, destroy_job, job) == 0);
    /* White-box synchronization: destroy has invalidated pending B, and cannot
     * free the job until we release A. No API calls race destruction. */
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&job->mu);
        bool stopping = job->stopping;
        pthread_mutex_unlock(&job->mu);
        if (stopping) break;
        assert(i < 4999);
        pause_poll();
    }
    assert(atomic_load(&cleanups) == before);
    release_gate(&g);
    assert(pthread_join(destroyer, NULL) == 0);
    assert(atomic_load(&cleanups) == before + 1 && g.calls == 1);
    gate_done(&g);
}

static void test_failure_and_validation(void)
{
    assert(!chat_history_job_create(NULL));
    assert(!chat_history_job_create_with_reader(&manager, NULL, NULL));
    fail_alloc = true;
    assert(!chat_history_job_create(&manager));
    fail_alloc = false;
    fail_mutex = true;
    assert(!chat_history_job_create(&manager));
    fail_mutex = false;
    fail_cond = true;
    assert(!chat_history_job_create(&manager));
    fail_cond = false;
    fail_thread = true;
    assert(!chat_history_job_create(&manager));
    fail_thread = false;
    Gate g;
    gate_init(&g, true);
    g.fail = true;
    ChatHistoryJob *job = chat_history_job_create_with_reader(&manager, gate_read, &g);
    assert(chat_history_job_start(job, "/p", "valid", &latest, 42));
    wait_calls(&g, 1);
    char long_path[PATH_MAX + 1], long_id[41];
    memset(long_path, 'p', sizeof(long_path));
    long_path[PATH_MAX] = 0;
    memset(long_id, 'c', sizeof(long_id));
    long_id[40] = 0;
    assert(!chat_history_job_start(job, long_path, "x", &latest, 0));
    assert(!chat_history_job_start(job, "/p", long_id, &latest, 0));
    assert(!chat_history_job_start(job, "", "x", &latest, 0));
    assert(!chat_history_job_start(job, "/p\n", "x", &latest, 0));
    assert(!chat_history_job_start(job, "/p", "../x", &latest, 0));
    assert(!chat_history_job_start(job, "/p", "", &latest, 0));
    assert(!chat_history_job_start(job, NULL, "x", &latest, 0));
    assert(!chat_history_job_start(job, "/p", NULL, &latest, 0));
    assert(!chat_history_job_start(job, "/p", "x", NULL, 0));
    ChatWindowRequest bad[] = {{-1, 0, 0}, {4, 0, 0}, {0, -1, 0},
        {0, (int64_t)INT_MAX + 1, 0}, {0, 0, 1}, {CHAT_WINDOW_FOCUS, 0, SIZE_MAX}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        assert(!chat_history_job_start(job, "/p", "x", &bad[i], 0));
    release_gate(&g);
    ChatHistoryResult result = wait_result(job);
    assert(result.ticket == 42 && !result.ok && result.why[0]);
    assert(!result.page.latest_user_docs && atomic_load(&pages) == 0);
    /* Internal generation cannot wrap and make an obsolete request current. */
    pthread_mutex_lock(&job->mu);
    job->generation = UINT64_MAX;
    pthread_mutex_unlock(&job->mu);
    assert(!chat_history_job_start(job, "/p", "x", &latest, 0));
    chat_history_job_cancel(job);
    chat_history_job_destroy(job);
    gate_done(&g);
    job = chat_history_job_create(&manager);
    assert(job && chat_history_job_start(job, "/p", "x", &latest, 1));
    result = wait_result(job);
    assert(!result.ok);
    chat_history_job_destroy(job);
    chat_history_job_destroy(NULL);
    chat_history_job_cancel(NULL);
    assert(!chat_history_job_busy(NULL));
    assert(!chat_history_job_poll(NULL, &result));
    assert(!chat_history_job_start(NULL, "/p", "x", &latest, 0));
}

int main(void)
{
    test_latest_bounded();
    test_cancel();
    test_no_transient_stale_result();
    test_cancel_only_inflight();
    test_cancel_restart_and_replace_result();
    test_shutdown();
    test_failure_and_validation();
    puts("chat_history_job: all tests passed");
    return 0;
}
