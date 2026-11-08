/* Measures what the real chat worker does when llama-server accepts a
 * completion request and never answers. A Unix-socket stand-in answers
 * /tokenize normally and hangs /v1/chat/completions in one of two ways; the
 * probe sends a message through a real ChatSession, requests cancellation
 * after CANCEL_AFTER_S, samples the process to record where every thread
 * is while it waits, and reports when the session actually stopped being
 * busy. Diagnostic only; never linked into the application.
 *
 *   chat_hang_probe MODE OUT_DIR      MODE: silent | headers
 */
#define _DARWIN_C_SOURCE
#include "llm/chat.h"
#include "llm/server.h"
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CANCEL_AFTER_S 3.0
#define SAMPLE_AFTER_CANCEL_S 5.0
#define GIVE_UP_S 420.0

typedef struct {
    int lfd;
    atomic_bool stop;
    bool headers;
    char path[104];
    double accepted_at, request_read_at;
} Stub;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static ssize_t read_request(int fd, char *buf, size_t cap)
{
    size_t used = 0, expected = 0;
    while (used + 1 < cap) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 5000) <= 0) return -1;
        ssize_t n = recv(fd, buf + used, cap - used - 1, 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        buf[used] = 0;
        char *headers = strstr(buf, "\r\n\r\n");
        if (!headers) continue;
        if (!expected) {
            expected = (size_t)(headers + 4 - buf);
            char *cl = strstr(buf, "Content-Length:");
            if (cl && cl < headers) expected += strtoull(cl + 15, NULL, 10);
        }
        if (used >= expected) return (ssize_t)used;
    }
    return -1;
}

static void send_all(int fd, const char *s) { (void)send(fd, s, strlen(s), 0); }

static void *stub_main(void *arg)
{
    Stub *st = arg;
    static char buf[1 << 20];
    while (!atomic_load(&st->stop)) {
        struct pollfd p = { .fd = st->lfd, .events = POLLIN };
        if (poll(&p, 1, 50) <= 0) continue;
        int c = accept(st->lfd, NULL, NULL);
        if (c < 0) continue;
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        ssize_t got = read_request(c, buf, sizeof(buf));
        if (got <= 0) { close(c); continue; }
        if (strstr(buf, "POST /tokenize")) {
            send_all(c, "HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\n{\"tokens\":[1,2]}");
            close(c);
            continue;
        }
        st->accepted_at = now();
        st->request_read_at = now();
        if (st->headers) {
            send_all(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
        }
        /* The server-side hang: hold the connection open, never write again. */
        while (!atomic_load(&st->stop)) usleep(10000);
        close(c);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "silent") && strcmp(argv[1], "headers"))) {
        fprintf(stderr, "usage: chat_hang_probe silent|headers OUT_DIR\n");
        return 2;
    }
    Stub st = { 0 };
    atomic_init(&st.stop, false);
    st.headers = !strcmp(argv[1], "headers");
    snprintf(st.path, sizeof(st.path), "/tmp/hang-%ld.sock", (long)getpid());
    unlink(st.path);
    st.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", st.path);
    if (bind(st.lfd, (struct sockaddr *)&a, sizeof(a)) || listen(st.lfd, 4)) { perror("listen"); return 2; }
    pthread_t th;
    if (pthread_create(&th, NULL, stub_main, &st)) return 2;

    LlmServerConfig cfg;
    llm_server_default_config(&cfg, "");
    LlmServer *srv = llm_server_create(&cfg);
    llm_server_test_attach(srv, st.path, "k");
    ChatSession *chat = chat_create(srv);
    char why[512] = "";
    double t0 = now();
    bool sent = chat_send(chat, "Where did we leave the lantern?", NULL, why, sizeof(why));
    printf("mode=%s sent=%d why=%s\n", argv[1], sent, why);
    fflush(stdout);
    bool cancelled = false, sampled = false;
    double cancel_at = -1, idle_at = -1;
    while (chat_busy(chat) && now() - t0 < GIVE_UP_S) {
        chat_poll(chat);
        double t = now() - t0;
        if (!cancelled && t >= CANCEL_AFTER_S) {
            chat_cancel(chat);
            cancelled = true;
            cancel_at = t;
            printf("cancel requested at %.3f s (server accepted the completion at %.3f s)\n", t,
                   st.accepted_at > 0 ? st.accepted_at - t0 : -1.0);
            fflush(stdout);
        }
        if (cancelled && !sampled && t >= cancel_at + SAMPLE_AFTER_CANCEL_S) {
            sampled = true;
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "/usr/bin/sample %ld 1 -mayDie -file %s/sample-%s.txt >/dev/null 2>&1",
                     (long)getpid(), argv[2], argv[1]);
            printf("sampling process at %.3f s\n", t);
            fflush(stdout);
            (void)system(cmd);
        }
        usleep(2000);
    }
    if (!chat_busy(chat)) idle_at = now() - t0;
    chat_poll(chat);
    const char *notice = chat_take_notice(chat);
    chat_lock(chat);
    size_t count = chat_count(chat);
    const ChatMessage *reply = count ? chat_message(chat, count - 1) : NULL;
    printf("idle_at=%.3f s (cancel_at=%.3f, gap=%.3f) still_busy=%d reply_error=%d reply=\"%s\" notice=\"%s\"\n",
           idle_at, cancel_at, idle_at >= 0 ? idle_at - cancel_at : -1.0, chat_busy(chat),
           reply ? reply->error : -1, reply && reply->content ? reply->content : "", notice ? notice : "");
    chat_unlock(chat);
    fflush(stdout);
    atomic_store(&st.stop, true);
    if (chat_busy(chat)) {
        printf("worker still busy after %.0f s; destroying with the stub closed\n", GIVE_UP_S);
        fflush(stdout);
    }
    chat_destroy(chat);
    llm_server_destroy(srv);
    pthread_join(th, NULL);
    close(st.lfd);
    unlink(st.path);
    return 0;
}
