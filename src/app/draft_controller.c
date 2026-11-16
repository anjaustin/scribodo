#include "app/draft_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/doc_io.h"

uint64_t draft_text_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 1099511628211ULL;
    return h;
}

void draft_controller_init(DraftController *draft)
{
    if (!draft) return;
    memset(draft, 0, sizeof(*draft));
    atomic_init(&draft->done, false);
}

static void run_write(DraftController *draft)
{
    DraftWrite *w = &draft->write;
    if (draft->write_hook) draft->write_hook(draft->write_hook_ctx);
    w->ok = doc_write_draft(w->doc, w->text, w->len, w->previous, w->out, sizeof(w->out));
}

static void *write_worker(void *arg)
{
    DraftController *draft = arg;
    run_write(draft);
    atomic_store_explicit(&draft->done, true, memory_order_release);
    return NULL;
}

/* The write has finished (joined): adopt its result. */
static void adopt(DraftController *draft)
{
    if (draft->threaded) pthread_join(draft->thread, NULL);
    draft->threaded = false;
    draft->busy = false;
    atomic_store_explicit(&draft->done, false, memory_order_relaxed);
    DraftWrite *w = &draft->write;
    free(w->text);
    w->text = NULL;
    if (draft->discard_pending) {
        draft->discard_pending = false;
        if (w->ok) unlink(w->out); /* its predecessor went with the discard */
        return;
    }
    if (w->ok) {
        snprintf(draft->path, sizeof(draft->path), "%s", w->out);
        draft->hash = w->hash;
    } else if (!draft->path[0]) {
        draft->doc[0] = '\0'; /* no draft and none coming: the document owns nothing yet */
    }
}

void draft_controller_set_write_hook(DraftController *draft, DraftWriteHook hook, void *ctx)
{
    draft->write_hook = hook;
    draft->write_hook_ctx = ctx;
}



bool draft_controller_poll(DraftController *draft)
{
    if (!draft->busy) return false;
    if (draft->threaded && !atomic_load_explicit(&draft->done, memory_order_acquire)) return true;
    adopt(draft);
    return false;
}

void draft_controller_wait(DraftController *draft)
{
    if (draft->busy) adopt(draft);
}

void draft_controller_destroy(DraftController *draft)
{
    if (!draft) return;
    draft_controller_wait(draft);
}

void draft_controller_discard(DraftController *draft)
{
    if (draft->path[0]) unlink(draft->path);
    draft->path[0] = draft->doc[0] = '\0';
    draft->hash = 0;
    if (draft->busy) draft->discard_pending = true;
}

bool draft_controller_tick(DraftController *draft, const char *doc, bool dirty, double now, DraftTextFn text, void *ctx)
{
    (void)draft_controller_poll(draft);
    if (!doc || !dirty || (draft->doc[0] && strcmp(draft->doc, doc) != 0)) {
        if (draft->path[0] || (draft->busy && !draft->discard_pending)) draft_controller_discard(draft);
        return false;
    }
    if (now - draft->at < DRAFT_INTERVAL_S || draft->busy) return false;
    draft->at = now;
    char *buffer = text(ctx);
    if (!buffer) return false;
    uint64_t h = draft_text_hash(buffer);
    if (h == draft->hash) {
        free(buffer);
        return false;
    }
    DraftWrite *w = &draft->write;
    snprintf(w->doc, sizeof(w->doc), "%s", doc);
    snprintf(w->previous, sizeof(w->previous), "%s", draft->path);
    w->text = buffer;
    w->len = strlen(buffer);
    w->hash = h;
    w->ok = false;
    w->out[0] = '\0';
    snprintf(draft->doc, sizeof(draft->doc), "%s", doc);
    draft->busy = true;
    atomic_store_explicit(&draft->done, false, memory_order_relaxed);
    draft->threaded = pthread_create(&draft->thread, NULL, write_worker, draft) == 0;
    if (!draft->threaded) run_write(draft); /* no thread to be had: write here, as before the worker */
    return true;
}
