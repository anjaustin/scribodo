#define _DARWIN_C_SOURCE
#include "observer.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app/chat_store.h"
#include "app/revisions.h"
#include "cJSON.h"
#include "llm/http.h"
#include "llm/llm_text.h"

#define CHUNK_MAX 3000        /* bytes of new text per extraction request */
#define MAX_SEGS 64
#define IDLE_DELAY 1.5        /* seconds of chat idleness before observer work */
#define RETRY_DELAY 5.0       /* after a failed request */
#define MAX_JUDGE_PAIRS 6     /* canon checks per job (each is two votes) */
#define MAX_DOC_BYTES (4u << 20)

/* All finding writes (observer worker and UI status changes) take this. */
static pthread_mutex_t g_findings_mu = PTHREAD_MUTEX_INITIALIZER;

struct Observer {
    LlmServer *server;
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop, poked, enabled, running;
    char project[PATH_MAX];
    uint64_t gen;            /* bumps on project change: stale results are discarded */
    double idle_since;       /* last time chat was active */
    double retry_at;
    atomic_bool cancel;
    atomic_bool chat_active;
    char status[200];
    uint64_t version;
    unsigned jobs_done;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_status(Observer *o, const char *s)
{
    pthread_mutex_lock(&o->mu);
    snprintf(o->status, sizeof(o->status), "%s", s);
    pthread_mutex_unlock(&o->mu);
}

/* ---- text helpers ------------------------------------------------------------------ */

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

static size_t cp_count(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

static bool valid_utf8(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned char c = u[i];
        size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!k || i + k > n || (k == 2 && c < 0xC2)) return false;
        for (size_t j = 1; j < k; j++)
            if ((u[i + j] & 0xC0) != 0x80) return false;
        i += k;
    }
    return true;
}

static char *dupn(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

size_t obs_paragraphs(const char *text, size_t *starts, size_t *lens, size_t max)
{
    size_t n = strlen(text), k = 0, i = 0;
    while (i < n && k < max) {
        while (i < n && (text[i] == '\n' || text[i] == '\r' || text[i] == ' ' || text[i] == '\t')) i++;
        if (i >= n) break;
        size_t s = i;
        /* up to a blank line */
        while (i < n) {
            if (text[i] == '\n') {
                size_t j = i + 1;
                while (j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r')) j++;
                if (j >= n || text[j] == '\n') break;
            }
            i++;
        }
        size_t e = i;
        while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r' || text[e - 1] == '\n')) e--;
        starts[k] = s;
        lens[k] = e - s;
        k++;
    }
    return k;
}

/* Normalized form for matching: curly quotes -> straight, whitespace runs ->
 * one space, emphasis markers and ZWNJ dropped, ASCII lowercased. map[i] /
 * map_end[i] give the source byte range of normalized byte i. */
static size_t normalize(const char *s, size_t n, char *out, size_t *map, size_t *map_end)
{
    size_t k = 0;
    bool space = false;
    for (size_t i = 0; i < n;) {
        const unsigned char *u = (const unsigned char *)s + i;
        size_t clen = u[0] < 0x80 ? 1 : (u[0] & 0xE0) == 0xC0 ? 2 : (u[0] & 0xF0) == 0xE0 ? 3 : 4;
        if (i + clen > n) clen = n - i;
        char rep = 0;
        bool skip = false;
        if (u[0] == ' ' || u[0] == '\n' || u[0] == '\t' || u[0] == '\r') {
            if (!space && k > 0) rep = ' ';
            else skip = true;
            space = true;
        } else {
            space = false;
            if (u[0] == '*' || u[0] == '_') skip = true;
            else if (clen == 3 && u[0] == 0xE2 && u[1] == 0x80 && u[2] == 0x8C) skip = true; /* ZWNJ */
            else if (clen == 3 && u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x98 || u[2] == 0x99)) rep = '\'';
            else if (clen == 3 && u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x9C || u[2] == 0x9D)) rep = '"';
            else if (clen == 2 && u[0] == 0xC2 && (u[1] == 0xAB || u[1] == 0xBB)) rep = '"';
        }
        if (!skip) {
            if (rep) {
                out[k] = rep;
                map[k] = i;
                map_end[k] = i + clen;
                k++;
            } else {
                for (size_t j = 0; j < clen; j++) {
                    char c = s[i + j];
                    out[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                    map[k] = i;
                    map_end[k] = i + clen;
                    k++;
                }
            }
        }
        i += clen;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
    return k;
}

static const char *memfind(const char *h, size_t hn, const char *n, size_t nn)
{
    if (nn == 0 || nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return NULL;
}

static bool locate_once(const char *text, size_t n, const char *evidence, size_t en, size_t *start, size_t *len);

bool obs_locate(const char *text, size_t n, const char *evidence, size_t *start, size_t *len)
{
    if (!evidence) return false;
    /* As given (an opening quote mark is often part of the passage), then
     * without quote marks the model wrapped around it. */
    while (*evidence == ' ' || *evidence == '\n') evidence++;
    size_t en = strlen(evidence);
    while (en && (evidence[en - 1] == ' ' || evidence[en - 1] == '\n')) en--;
    if (locate_once(text, n, evidence, en, start, len)) return true;
    while (en && (*evidence == '"' || *evidence == '\'')) {
        evidence++;
        en--;
    }
    while (en && (evidence[en - 1] == '"' || evidence[en - 1] == '\'')) en--;
    return locate_once(text, n, evidence, en, start, len);
}

static bool locate_once(const char *text, size_t n, const char *evidence, size_t en, size_t *start, size_t *len)
{
    if (en < 2) return false;
    const char *hit = memfind(text, n, evidence, en);
    if (hit) {
        *start = (size_t)(hit - text);
        *len = en;
        return true;
    }
    char *nt = malloc(n * 1 + 1), *ne = malloc(en + 1);
    size_t *map = malloc((n + 1) * sizeof(size_t)), *map_end = malloc((n + 1) * sizeof(size_t));
    size_t *emap = malloc((en + 1) * sizeof(size_t)), *emap_end = malloc((en + 1) * sizeof(size_t));
    bool ok = false;
    if (nt && ne && map && map_end && emap && emap_end) {
        size_t tn = normalize(text, n, nt, map, map_end);
        size_t evn = normalize(evidence, en, ne, emap, emap_end);
        /* Full match, then ever shorter word prefixes (at least 4 words and
         * 20 bytes): models often paraphrase the end of a quote. */
        size_t try_len = evn;
        int words = 1;
        for (size_t i = 0; i < evn; i++) words += ne[i] == ' ';
        while (try_len >= 2 && !ok) {
            const char *h = memfind(nt, tn, ne, try_len);
            if (h && (try_len == evn || try_len >= 20)) {
                size_t a = (size_t)(h - nt), z = a + try_len - 1;
                *start = map[a];
                *len = map_end[z] - map[a];
                ok = true;
                break;
            }
            if (words <= 4) break;
            /* drop the last word */
            size_t cut = try_len;
            while (cut > 0 && ne[cut - 1] != ' ') cut--;
            if (cut == 0) break;
            try_len = cut - 1;
            words--;
        }
    }
    free(nt);
    free(ne);
    free(map);
    free(map_end);
    free(emap);
    free(emap_end);
    return ok;
}

/* ---- overlap ---------------------------------------------------------------------- */

static bool word_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80;
}

static bool is_upper_start(const unsigned char *w)
{
    return (w[0] >= 'A' && w[0] <= 'Z') || (w[0] == 0xC3 && w[1] >= 0x80 && w[1] <= 0x9E) ||
           (w[0] == 0xC5 && (w[1] & 1) == 0 && w[1] >= 0x80);
}

static bool stopword(const char *w, size_t n)
{
    static const char *const stop[] = {
        "the", "and", "that", "with", "this", "from", "have", "were", "they", "their", "there", "what", "when",
        "which", "would", "could", "should", "about", "into", "over", "only", "then", "than", "them", "your",
        "will", "been", "being", "does", "said", "says", "like", "just", "very", "also", "here", "she", "her",
        "his", "him", "its", "for", "not", "but", "you", "are", "was", "had", "has", "one", "who", "how", "why",
        "is", "a", "an", "of", "to", "in", "on", "at", "it", "he", "as", "by", "or", "be",
    };
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); i++)
        if (strlen(stop[i]) == n && !strncasecmp(stop[i], w, n)) return true;
    return false;
}

