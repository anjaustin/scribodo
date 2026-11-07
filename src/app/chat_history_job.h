/* Bounded asynchronous DB display-window reads; never falls back to full history. */
#ifndef SCRIBODO_CHAT_HISTORY_JOB_H
#define SCRIBODO_CHAT_HISTORY_JOB_H

#include "app/chat_history.h"

typedef struct ChatHistoryJob ChatHistoryJob;
typedef struct {
    uint64_t ticket;
    bool ok;
    ChatWindowPage page;
    char why[256];
} ChatHistoryResult;

/* Reader runs on the single worker, not the caller. On failure it may leave a
 * partially allocated, freeable page. ctx and manager must outlive the job. */
typedef bool (*ChatHistoryRead)(StoreManager *manager, const char *project,
                                  const char *conversation, const ChatWindowRequest *request,
                                  ChatWindowPage *page, void *ctx);
ChatHistoryJob *chat_history_job_create(StoreManager *manager);
ChatHistoryJob *chat_history_job_create_with_reader(StoreManager *manager, ChatHistoryRead reader, void *ctx);
/* Joins the worker (including store_thread_done) BEFORE manager destruction.
 * Destroy must not race other API calls. NULL is allowed. */
void chat_history_job_destroy(ChatHistoryJob *job);

/* Copies inputs, replacing any pending request and discarding any old result.
 * Exactly one worker, one pending request, and one result slot; no per-start
 * allocation or spawning. project must fit PATH_MAX including NUL; conversation
 * must fit 40 bytes including NUL and contain only ASCII letters/digits/-/_.
 * Invalid input leaves existing work unchanged. Tickets need not be unique or
 * increasing: an internal nonwrapping generation determines freshness. */
bool chat_history_job_start(ChatHistoryJob *job, const char *project, const char *conversation,
                            const ChatWindowRequest *request, uint64_t ticket);
/* Invalidates pending/completed/inflight outputs promptly, but CANNOT interrupt
 * a blocking DB/network read. A new request waits for that read to return. */
void chat_history_job_cancel(ChatHistoryJob *job);
/* Transfers page ownership to caller, who must chat_window_page_free(&out->page)
 * before reusing it. Returns false without touching out when no result exists. */
bool chat_history_job_poll(ChatHistoryJob *job, ChatHistoryResult *out);
/* Includes an unconsumed result and cancelled-but-still-running reads. */
bool chat_history_job_busy(ChatHistoryJob *job);

#endif
