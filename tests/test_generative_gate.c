#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "llm/generative_gate.h"
#include "test_util.h"

typedef struct {
    GenerativeGate *gate;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int phase;
    atomic_bool cancel;
    bool admitted, cancelled, valid;
} Background;

/* Simulate an observer HTTP call that ignores cancellation until explicitly
 * drained. Barriers, not sleeps, make foreground/preemption ordering exact. */
static void *background(void *arg)
{
    Background *b = arg;
    GenerativeTicket ticket = { 0 };
    b->admitted = generative_gate_try_background(b->gate, &ticket, &b->cancel);
    pthread_mutex_lock(&b->mu);
    b->phase = 1;
    pthread_cond_broadcast(&b->cv);
    while (b->phase < 2) pthread_cond_wait(&b->cv, &b->mu);
    pthread_mutex_unlock(&b->mu);
    b->cancelled = atomic_load(&b->cancel);
    b->valid = generative_gate_ticket_valid(&ticket);
    assert(generative_gate_finish(&ticket));
    return NULL;
}

static void test_preemption_and_drain(void)
{
    GenerativeGate *g = generative_gate_create();
    Background b = { .gate = g, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
    atomic_init(&b.cancel, false);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, background, &b) == 0);
    pthread_mutex_lock(&b.mu);
    while (b.phase < 1) pthread_cond_wait(&b.cv, &b.mu);
    pthread_mutex_unlock(&b.mu);
    CHECK(b.admitted);
    uint64_t fence = generative_gate_fence(g);
    GenerativeTicket fg = { 0 }, other = { 0 };
    atomic_bool cancel = false;
    CHECK(generative_gate_foreground_begin(g, &fg, &cancel));
    CHECK(atomic_load(&b.cancel));
    CHECK(!generative_gate_fence_valid(g, fence));
    CHECK(!generative_gate_try_foreground(&fg));
    CHECK(!generative_gate_try_background(g, &other, NULL));
    CHECK(!generative_gate_destroy(g));
    pthread_mutex_lock(&b.mu);
    b.phase = 2;
    pthread_cond_broadcast(&b.cv);
    pthread_mutex_unlock(&b.mu);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(b.cancelled && !b.valid); /* ignored cancellation cannot publish */
    CHECK(generative_gate_try_foreground(&fg));
    CHECK(!generative_gate_try_background(g, &other, NULL));
    CHECK(generative_gate_finish(&fg));
    CHECK(generative_gate_try_background(g, &other, NULL));
    CHECK(generative_gate_finish(&other));
    CHECK(generative_gate_destroy(g));
    pthread_cond_destroy(&b.cv);
    pthread_mutex_destroy(&b.mu);
}

static void test_fifo_and_cancelled_intent(void)
{
    GenerativeGate *g = generative_gate_create();
    GenerativeTicket a = { 0 }, b = { 0 }, c = { 0 }, bg = { 0 };
    atomic_bool cancel_a = false;
    CHECK(generative_gate_foreground_begin(g, &a, &cancel_a));
    uint64_t fence = generative_gate_fence(g);
    CHECK(!generative_gate_foreground_begin(g, &a, &cancel_a));
    for (int i = 0; i < 100; i++) CHECK(!generative_gate_try_foreground(&b));
    CHECK(generative_gate_fence(g) == fence);
    CHECK(generative_gate_foreground_begin(g, &b, NULL));
    CHECK(generative_gate_foreground_begin(g, &c, NULL));
    CHECK(!generative_gate_try_foreground(&b));
    atomic_store(&cancel_a, true);
    CHECK(!generative_gate_try_foreground(&a));
    CHECK(generative_gate_finish(&a)); /* abandoned waiter must unregister */
    CHECK(generative_gate_try_foreground(&b));
    CHECK(generative_gate_finish(&c)); /* cancel queue tail while b runs */
    CHECK(!generative_gate_try_background(g, &bg, NULL));
    CHECK(generative_gate_finish(&b));
    CHECK(generative_gate_try_background(g, &bg, NULL));
    CHECK(generative_gate_finish(&bg));
    CHECK(!generative_gate_finish(&bg));
    CHECK(generative_gate_destroy(g));
}

static void test_stale_lease(void)
{
    GenerativeGate *g = generative_gate_create();
    GenerativeTicket t = { 0 };
    CHECK(generative_gate_try_background(g, &t, NULL));
    GenerativeTicket stale = t; /* deliberate misuse must not release owner */
    CHECK(!generative_gate_finish(&stale));
    CHECK(generative_gate_ticket_valid(&t));
    CHECK(generative_gate_finish(&t));
    CHECK(generative_gate_try_background(g, &t, NULL));
    CHECK(t.serial != stale.serial);
    CHECK(!generative_gate_ticket_valid(&stale));
    CHECK(!generative_gate_finish(&stale));
    CHECK(generative_gate_ticket_valid(&t));
    CHECK(generative_gate_finish(&t));
    CHECK(generative_gate_destroy(g));
}