typedef struct {
    char w[48];
    bool name;
} Tok;

static size_t tokens(const char *s, Tok *out, size_t max)
{
    size_t k = 0;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; u[i] && k < max;) {
        while (u[i] && !word_byte(u[i])) i++;
        size_t a = i;
        while (u[i] && (word_byte(u[i]) || (u[i] == '\'' && word_byte(u[i + 1])) ||
                        (u[i] == 0xE2 && u[i + 1] == 0x80 && u[i + 2] == 0x99)))
            i += (u[i] == 0xE2) ? 3 : 1;
        size_t n = i - a;
        if (n == 0) continue;
        /* strip a possessive ('s / ’s) */
        if (n > 2 && u[a + n - 1] == 's' && u[a + n - 2] == '\'') n -= 2;
        else if (n > 4 && u[a + n - 1] == 's' && u[a + n - 2] == 0x99 && u[a + n - 3] == 0x80) n -= 4;
        if (n >= sizeof(out[0].w) || stopword(s + a, n)) continue;
        bool name = is_upper_start(u + a);
        if (!name && cp_count(s + a, n) < 4) continue;
        bool dup = false;
        for (size_t j = 0; j < k && !dup; j++) dup = strlen(out[j].w) == n && !strncasecmp(out[j].w, s + a, n);
        if (dup) continue;
        memcpy(out[k].w, s + a, n);
        out[k].w[n] = '\0';
        out[k].name = name;
        k++;
    }
    return k;
}

int obs_overlap(const char *a, const char *b)
{
    Tok ta[64], tb[64];
    size_t na = tokens(a, ta, 64), nb = tokens(b, tb, 64);
    int score = 0;
    for (size_t i = 0; i < na; i++)
        for (size_t j = 0; j < nb; j++)
            if (!strcasecmp(ta[i].w, tb[j].w)) {
                score += ta[i].name || tb[j].name ? 2 : 1;
                break;
            }
    return score;
}

/* ---- fake backend ------------------------------------------------------------------- */

/* "<Name>'s <attr> is|are <value>" -> fields (all lowercased except name). */
static bool attr_pattern(const char *s, char *name, char *attr, char *value)
{
    const char *p = strstr(s, "'s ");
    size_t plen = 3;
    const char *q = strstr(s, "\xE2\x80\x99s ");
    if (!p || (q && q < p)) {
        p = q;
        plen = 5;
    }
    if (!p) return false;
    const char *ns = p;
    while (ns > s && word_byte((unsigned char)ns[-1])) ns--;
    if (p - ns < 2 || p - ns > 40) return false;
    snprintf(name, 48, "%.*s", (int)(p - ns), ns);
    const char *a = p + plen;
    int an = 0;
    while (a[an] && word_byte((unsigned char)a[an])) an++;
    if (!an || an > 40) return false;
    snprintf(attr, 48, "%.*s", an, a);
    const char *v = a + an;
    if (!strncmp(v, " is ", 4)) v += 4;
    else if (!strncmp(v, " are ", 5)) v += 5;
    else return false;
    int vn = 0;
    while (v[vn] && word_byte((unsigned char)v[vn])) vn++;
    if (!vn || vn > 40) return false;
    snprintf(value, 48, "%.*s", vn, v);
    return true;
}

static void add_finding(cJSON *arr, const char *type, const char *desc, const char *ev, const char *subject)
{
    if (cJSON_GetArraySize(arr) >= 8) return;
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", type);
    cJSON_AddStringToObject(f, "subject", subject ? subject : "");
    cJSON_AddStringToObject(f, "description", desc);
    cJSON_AddStringToObject(f, "evidence", ev);
    cJSON_AddItemToArray(arr, f);
}

