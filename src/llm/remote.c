/* The optional remote model: configuration and an HTTPS transport (libcurl,
 * the system's). See remote.h. */
#define _DARWIN_C_SOURCE
#include "remote.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

static char *slurp(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (buf) buf[n] = '\0';
    return buf;
}

/* https://host[:port]/path, or http://127.0.0.1|localhost[:port]/path. */
static bool parse_url(const char *url, char *host, size_t hostlen, char *err, size_t errlen)
{
    const char *rest;
    bool tls;
    if (!strncasecmp(url, "https://", 8)) rest = url + 8, tls = true;
    else if (!strncasecmp(url, "http://", 7)) rest = url + 7, tls = false;
    else {
        snprintf(err, errlen, "base_url must start with https://");
        return false;
    }
    size_t n = strcspn(rest, ":/?#");
    if (n == 0 || n >= hostlen) {
        snprintf(err, errlen, "base_url has no host");
        return false;
    }
    if (memchr(rest, '@', strcspn(rest, "/?#"))) {
        snprintf(err, errlen, "base_url must not contain credentials");
        return false;
    }
    snprintf(host, hostlen, "%.*s", (int)n, rest);
    if (!tls && strcasecmp(host, "127.0.0.1") != 0 && strcasecmp(host, "localhost") != 0) {
        snprintf(err, errlen, "base_url must use https:// (plain http only to this machine)");
        return false;
    }
    return true;
}

bool remote_parse(const char *json, RemoteConfig *c, char *err, size_t errlen)
{
    memset(c, 0, sizeof(*c));
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        snprintf(err, errlen, "remote.json isn't valid JSON");
        cJSON_Delete(j);
        return false;
    }
    const cJSON *name = cJSON_GetObjectItem(j, "name"), *url = cJSON_GetObjectItem(j, "base_url"),
                *model = cJSON_GetObjectItem(j, "model"), *kf = cJSON_GetObjectItem(j, "api_key_file"),
                *ctx = cJSON_GetObjectItem(j, "context_tokens");
    bool ok = cJSON_IsString(url) && cJSON_IsString(model) && cJSON_IsString(kf) && model->valuestring[0];
    if (!ok) snprintf(err, errlen, "remote.json needs base_url, model and api_key_file");
    ok = ok && strlen(url->valuestring) < sizeof(c->base_url) && strlen(model->valuestring) < sizeof(c->model);
    ok = ok && parse_url(url->valuestring, c->host, sizeof(c->host), err, errlen);
    if (ok) {
        snprintf(c->name, sizeof(c->name), "%s", cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : c->host);
        snprintf(c->base_url, sizeof(c->base_url), "%s", url->valuestring);
        size_t n = strlen(c->base_url);
        while (n > 0 && c->base_url[n - 1] == '/') c->base_url[--n] = '\0';
        snprintf(c->model, sizeof(c->model), "%s", model->valuestring);
        c->context_tokens = cJSON_IsNumber(ctx) && ctx->valuedouble >= 2048 && ctx->valuedouble <= 2e6 ? (int)ctx->valuedouble : 32000;
        /* The key: from a private file of the author's, never from
         * remote.json itself, and never one of Scribodo's own secrets (which
         * would then go to the provider as the key). */
        struct stat st;
        char real[1024] = "", secrets[1024];
        snprintf(secrets, sizeof(secrets), "%s/Library/Application Support/Scribodo/services/", getenv("HOME") ? getenv("HOME") : "");
        if (stat(kf->valuestring, &st) != 0 || !realpath(kf->valuestring, real)) {
            snprintf(err, errlen, "The API key file doesn't exist");
            ok = false;
        } else if (st.st_mode & 077) {
            snprintf(err, errlen, "The API key file must be private (chmod 600)");
            ok = false;
        } else if (st.st_uid != getuid() || !S_ISREG(st.st_mode)) {
            snprintf(err, errlen, "The API key file must be a file of yours");
            ok = false;
        } else if (!strncmp(real, secrets, strlen(secrets)) || strstr(real, "/Scribodo/services/")) {
            snprintf(err, errlen, "The API key file can't be one of Scribodo's own service secrets");
            ok = false;
        } else {
            char *k = slurp(kf->valuestring, sizeof(c->key) - 1);
            if (k) {
                k[strcspn(k, "\r\n")] = '\0';
                snprintf(c->key, sizeof(c->key), "%s", k);
                memset(k, 0, strlen(k));
                free(k);
            }
            for (const char *p = c->key; ok && *p; p++)
                if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7E) ok = false; /* no header injection */
            if (!ok || !c->key[0]) {
                snprintf(err, errlen, "The API key file is empty or not a key");
                ok = false;
            }
        }
    }
    cJSON_Delete(j);
    if (!ok) memset(c->key, 0, sizeof(c->key));
    return ok;
}

