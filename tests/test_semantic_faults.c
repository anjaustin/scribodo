/* Real embedding request/response code, with thread-local allocation faults. */
#define _DARWIN_C_SOURCE
#include "fault_alloc.h"
#include "test_util.h"
#include "test_state_fixture.h"
#include "core/json.h"
#include "llm/semantic.h"
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    int fd;
    int inputs;
    char path[104];
    const char *response;
    size_t response_len;
    atomic_bool stop;
    atomic_int bad_requests;
    pthread_t thread;
} EmbedStub;

static char root[512];
static StoreManager *store;

static bool send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t sent = send(fd, p, n, 0);
        if (sent <= 0) return false;
        p += sent;
        n -= (size_t)sent;
    }
    return true;
}

static void *serve(void *arg)
{
    EmbedStub *s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        if (poll(&pfd, 1, 10) <= 0) continue;
        int fd = accept(s->fd, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        struct timeval timeout = { .tv_sec = 10 };
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        char request[8192] = "";
        size_t used = 0, need = sizeof(request);
        char *body = NULL;
        while (used < sizeof(request) - 1 && used < need) {
            ssize_t n = recv(fd, request + used, sizeof(request) - 1 - used, 0);
            if (n <= 0) break;
            used += (size_t)n;
            request[used] = '\0';
            char *end = strstr(request, "\r\n\r\n"), *cl = strstr(request, "Content-Length:");
            if (end && cl) {
                body = end + 4;
                need = (size_t)(body - request) + strtoul(cl + 15, NULL, 10);
            }
        }
        cJSON *j = body && used == need ? json_parse(body) : NULL;
        cJSON *input = cJSON_GetObjectItem(j, "input");
        bool valid = cJSON_IsArray(input) && cJSON_GetArraySize(input) == s->inputs;
        for (int i = 0; valid && i < s->inputs; i++) valid = cJSON_IsString(cJSON_GetArrayItem(input, i));
        if (!valid) atomic_fetch_add(&s->bad_requests, 1);
        cJSON_Delete(j);
        const char *response = valid ? s->response : "{}";
        size_t response_len = valid ? s->response_len : 2;
        char header[256];
        int n = snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         valid ? "200 OK" : "400 Bad Request", response_len);
        if (send_all(fd, header, (size_t)n)) send_all(fd, response, response_len);
        close(fd);
    }
    return NULL;
}

static bool start_bytes(EmbedStub *s, const char *response, size_t response_len, int inputs, LlmServer **server, SemanticClient **client)
{
    memset(s, 0, sizeof(*s));
    atomic_init(&s->stop, false);
    atomic_init(&s->bad_requests, 0);
    *server = NULL;
    *client = NULL;
    s->inputs = inputs;
    int len = snprintf(s->path, sizeof(s->path), "%s/embed.sock", root);
    if (len < 0 || (size_t)len >= sizeof(s->path)) return false;
    s->response = response;
    s->response_len = response_len;
    s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->path);
    if (s->fd < 0) return false;
    if (bind(s->fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(s->fd);
        return false;
    }
    if (listen(s->fd, 4)) goto fail;
    LlmServerConfig cfg = { 0 };
    *server = llm_server_create(&cfg);
    if (!*server) goto fail;
    llm_server_test_attach(*server, s->path, "test-key");
    *client = semantic_client_create(store, *server);
    if (*client && pthread_create(&s->thread, NULL, serve, s) == 0) return true;
fail:
    semantic_client_destroy(*client);
    llm_server_destroy(*server);
    *client = NULL;
    *server = NULL;
    close(s->fd);
    unlink(s->path);
    return false;
}

static bool start(EmbedStub *s, const char *response, int inputs, LlmServer **server, SemanticClient **client)
{
    return start_bytes(s, response, strlen(response), inputs, server, client);
}