char *obs_fake_extract(const char *chunk)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "findings");
    size_t n = strlen(chunk);
    /* Sentences: questions, and attribute statements. */
    for (size_t i = 0; i < n;) {
        while (i < n && (chunk[i] == ' ' || chunk[i] == '\n')) i++;
        size_t s = i;
        while (i < n && chunk[i] != '.' && chunk[i] != '?' && chunk[i] != '!' && chunk[i] != '\n') i++;
        if (i < n && chunk[i] != '\n') i++;
        if (i - s < 3) continue;
        char *sent = dupn(chunk + s, i - s);
        if (!sent) break;
        char nm[48], at[48], va[48];
        if (sent[strlen(sent) - 1] == '?') add_finding(arr, "question", sent, sent, "");
        else if (attr_pattern(sent, nm, at, va)) add_finding(arr, "event", sent, sent, nm);
        free(sent);
    }
    /* Test hook: a finding whose evidence is not in the text (as a model
     * might invent), which must never be recorded. */
    if (strstr(chunk, "[[invent]]")) add_finding(arr, "event", "Ingrid sails away.", "Ingrid sailed away at dawn.", "Ingrid");
    /* Names: capitalised words that aren't sentence starts or ALL CAPS. */
    char seen[16][48];
    size_t nseen = 0;
    const unsigned char *u = (const unsigned char *)chunk;
    for (size_t i = 0; i < n;) {
        while (i < n && !word_byte(u[i])) i++;
        size_t a = i;
        while (i < n && word_byte(u[i])) i++;
        size_t len = i - a;
        if (len < 3 || len >= 48 || !is_upper_start(u + a)) continue;
        size_t b = a;
        while (b > 0 && (u[b - 1] == ' ' || u[b - 1] == '"' || u[b - 1] == '\'' || u[b - 1] == 0x9C)) b--;
        bool initial = b == 0 || u[b - 1] == '.' || u[b - 1] == '?' || u[b - 1] == '!' || u[b - 1] == '\n' || u[b - 1] == ':';
        bool caps = len > 1 && u[a + 1] >= 'A' && u[a + 1] <= 'Z';
        /* A sentence may start with a name ("Dvořák waited."), but not with
         * a common word ("The", "Should"). */
        static const char *const starters[] = { "Then", "When", "There", "This", "That", "What", "Should", "Could",
                                                "Would", "Does", "Did", "Will", "Can", "After", "Before", "Where" };
        bool starter = false;
        for (size_t q = 0; q < sizeof(starters) / sizeof(starters[0]) && !starter; q++)
            starter = strlen(starters[q]) == len && !strncmp(starters[q], chunk + a, len);
        if (stopword(chunk + a, len) || (initial && starter) || caps) continue;
        char w[48];
        snprintf(w, sizeof(w), "%.*s", (int)len, chunk + a);
        bool dup = false;
        for (size_t k = 0; k < nseen && !dup; k++) dup = !strcmp(seen[k], w);
        if (dup || nseen == 16) continue;
        snprintf(seen[nseen++], 48, "%s", w);
        char desc[96];
        snprintf(desc, sizeof(desc), "%s is mentioned.", w);
        add_finding(arr, "entity", desc, w, w);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *obs_fake_judge(const char *canon, const char *statement)
{
    char n1[48], a1[48], v1[48], n2[48], a2[48], v2[48];
    bool conflict = attr_pattern(canon, n1, a1, v1) && attr_pattern(statement, n2, a2, v2) && !strcasecmp(n1, n2) &&
                    !strcasecmp(a1, a2) && strcasecmp(v1, v2) != 0;
    cJSON *root = cJSON_CreateObject();
    char check[300];
    if (conflict) snprintf(check, sizeof(check), "Canon says %s's %s: %s; the new text says %s.", n1, a1, v1, v2);
    else snprintf(check, sizeof(check), "No contradiction found.");
    cJSON_AddStringToObject(root, "check", check);
    cJSON_AddStringToObject(root, "verdict", conflict ? "conflict" : "unrelated");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* ---- model backend ------------------------------------------------------------------ */

static const char *k_extract_system =
    "You are a careful continuity assistant for a novelist. Read the passage and list what it establishes about the "
    "story. Use only what the passage states; never invent. For each finding give its type: entity (a named "
    "character, place or object), event (something that happens), relationship (how two named entities relate), or "
    "question (something the text leaves open). subject: the name of the main entity it is about. description: one "
    "short factual sentence that uses names, not pronouns. Relationships must link two named entities. evidence: copy the exact words from the passage that support it (a short phrase or sentence, "
    "verbatim). For relationships also give relation and object. At most 8 findings; fewer is fine.";

static const char *k_judge_system =
    "You check a novel for continuity errors. CANON is a fact the author has established. NEW is from the latest "
    "text. First, in check, say what NEW states or implies about the same person, place or thing as CANON (details "
    "such as appearance, dates, family ties, injuries, or whether something happened before count). Then verdict: "
    "conflict if CANON and NEW cannot both be true; consistent if they agree; unrelated if NEW says nothing about "
    "what CANON establishes.";

static cJSON *extract_schema(void)
{
    const char *js =
        "{\"type\":\"object\",\"properties\":{\"findings\":{\"type\":\"array\",\"maxItems\":8,\"items\":{"
        "\"type\":\"object\",\"properties\":{"
        "\"type\":{\"type\":\"string\",\"enum\":[\"entity\",\"event\",\"relationship\",\"question\"]},"
        "\"subject\":{\"type\":\"string\",\"maxLength\":60},"
        "\"description\":{\"type\":\"string\",\"maxLength\":200},"
        "\"evidence\":{\"type\":\"string\",\"maxLength\":300},"
        "\"relation\":{\"type\":\"string\",\"maxLength\":40},"
        "\"object\":{\"type\":\"string\",\"maxLength\":60}},"
        "\"required\":[\"type\",\"subject\",\"description\",\"evidence\"]}}},\"required\":[\"findings\"]}";
    return cJSON_Parse(js);
}

static cJSON *judge_schema(void)
{
    /* "check" comes first: the model explains before it decides (measured:
     * conflict recall 4/6 -> 6/6). */
    return cJSON_Parse("{\"type\":\"object\",\"properties\":{\"check\":{\"type\":\"string\",\"maxLength\":300},"
                       "\"verdict\":{\"type\":\"string\",\"enum\":[\"conflict\",\"consistent\",\"unrelated\"]}},"
                       "\"required\":[\"check\",\"verdict\"]}");
}

/* One constrained completion; returns the content (malloc'd) or NULL. */
static char *complete(Observer *o, const char *system, const char *user, cJSON *schema, int max_tokens)
{
    if (!o->server || llm_server_state(o->server, NULL, 0) != LLM_READY) {
        cJSON_Delete(schema);
        return NULL;
    }
    char *sys_n = llm_neutralize(system), *user_n = llm_neutralize(user);
    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m1 = cJSON_CreateObject(), *m2 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "system");
    cJSON_AddStringToObject(m1, "content", sys_n ? sys_n : "");
    cJSON_AddStringToObject(m2, "role", "user");
    cJSON_AddStringToObject(m2, "content", user_n ? user_n : "");
    cJSON_AddItemToArray(msgs, m1);
    cJSON_AddItemToArray(msgs, m2);
    free(sys_n);
    free(user_n);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    cJSON_AddNumberToObject(root, "temperature", 0.0);
    cJSON *rf = cJSON_AddObjectToObject(root, "response_format");
    cJSON_AddStringToObject(rf, "type", "json_schema");
    cJSON *js = cJSON_AddObjectToObject(rf, "json_schema");
    cJSON_AddStringToObject(js, "name", "observer");
    cJSON_AddItemToObject(js, "schema", schema);
    cJSON *kw = cJSON_AddObjectToObject(root, "chat_template_kwargs");
    cJSON_AddBoolToObject(kw, "enable_thinking", false);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return NULL;
    HttpResult r;
    char *content = NULL;
    if (http_request(llm_server_socket(o->server), "POST", "/v1/chat/completions", llm_server_key(o->server), body, NULL,
                     NULL, &o->cancel, 180000, &r) &&
        r.status == 200 && r.body) {
        cJSON *j = cJSON_Parse(r.body);
        cJSON *ch = j ? cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0) : NULL;
        cJSON *c = ch ? cJSON_GetObjectItem(cJSON_GetObjectItem(ch, "message"), "content") : NULL;
        if (cJSON_IsString(c)) content = strdup(c->valuestring);
        cJSON_Delete(j);
    }
    http_result_free(&r);
    free(body);
    return content;
}

