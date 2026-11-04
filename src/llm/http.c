#define _DARWIN_C_SOURCE
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum { ST_HEADERS, ST_CHUNK_SIZE, ST_CHUNK_DATA, ST_CHUNK_CRLF, ST_BODY, ST_TRAILERS, ST_DONE };

#define MAX_LINE (4u << 20)  /* one SSE line */
#define MAX_BODY (64u << 20) /* whole-body responses (e.g. /tokenize of a novel) */

static bool grow(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    char *b = realloc(*buf, c);
    if (!b) return false;
    *buf = b;
    *cap = c;
    return true;
}

void http_decoder_init(HttpDecoder *d, HttpLineFn on_line, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->content_length = -1;
    d->on_line = on_line;
    d->ctx = ctx;
}

void http_decoder_free(HttpDecoder *d)
{
    free(d->hdr);
    free(d->line);
    free(d->body);
    d->hdr = d->line = d->body = NULL;
}

static void emit_line(HttpDecoder *d)
{
    size_t n = d->line_len;
    if (n && d->line[n - 1] == '\r') n--;
    if (d->on_line && !d->stopped) {
        d->line[n] = '\0';
        if (!d->on_line(d->line, n, d->ctx)) d->stopped = true;
    }
    d->line_len = 0;
}

/* Body bytes after de-chunking: either accumulated whole, or split into
 * lines. A line may span any number of chunks. */
static bool body_bytes(HttpDecoder *d, const char *p, size_t n)
{
    if (!d->on_line) {
        if (d->body_len + n > MAX_BODY) return false;
        if (!grow(&d->body, &d->body_cap, d->body_len + n + 1)) return false;
        memcpy(d->body + d->body_len, p, n);
        d->body_len += n;
        d->body[d->body_len] = '\0';
        return true;
    }
    for (size_t i = 0; i < n; i++) {
        if (d->line_len >= MAX_LINE || !grow(&d->line, &d->line_cap, d->line_len + 2)) return false;
        if (p[i] == '\n') emit_line(d);
        else d->line[d->line_len++] = p[i];
    }
    return true;
}