bool remote_load(RemoteConfig *c, char *err, size_t errlen)
{
    if (errlen) err[0] = '\0';
    memset(c, 0, sizeof(*c));
    char path[1024];
    const char *env = getenv("SCRIBODO_REMOTE_CONFIG");
    if (env) snprintf(path, sizeof(path), "%s", env);
    else snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/remote.json", getenv("HOME") ? getenv("HOME") : "");
    struct stat st;
    if (stat(path, &st) != 0) return false; /* not configured: the normal case */
    /* Others able to change it could point the key, or the content, anywhere. */
    if (st.st_uid != getuid() || (st.st_mode & 022)) {
        snprintf(err, errlen, "remote.json must be yours and not writable by others (chmod 600)");
        return false;
    }
    char *json = slurp(path, 64 * 1024);
    if (!json) return false;
    bool ok = remote_parse(json, c, err, errlen);
    free(json);
    return ok;
}

char *remote_adapt_request(const RemoteConfig *c, const char *local_request)
{
    cJSON *j = cJSON_Parse(local_request);
    if (!j) return NULL;
    static const char *const local_only[] = { "repeat_penalty", "id_slot", "cache_prompt", "chat_template_kwargs", "seed",
                                              "top_k", "min_p" };
    for (size_t i = 0; i < sizeof(local_only) / sizeof(local_only[0]); i++) cJSON_DeleteItemFromObject(j, local_only[i]);
    cJSON_DeleteItemFromObject(j, "model");
    cJSON_AddStringToObject(j, "model", c->model);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

/* ---- transport ------------------------------------------------------------------ */

typedef struct {
    HttpLineFn on_line;
    void *ctx;
    char *line;
    size_t len, cap;
    char *body;
    size_t blen, bcap;
    bool stopped;
    atomic_bool *cancel;
} Sink;

static bool grow(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    char *b = realloc(*buf, c);
    if (!b) return false;
    *buf = b;
    *cap = c;
    return true;
}

static size_t on_data(char *data, size_t size, size_t nmemb, void *arg)
{
    Sink *s = arg;
    size_t n = size * nmemb;
    if (!s->on_line) {
        if (!grow(&s->body, &s->bcap, s->blen + n + 1)) return 0;
        memcpy(s->body + s->blen, data, n);
        s->blen += n;
        s->body[s->blen] = '\0';
        return n;
    }
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            size_t l = s->len;
            if (l && s->line[l - 1] == '\r') l--;
            if (!grow(&s->line, &s->cap, l + 1)) return 0;
            s->line[l] = '\0';
            if (!s->on_line(s->line, l, s->ctx)) {
                s->stopped = true;
                return 0;
            }
            s->len = 0;
        } else {
            if (s->len >= 4u << 20) return 0; /* a 4 MB line isn't a reply: stop */
            if (!grow(&s->line, &s->cap, s->len + 2)) return 0;
            s->line[s->len++] = data[i];
        }
    }
    return n;
}

static int on_progress(void *arg, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    (void)dt, (void)dn, (void)ut, (void)un;
    Sink *s = arg;
    return s->cancel && atomic_load(s->cancel) ? 1 : 0;
}

bool remote_request(const RemoteConfig *c, const char *path, const char *body, HttpLineFn on_line, void *ctx,
                    atomic_bool *cancel, int timeout_ms, HttpResult *r)
{
    memset(r, 0, sizeof(*r));
    static atomic_bool inited;
    if (!atomic_exchange(&inited, true)) curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *h = curl_easy_init();
    if (!h) {
        snprintf(r->error, sizeof(r->error), "Couldn't start the connection");
        return false;
    }
    char url[1024], auth[600];
    snprintf(url, sizeof(url), "%s%s", c->base_url, path);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->key);
    struct curl_slist *hdr = curl_slist_append(NULL, auth);
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, "Accept: text/event-stream, application/json");
    Sink s = { .on_line = on_line, .ctx = ctx, .cancel = cancel };
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &s);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    /* timeout_ms bounds silence, not the whole reply (replies stream). */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, (long)(timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1));
    /* No redirects (a redirect could take the key elsewhere), no proxies
     * from the environment for loopback, TLS verified. */
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (!strcasecmp(c->host, "127.0.0.1") || !strcasecmp(c->host, "localhost")) curl_easy_setopt(h, CURLOPT_NOPROXY, "*");
    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    r->status = (int)status;
    /* A last line without a newline. */
    if (rc == CURLE_OK && on_line && s.len && !s.stopped) {
        s.line[s.len] = '\0';
        on_line(s.line, s.len, ctx);
    }
    bool ok = rc == CURLE_OK || (s.stopped && rc == CURLE_WRITE_ERROR);
    if (!ok) snprintf(r->error, sizeof(r->error), "%s", rc == CURLE_ABORTED_BY_CALLBACK ? "cancelled" : curl_easy_strerror(rc));
    r->body = s.body;
    r->len = s.blen;
    free(s.line);
    curl_slist_free_all(hdr);
    memset(auth, 0, sizeof(auth));
    curl_easy_cleanup(h);
    return ok;
}