static char *backend_extract(Observer *o, const char *chunk)
{
    if (!o->server) {
        usleep(20000);
        return atomic_load(&o->cancel) ? NULL : obs_fake_extract(chunk);
    }
    size_t n = strlen(chunk) + 16;
    char *user = malloc(n);
    if (!user) return NULL;
    snprintf(user, n, "PASSAGE:\n%s", chunk);
    char *out = complete(o, k_extract_system, user, extract_schema(), 1400);
    free(user);
    return out;
}

/* Two votes, CANON/NEW in both orders; a conflict only if both agree
 * (measured: removes most false alarms without losing real conflicts). */
static bool backend_judge(Observer *o, const char *canon, const char *statement, char *check, size_t checklen)
{
    bool votes = true;
    check[0] = '\0';
    for (int pass = 0; pass < 2 && votes; pass++) {
        char *out = NULL;
        if (!o->server) {
            out = obs_fake_judge(canon, statement);
        } else {
            size_t n = strlen(canon) + strlen(statement) + 32;
            char *user = malloc(n);
            if (!user) return false;
            if (pass == 0) snprintf(user, n, "CANON: %s\nNEW: %s", canon, statement);
            else snprintf(user, n, "NEW: %s\nCANON: %s", statement, canon);
            out = complete(o, k_judge_system, user, judge_schema(), 260);
            free(user);
        }
        cJSON *j = out ? cJSON_Parse(out) : NULL;
        cJSON *v = j ? cJSON_GetObjectItem(j, "verdict") : NULL, *c = j ? cJSON_GetObjectItem(j, "check") : NULL;
        votes = cJSON_IsString(v) && !strcmp(v->valuestring, "conflict");
        if (votes && pass == 0 && cJSON_IsString(c)) snprintf(check, checklen, "%s", c->valuestring);
        cJSON_Delete(j);
        free(out);
        if (atomic_load(&o->cancel)) return false;
    }
    return votes;
}

/* ---- state ------------------------------------------------------------------------ */

static void state_path(const char *project, char *out, size_t outlen, bool create)
{
    char a[PATH_MAX + 32], b[PATH_MAX + 64];
    snprintf(a, sizeof(a), "%s/.scribodo", project);
    snprintf(b, sizeof(b), "%s/observer", a);
    if (create) {
        mkdir(a, 0755);
        mkdir(b, 0755);
    }
    snprintf(out, outlen, "%s/state.json", b);
}

static cJSON *state_load(const char *project)
{
    char path[PATH_MAX + 128];
    state_path(project, path, sizeof(path), false);
    FILE *f = fopen(path, "rb");
    cJSON *j = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = n > 0 && n < (64l << 20) ? malloc((size_t)n + 1) : NULL;
        if (buf) {
            buf[fread(buf, 1, (size_t)n, f)] = '\0';
            j = cJSON_Parse(buf);
            free(buf);
        }
        fclose(f);
    }
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        j = cJSON_CreateObject();
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(j, "docs"))) cJSON_AddObjectToObject(j, "docs");
    if (!cJSON_IsObject(cJSON_GetObjectItem(j, "chats"))) cJSON_AddObjectToObject(j, "chats");
    return j;
}

static bool state_save(const char *project, cJSON *st)
{
    char path[PATH_MAX + 128], tmp[PATH_MAX + 160];
    state_path(project, path, sizeof(path), true);
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    char *s = cJSON_PrintUnformatted(st);
    if (!s) return false;
    int fd = mkstemp(tmp);
    bool ok = fd >= 0;
    size_t n = strlen(s);
    if (ok) {
        ok = write(fd, s, n) == (ssize_t)n && fsync(fd) == 0;
        ok = close(fd) == 0 && ok;
        ok = ok && rename(tmp, path) == 0;
        if (!ok) unlink(tmp);
    }
    free(s);
    return ok;
}

static bool hash_seen(cJSON *arr, uint64_t h)
{
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    if (cJSON_IsString(it) && !strcmp(it->valuestring, hex)) return true;
    return false;
}

/* ---- jobs ------------------------------------------------------------------------- */

typedef struct {
    size_t chunk_off, src_off, len; /* bytes */
    long message;                   /* chat message index, or -1 */
} Seg;

typedef struct {
    bool is_doc;
    char name[256];       /* document file name, or chat id */
    char path[PATH_MAX + 300];
    char *src;            /* document text (doc jobs) */
    char **msgs;          /* message texts (chat jobs), by index */
    size_t nmsgs;
    size_t msg_to;        /* chat: messages read after this job */
    char *chunk;
    size_t chunk_len;
    Seg segs[MAX_SEGS];
    size_t nsegs;
    uint64_t hashes[MAX_SEGS * 4];
    size_t nhashes;
} Job;

static void job_free(Job *j)
{
    free(j->src);
    for (size_t i = 0; i < j->nmsgs; i++) free(j->msgs[i]);
    free(j->msgs);
    free(j->chunk);
    memset(j, 0, sizeof(*j));
}

