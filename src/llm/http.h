/* Minimal HTTP/1.1 client over a Unix domain socket, for talking to the
 * local llama-server only. Supports Content-Length and chunked bodies, and
 * delivers the body either whole or line by line (for server-sent events).
 * One request per connection ("Connection: close").
 */
#ifndef SCRIBODO_HTTP_H
#define SCRIBODO_HTTP_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int status;    /* HTTP status, or 0 if no response was received */
    char *body;    /* malloc'd, NUL-terminated (whole-body requests only) */
    size_t len;
    char error[160];
} HttpResult;

/* Called once per body line, without the trailing "\n" / "\r\n". Return
 * false to stop reading. */
typedef bool (*HttpLineFn)(const char *line, size_t len, void *ctx);

/* Perform a request on `socket_path` (a Unix socket, or "tcp:PORT" for
 * 127.0.0.1:PORT; no other host can be named). `body` may be NULL. If `on_line` is non-NULL the body is
 * streamed to it line by line and result->body stays NULL. `cancel`, if
 * non-NULL, is polled between reads; setting it aborts the request.
 * `timeout_ms` bounds each wait for data (not the whole request). */
bool http_request(const char *socket_path, const char *method, const char *path,
                  const char *bearer, const char *body, HttpLineFn on_line, void *ctx,
                  atomic_bool *cancel, int timeout_ms, HttpResult *result);

void http_result_free(HttpResult *r);

/* ---- exposed for tests ----------------------------------------------- */

/* Incremental decoder for an HTTP response byte stream. */
typedef struct {
    int state;          /* internal */
    int status;
    bool chunked;
    long content_length; /* -1 if unknown */
    long remaining;      /* bytes left in current chunk / body */
    char *hdr;           /* header accumulation */
    size_t hdr_len, hdr_cap;
    char *line;          /* body line accumulation */
    size_t line_len, line_cap;
    char *body;          /* whole-body accumulation (when not streaming lines) */
    size_t body_len, body_cap;
    bool done;
    bool failed;
    HttpLineFn on_line;
    void *ctx;
    bool stopped;        /* on_line returned false */
} HttpDecoder;

void http_decoder_init(HttpDecoder *d, HttpLineFn on_line, void *ctx);
/* Feed bytes; returns false on a protocol error. */
bool http_decoder_feed(HttpDecoder *d, const char *data, size_t n);
/* Call at EOF (needed for bodies without length). */
void http_decoder_finish(HttpDecoder *d);
void http_decoder_free(HttpDecoder *d);

#endif
