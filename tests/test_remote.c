#define _DARWIN_C_SOURCE
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app/chat_store.h"
#include "app/store.h"
#include "llm/chat.h"
#include "llm/http.h"
#include "llm/remote.h"
#include "test_state_fixture.h"
#include "test_util.h"

static char g_dir[512];
static StoreManager *g_store;

typedef struct { int done; } DoneCtx;
static void on_done(ChatSession *session, size_t index, void *context)
{
    (void)session;
    (void)index;
    ((DoneCtx *)context)->done++;
}

static void wait_idle(ChatSession *session, DoneCtx *done, int want)
{
    for (int i = 0; i < 6000 && (chat_busy(session) || done->done < want); i++) {
        chat_poll(session);
        usleep(5000);
    }
    chat_poll(session);
}

/* ---- the optional remote model ---------------------------------------------------- */

typedef enum { RSTUB_OK, RSTUB_401, RSTUB_REDIRECT, RSTUB_BIG } RStubMode;
typedef struct {
    int lfd, port;
    RStubMode mode;
    atomic_int stop, requests;
    char last[65536];
} RStub;

static void *rstub_main(void *arg)
{
    RStub *st = arg;
    while (!atomic_load(&st->stop)) {
        struct pollfd pfd = { .fd = st->lfd, .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0) continue;
        int c = accept(st->lfd, NULL, NULL);
        if (c < 0) continue;
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        struct timeval io_timeout = { .tv_sec = 10 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
        /* Read headers and the whole body (Content-Length). */
        size_t got = 0;
        long need = -1;
        while (got < sizeof(st->last) - 1) {
            ssize_t r = recv(c, st->last + got, sizeof(st->last) - 1 - got, 0);
            if (r <= 0) break;
            got += (size_t)r;
            st->last[got] = 0;
            char *end = strstr(st->last, "\r\n\r\n");
            if (end && need < 0) {
                char *cl = strcasestr(st->last, "Content-Length:");
                need = (long)(end + 4 - st->last) + (cl ? atol(cl + 15) : 0);
            }
            if (need >= 0 && (long)got >= need) break;
        }
        atomic_fetch_add(&st->requests, 1);
        if (st->mode == RSTUB_BIG) {
            const char *head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n";
            send(c, head, strlen(head), 0);
            char block[65536];
            memset(block, 'x', sizeof(block));
            size_t sent = 0, want = REMOTE_MAX_RESPONSE_BYTES + sizeof(block);
            while (sent < want) {
                ssize_t n = send(c, block, sizeof(block), 0);
                if (n <= 0) break;
                sent += (size_t)n;
            }
            close(c);
            continue;
        }
        const char *r;
        if (st->mode == RSTUB_401) r = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\nConnection: close\r\n\r\n{\"error\":\"invalid api key\"}";
        else if (st->mode == RSTUB_REDIRECT) r = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:9/steal\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        else r = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                 "data: {\"choices\":[{\"delta\":{\"content\":\"The knight drew his\"}}]}\n\n"
                 "data: {\"choices\":[{\"delta\":{\"content\":\" sword.\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        send(c, r, strlen(r), 0);
        close(c);
    }
    return NULL;
}

static bool rstub_start(RStub *st, RStubMode mode, pthread_t *th)
{
    memset(st, 0, sizeof(*st));
    st->mode = mode;
    st->lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    if (bind(st->lfd, (struct sockaddr *)&a, sizeof(a)) || listen(st->lfd, 4) || getsockname(st->lfd, (struct sockaddr *)&a, &al))
        return false;
    st->port = ntohs(a.sin_port);
    return pthread_create(th, NULL, rstub_main, st) == 0;
}

static void rstub_stop(RStub *st, pthread_t th)
{
    atomic_store(&st->stop, 1);
    pthread_join(th, NULL);
    close(st->lfd);
}

static bool remote_cfg(RemoteConfig *c, const char *url, const char *key, int mode, char *err, size_t errlen)
{
    char kf[600], json[1400];
    snprintf(kf, sizeof(kf), "%s/remote-key", g_dir);
    FILE *f = fopen(kf, "wb");
    fputs(key, f);
    fclose(f);
    chmod(kf, (mode_t)mode);
    snprintf(json, sizeof(json), "{\"name\":\"Frontier\",\"base_url\":\"%s\",\"model\":\"m1\",\"api_key_file\":\"%s\"}", url, kf);
    return remote_parse(json, c, err, errlen);
}

static bool write_private(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    bool ok = f && fputs(text, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    return ok && chmod(path, 0600) == 0;
}

static void test_remote_json_boundaries(void)
{
    RemoteConfig c;
    char err[200], json[2000], key[700];
    snprintf(key, sizeof(key), "%s/strict-key", g_dir);
    CHECK(write_private(key, "sk-test"));
    const char *models[] = { "m", "m\\u0000hidden" };
    const char *tails[] = { " \t\r\n", " garbage", " {}" };
    for (size_t i = 0; i < 2; i++) {
        for (size_t k = 0; k < 3; k++) {
            snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"%s\",\"api_key_file\":\"%s\"}%s",
                     models[i], key, tails[k]);
            bool ok = remote_parse(json, &c, err, sizeof(err));
            CHECK(i == 0 && k == 0 ? ok && !strcmp(c.key, "sk-test") : !ok && err[0] && !c.key[0]);
        }
    }
    /* cJSON's decoded strings must not alias an input path or a field name. */
    snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"m\",\"api_key_file\":\"%s\\u0000other\"}", key);
    CHECK(!remote_parse(json, &c, err, sizeof(err)) && err[0] && !c.key[0]);
    snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"m\\\\u0000literal\",\"api_key_file\":\"%s\"}", key);
    CHECK(remote_parse(json, &c, err, sizeof(err)) && !strcmp(c.model, "m\\u0000literal"));
    snprintf(json, sizeof(json), "{\"base_url\\u0000other\":\"https://api.example.com/v1\",\"model\":\"m\",\"api_key_file\":\"%s\"}", key);
    CHECK(!remote_parse(json, &c, err, sizeof(err)) && err[0] && !c.key[0]);
    char oversized_path[2000];
    memset(oversized_path, 'x', sizeof(oversized_path) - 1);
    oversized_path[sizeof(oversized_path) - 1] = '\0';
    setenv("SCRIBODO_REMOTE_CONFIG", oversized_path, 1);
    CHECK(!remote_load(&c, err, sizeof(err)) && strstr(err, "path is too long") && !c.key[0]);
    unsetenv("SCRIBODO_REMOTE_CONFIG");
    char model[200];
    memset(model, 'x', sizeof(model) - 1);
    model[sizeof(model) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"%s\",\"api_key_file\":\"%s\"}", model, key);
    err[0] = '\0';
    CHECK(!remote_parse(json, &c, err, sizeof(err)) && err[0] && !c.key[0]);
}