static bool chunk_add(Job *j, const char *label, const char *text, size_t src_off, size_t len, long message)
{
    if (j->nsegs == MAX_SEGS) return false;
    if (j->chunk_len && j->chunk_len + len > CHUNK_MAX) return false;
    if (len > CHUNK_MAX) { /* an overlong paragraph: read its start */
        len = CHUNK_MAX;
        while (len > 0 && ((unsigned char)text[src_off + len] & 0xC0) == 0x80) len--;
    }
    size_t ll = label ? strlen(label) : 0;
    char *c = realloc(j->chunk, j->chunk_len + ll + len + 3);
    if (!c) return false;
    j->chunk = c;
    if (j->chunk_len) {
        memcpy(j->chunk + j->chunk_len, "\n\n", 2);
        j->chunk_len += 2;
    }
    memcpy(j->chunk + j->chunk_len, label, ll);
    j->chunk_len += ll;
    j->segs[j->nsegs++] = (Seg){ j->chunk_len, src_off, len, message };
    memcpy(j->chunk + j->chunk_len, text + src_off, len);
    j->chunk_len += len;
    j->chunk[j->chunk_len] = '\0';
    return true;
}

static char *read_file(const char *path, size_t *n)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > MAX_DOC_BYTES) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (buf) buf[got] = '\0';
    *n = got;
    return buf;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The next piece of unread stored text, or false if all is read. Marks
 * blank paragraphs read in `st` as it goes. */
static bool find_job(const char *project, cJSON *st, Job *j)
{
    memset(j, 0, sizeof(*j));
    cJSON *docs = cJSON_GetObjectItem(st, "docs"), *chats = cJSON_GetObjectItem(st, "chats");
    /* Documents, by name. */
    DIR *d = opendir(project);
    char *names[256];
    size_t nn = 0;
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL && nn < 256) {
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if ((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")))
            names[nn++] = strdup(ent->d_name);
    }
    if (d) closedir(d);
    qsort(names, nn, sizeof(char *), cmp_names);
    bool found = false;
    for (size_t i = 0; i < nn && !found; i++) {
        if (!names[i]) continue;
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", project, names[i]);
        size_t len = 0;
        char *text = read_file(path, &len);
        if (!text || strlen(text) != len || !valid_utf8(text, len)) { /* not a readable UTF-8 text file */
            free(text);
            continue;
        }
        cJSON *seen = cJSON_GetObjectItem(docs, names[i]);
        static size_t ps[4096], pl[4096];
        size_t np = obs_paragraphs(text, ps, pl, 4096);
        for (size_t p = 0; p < np; p++) {
            uint64_t h = fnv(text + ps[p], pl[p]);
            if (seen && hash_seen(seen, h)) continue;
            if (!chunk_add(j, NULL, text, ps[p], pl[p], -1)) break;
            if (j->nhashes < sizeof(j->hashes) / sizeof(j->hashes[0])) j->hashes[j->nhashes++] = h;
        }
        if (j->nsegs) {
            j->is_doc = true;
            snprintf(j->name, sizeof(j->name), "%s", names[i]);
            snprintf(j->path, sizeof(j->path), "%s", path);
            j->src = text;
            found = true;
        } else {
            free(text);
        }
    }
    for (size_t i = 0; i < nn; i++) free(names[i]);
    if (found) return true;

    /* Chats: messages beyond the count already read. */
    ChatSummary *list = NULL;
    size_t nc = chat_store_list(project, &list);
    for (size_t i = 0; i < nc && !found; i++) {
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load(project, list[i].id, &msgs, &n)) continue;
        cJSON *cnt = cJSON_GetObjectItem(chats, list[i].id);
        size_t from = cJSON_IsNumber(cnt) && cnt->valuedouble >= 0 ? (size_t)cnt->valuedouble : 0;
        if (from < n) {
            j->msgs = calloc(n, sizeof(char *));
            j->nmsgs = j->msgs ? n : 0;
            for (size_t k = 0; k < j->nmsgs; k++) j->msgs[k] = strdup(msgs[k].content ? msgs[k].content : "");
            size_t k = from;
            for (; k < n; k++) {
                if (msgs[k].error || !msgs[k].content || !msgs[k].content[0] || !j->msgs[k]) continue;
                const char *label = msgs[k].role == CHAT_USER ? "AUTHOR: " : "ASSISTANT: ";
                if (!chunk_add(j, label, j->msgs[k], 0, strlen(j->msgs[k]), (long)k)) break;
            }
            j->msg_to = k;
            snprintf(j->name, sizeof(j->name), "%s", list[i].id);
            found = true;
            if (!j->nsegs) { /* only failed/empty messages: just mark them read */
                cJSON_DeleteItemFromObject(chats, list[i].id);
                cJSON_AddNumberToObject(chats, list[i].id, (double)j->msg_to);
                job_free(j);
                found = false;
                memset(j, 0, sizeof(*j));
            }
        }
        chat_store_free_messages(msgs, n);
    }
    free(list);
    return found;
}

typedef struct {
    char type[24];
    char *desc;
    FindingInfo info;
    SourceRef src;
    bool keep;
} Cand;

static void norm_key(const char *s, char *out, size_t outlen)
{
    size_t k = 0;
    bool sep = false;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 2 < outlen; p++) {
        if (word_byte(*p)) {
            if (sep && k) out[k++] = ' ';
            out[k++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : (char)*p;
            sep = false;
        } else {
            sep = true;
        }
    }
    out[k] = '\0';
}

static void finding_key(const char *type, const char *subject, const char *rel, const char *obj, const char *desc,
                        char *out, size_t outlen)
{
    char a[200], b[200];
    if (!strcmp(type, "entity") && subject[0]) {
        norm_key(subject, a, sizeof(a));
        snprintf(out, outlen, "entity|%s", a);
    } else if (!strcmp(type, "relationship") && subject[0] && obj[0]) {
        norm_key(subject, a, sizeof(a));
        norm_key(obj, b, sizeof(b));
        char r[100];
        norm_key(rel, r, sizeof(r));
        snprintf(out, outlen, "relationship|%s|%s|%s", a, r, b);
    } else {
        norm_key(desc, a, sizeof(a));
        snprintf(out, outlen, "%s|%s", type, a);
    }
}