static void stop(EmbedStub *s, LlmServer *server, SemanticClient *client)
{
    atomic_store(&s->stop, true);
    pthread_join(s->thread, NULL);
    semantic_client_destroy(client);
    llm_server_destroy(server);
    close(s->fd);
    unlink(s->path);
}

static void vector_json(char *out, size_t cap, const char *index, const char *first, int dims)
{
    size_t used = (size_t)snprintf(out, cap, "{\"data\":[{%s\"embedding\":[%s", index, first);
    for (int i = 1; i < dims; i++) used += (size_t)snprintf(out + used, cap - used, ",0");
    snprintf(out + used, cap - used, "]}]}");
}

static bool unchanged(const float *out)
{
    for (int i = 0; i < SEM_DIM; i++) if (out[i] != 42.0f) return false;
    return true;
}

static void complete_json_spans(void)
{
    /* An exact span need not have an accessible terminator. */
    const char span[2] = { '{', '}' };
    cJSON *j = json_parse_complete(span, sizeof(span));
    CHECK(cJSON_IsObject(j));
    cJSON_Delete(j);
    const char *valid[] = { "{}", "{} \t\r\n", "[]", "true", "1", "\"text\"", "{\"literal\":\"\\\\u0000\"}" };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        j = json_parse_complete(valid[i], strlen(valid[i]));
        CHECK(j);
        cJSON_Delete(j);
    }
    const char *invalid[] = { "{} trailing", "{}{}", "{\"a\\u0000b\":1}", "{\"a\":\"b\\u0000c\"}", "", "{", "{}\0hidden" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        size_t len = i == 6 ? 9 : strlen(invalid[i]);
        j = json_parse_complete(invalid[i], len);
        CHECK(!j);
        cJSON_Delete(j);
    }
}

static void embedding_allocations(void)
{
    char response[4096];
    vector_json(response, sizeof(response), "\"index\":0,", "3", SEM_DIM);
    EmbedStub stub;
    LlmServer *server = NULL;
    SemanticClient *client = NULL;
    CHECK(start(&stub, response, 1, &server, &client));
    if (!client) return;
    const char *text = "Mara knows Zoë";
    for (int query = 0; query < 2; query++) {
        float out[SEM_DIM];
        char err[256] = "";
        fault_alloc_after(-1);
        CHECK(semantic_embed(client, &text, 1, query != 0, out, err, sizeof(err)));
        long count = fault_alloc_count();
        fault_alloc_after(-1);
        CHECK(count > 10 && out[0] == 1.0f && out[1] == 0.0f);
        for (long i = 0; i < count * 2; i++) {
            for (int d = 0; d < SEM_DIM; d++) out[d] = 42;
            err[0] = '\0';
            if (i < count) fault_alloc_once_after(i);
            else fault_alloc_after(i - count);
            bool ok = semantic_embed(client, &text, 1, query != 0, out, err, sizeof(err));
            fault_alloc_after(-1);
            CHECK(!ok && err[0] && unchanged(out));
        }
    }
    CHECK(atomic_load(&stub.bad_requests) == 0);
    stop(&stub, server, client);
}