static void test_remote_service_secret_aliases(void)
{
    char dir[700], secret[800], alias[800], hardlink[800], ordinary[800], json[1800], err[200];
    const char *env = getenv("SCRIBODO_SECRETS");
    char *saved = env ? strdup(env) : NULL;
    snprintf(dir, sizeof(dir), "%s/custom-services", g_dir);
    CHECK(mkdir(dir, 0700) == 0);
    snprintf(secret, sizeof(secret), "%s/qdrant_api_key", dir);
    snprintf(alias, sizeof(alias), "%s/key-alias", g_dir);
    snprintf(hardlink, sizeof(hardlink), "%s/key-hardlink", g_dir);
    snprintf(ordinary, sizeof(ordinary), "%s/ordinary-key", g_dir);
    CHECK(write_private(secret, "local-service-only"));
    CHECK(write_private(ordinary, "remote-provider-only"));
    CHECK(symlink(secret, alias) == 0);
    CHECK(link(secret, hardlink) == 0);
    setenv("SCRIBODO_SECRETS", dir, 1);
    const char *paths[] = { secret, alias, hardlink, ordinary };
    RemoteConfig c;
    for (size_t i = 0; i < 4; i++) {
        snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"m\",\"api_key_file\":\"%s\"}", paths[i]);
        bool ok = remote_parse(json, &c, err, sizeof(err));
        CHECK(i == 3 ? ok && !strcmp(c.key, "remote-provider-only") : !ok && strstr(err, "service secrets") && !c.key[0]);
    }
    if (saved) setenv("SCRIBODO_SECRETS", saved, 1);
    else unsetenv("SCRIBODO_SECRETS");
    free(saved);
}