/* Turn the model's JSON into located candidates. */
static size_t parse_candidates(const Job *j, const char *json, const char *revision, Cand *out, size_t max)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *arr = root ? cJSON_GetObjectItem(root, "findings") : NULL;
    size_t k = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (k == max) break;
        cJSON *type = cJSON_GetObjectItem(it, "type"), *desc = cJSON_GetObjectItem(it, "description"),
              *ev = cJSON_GetObjectItem(it, "evidence");
        if (!cJSON_IsString(type) || !cJSON_IsString(desc) || !cJSON_IsString(ev) || !desc->valuestring[0]) continue;
        const char *t = type->valuestring;
        if (strcmp(t, "entity") && strcmp(t, "event") && strcmp(t, "relationship") && strcmp(t, "question")) continue;
        size_t s = 0, len = 0;
        if (!obs_locate(j->chunk, j->chunk_len, ev->valuestring, &s, &len)) continue; /* unsupported: dropped */
        /* The segment (paragraph or message) the evidence falls in, clipped
         * to it: a match may start in an "AUTHOR:" label or run past the end. */
        const Seg *seg = NULL;
        for (size_t q = 0; q < j->nsegs && !seg; q++)
            if (s < j->segs[q].chunk_off + j->segs[q].len && s + len > j->segs[q].chunk_off) seg = &j->segs[q];
        if (!seg) continue; /* matched only a label or separator */
        if (s < seg->chunk_off) {
            len -= seg->chunk_off - s;
            s = seg->chunk_off;
        }
        size_t in = s - seg->chunk_off;
        if (in + len > seg->len) len = seg->len - in;
        if (len == 0) continue;
        Cand *c = &out[k];
        memset(c, 0, sizeof(*c));
        snprintf(c->type, sizeof(c->type), "%s", t);
        snprintf(c->info.type, sizeof(c->info.type), "%s", t);
        cJSON *sub = cJSON_GetObjectItem(it, "subject"), *rel = cJSON_GetObjectItem(it, "relation"),
              *obj = cJSON_GetObjectItem(it, "object");
        if (cJSON_IsString(sub)) snprintf(c->info.subject, sizeof(c->info.subject), "%s", sub->valuestring);
        if (cJSON_IsString(rel)) snprintf(c->info.relation, sizeof(c->info.relation), "%s", rel->valuestring);
        if (cJSON_IsString(obj)) snprintf(c->info.object, sizeof(c->info.object), "%s", obj->valuestring);
        /* A relationship links two *named* entities ("the eyes" and "the
         * sea" are not a relationship worth recording). */
        if (!strcmp(t, "relationship") && (!is_upper_start((const unsigned char *)c->info.subject) ||
                                           !is_upper_start((const unsigned char *)c->info.object)))
            continue;
        /* A description that never names its subject ("A man who is
         * hunched over the lens") gets the name in front. */
        if (!strcmp(t, "entity") && c->info.subject[0] && is_upper_start((const unsigned char *)c->info.subject) &&
            !strstr(desc->valuestring, c->info.subject)) {
            size_t dn = strlen(c->info.subject) + strlen(desc->valuestring) + 8;
            c->desc = malloc(dn);
            if (c->desc) snprintf(c->desc, dn, "%s \xE2\x80\x94 %s", c->info.subject, desc->valuestring);
        } else {
            c->desc = strdup(desc->valuestring);
        }
        const char *base = j->is_doc ? j->src : j->msgs[seg->message];
        size_t byte_start = seg->src_off + in;
        c->src.kind = j->is_doc ? SRC_DOC : SRC_CHAT;
        if (j->is_doc) {
            snprintf(c->src.doc, sizeof(c->src.doc), "%s", j->name);
            snprintf(c->src.revision, sizeof(c->src.revision), "%s", revision);
        } else {
            snprintf(c->src.chat, sizeof(c->src.chat), "%s", j->name);
            c->src.message = seg->message;
        }
        c->src.start = cp_count(base, byte_start);
        c->src.len = cp_count(base + byte_start, len);
        c->src.quote = dupn(base + byte_start, len);
        c->keep = c->desc && c->src.quote;
        if (c->keep) k++;
        else {
            free(c->desc);
            free(c->src.quote);
        }
    }
    cJSON_Delete(root);
    return k;
}

/* The judge's explanation, readable on its own: its first sentence, with
 * the prompt's CANON/NEW labels in plain words. */
static void plain_check(const char *check, char *out, size_t outlen)
{
    if (!check[0]) {
        snprintf(out, outlen, "This may contradict a canon entry.");
        return;
    }
    char buf[300];
    size_t k = 0;
    for (const char *p = check; *p && k + 16 < sizeof(buf);) {
        if (!strncmp(p, "CANON", 5)) {
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "canon");
            p += 5;
        } else if (!strncmp(p, "NEW", 3)) {
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "the new text");
            p += 3;
        } else {
            buf[k++] = *p++;
        }
    }
    buf[k] = '\0';
    /* first sentence (a period followed by a space and a capital, or the end) */
    char *end = buf;
    while ((end = strchr(end, '.')) != NULL) {
        if (end[1] == '\0' || (end[1] == ' ' && end[2] >= 'A' && end[2] <= 'Z')) {
            end[1] = '\0';
            break;
        }
        end++;
    }
    if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 32);
    snprintf(out, outlen, "%s", buf);
}