static void test_close_and_lifetime(void)
{
    GenerativeGate *g = generative_gate_create();
    GenerativeTicket active = { 0 }, queued = { 0 }, denied = { 0 };
    atomic_bool a = false, q = false;
    CHECK(generative_gate_try_background(g, &active, &a));
    CHECK(generative_gate_foreground_begin(g, &queued, &q));
    generative_gate_close(g);
    CHECK(atomic_load(&a) && atomic_load(&q));
    CHECK(!generative_gate_destroy(g));
    CHECK(!generative_gate_ticket_valid(&active));
    CHECK(!generative_gate_try_foreground(&queued));
    CHECK(!generative_gate_foreground_begin(g, &denied, NULL));
    CHECK(!generative_gate_try_background(g, &denied, NULL));
    CHECK(generative_gate_finish(&active));
    CHECK(!generative_gate_destroy(g)); /* queued intent still borrows gate */
    CHECK(generative_gate_finish(&queued));
    CHECK(generative_gate_destroy(g));
    /* The gate must no longer retain the stack cancellation tokens. */
}

static void test_shutdown_joins_borrower(void)
{
    GenerativeGate *g = generative_gate_create();
    Background b = { .gate = g, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
    atomic_init(&b.cancel, false);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, background, &b) == 0);
    pthread_mutex_lock(&b.mu);
    while (b.phase < 1) pthread_cond_wait(&b.cv, &b.mu);
    pthread_mutex_unlock(&b.mu);
    CHECK(b.admitted);
    generative_gate_close(g);
    CHECK(!generative_gate_destroy(g));
    CHECK(atomic_load(&b.cancel));
    pthread_mutex_lock(&b.mu);
    b.phase = 2;
    pthread_cond_broadcast(&b.cv);
    pthread_mutex_unlock(&b.mu);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(b.cancelled && !b.valid);
    CHECK(generative_gate_destroy(g));
    pthread_cond_destroy(&b.cv);
    pthread_mutex_destroy(&b.mu);
}

typedef struct {
    GenerativeGate *gate;
    atomic_int inside, errors, claims;
} Stress;

static void *stress(void *arg)
{
    Stress *s = arg;
    for (int i = 0; i < 10000; i++) {
        atomic_bool cancel = false;
        GenerativeTicket t = { 0 };
        bool admitted;
        if (i % 3 == 0) {
            if (!generative_gate_foreground_begin(s->gate, &t, &cancel)) {
                atomic_fetch_add(&s->errors, 1);
                continue;
            }
            admitted = generative_gate_try_foreground(&t);
        } else admitted = generative_gate_try_background(s->gate, &t, &cancel);
        if (admitted) {
            if (atomic_fetch_add(&s->inside, 1) != 0) atomic_fetch_add(&s->errors, 1);
            atomic_fetch_add(&s->claims, 1);
            (void)atomic_load(&cancel);
            (void)generative_gate_ticket_valid(&t);
            sched_yield();
            if (atomic_fetch_sub(&s->inside, 1) != 1) atomic_fetch_add(&s->errors, 1);
        }
        if (t.gate && !generative_gate_finish(&t)) atomic_fetch_add(&s->errors, 1);
    }
    return NULL;
}

static void test_concurrent_claims(void)
{
    Stress s = { .gate = generative_gate_create() };
    atomic_init(&s.inside, 0);
    atomic_init(&s.errors, 0);
    atomic_init(&s.claims, 0);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) CHECK(pthread_create(&threads[i], NULL, stress, &s) == 0);
    for (int i = 0; i < 4; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(atomic_load(&s.errors) == 0);
    CHECK(atomic_load(&s.claims) > 0);
    CHECK(atomic_load(&s.inside) == 0);
    CHECK(generative_gate_destroy(s.gate));
}

int main(void)
{
    const TestCase tests[] = {
        { "preemption_and_drain", test_preemption_and_drain },
        { "fifo_and_cancelled_intent", test_fifo_and_cancelled_intent },
        { "stale_lease", test_stale_lease },
        { "close_and_lifetime", test_close_and_lifetime },
        { "shutdown_joins_borrower", test_shutdown_joins_borrower },
        { "concurrent_claims", test_concurrent_claims },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