static void test_remote_file_reads(void)
{
    char path[700], link[700], cfg[700], key[700], json[1600], dangling[700];
    snprintf(path, sizeof(path), "%s/config-fifo", g_dir);
    snprintf(link, sizeof(link), "%s/config-link", g_dir);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(symlink(path, link) == 0);
    snprintf(dangling, sizeof(dangling), "%s/dangling-config", g_dir);
    CHECK(symlink("missing-remote-config", dangling) == 0);
    snprintf(cfg, sizeof(cfg), "%s/file-config.json", g_dir);
    snprintf(key, sizeof(key), "%s/file-key", g_dir);
    snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"m\",\"api_key_file\":\"%s\"}", path);
    FILE *f = fopen(cfg, "wb");
    CHECK(f);
    if (!f) return;
    fputs(json, f);
    fclose(f);
    const char *paths[] = { path, link, g_dir, cfg, dangling };
    /* Bound each regression independently: the old fopen blocks on the FIFO. */
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            alarm(2);
            setenv("SCRIBODO_REMOTE_CONFIG", paths[i], 1);
            RemoteConfig c;
            char err[200] = "";
            bool ok = remote_load(&c, err, sizeof(err));
            _exit(!ok && err[0] && !c.key[0] ? 0 : 1);
        }
        if (pid > 0) {
            int status = 0;
            CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
    }
    RemoteConfig c;
    char err[200] = "";
    snprintf(json, sizeof(json), "{\"base_url\":\"https://api.example.com/v1\",\"model\":\"m\",\"api_key_file\":\"%s\"}", key);
    f = fopen(key, "wb");
    CHECK(f);
    if (!f) return;
    fputs("sk-test\r\n", f);
    fclose(f);
    chmod(key, 0600);
    f = fopen(cfg, "wb");
    if (!f) return;
    fputs(json, f);
    fclose(f);
    setenv("SCRIBODO_REMOTE_CONFIG", cfg, 1);
    CHECK(remote_load(&c, err, sizeof(err)) && !strcmp(c.key, "sk-test"));
    /* Exact boundaries: an ordinary CRLF is fine, a truncated key isn't. */
    char long_key[514];
    memset(long_key, 'k', sizeof(long_key));
    long_key[511] = '\r';
    long_key[512] = '\n';
    f = fopen(key, "wb");
    if (!f) return;
    fwrite(long_key, 1, 513, f);
    fclose(f);
    CHECK(remote_load(&c, err, sizeof(err)) && strlen(c.key) == 511);
    long_key[511] = 'k';
    f = fopen(key, "wb");
    if (!f) return;
    fwrite(long_key, 1, 512, f);
    fclose(f);
    CHECK(!remote_load(&c, err, sizeof(err)) && err[0] && !c.key[0]);
    /* Embedded NULs and an extra key line must not be silently ignored. */
    const char *bad_keys[] = { "sk-test\nsecond-key", "sk-test\0hidden" };
    const size_t bad_lengths[] = { 18, 14 };
    for (size_t i = 0; i < 2; i++) {
        f = fopen(key, "wb");
        if (!f) return;
        fwrite(bad_keys[i], 1, bad_lengths[i], f);
        fclose(f);
        CHECK(!remote_load(&c, err, sizeof(err)) && err[0] && !c.key[0]);
    }
    f = fopen(cfg, "wb");
    if (!f) return;
    fputs(json, f);
    fputc(0, f);
    fputs("hidden", f);
    fclose(f);
    CHECK(!remote_load(&c, err, sizeof(err)) && strstr(err, "completely") && !c.key[0]);
    f = fopen(cfg, "wb");
    if (!f) return;
    fputs(json, f);
    for (size_t i = strlen(json); i <= 64 * 1024; i++) fputc(' ', f);
    fclose(f);
    CHECK(!remote_load(&c, err, sizeof(err)) && strstr(err, "too large") && !c.key[0]);
    unlink(cfg);
    CHECK(!remote_load(&c, err, sizeof(err)) && !err[0] && !c.key[0]);
    unsetenv("SCRIBODO_REMOTE_CONFIG");
}