/* Returns true if the job completed (its text is now read). */
static bool run_job(Observer *o, const char *project, uint64_t gen, Job *j, cJSON *st)
{
    char *json = backend_extract(o, j->chunk);
    if (!json) return false;
    Cand cands[16];
    size_t nc = parse_candidates(j, json, "", cands, 16);
    free(json);

    /* Deduplicate against stored findings (and within this batch). */
    pthread_mutex_lock(&g_findings_mu);
    Record *existing = NULL, *canon = NULL, *notes = NULL;
    size_t ne = records_list(project, REC_FINDING, &existing);
    pthread_mutex_unlock(&g_findings_mu);
    size_t ncanon = records_list(project, REC_CANON, &canon), nnotes = records_list(project, REC_NOTE, &notes);
    for (size_t i = 0; i < nc; i++) {
        char key[400], other[400];
        finding_key(cands[i].type, cands[i].info.subject, cands[i].info.relation, cands[i].info.object, cands[i].desc, key,
                    sizeof(key));
        for (size_t e = 0; e < ne && cands[i].keep; e++) {
            const RecordVersion *v = record_current(&existing[e]);
            finding_key(v->finding.type, v->finding.subject, v->finding.relation, v->finding.object, v->content, other,
                        sizeof(other));
            if (!strcmp(key, other)) cands[i].keep = false;
        }
        for (size_t p = 0; p < i && cands[i].keep; p++) {
            if (!cands[p].keep) continue;
            finding_key(cands[p].type, cands[p].info.subject, cands[p].info.relation, cands[p].info.object, cands[p].desc,
                        other, sizeof(other));
            if (!strcmp(key, other)) cands[i].keep = false;
        }
    }

    /* Compare with committed canon (possible conflicts) and notes (overlaps). */
    typedef struct {
        size_t cand;
        char canon_id[40];
        char *canon_text;
        char check[300];
    } Conflict;
    Conflict conflicts[MAX_JUDGE_PAIRS];
    size_t nconf = 0, pairs = 0;
    bool cancelled = false;
    for (size_t i = 0; i < nc && !cancelled; i++) {
        if (!cands[i].keep || !strcmp(cands[i].type, "question")) continue; /* a question asserts nothing */
        char stmt[600];
        snprintf(stmt, sizeof(stmt), "%s (\xE2\x80\x9C%.300s\xE2\x80\x9D)", cands[i].desc, cands[i].src.quote);
        /* best two canon entries by overlap */
        size_t best[2] = { SIZE_MAX, SIZE_MAX };
        int score[2] = { 1, 1 };
        for (size_t c = 0; c < ncanon; c++) {
            const RecordVersion *v = record_current(&canon[c]);
            if (strcmp(v->status, "committed")) continue;
            int sc = obs_overlap(stmt, v->content);
            if (sc > score[0]) {
                best[1] = best[0];
                score[1] = score[0];
                best[0] = c;
                score[0] = sc;
            } else if (sc > score[1]) {
                best[1] = c;
                score[1] = sc;
            }
        }
        for (int b = 0; b < 2 && pairs < MAX_JUDGE_PAIRS; b++) {
            if (best[b] == SIZE_MAX) continue;
            pairs++;
            const RecordVersion *v = record_current(&canon[best[b]]);
            char check[300];
            bool conflict = backend_judge(o, v->content, stmt, check, sizeof(check));
            if (atomic_load(&o->cancel)) {
                cancelled = true;
                break;
            }
            for (size_t q = 0; conflict && q < nconf; q++) /* same canon, same passage: once */
                if (!strcmp(conflicts[q].canon_id, canon[best[b]].id) && cands[conflicts[q].cand].src.start == cands[i].src.start)
                    conflict = false;
            for (size_t e = 0; conflict && e < ne; e++) { /* already flagged earlier */
                const RecordVersion *ev = record_current(&existing[e]);
                conflict = !(!strcmp(ev->finding.type, "conflict") && !strcmp(ev->finding.related, canon[best[b]].id) &&
                             ev->nsources && ev->sources[0].quote && !strcmp(ev->sources[0].quote, cands[i].src.quote));
            }
            if (conflict && nconf < MAX_JUDGE_PAIRS) {
                Conflict *cf = &conflicts[nconf++];
                cf->cand = i;
                snprintf(cf->canon_id, sizeof(cf->canon_id), "%s", canon[best[b]].id);
                cf->canon_text = strdup(v->content);
                plain_check(check, cf->check, sizeof(cf->check));
            }
        }
        /* A related note (no model call). */
        for (size_t n = 0; n < nnotes; n++) {
            const RecordVersion *v = record_current(&notes[n]);
            if (!strcmp(v->status, "deleted")) continue;
            if (obs_overlap(stmt, v->content) >= 3) {
                snprintf(cands[i].info.related, sizeof(cands[i].info.related), "%s", notes[n].id);
                cands[i].info.related_text = strdup(v->content);
                break;
            }
        }
    }

    bool ok = !cancelled && !atomic_load(&o->cancel);
    /* Document sources point at an immutable snapshot of the text that was
     * read (usually the save's own revision, reused). Only taken when there
     * is something to point at. */
    bool any = nconf > 0;
    for (size_t i = 0; i < nc; i++) any = any || cands[i].keep;
    if (ok && any && j->is_doc) {
        char revision[160] = "";
        ok = revisions_snapshot(project, j->path, j->src, revision, sizeof(revision));
        for (size_t i = 0; i < nc; i++) snprintf(cands[i].src.revision, sizeof(cands[i].src.revision), "%s", revision);
    }
    pthread_mutex_lock(&o->mu);
    ok = ok && gen == o->gen; /* project unchanged */
    pthread_mutex_unlock(&o->mu);
    if (ok) {
        pthread_mutex_lock(&g_findings_mu);
        for (size_t i = 0; i < nc; i++) {
            if (!cands[i].keep) continue;
            char id[40];
            records_new_id(REC_FINDING, id, sizeof(id));
            records_append_ex(project, REC_FINDING, id, "new", cands[i].desc, &cands[i].src, 1, &cands[i].info, NULL);
        }
        for (size_t c = 0; c < nconf; c++) {
            Cand *cd = &cands[conflicts[c].cand];
            FindingInfo fi = { 0 };
            snprintf(fi.type, sizeof(fi.type), "conflict");
            snprintf(fi.subject, sizeof(fi.subject), "%s", cd->info.subject);
            snprintf(fi.related, sizeof(fi.related), "%s", conflicts[c].canon_id);
            fi.related_text = conflicts[c].canon_text;
            char id[40];
            records_new_id(REC_FINDING, id, sizeof(id));
            records_append_ex(project, REC_FINDING, id, "new", conflicts[c].check, &cd->src, 1, &fi, NULL);
        }
        pthread_mutex_unlock(&g_findings_mu);
        /* Mark the text read. */
        if (j->is_doc) {
            cJSON *docs = cJSON_GetObjectItem(st, "docs");
            cJSON *arr = cJSON_GetObjectItem(docs, j->name);
            if (!cJSON_IsArray(arr)) {
                cJSON_DeleteItemFromObject(docs, j->name);
                arr = cJSON_AddArrayToObject(docs, j->name);
            }
            for (size_t h = 0; h < j->nhashes; h++) {
                char hex[17];
                snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)j->hashes[h]);
                cJSON_AddItemToArray(arr, cJSON_CreateString(hex));
            }
        } else {
            cJSON *chats = cJSON_GetObjectItem(st, "chats");
            cJSON_DeleteItemFromObject(chats, j->name);
            cJSON_AddNumberToObject(chats, j->name, (double)j->msg_to);
        }
        ok = state_save(project, st);
    }
    for (size_t i = 0; i < nc; i++) {
        free(cands[i].desc);
        free(cands[i].src.quote);
        free(cands[i].info.related_text);
    }
    for (size_t c = 0; c < nconf; c++) free(conflicts[c].canon_text);
    records_free(existing, ne);
    records_free(canon, ncanon);
    records_free(notes, nnotes);
    return ok;
}

