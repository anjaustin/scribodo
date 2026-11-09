/* The optional remote model: configuration and an HTTPS transport (libcurl,
 * the system's). See remote.h. */
#define _DARWIN_C_SOURCE
#include "remote.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/json.h"

/* Paths catch ordinary mistakes; descriptor identity also catches a hard link
 * outside the service directory. No service credential contents are read. */
static bool service_secret(const char *path, const struct stat *opened)
{
    if (strstr(path, "/Scribodo/services/")) return true;
    char defaults[PATH_MAX];
    int n = snprintf(defaults, sizeof(defaults), "%s/Library/Application Support/Scribodo/services",
                     getenv("HOME") ? getenv("HOME") : "");
    if (n < 0 || (size_t)n >= sizeof(defaults)) return true;
    const char *roots[] = { defaults, getenv("SCRIBODO_SECRETS") };
    const char *names[] = { "db_password", "db_root_password", "qdrant_api_key", "qdrant.env" };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (!roots[i] || !roots[i][0]) continue;
        char canonical[PATH_MAX];
        if (realpath(roots[i], canonical)) {
            size_t len = strlen(canonical);
            if (!strncmp(path, canonical, len) && (path[len] == '/' || path[len] == '\0' || len == 1)) return true;
        }
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            char candidate[PATH_MAX + 64];
            n = snprintf(candidate, sizeof(candidate), "%s/%s", roots[i], names[k]);
            if (n < 0 || (size_t)n >= sizeof(candidate)) return true;
            struct stat st;
            if (stat(candidate, &st) == 0) {
                if (st.st_dev == opened->st_dev && st.st_ino == opened->st_ino) return true;
            } else if (errno != ENOENT && errno != ENOTDIR) {
                return true; /* Can't verify the credential exclusion safely. */
            }
        }
    }
    return false;
}

/* Validate the opened descriptor, not a prior stat of a pathname. O_NONBLOCK
 * prevents a FIFO from hanging startup; only complete, stable regular files
 * are accepted. No truncation of configuration or credentials is safe. */
static char *slurp(const char *path, size_t max, bool key, char *err, size_t errlen)
{
    const char *label = key ? "The API key file" : "remote.json";
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* An absent config is normal; other failures must be visible. */
        bool missing = !key && errno == ENOENT;
        if (missing) {
            struct stat link;
            missing = lstat(path, &link) != 0 && errno == ENOENT;
        }
        if (!missing) snprintf(err, errlen, "Couldn't open %s", label);
        return NULL;
    }
    struct stat before, after;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) {
        snprintf(err, errlen, "%s must be a regular file", label);
        close(fd);
        return NULL;
    }
    if (before.st_uid != getuid() || (before.st_mode & (key ? 077 : 022))) {
        snprintf(err, errlen, key ? "The API key file must be yours and private (chmod 600)" :
                                   "remote.json must be yours and not writable by others (chmod 600)");
        close(fd);
        return NULL;
    }
    if (key && service_secret(path, &before)) {
        snprintf(err, errlen, "The API key file can't be one of Scribodo's own service secrets (or couldn't be checked)");
        close(fd);
        return NULL;
    }
    if (before.st_size < 0 || (uint64_t)before.st_size > max) {
        snprintf(err, errlen, "%s is too large", label);
        close(fd);
        return NULL;
    }
    size_t n = (size_t)before.st_size, off = 0;
    char *buf = malloc(n + 1);
    if (!buf) {
        snprintf(err, errlen, "Not enough memory to read %s", label);
        close(fd);
        return NULL;
    }
    while (off < n) {
        ssize_t got = read(fd, buf + off, n - off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        off += (size_t)got;
    }
    char extra;
    ssize_t tail;
    do tail = read(fd, &extra, 1); while (tail < 0 && errno == EINTR);
    bool stable = fstat(fd, &after) == 0 && before.st_size == after.st_size &&
                  before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
                  before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
    int closed = close(fd);
    if (off != n || tail != 0 || !stable || closed != 0 || memchr(buf, 0, n)) {
        snprintf(err, errlen, "Couldn't read %s completely (changed, damaged or unreadable)", label);
        if (key) memset(buf, 0, n);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
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
    if (errlen) err[0] = '\0';
    cJSON *j = json ? json_parse_complete(json, strlen(json)) : NULL;
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
    if (ok && (strlen(url->valuestring) >= sizeof(c->base_url) || strlen(model->valuestring) >= sizeof(c->model))) {
        snprintf(err, errlen, "remote.json base_url or model is too long");
        ok = false;
    }
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
        char real[PATH_MAX] = "";
        if (!realpath(kf->valuestring, real)) {
            snprintf(err, errlen, "The API key file doesn't exist");
            ok = false;
        } else {
            /* Permit a maximum-length key plus a conventional CRLF, but never
             * silently truncate a long credential or ignore extra key lines. */
            char *k = slurp(real, sizeof(c->key) + 1, true, err, errlen);
            bool read_ok = k != NULL;
            if (!read_ok) ok = false;
            if (k) {
                size_t bytes = strlen(k);
                size_t len = strcspn(k, "\r\n");
                if (!len || len >= sizeof(c->key)) ok = false;
                for (size_t i = len; ok && i < bytes; i++)
                    if (k[i] != '\r' && k[i] != '\n') ok = false;
                if (ok) {
                    memcpy(c->key, k, len);
                    c->key[len] = '\0';
                }
                memset(k, 0, bytes);
                free(k);
            }
            for (const char *p = c->key; ok && *p; p++)
                if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7E) ok = false; /* no header injection */
            if (ok && !c->key[0]) ok = false;
            if (!ok && read_ok) snprintf(err, errlen, "The API key file is empty or not a key");
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
    int n = env ? snprintf(path, sizeof(path), "%s", env) :
                  snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/remote.json", getenv("HOME") ? getenv("HOME") : "");
    if (n < 0 || (size_t)n >= sizeof(path)) {
        snprintf(err, errlen, "The remote configuration path is too long");
        return false;
    }
    char *json = slurp(path, 64 * 1024, false, err, errlen);
    if (!json) return false;
    bool ok = remote_parse(json, c, err, errlen);
    free(json);
    return ok;
}

char *remote_adapt_request(const RemoteConfig *c, const char *local_request)
{
    cJSON *j = json_parse(local_request);
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
    size_t total;
    bool stopped;
    bool too_large;
    atomic_bool *cancel;
} Sink;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_init_result = CURLE_FAILED_INIT;
static void init_curl(void) { curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT); }

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
    if (size && nmemb > SIZE_MAX / size) return 0;
    size_t n = size * nmemb;
    if (s->total > REMOTE_MAX_RESPONSE_BYTES || n > REMOTE_MAX_RESPONSE_BYTES - s->total) {
        s->too_large = true;
        return 0;
    }
    s->total += n;
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
    if (pthread_once(&curl_once, init_curl) != 0 || curl_init_result != CURLE_OK) {
        snprintf(r->error, sizeof(r->error), "Couldn't initialise network support");
        return false;
    }
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
    bool ok = rc == CURLE_OK || (s.stopped && !s.too_large && rc == CURLE_WRITE_ERROR);
    if (!ok)
        snprintf(r->error, sizeof(r->error), "%s",
                 s.too_large ? "response exceeded the 16 MB safety limit" :
                 rc == CURLE_ABORTED_BY_CALLBACK ? "cancelled" : curl_easy_strerror(rc));
    r->body = s.body;
    r->len = s.blen;
    free(s.line);
    curl_slist_free_all(hdr);
    memset(auth, 0, sizeof(auth));
    curl_easy_cleanup(h);
    return ok;
}