static void test_remote(void)
{
    char cmd_buf[900];
    RemoteConfig c;
    char err[200];
    /* Configuration: HTTPS (plain http only to this machine), no credentials
     * in the URL, a private key file. */
    CHECK(remote_cfg(&c, "https://api.example.com/v1/", "sk-test", 0600, err, sizeof(err)) && !strcmp(c.host, "api.example.com") &&
          !strcmp(c.base_url, "https://api.example.com/v1") && !strcmp(c.key, "sk-test") && !strcmp(c.name, "Frontier"));
    CHECK(!remote_cfg(&c, "http://api.example.com/v1", "sk-test", 0600, err, sizeof(err)) && strstr(err, "https"));
    CHECK(!remote_cfg(&c, "https://user:pw@api.example.com/v1", "sk-test", 0600, err, sizeof(err)) && strstr(err, "credentials"));
    CHECK(!remote_cfg(&c, "https://api.example.com/v1", "sk-test", 0644, err, sizeof(err)) && strstr(err, "private") && !c.key[0]);
    CHECK(!remote_cfg(&c, "https://api.example.com/v1", "sk test\r\nX-Evil: 1", 0600, err, sizeof(err))); /* no header injection */
    CHECK(!remote_cfg(&c, "https://api.example.com/v1", "sk\x01", 0600, err, sizeof(err)));
    CHECK(!remote_parse("{\"base_url\":\"https://a.b/v1\"}", &c, err, sizeof(err)));
    CHECK(remote_cfg(&c, "http://127.0.0.1:1/v1", "sk-test", 0600, err, sizeof(err)));
    /* Adapting the local request: llama-server extras out, the model in. */
    char *a = remote_adapt_request(&c, "{\"messages\":[],\"stream\":true,\"id_slot\":0,\"cache_prompt\":true,\"repeat_penalty\":1.05,"
                                       "\"chat_template_kwargs\":{\"enable_thinking\":false}}");
    CHECK(a && strstr(a, "\"model\":\"m1\"") && strstr(a, "\"stream\":true") && !strstr(a, "id_slot") && !strstr(a, "cache_prompt") &&
          !strstr(a, "repeat_penalty") && !strstr(a, "chat_template_kwargs"));
    free(a);

    /* A chat through the remote, then back to local: nothing more is sent. */
    RStub st;
    pthread_t th;
    CHECK(rstub_start(&st, RSTUB_OK, &th));
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", st.port);
    CHECK(remote_cfg(&c, url, "sk-test", 0600, err, sizeof(err)));
    ChatSession *s = chat_create(NULL); /* the local model: the fake one */
    DoneCtx d = { 0 };
    chat_on_message_done(s, on_done, &d);
    CHECK(chat_select_remote(s, &c) && chat_remote_selected(s));
    CHECK(chat_send(s, "Tell me", NULL, err, sizeof(err)));
    wait_idle(s, &d, 2);
    chat_lock(s);
    const ChatMessage *m = chat_message(s, 1);
    CHECK(m && !m->error && !strcmp(m->content, "The knight drew his sword."));
    CHECK(m && m->via && !strcmp(m->via, "Frontier \xC2\xB7 127.0.0.1")); /* the reply says where it came from */
    CHECK(chat_message(s, 0)->via && !strcmp(chat_message(s, 0)->via, m->via)); /* and the message where it went */
    chat_unlock(s);
    CHECK(atomic_load(&st.requests) == 1 && strstr(st.last, "POST /v1/chat/completions") &&
          strstr(st.last, "Authorization: Bearer sk-test") && strstr(st.last, "\"model\":\"m1\"") && strstr(st.last, "Tell me") &&
          !strstr(st.last, "id_slot"));
    CHECK(chat_select_remote(s, NULL) && !chat_remote_selected(s));
    d.done = 0;
    CHECK(chat_send(s, "Locally now", NULL, err, sizeof(err)));
    wait_idle(s, &d, 2);
    chat_lock(s);
    m = chat_message(s, 3);
    CHECK(m && strstr(m->content, "fake model") && strstr(m->content, "Locally now"));
    CHECK(m && !m->via && !chat_message(s, 2)->via); /* local messages carry no via */
    /* Saved and reloaded, the marks stay with their messages. */
    ChatMessage saved[4];
    for (int i = 0; i < 4; i++) saved[i] = *chat_message(s, (size_t)i);
    chat_unlock(s);
    char proj[600];
    snprintf(proj, sizeof(proj), "%s/remote-proj", g_dir);
    mkdir(proj, 0755);
    for (int i = 0; i < 4; i++) CHECK(chat_store_append(g_store, proj, "20261104-120000-abcd", "Remote", &saved[i]));
    ChatMessage *back = NULL;
    size_t nb = 0;
    CHECK(chat_store_load(g_store, proj, "20261104-120000-abcd", &back, &nb) && nb == 4 && back[0].via && back[1].via &&
          !strcmp(back[1].via, "Frontier \xC2\xB7 127.0.0.1") && !back[2].via && !back[3].via);
    chat_store_free_messages(back, nb);
    chat_lock(s);
    chat_unlock(s);
    CHECK(atomic_load(&st.requests) == 1);
    chat_destroy(s);
    rstub_stop(&st, th);

    /* A refused key is said plainly; nothing falls back to local. */
    CHECK(rstub_start(&st, RSTUB_401, &th));
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", st.port);
    CHECK(remote_cfg(&c, url, "sk-bad", 0600, err, sizeof(err)));
    s = chat_create(NULL);
    d.done = 0;
    chat_on_message_done(s, on_done, &d);
    CHECK(chat_select_remote(s, &c));
    CHECK(chat_send(s, "Tell me", NULL, err, sizeof(err)));
    wait_idle(s, &d, 2);
    chat_lock(s);
    m = chat_message(s, 1);
    CHECK(m && m->error && strstr(m->content, "refused the API key") && !strstr(m->content, "fake model"));
    chat_unlock(s);
    chat_destroy(s);
    rstub_stop(&st, th);

    /* Red-team 009: remote.json writable by others, or a key file that is
     * one of Scribodo's own secrets, is refused. */
    {
        char cfgp[700], json[1400], sd[700], kf2[800];
        snprintf(cfgp, sizeof(cfgp), "%s/remote.json", g_dir);
        snprintf(sd, sizeof(sd), "%s/Scribodo/services", g_dir);
        snprintf(cmd_buf, sizeof(cmd_buf), "mkdir -p '%s'", sd);
        system(cmd_buf);
        snprintf(kf2, sizeof(kf2), "%s/qdrant_api_key", sd);
        FILE *kfp = fopen(kf2, "wb");
        fputs("not-for-providers", kfp);
        fclose(kfp);
        chmod(kf2, 0600);
        snprintf(json, sizeof(json), "{\"base_url\":\"https://a.example/v1\",\"model\":\"m\",\"api_key_file\":\"%s\"}", kf2);
        CHECK(!remote_parse(json, &c, err, sizeof(err)) && strstr(err, "service secrets"));
        FILE *cf = fopen(cfgp, "wb");
        fputs(json, cf);
        fclose(cf);
        chmod(cfgp, 0666);
        setenv("SCRIBODO_REMOTE_CONFIG", cfgp, 1);
        CHECK(!remote_load(&c, err, sizeof(err)) && strstr(err, "not writable by others"));
        unsetenv("SCRIBODO_REMOTE_CONFIG");
    }

    /* Local turns before switching never reach the remote model. */
    CHECK(rstub_start(&st, RSTUB_OK, &th));
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", st.port);
    CHECK(remote_cfg(&c, url, "sk-test", 0600, err, sizeof(err)));
    s = chat_create(NULL);
    d.done = 0;
    chat_on_message_done(s, on_done, &d);
    CHECK(chat_send(s, "PRIVATE local turn", NULL, err, sizeof(err)));
    wait_idle(s, &d, 2);
    CHECK(chat_select_remote(s, &c));
    CHECK(chat_send(s, "Now remote", NULL, err, sizeof(err)));
    wait_idle(s, &d, 4);
    CHECK(atomic_load(&st.requests) == 1 && strstr(st.last, "Now remote") && !strstr(st.last, "PRIVATE"));
    d.done = 0;
    CHECK(chat_send(s, "Remote again", NULL, err, sizeof(err)));
    wait_idle(s, &d, 2);
    CHECK(atomic_load(&st.requests) == 2 && strstr(st.last, "Now remote") && strstr(st.last, "Remote again") &&
          !strstr(st.last, "PRIVATE")); /* the remote exchange is its context */
    chat_destroy(s);
    rstub_stop(&st, th);

    /* Redirects are never followed (they could take the key elsewhere). */
    CHECK(rstub_start(&st, RSTUB_REDIRECT, &th));
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", st.port);
    CHECK(remote_cfg(&c, url, "sk-test", 0600, err, sizeof(err)));
    HttpResult r;
    remote_request(&c, "/chat/completions", "{}", NULL, NULL, NULL, 5000, &r);
    CHECK(r.status == 302 && atomic_load(&st.requests) == 1);
    http_result_free(&r);
    rstub_stop(&st, th);

    /* The wire-level cap protects non-streamed error/JSON responses too. */
    CHECK(rstub_start(&st, RSTUB_BIG, &th));
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", st.port);
    CHECK(remote_cfg(&c, url, "sk-test", 0600, err, sizeof(err)));
    CHECK(!remote_request(&c, "/chat/completions", "{}", NULL, NULL, NULL, 5000, &r));
    CHECK(strstr(r.error, "safety limit") && r.len <= REMOTE_MAX_RESPONSE_BYTES);
    http_result_free(&r);
    rstub_stop(&st, th);
}