static bool parse_headers(HttpDecoder *d)
{
    d->hdr[d->hdr_len] = '\0';
    if (strncmp(d->hdr, "HTTP/1.", 7) != 0 || d->hdr_len < 12) return false;
    d->status = atoi(d->hdr + 9);
    for (char *l = strstr(d->hdr, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        char *eol = strstr(h, "\r\n");
        size_t hl = eol ? (size_t)(eol - h) : strlen(h);
        if (hl > 18 && !strncasecmp(h, "Transfer-Encoding:", 18)) {
            /* Match "chunked" within this header line only. */
            for (size_t k = 18; k + 7 <= hl; k++)
                if (!strncasecmp(h + k, "chunked", 7)) d->chunked = true;
        }
        if (hl > 15 && !strncasecmp(h, "Content-Length:", 15)) d->content_length = strtol(h + 15, NULL, 10);
    }
    if (d->chunked) {
        d->state = ST_CHUNK_SIZE;
    } else {
        d->state = d->content_length == 0 ? ST_DONE : ST_BODY;
        d->remaining = d->content_length;
    }
    d->hdr_len = 0; /* hdr is reused for chunk-size and trailer lines */
    return true;
}

static bool fail(HttpDecoder *d)
{
    d->failed = true;
    return false;
}

bool http_decoder_feed(HttpDecoder *d, const char *data, size_t n)
{
    size_t i = 0;
    while (i < n && !d->failed && d->state != ST_DONE) {
        if (d->state == ST_HEADERS) {
            if (!grow(&d->hdr, &d->hdr_cap, d->hdr_len + 2)) return fail(d);
            d->hdr[d->hdr_len++] = data[i++];
            if (d->hdr_len > 65536) return fail(d);
            if (d->hdr_len >= 4 && !memcmp(d->hdr + d->hdr_len - 4, "\r\n\r\n", 4)) {
                d->hdr_len -= 2; /* keep one CRLF so every header line is preceded by CRLF */
                if (!parse_headers(d)) return fail(d);
            }
        } else if (d->state == ST_CHUNK_SIZE || d->state == ST_TRAILERS) {
            char c = data[i++];
            if (c == '\r') continue;
            if (c != '\n') {
                if (d->hdr_len > 1024 || !grow(&d->hdr, &d->hdr_cap, d->hdr_len + 2)) return fail(d);
                d->hdr[d->hdr_len++] = c;
                continue;
            }
            d->hdr[d->hdr_len] = '\0';
            if (d->state == ST_CHUNK_SIZE) {
                char *end = NULL;
                long size = strtol(d->hdr, &end, 16); /* ignores ";ext" */
                if (end == d->hdr || size < 0) return fail(d);
                d->remaining = size;
                d->state = size == 0 ? ST_TRAILERS : ST_CHUNK_DATA;
            } else if (d->hdr_len == 0) {
                d->state = ST_DONE;
            }
            d->hdr_len = 0;
        } else if (d->state == ST_CHUNK_DATA || d->state == ST_BODY) {
            size_t take = n - i;
            if (d->remaining >= 0 && (size_t)d->remaining < take) take = (size_t)d->remaining;
            if (!body_bytes(d, data + i, take)) return fail(d);
            i += take;
            if (d->remaining >= 0) {
                d->remaining -= (long)take;
                if (d->remaining == 0) d->state = d->state == ST_CHUNK_DATA ? ST_CHUNK_CRLF : ST_DONE;
            }
        } else if (d->state == ST_CHUNK_CRLF) {
            if (data[i++] == '\n') d->state = ST_CHUNK_SIZE;
        }
    }
    if (d->state == ST_DONE && !d->done) {
        d->done = true;
        if (d->on_line && d->line_len) emit_line(d); /* final unterminated line */
    }
    return !d->failed;
}

void http_decoder_finish(HttpDecoder *d)
{
    if (d->done) return;
    if (d->state == ST_BODY && d->content_length < 0) {
        d->done = true; /* body delimited by connection close */
        if (d->on_line && d->line_len) emit_line(d);
    }
}

void http_result_free(HttpResult *r)
{
    free(r->body);
    r->body = NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Send everything, but never block past `deadline` or after `cancel`: the
 * socket has a 250 ms send timeout, so a server that stops reading cannot
 * wedge the caller (red-team 003: Stop and quit used to hang). */
static bool send_all(int fd, const char *p, size_t n, atomic_bool *cancel, double deadline, char *err, size_t errlen)
{
    while (n) {
        if (cancel && atomic_load(cancel)) {
            snprintf(err, errlen, "cancelled");
            return false;
        }
        if (now_ms() > deadline) {
            snprintf(err, errlen, "timed out sending");
            return false;
        }
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            snprintf(err, errlen, "send: %s", strerror(errno));
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Connect without blocking indefinitely (a wedged server's full backlog). */
static bool connect_bounded(int fd, const struct sockaddr_un *addr, atomic_bool *cancel, double deadline,
                            char *err, size_t errlen)
{
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, (const struct sockaddr *)addr, sizeof(*addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EAGAIN) {
        snprintf(err, errlen, "connect: %s", strerror(errno));
        return false;
    }
    while (rc != 0) {
        if (cancel && atomic_load(cancel)) {
            snprintf(err, errlen, "cancelled");
            return false;
        }
        if (now_ms() > deadline) {
            snprintf(err, errlen, "timed out connecting");
            return false;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, 100) > 0) {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr) {
                snprintf(err, errlen, "connect: %s", strerror(soerr));
                return false;
            }
            rc = 0;
        }
    }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv = { 0, 250000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
}

bool http_request(const char *socket_path, const char *method, const char *path,
                  const char *bearer, const char *body, HttpLineFn on_line, void *ctx,
                  atomic_bool *cancel, int timeout_ms, HttpResult *res)
{
    memset(res, 0, sizeof(*res));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(res->error, sizeof(res->error), "socket: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        close(fd);
        snprintf(res->error, sizeof(res->error), "socket path too long");
        return false;
    }
    strcpy(addr.sun_path, socket_path);
    double deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 30000);
    if (!connect_bounded(fd, &addr, cancel, deadline, res->error, sizeof(res->error))) {
        close(fd);
        return false;
    }

    size_t blen = body ? strlen(body) : 0;
    char head[1024];
    int hl = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                      "%s%s%s"
                      "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                      method, path, bearer ? "Authorization: Bearer " : "", bearer ? bearer : "",
                      bearer ? "\r\n" : "", blen);
    if (hl <= 0 || (size_t)hl >= sizeof(head)) {
        snprintf(res->error, sizeof(res->error), "request header too long");
        close(fd);
        return false;
    }
    if (!send_all(fd, head, (size_t)hl, cancel, deadline, res->error, sizeof(res->error)) ||
        (blen && !send_all(fd, body, blen, cancel, deadline, res->error, sizeof(res->error)))) {
        close(fd);
        return false;
    }

    HttpDecoder d;
    http_decoder_init(&d, on_line, ctx);
    char buf[16384];
    bool ok = true;
    for (;;) {
        if (cancel && atomic_load(cancel)) {
            snprintf(res->error, sizeof(res->error), "cancelled");
            ok = false;
            break;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = 100; /* poll in short slices so cancellation is prompt */
        int waited = 0, pr = 0;
        while ((pr = poll(&pfd, 1, wait)) == 0) {
            waited += wait;
            if ((cancel && atomic_load(cancel)) || waited >= timeout_ms) break;
        }
        if (pr == 0) {
            snprintf(res->error, sizeof(res->error), cancel && atomic_load(cancel) ? "cancelled" : "timed out");
            ok = false;
            break;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            snprintf(res->error, sizeof(res->error), "poll: %s", strerror(errno));
            ok = false;
            break;
        }
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            snprintf(res->error, sizeof(res->error), "recv: %s", strerror(errno));
            ok = false;
            break;
        }
        if (r == 0) {
            http_decoder_finish(&d);
            break;
        }
        if (!http_decoder_feed(&d, buf, (size_t)r)) {
            snprintf(res->error, sizeof(res->error), "malformed HTTP response");
            ok = false;
            break;
        }
        if (d.done || d.stopped) break;
    }
    close(fd);
    res->status = d.status;
    if (!on_line) {
        res->body = d.body;
        res->len = d.body_len;
        d.body = NULL;
    }
    if (ok && d.status == 0) {
        snprintf(res->error, sizeof(res->error), "no HTTP response");
        ok = false;
    }
    http_decoder_free(&d);
    return ok;
}