static void embedding_responses(void)
{
    struct { const char *index, *first; int dims; bool ok; } cases[] = {
        { "\"index\":0,", "3", SEM_DIM, true },
        { "", "3", SEM_DIM, true }, /* Older compatible servers omit index. */
        { "\"index\":-1,", "3", SEM_DIM, false },
        { "\"index\":0.5,", "3", SEM_DIM, false },
        { "\"index\":1e100,", "3", SEM_DIM, false },
        { "\"index\":\"0\",", "3", SEM_DIM, false },
        { "\"index\":0,", "\"3\"", SEM_DIM, false },
        { "\"index\":0,", "1e999", SEM_DIM, false },
        { "\"index\":0,", "1e100", SEM_DIM, false },
        { "\"index\":0,", "3", SEM_DIM - 1, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char response[4096];
        vector_json(response, sizeof(response), cases[i].index, cases[i].first, cases[i].dims);
        EmbedStub stub;
        LlmServer *server = NULL;
        SemanticClient *client = NULL;
        CHECK(start(&stub, response, 1, &server, &client));
        if (!client) return;
        const char *text = "Mara";
        float out[SEM_DIM];
        for (int d = 0; d < SEM_DIM; d++) out[d] = 42;
        char err[256] = "";
        bool ok = semantic_embed(client, &text, 1, false, out, err, sizeof(err));
        CHECK(ok == cases[i].ok);
        CHECK(ok ? out[0] == 1.0f && out[1] == 0.0f : err[0] && unchanged(out));
        stop(&stub, server, client);
    }
}

static void embedding_batches(void)
{
    const char *texts[] = { "Mara", "Zoë" };
    for (int mode = 0; mode < 4; mode++) {
        char first[4096], second[4096], response[8192];
        vector_json(first, sizeof(first), "\"index\":1,", "3", SEM_DIM);
        vector_json(second, sizeof(second), mode == 1 ? "\"index\":1," : "\"index\":0,",
                    mode == 2 ? "\"bad\"" : "-3", SEM_DIM);
        /* Merge two data items without rebuilding production response logic. */
        first[strlen(first) - 2] = '\0';
        snprintf(response, sizeof(response), "%s,%s", first, second + strlen("{\"data\":["));
        if (mode == 3) snprintf(response, sizeof(response), "%s", second); /* Missing one vector. */
        EmbedStub stub;
        LlmServer *server = NULL;
        SemanticClient *client = NULL;
        CHECK(start(&stub, response, 2, &server, &client));
        if (!client) return;
        float out[SEM_DIM * 2];
        for (int d = 0; d < SEM_DIM * 2; d++) out[d] = 42;
        char err[256] = "";
        bool ok = semantic_embed(client, texts, 2, false, out, err, sizeof(err));
        CHECK(mode == 0 ? ok && out[0] == -1.0f && out[SEM_DIM] == 1.0f :
                          !ok && err[0] && unchanged(out) && unchanged(out + SEM_DIM));
        stop(&stub, server, client);
    }
}

static void embedding_json_boundaries(void)
{
    for (int mode = 0; mode < 5; mode++) {
        char response[8192];
        vector_json(response, sizeof(response), mode == 4 ? "\"index\\u0000other\":0," : "\"index\":0,", "3", SEM_DIM);
        size_t len = strlen(response);
        const char *tail = mode == 0 ? " \t\r\n" : mode == 1 ? " garbage" : mode == 2 ? " {}" : "hidden";
        if (mode != 4) {
            if (mode == 3) response[len++] = '\0';
            memcpy(response + len, tail, strlen(tail));
            len += strlen(tail);
        }
        EmbedStub stub;
        LlmServer *server = NULL;
        SemanticClient *client = NULL;
        CHECK(start_bytes(&stub, response, len, 1, &server, &client));
        if (!client) return;
        const char *text = "Mara";
        float out[SEM_DIM];
        for (int d = 0; d < SEM_DIM; d++) out[d] = 42;
        char err[256] = "";
        bool ok = semantic_embed(client, &text, 1, false, out, err, sizeof(err));
        CHECK(mode == 0 ? ok && out[0] == 1.0f : !ok && err[0] && unchanged(out));
        stop(&stub, server, client);
    }
}

int main(void)
{
    if (!test_state_fixture_init(root, sizeof(root), &store)) return 1;
    /* Never read the author's service secrets. */
    setenv("SCRIBODO_SECRETS", root, 1);
    const TestCase tests[] = { { "complete_json_spans", complete_json_spans },
                              { "embedding_allocations", embedding_allocations },
                              { "embedding_responses", embedding_responses },
                              { "embedding_batches", embedding_batches },
                              { "embedding_json_boundaries", embedding_json_boundaries } };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    test_state_fixture_destroy(root, store);
    return rc;
}
