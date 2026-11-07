#include "generative_gate.h"

#include <pthread.h>
#include <stdlib.h>

struct GenerativeGate {
    pthread_mutex_t mu;
    bool closing;
    uint64_t serial, fence, active_serial;
    GenerativeTicket *active, *head, *tail;
};

GenerativeGate *generative_gate_create(void)
{
    GenerativeGate *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    if (pthread_mutex_init(&g->mu, NULL) != 0) {
        free(g);
        return NULL;
    }
    g->fence = 1;
    return g;
}

static void cancel_ticket(GenerativeTicket *t)
{
    if (t && t->cancel) atomic_store(t->cancel, true);
}

void generative_gate_close(GenerativeGate *g)
{
    if (!g) return;
    pthread_mutex_lock(&g->mu);
    g->closing = true;
    g->fence++;
    cancel_ticket(g->active);
    for (GenerativeTicket *t = g->head; t; t = t->next) cancel_ticket(t);
    pthread_mutex_unlock(&g->mu);
}

bool generative_gate_destroy(GenerativeGate *g)
{
    if (!g) return true;
    pthread_mutex_lock(&g->mu);
    bool idle = !g->active && !g->head;
    pthread_mutex_unlock(&g->mu);
    if (!idle) return false;
    pthread_mutex_destroy(&g->mu);
    free(g);
    return true;
}

static void register_ticket(GenerativeGate *g, GenerativeTicket *t, atomic_bool *cancel, bool foreground)
{
    t->gate = g;
    t->serial = ++g->serial;
    t->fence = g->fence;
    t->foreground = foreground;
    t->admitted = false;
    t->cancel = cancel;
    t->next = NULL;
}

bool generative_gate_foreground_begin(GenerativeGate *g, GenerativeTicket *t, atomic_bool *cancel)
{
    if (!g || !t || t->gate || (cancel && atomic_load(cancel))) return false;
    pthread_mutex_lock(&g->mu);
    bool ok = !g->closing;
    if (ok) {
        g->fence++;
        register_ticket(g, t, cancel, true);
        if (g->tail) g->tail->next = t;
        else g->head = t;
        g->tail = t;
        if (g->active && !g->active->foreground) cancel_ticket(g->active);
    }
    pthread_mutex_unlock(&g->mu);
    return ok;
}

bool generative_gate_try_foreground(GenerativeTicket *t)
{
    if (!t || !t->gate) return false;
    GenerativeGate *g = t->gate;
    pthread_mutex_lock(&g->mu);
    bool ok = !g->closing && !g->active && g->head == t && t->foreground &&
              !(t->cancel && atomic_load(t->cancel));
    if (ok) {
        g->head = t->next;
        if (!g->head) g->tail = NULL;
        t->next = NULL;
        t->admitted = true;
        g->active = t;
        g->active_serial = t->serial;
    }
    pthread_mutex_unlock(&g->mu);
    return ok;
}

bool generative_gate_try_background(GenerativeGate *g, GenerativeTicket *t, atomic_bool *cancel)
{
    if (!g || !t || t->gate) return false;
    pthread_mutex_lock(&g->mu);
    bool ok = !g->closing && !g->active && !g->head && !(cancel && atomic_load(cancel));
    if (ok) {
        register_ticket(g, t, cancel, false);
        t->admitted = true;
        g->active = t;
        g->active_serial = t->serial;
    }
    pthread_mutex_unlock(&g->mu);
    return ok;
}

bool generative_gate_finish(GenerativeTicket *t)
{
    if (!t || !t->gate) return false;
    GenerativeGate *g = t->gate;
    pthread_mutex_lock(&g->mu);
    bool ok = g->active == t && g->active_serial == t->serial;
    if (ok) g->active = NULL;
    else {
        GenerativeTicket *prev = NULL, *p = g->head;
        while (p && p != t) { prev = p; p = p->next; }
        ok = p == t;
        if (ok) {
            if (prev) prev->next = t->next;
            else g->head = t->next;
            if (g->tail == t) g->tail = prev;
        }
    }
    if (ok) {
        t->gate = NULL;
        t->admitted = false;
        t->cancel = NULL;
        t->next = NULL;
    }
    pthread_mutex_unlock(&g->mu);
    return ok;
}

uint64_t generative_gate_fence(GenerativeGate *g)
{
    if (!g) return 0;
    pthread_mutex_lock(&g->mu);
    uint64_t fence = g->fence;
    pthread_mutex_unlock(&g->mu);
    return fence;
}

bool generative_gate_fence_valid(GenerativeGate *g, uint64_t fence)
{
    if (!g) return true;
    pthread_mutex_lock(&g->mu);
    bool ok = !g->closing && fence == g->fence;
    pthread_mutex_unlock(&g->mu);
    return ok;
}

bool generative_gate_ticket_valid(GenerativeTicket *t)
{
    if (!t || !t->gate) return false;
    GenerativeGate *g = t->gate;
    pthread_mutex_lock(&g->mu);
    bool ok = !g->closing && g->active == t && g->active_serial == t->serial &&
              (t->foreground || t->fence == g->fence) && !(t->cancel && atomic_load(t->cancel));
    pthread_mutex_unlock(&g->mu);
    return ok;
}
