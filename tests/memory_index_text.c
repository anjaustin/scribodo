/* See memory_index_text.h. */
#include "memory_index_text.h"

#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/utf8.h"

/* Harness code (retired experiment, journal/answer-cycle4_retire_index.md): the flag is read here, not in the product. */
static const char *index_text_file_setting(void)
{
    const char *v = getenv("SCRIBODO_INDEX_TEXT_FILE");
    return v && v[0] ? v : "";
}

#define INDEX_TEXT_FILE_BYTES (64u * 1024u * 1024u)

typedef struct {
    char hash[65];
    char *text; /* owned */
} Entry;

static struct {
    bool enabled;
    Entry *entries;
    size_t count;
} g_table;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static bool hex64(const char *s)
{
    if (!s || strnlen(s, 65) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static void sha_hex(const char *text, size_t n, char out[65])
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(text, (CC_LONG)n, digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static int by_hash(const void *a, const void *b) { return strcmp(((const Entry *)a)->hash, ((const Entry *)b)->hash); }

bool memory_index_text_valid(const char *text, size_t *bytes)
{
    if (!text) return false;
    size_t len = strnlen(text, MEMORY_INDEX_TEXT_BYTES + 1);
    if (!len || len > MEMORY_INDEX_TEXT_BYTES) return false;
    uint32_t *cps = malloc(len * sizeof(*cps));
    if (!cps) return false;
    size_t invalid = 0;
    utf8_decode((const uint8_t *)text, len, cps, &invalid);
    free(cps);
    if (invalid) return false;
    if (bytes) *bytes = len;
    return true;
}

static void drop_locked(void)
{
    for (size_t i = 0; i < g_table.count; i++) free(g_table.entries[i].text);
    free(g_table.entries);
    g_table.entries = NULL;
    g_table.count = 0;
    g_table.enabled = false;
}

void memory_index_text_disable(void)
{
    pthread_mutex_lock(&g_mu);
    drop_locked();
    pthread_mutex_unlock(&g_mu);
}

static char *read_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return NULL;
    }
    size_t cap = 1u << 20, len = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (len < cap) break;
        if (cap >= INDEX_TEXT_FILE_BYTES) {
            free(buf);
            buf = NULL;
            snprintf(err, errlen, "%s is over %u bytes", path, INDEX_TEXT_FILE_BYTES);
            break;
        }
        cap *= 2;
        char *grown = realloc(buf, cap + 1);
        if (!grown) {
            free(buf);
            buf = NULL;
        } else {
            buf = grown;
        }
    }
    fclose(f);
    if (!buf) {
        if (!err[0]) snprintf(err, errlen, "out of memory reading %s", path);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

bool memory_index_text_load(const char *path, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    char *raw = read_file(path, err, errlen);
    if (!raw) return false;
    cJSON *root = cJSON_ParseWithLength(raw, strlen(raw));
    free(raw);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, errlen, "%s is not a JSON object", path);
        return false;
    }
    size_t n = 0;
    for (cJSON *c = root->child; c; c = c->next) n++;
    Entry *entries = n ? calloc(n, sizeof(*entries)) : NULL;
    if (n && !entries) {
        cJSON_Delete(root);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    size_t count = 0;
    bool ok = true;
    for (cJSON *c = root->child; c && ok; c = c->next) {
        size_t bytes;
        if (!hex64(c->string)) {
            snprintf(err, errlen, "key %zu is not a lowercase sha256 hex digest", count);
            ok = false;
        } else if (!cJSON_IsString(c) || !memory_index_text_valid(c->valuestring, &bytes)) {
            snprintf(err, errlen, "entry %.12s… is not strict UTF-8 text of 1..%u bytes", c->string, MEMORY_INDEX_TEXT_BYTES);
            ok = false;
        } else {
            Entry *e = &entries[count];
            memcpy(e->hash, c->string, 65);
            e->text = strdup(c->valuestring);
            ok = e->text != NULL;
            if (!ok) snprintf(err, errlen, "out of memory");
            else count++;
        }
    }
    cJSON_Delete(root);
    if (ok && count) {
        qsort(entries, count, sizeof(*entries), by_hash);
        for (size_t i = 1; i < count && ok; i++)
            if (!strcmp(entries[i - 1].hash, entries[i].hash)) {
                snprintf(err, errlen, "duplicate key %.12s…", entries[i].hash);
                ok = false;
            }
    }
    if (!ok) {
        for (size_t i = 0; i < count; i++) free(entries[i].text);
        free(entries);
        return false;
    }
    pthread_mutex_lock(&g_mu);
    drop_locked();
    g_table.entries = entries;
    g_table.count = count;
    g_table.enabled = true;
    pthread_mutex_unlock(&g_mu);
    return true;
}

static void init_once(void)
{
    const char *path = index_text_file_setting();
    if (!path[0]) return;
    pthread_mutex_lock(&g_mu);
    bool loaded = g_table.enabled; /* a test loaded a table before the first use */
    pthread_mutex_unlock(&g_mu);
    if (loaded) return;
    char err[256];
    if (!memory_index_text_load(path, err, sizeof(err))) {
        fprintf(stderr, "SCRIBODO_INDEX_TEXT_FILE: %s\n", err);
        abort();
    }
}

bool memory_index_text_enabled(void)
{
    pthread_once(&g_once, init_once);
    pthread_mutex_lock(&g_mu);
    bool enabled = g_table.enabled;
    pthread_mutex_unlock(&g_mu);
    return enabled;
}

const char *memory_index_text_for(const char *text)
{
    if (!text || !memory_index_text_enabled()) return NULL;
    Entry key;
    sha_hex(text, strlen(text), key.hash);
    pthread_mutex_lock(&g_mu);
    const Entry *found = g_table.count ? bsearch(&key, g_table.entries, g_table.count, sizeof(key), by_hash) : NULL;
    const char *out = found ? found->text : text;
    pthread_mutex_unlock(&g_mu);
    return out;
}

const char *memory_index_text_match_column(void)
{
    return memory_index_text_enabled() ? "u.index_text" : "u.text";
}
