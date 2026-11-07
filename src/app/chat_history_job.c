#define _POSIX_C_SOURCE 200809L
#include "app/chat_history_job.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char project[PATH_MAX];
    char conversation[40];
    ChatWindowRequest window;
    uint64_t ticket, generation;
} HistoryRequest;

struct ChatHistoryJob {
    StoreManager *manager;
    ChatHistoryRead reader;
    void *ctx;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t wake;
    bool stopping, pending, inflight, wanted, has_result;
    uint64_t generation;
    HistoryRequest request;
    ChatHistoryResult result;
};

static void discard_result(ChatHistoryJob *job)
{
    chat_window_page_free(&job->result.page);
    job->result = (ChatHistoryResult){0};
    job->has_result = false;
}

static bool read_window(StoreManager *manager, const char *project, const char *conversation,
                         const ChatWindowRequest *request, ChatWindowPage *page, void *ctx)
{
    return chat_store_window(manager, project, conversation, request, page);
}

static void *history_main(void *arg)
{
    ChatHistoryJob *job = arg;
    pthread_mutex_lock(&job->mu);
    for (;;) {
        while (!job->pending && !job->stopping) pthread_cond_wait(&job->wake, &job->mu);
        if (job->stopping) break;
        HistoryRequest request = job->request;
        job->pending = false;
        job->inflight = true;
        pthread_mutex_unlock(&job->mu);

        ChatHistoryResult result = {.ticket = request.ticket};
        result.ok = job->reader(job->manager, request.project, request.conversation,
                                &request.window, &result.page, job->ctx);
        if (!result.ok) {
            chat_window_page_free(&result.page);
            strcpy(result.why, "Unable to read the database chat display window.");
        }

        pthread_mutex_lock(&job->mu);
        job->inflight = false;
        if (!job->stopping && job->wanted && request.generation == job->generation) {
            discard_result(job);
            job->result = result;
            job->has_result = true;
        } else {
            chat_window_page_free(&result.page);
        }
    }
    pthread_mutex_unlock(&job->mu);
    store_thread_done(job->manager);
    return NULL;
}

ChatHistoryJob *chat_history_job_create_with_reader(StoreManager *manager, ChatHistoryRead reader, void *ctx)
{
    if (!manager || !reader) return NULL;
    ChatHistoryJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->manager = manager;
    job->reader = reader;
    job->ctx = ctx;
    if (pthread_mutex_init(&job->mu, NULL) != 0) { free(job); return NULL; }
    if (pthread_cond_init(&job->wake, NULL) != 0) {
        pthread_mutex_destroy(&job->mu);
        free(job);
        return NULL;
    }
    if (pthread_create(&job->thread, NULL, history_main, job) != 0) {
        pthread_cond_destroy(&job->wake);
        pthread_mutex_destroy(&job->mu);
        free(job);
        return NULL;
    }
    return job;
}

ChatHistoryJob *chat_history_job_create(StoreManager *manager)
{
    return chat_history_job_create_with_reader(manager, read_window, NULL);
}

bool chat_history_job_start(ChatHistoryJob *job, const char *project, const char *conversation,
                            const ChatWindowRequest *request, uint64_t ticket)
{
    if (!job || !project || !conversation || !request) return false;
    size_t plen = strnlen(project, PATH_MAX), clen = strnlen(conversation, 40);
    if (!plen || plen >= PATH_MAX || !clen || clen >= 40 ||
        request->direction < CHAT_WINDOW_LATEST || request->direction > CHAT_WINDOW_FOCUS ||
        request->sequence < 0 || request->sequence > INT_MAX || request->start >= INT64_MAX ||
        (request->direction != CHAT_WINDOW_FOCUS && request->start)) return false;
    for (size_t i = 0; i < plen; i++)
        if ((unsigned char)project[i] < 32 || project[i] == 127) return false;
    for (size_t i = 0; i < clen; i++) {
        unsigned char c = (unsigned char)conversation[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    pthread_mutex_lock(&job->mu);
    if (job->stopping || job->generation == UINT64_MAX) {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    memcpy(job->request.project, project, plen + 1);
    memcpy(job->request.conversation, conversation, clen + 1);
    job->request.window = *request;
    job->request.ticket = ticket;
    job->request.generation = ++job->generation;
    job->pending = job->wanted = true;
    discard_result(job);
    pthread_cond_signal(&job->wake);
    pthread_mutex_unlock(&job->mu);
    return true;
}

void chat_history_job_cancel(ChatHistoryJob *job)
{
    if (!job) return;
    pthread_mutex_lock(&job->mu);
    job->pending = job->wanted = false;
    discard_result(job);
    pthread_mutex_unlock(&job->mu);
}

bool chat_history_job_poll(ChatHistoryJob *job, ChatHistoryResult *out)
{
    if (!job || !out) return false;
    pthread_mutex_lock(&job->mu);
    bool ready = job->has_result;
    if (ready) {
        *out = job->result;
        job->result = (ChatHistoryResult){0};
        job->has_result = false;
    }
    pthread_mutex_unlock(&job->mu);
    return ready;
}

bool chat_history_job_busy(ChatHistoryJob *job)
{
    if (!job) return false;
    pthread_mutex_lock(&job->mu);
    bool busy = job->pending || job->inflight || job->has_result;
    pthread_mutex_unlock(&job->mu);
    return busy;
}

void chat_history_job_destroy(ChatHistoryJob *job)
{
    if (!job) return;
    pthread_mutex_lock(&job->mu);
    job->stopping = true;
    job->pending = job->wanted = false;
    discard_result(job);
    pthread_cond_signal(&job->wake);
    pthread_mutex_unlock(&job->mu);
    pthread_join(job->thread, NULL);
    pthread_cond_destroy(&job->wake);
    pthread_mutex_destroy(&job->mu);
    free(job);
}
