/* Standalone (no model, GUI or database):
 * cc -std=c11 -Wall -Wextra -Werror -pthread -Isrc -Itests \
 *    tests/test_http_deadlines.c src/llm/http.c -o "$TMPDIR/test_http_deadlines"
 * Add -fsanitize=address,undefined or -fsanitize=thread for sanitizer runs.
 */
#define _DARWIN_C_SOURCE
#include "llm/http.h"
#include "test_util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "core/deadline_clock.h"

enum { DRIP, SLOW_DRIP, TRUNCATED, MALFORMED, UPLOAD, CANCEL_UPLOAD, CLOSE_NOW, SLOW_CALLBACK, CANCEL_RECEIVE };
typedef struct {
    int listener, mode;
    char path[104];
    pthread_t thread;
    atomic_bool stop, cancel;
    atomic_int accepted, sent;
    size_t uploaded;
} Fixture;

static double ms(void)
{
    struct timespec t;
    clock_gettime(SCRIBODO_DEADLINE_CLOCK, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

static void pause_ms(long n)
{
    struct timespec t = { n / 1000, n % 1000 * 1000000 };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static void *serve(void *arg)
{
    Fixture *f = arg;
    struct pollfd p = { .fd = f->listener, .events = POLLIN };
    while (!atomic_load(&f->stop)) {
        if (poll(&p, 1, 20) <= 0) continue;
        int fd = accept(f->listener, NULL, NULL);
        if (fd < 0) continue;
        atomic_fetch_add(&f->accepted, 1);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        fcntl(fd, F_SETFL, O_NONBLOCK);
        if (f->mode == CLOSE_NOW) { close(fd); break; }
        if (f->mode == UPLOAD || f->mode == CANCEL_UPLOAD) {
            /* Intentionally never read until the client has returned. */
            if (f->mode == CANCEL_UPLOAD) {
                pause_ms(20);
                atomic_store(&f->cancel, true);
            }
            while (!atomic_load(&f->stop)) pause_ms(1);
            char b[4096];
            ssize_t n;
            while ((n = recv(fd, b, sizeof(b), 0)) > 0) f->uploaded += (size_t)n;
        } else {
            /* Wait for request headers before replying. */
            char b[4096];
            while (!atomic_load(&f->stop)) {
                ssize_t n = recv(fd, b, sizeof(b), 0);
                if (n > 0) break;
                if (n == 0) break;
                pause_ms(1);
            }
            const char *head = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n";
            if (f->mode == MALFORMED)
                head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nxyz\r\n";
            if (f->mode == SLOW_CALLBACK)
                head = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\na\nb\n";
            (void)send(fd, head, strlen(head), 0);
            if (f->mode == CANCEL_RECEIVE) {
                pause_ms(20);
                atomic_store(&f->cancel, true);
                while (!atomic_load(&f->stop)) pause_ms(1);
            }
            if (f->mode == TRUNCATED) (void)send(fd, "x", 1, 0);
            if (f->mode == DRIP || f->mode == SLOW_DRIP) {
                for (int i = 0; i < 20 && !atomic_load(&f->stop); i++) {
                    pause_ms(f->mode == SLOW_DRIP ? 40 : 10);
                    if (send(fd, "x", 1, 0) != 1) break;
                    atomic_fetch_add(&f->sent, 1);
                }
            }
        }
        close(fd);
        break;
    }
    return NULL;
}

static void start(Fixture *f, int mode)
{
    memset(f, 0, sizeof(*f));
    atomic_init(&f->stop, false);
    atomic_init(&f->cancel, false);
    atomic_init(&f->accepted, 0);
    atomic_init(&f->sent, 0);
    f->mode = mode;
    /* Short unique path: macOS sockaddr_un is only 104 bytes. */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp/";
    int len = snprintf(f->path, sizeof(f->path), "%sscribodo-http-%ld-XXXXXX", tmpdir, (long)getpid());
    CHECK(len > 0 && (size_t)len < sizeof(f->path));
    int tmp = mkstemp(f->path);
    CHECK(tmp >= 0);
    close(tmp);
    unlink(f->path);
    f->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(f->listener >= 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, f->path);
    CHECK(bind(f->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(f->listener, 4) == 0);
    CHECK(pthread_create(&f->thread, NULL, serve, f) == 0);
}

static void stop(Fixture *f)
{
    atomic_store(&f->stop, true);
    CHECK(pthread_join(f->thread, NULL) == 0);
    close(f->listener);
    unlink(f->path);
}

static void invalid_deadlines(void)
{
    Fixture f;
    start(&f, DRIP);
    double deadlines[] = { NAN, INFINITY, -INFINITY, 0, -1, ms() - 1 };
    for (size_t i = 0; i < sizeof(deadlines) / sizeof(*deadlines); i++) {
        HttpResult r;
        CHECK(!http_request_until(f.path, "GET", "/", NULL, NULL, NULL, NULL,
                                  NULL, 1000, deadlines[i], &r));
        CHECK(r.body == NULL && r.len == 0 && r.status == 0 && r.error[0]);
        http_result_free(&r);
    }
    /* A queued connection is observable even if the server was not scheduled. */
    struct pollfd p = { .fd = f.listener, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(atomic_load(&f.accepted) == 0);
    stop(&f);
}

static void deadline_drip(void)
{
    Fixture f;
    start(&f, DRIP);
    HttpResult r;
    double begin = ms();
    CHECK(!http_request_until(f.path, "GET", "/", NULL, NULL, NULL, NULL,
                              NULL, 1000, begin + 40, &r));
    CHECK(strstr(r.error, "timed out") != NULL);
    CHECK(r.body == NULL && r.len == 0);
    CHECK(atomic_load(&f.sent) < 20);
    CHECK(ms() - begin < 5000); /* watchdog, not a scheduling-sensitive SLA */
    http_result_free(&r);
    stop(&f);
}

static void legacy_drip(void)
{
    /* Each 40 ms gap is far inside the 300 ms per-receive timeout, while the
     * 800 ms total transfer exceeds it: only a timeout that resets on data
     * completes. The margin absorbs sanitizer and hosted-runner stalls. */
    Fixture f;
    start(&f, SLOW_DRIP);
    HttpResult r;
    CHECK(http_request(f.path, "GET", "/", NULL, NULL, NULL, NULL, NULL, 300, &r));
    CHECK(r.status == 200 && r.len == 20 && r.body != NULL);
    http_result_free(&r);
    stop(&f);
    CHECK(atomic_load(&f.sent) == 20);
}

static void upload_cases(void)
{
    size_t size = 8u << 20;
    char *body = malloc(size + 1);
    CHECK(body != NULL);
    if (!body) return;
    memset(body, 'x', size);
    body[size] = 0;
    int modes[] = { UPLOAD, CANCEL_UPLOAD, CLOSE_NOW };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
        Fixture f;
        start(&f, modes[i]);
        HttpResult r;
        double begin = ms();
        CHECK(!http_request_until(f.path, "POST", "/", NULL, body, NULL, NULL,
                                  &f.cancel, 10000, begin + (modes[i] == UPLOAD ? 20 : 5000), &r));
        CHECK(r.body == NULL && r.len == 0);
        if (modes[i] == UPLOAD) CHECK(strstr(r.error, "timed out") != NULL);
        if (modes[i] == CANCEL_UPLOAD) CHECK(strcmp(r.error, "cancelled") == 0);
        CHECK(ms() - begin < 10000);
        http_result_free(&r);
        stop(&f);
        if (modes[i] != CLOSE_NOW) CHECK(f.uploaded > 0 && f.uploaded < size);
    }
    free(body);
}

static void broken_responses(void)
{
    int modes[] = { TRUNCATED, MALFORMED };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
        Fixture f;
        start(&f, modes[i]);
        HttpResult r;
        CHECK(!http_request_until(f.path, "GET", "/", NULL, NULL, NULL, NULL,
                                  NULL, 1000, ms() + 5000, &r));
        CHECK(r.body == NULL && r.len == 0 && r.error[0]);
        http_result_free(&r);
        stop(&f);
    }
}

static void receive_cancellation(void)
{
    Fixture f;
    start(&f, CANCEL_RECEIVE);
    HttpResult r;
    CHECK(!http_request_until(f.path, "GET", "/", NULL, NULL, NULL, NULL,
                              &f.cancel, 10000, ms() + 5000, &r));
    CHECK(strcmp(r.error, "cancelled") == 0);
    CHECK(r.body == NULL && r.len == 0);
    http_result_free(&r);
    stop(&f);
}

static bool slow_line(const char *line, size_t n, void *ctx)
{
    (void)line;
    (void)n;
    (*(int *)ctx)++;
    pause_ms(30);
    return true;
}

static void decoder_deadline(void)
{
    Fixture f;
    start(&f, SLOW_CALLBACK);
    HttpResult r;
    int lines = 0;
    CHECK(!http_request_until(f.path, "GET", "/", NULL, NULL, slow_line, &lines,
                              NULL, 1000, ms() + 20, &r));
    CHECK(strstr(r.error, "timed out") != NULL);
    CHECK(lines <= 1);
    CHECK(r.body == NULL && r.len == 0);
    http_result_free(&r);
    stop(&f);
}

static int fd_count(void)
{
    int count = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}

int main(void)
{
    alarm(30); /* Whole-suite deadlock watchdog. */
    int before = fd_count();
    TestCase cases[] = {
        { "invalid/expired deadlines never connect", invalid_deadlines },
        { "total deadline stops a slow drip", deadline_drip },
        { "legacy receive timeout resets on data", legacy_drip },
        { "partial upload deadline/cancellation/disconnect", upload_cases },
        { "truncated and malformed bodies rejected", broken_responses },
        { "cancellation interrupts receive wait", receive_cancellation },
        { "decoder work counts against deadline", decoder_deadline },
    };
    (void)run_tests(cases, sizeof(cases) / sizeof(*cases));
    CHECK(fd_count() == before);
    return g_failures ? 1 : 0;
}