/* ---- worker ------------------------------------------------------------------------- */

static void *worker(void *arg)
{
    Observer *o = arg;
    for (;;) {
        pthread_mutex_lock(&o->mu);
        bool waiting_shown = false;
        while (!o->stop) {
            double t = now_s();
            bool idle = !atomic_load(&o->chat_active) && t - o->idle_since >= IDLE_DELAY;
            bool ready = o->poked && o->project[0] && o->enabled && t >= o->retry_at;
            if (ready && idle) break;
            if (ready && !idle && !waiting_shown) {
                snprintf(o->status, sizeof(o->status), "Waiting while you chat\xE2\x80\xA6");
                waiting_shown = true;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&o->cv, &o->mu, &ts);
        }
        if (o->stop) {
            pthread_mutex_unlock(&o->mu);
            break;
        }
        char project[PATH_MAX];
        snprintf(project, sizeof(project), "%s", o->project);
        uint64_t gen = o->gen;
        o->poked = false;
        o->running = true;
        atomic_store(&o->cancel, false);
        pthread_mutex_unlock(&o->mu);

        cJSON *st = state_load(project);
        Job j;
        if (!find_job(project, st, &j)) {
            state_save(project, st); /* blank chats/paragraphs marked read */
            cJSON_Delete(st);
            pthread_mutex_lock(&o->mu);
            if (gen == o->gen) snprintf(o->status, sizeof(o->status), "Up to date");
            o->running = false;
            pthread_mutex_unlock(&o->mu);
            continue;
        }
        char msg[300];
        snprintf(msg, sizeof(msg), "Reading %s\xE2\x80\xA6", j.is_doc ? j.name : "the chat");
        set_status(o, msg);
        bool done = run_job(o, project, gen, &j, st);
        job_free(&j);
        cJSON_Delete(st);
        pthread_mutex_lock(&o->mu);
        o->running = false;
        o->poked = true; /* more may be waiting (or this one, again) */
        if (done) {
            o->version++;
            o->jobs_done++;
        } else if (!atomic_load(&o->cancel) && gen == o->gen) {
            o->retry_at = now_s() + RETRY_DELAY;
            snprintf(o->status, sizeof(o->status), "%s",
                     o->server && llm_server_state(o->server, NULL, 0) != LLM_READY ? "Waiting for the model\xE2\x80\xA6"
                                                                                   : "Couldn't read that; will retry");
        }
        pthread_mutex_unlock(&o->mu);
    }
    return NULL;
}

Observer *observer_create(LlmServer *server)
{
    Observer *o = calloc(1, sizeof(Observer));
    if (!o) return NULL;
    o->server = server;
    o->enabled = true;
    pthread_mutex_init(&o->mu, NULL);
    pthread_cond_init(&o->cv, NULL);
    atomic_init(&o->cancel, false);
    atomic_init(&o->chat_active, false);
    o->idle_since = now_s() - IDLE_DELAY;
    snprintf(o->status, sizeof(o->status), "Open a document to start");
    if (pthread_create(&o->thread, NULL, worker, o) != 0) {
        pthread_mutex_destroy(&o->mu);
        pthread_cond_destroy(&o->cv);
        free(o);
        return NULL;
    }
    o->thread_started = true;
    return o;
}

void observer_destroy(Observer *o)
{
    if (!o) return;
    pthread_mutex_lock(&o->mu);
    o->stop = true;
    atomic_store(&o->cancel, true);
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
    if (o->thread_started) pthread_join(o->thread, NULL);
    pthread_mutex_destroy(&o->mu);
    pthread_cond_destroy(&o->cv);
    free(o);
}

void observer_set_project(Observer *o, const char *project_dir)
{
    pthread_mutex_lock(&o->mu);
    if (strcmp(o->project, project_dir ? project_dir : "") != 0) {
        snprintf(o->project, sizeof(o->project), "%s", project_dir ? project_dir : "");
        o->gen++;
        atomic_store(&o->cancel, true);
        o->retry_at = 0;
        snprintf(o->status, sizeof(o->status), "%s", o->project[0] ? "Starting\xE2\x80\xA6" : "Open a document to start");
    }
    o->poked = true;
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void observer_poke(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    o->poked = true;
    o->retry_at = 0;
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void observer_set_chat_active(Observer *o, bool active)
{
    bool was = atomic_exchange(&o->chat_active, active);
    if (active) {
        atomic_store(&o->cancel, true); /* the job is retried once chat is idle */
        pthread_mutex_lock(&o->mu);
        o->idle_since = now_s();
        pthread_mutex_unlock(&o->mu);
    } else if (was) {
        pthread_mutex_lock(&o->mu);
        o->idle_since = now_s();
        pthread_cond_signal(&o->cv);
        pthread_mutex_unlock(&o->mu);
    }
}

void observer_set_enabled(Observer *o, bool enabled)
{
    pthread_mutex_lock(&o->mu);
    o->enabled = enabled;
    if (!enabled) atomic_store(&o->cancel, true);
    snprintf(o->status, sizeof(o->status), "%s", enabled ? "Starting\xE2\x80\xA6" : "Paused");
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void observer_status(Observer *o, char *out, size_t outlen)
{
    pthread_mutex_lock(&o->mu);
    snprintf(out, outlen, "%s", o->status);
    pthread_mutex_unlock(&o->mu);
}

uint64_t observer_version(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    uint64_t v = o->version;
    pthread_mutex_unlock(&o->mu);
    return v;
}

bool observer_idle(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    bool idle = !o->running && (!o->poked || !o->enabled || !o->project[0]);
    pthread_mutex_unlock(&o->mu);
    return idle;
}

unsigned observer_jobs_done(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    unsigned v = o->jobs_done;
    pthread_mutex_unlock(&o->mu);
    return v;
}

bool observer_set_finding_status(Observer *o, const char *project_dir, const char *id, const char *status)
{
    pthread_mutex_lock(&g_findings_mu);
    Record *list = NULL;
    size_t n = records_list(project_dir, REC_FINDING, &list);
    bool ok = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].id, id)) continue;
        const RecordVersion *v = record_current(&list[i]);
        ok = records_append_ex(project_dir, REC_FINDING, id, status, v->content, v->sources, v->nsources, &v->finding, NULL);
        break;
    }
    records_free(list, n);
    pthread_mutex_unlock(&g_findings_mu);
    if (ok && o) {
        pthread_mutex_lock(&o->mu);
        o->version++;
        pthread_mutex_unlock(&o->mu);
    }
    return ok;
}