static void test_remote_key_line_boundaries(void)
{
    const struct { const char *key; bool valid; } cases[] = {
        { "", false }, { "\r", false }, { "\n", false }, { "\r\n", false },
        { "\n\r\n", false }, { "token", true }, { "token\n", true },
        { "token\r\n", true }, { "token\r\n\n", true },
        { "token\nsecond", false }, { "token\rx", false }, { "token \r\n", false }
    };
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            RemoteConfig c;
            char err[200] = "";
            size_t errlen = pass == 0 ? sizeof(err) : 0;
            CHECK(remote_cfg(&c, "https://api.example.com/v1", cases[i].key, 0600, err, errlen) == cases[i].valid);
            if (cases[i].valid) CHECK(!strcmp(c.key, "token"));
            else {
                bool zero = true;
                for (size_t k = 0; k < sizeof(c.key); k++) if (c.key[k]) zero = false;
                CHECK(zero);
                if (errlen) CHECK(err[0]);
            }
        }
    }
}

int main(void)
{
    if (!test_state_fixture_init(g_dir, sizeof(g_dir), &g_store)) return 1;
    const TestCase tests[] = { { "remote_file_reads", test_remote_file_reads },
                              { "remote_json_boundaries", test_remote_json_boundaries },
                              { "remote_service_secret_aliases", test_remote_service_secret_aliases },
                              { "remote_key_line_boundaries", test_remote_key_line_boundaries },
                              { "remote", test_remote } };
    int result = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    test_state_fixture_destroy(g_dir, g_store);
    return result;
}
