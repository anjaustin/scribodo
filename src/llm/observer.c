#define _DARWIN_C_SOURCE
#include "observer.h"

#include "core/utf8.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app/chat_store.h"
#include "app/store.h"
#include "llm/semantic.h"
#include "app/revisions.h"
#include "cJSON.h"
#include "llm/http.h"
#include "llm/llm_text.h"

#define CHUNK_MAX 3000        /* bytes of new text per extraction request */
#define PIECE_MAX 2000        /* longest piece of one paragraph or message */
#define IDLE_DELAY 1.5        /* seconds of chat idleness before observer work */
#define RETRY_DELAY 5.0       /* after a failed request */
#define MAX_JUDGE_PAIRS 24    /* canon checks per job: 8 findings x 3 entries, so none are skipped */
#define MAX_DOC_BYTES (16u << 20) /* the editor's own limit is lower */

/* All finding writes (observer worker and UI status changes) take this. */
static pthread_mutex_t g_findings_mu = PTHREAD_MUTEX_INITIALIZER;

struct Observer {
    LlmServer *server;
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop, poked, enabled, running;
    bool io_error;           /* can't write the observer's files: back off */
    bool rebuild;            /* rebuild the project's semantic index next */
    struct PendingRename {
        char project[PATH_MAX], from[256], to[256];
    } *renames;              /* document renames to apply to that project's state */
    size_t nrenames;
    char project[PATH_MAX];
    uint64_t gen;            /* bumps on project change: stale results are discarded */
    double idle_since;       /* last time chat was active */
    double retry_at;
    atomic_bool cancel;
    atomic_bool chat_active;
    char status[200];
    uint64_t version;
    unsigned jobs_done;
    unsigned attempts;      /* jobs started, including failed ones (tests) */
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

static bool word_byte(unsigned char c);

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

/* A match must not start or end inside a word ("ed" is not evidence
 * found in "climbed"). */
static bool word_bounded(const char *h, size_t hn, size_t at, size_t len)
{
    bool starts_word = word_byte((unsigned char)h[at]), ends_word = word_byte((unsigned char)h[at + len - 1]);
    if (starts_word && at > 0 && word_byte((unsigned char)h[at - 1]) && ((unsigned char)h[at] & 0xC0) != 0x80) return false;
    if (starts_word && at > 0 && ((unsigned char)h[at] & 0xC0) == 0x80) return false; /* mid-character */
    if (ends_word && at + len < hn && word_byte((unsigned char)h[at + len]) && ((unsigned char)h[at + len] & 0xC0) != 0x80)
        return false;
    if (ends_word && at + len < hn && ((unsigned char)h[at + len] & 0xC0) == 0x80) return false;
    return true;
}

static const char *memfind_word(const char *h, size_t hn, const char *n, size_t nn)
{
    for (size_t from = 0; from + nn <= hn;) {
        const char *hit = memfind(h + from, hn - from, n, nn);
        if (!hit) return NULL;
        size_t at = (size_t)(hit - h);
        if (word_bounded(h, hn, at, nn)) return hit;
        from = at + 1;
    }
    return NULL;
}

static bool locate_once(const char *text, size_t n, const char *evidence, size_t en, size_t *start, size_t *len)
{
    if (en < 2) return false;
    const char *hit = memfind_word(text, n, evidence, en);
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
            const char *h = memfind_word(nt, tn, ne, try_len);
            /* A prefix must cover most of the quote: a made-up claim can't
             * borrow a sentence's first words as its "evidence". */
            if (h && (try_len == evn || (try_len >= 20 && try_len * 100 >= evn * 60))) {
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
    /* ...and one citing a trivial word that is in the text (prompt injection). */
    if (strstr(chunk, "[[weak]]")) add_finding(arr, "event", "Ingrid is the murderer.", "the", "Ingrid");
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

static cJSON *state_load_files(const char *project);

/* ---- read state in the project store ---------------------------------------- */

/* The same JSON shape as state.json, built from observer_pieces and
 * observer_chats (keyed by ids, presented by current names). */
static cJSON *state_load_db(Db *db, const char *pid)
{
    DbRows r, c;
    /* A failed query is not an empty state (that would re-read everything):
     * NULL, and the caller treats the store as unavailable. */
    if (!db_query(db,
                  "SELECT d.name, p.piece_hash FROM observer_pieces p JOIN documents d ON d.id = p.document_id"
                  " WHERE d.project_id = ? ORDER BY d.name",
                  (DbArg[1]){ DB_S(pid) }, 1, &r))
        return NULL;
    if (!db_query(db,
                  "SELECT o.conversation_id, o.msg, o.off FROM observer_chats o JOIN conversations c ON c.id = o.conversation_id"
                  " WHERE c.project_id = ?",
                  (DbArg[1]){ DB_S(pid) }, 1, &c)) {
        db_rows_free(&r);
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON *docs = cJSON_AddObjectToObject(j, "docs"), *chats = cJSON_AddObjectToObject(j, "chats");
    for (size_t i = 0; i < r.nrows; i++) {
        cJSON *arr = cJSON_GetObjectItem(docs, db_cell(&r, i, 0));
        if (!arr) arr = cJSON_AddArrayToObject(docs, db_cell(&r, i, 0));
        cJSON_AddItemToArray(arr, cJSON_CreateString(db_cell(&r, i, 1)));
    }
    for (size_t i = 0; i < c.nrows; i++) {
        cJSON *o = cJSON_AddObjectToObject(chats, db_cell(&c, i, 0));
        cJSON_AddNumberToObject(o, "n", atof(db_cell(&c, i, 1)));
        cJSON_AddNumberToObject(o, "off", atof(db_cell(&c, i, 2)));
    }
    db_rows_free(&r);
    db_rows_free(&c);
    return j;
}

/* Run "INSERT IGNORE INTO observer_pieces VALUES (?,?),…" or a matching
 * DELETE for up to 200 (document, hash) pairs at a time. */
static bool pieces_batch(Db *db, bool insert, const char *did, const char *const *hashes, size_t n)
{
    bool ok = true;
    for (size_t at = 0; ok && at < n; at += 200) {
        size_t k = n - at < 200 ? n - at : 200;
        char *sql = malloc(160 + k * 8);
        DbArg *args = malloc((1 + 2 * k) * sizeof(DbArg));
        if (!sql || !args) {
            free(sql);
            free(args);
            return false;
        }
        size_t na = 0, len = 0;
        if (insert) {
            len = (size_t)sprintf(sql, "INSERT IGNORE INTO observer_pieces (document_id, piece_hash) VALUES ");
            for (size_t q = 0; q < k; q++) {
                len += (size_t)sprintf(sql + len, "%s(?,?)", q ? "," : "");
                args[na++] = DB_S(did);
                args[na++] = DB_S(hashes[at + q]);
            }
        } else {
            len = (size_t)sprintf(sql, "DELETE FROM observer_pieces WHERE document_id = ? AND piece_hash IN (");
            args[na++] = DB_S(did);
            for (size_t q = 0; q < k; q++) {
                len += (size_t)sprintf(sql + len, "%s?", q ? "," : "");
                args[na++] = DB_S(hashes[at + q]);
            }
            sprintf(sql + len, ")");
        }
        ok = db_run(db, sql, args, na, NULL);
        free(sql);
        free(args);
    }
    return ok;
}

static bool in_array(cJSON *arr, const char *s)
{
    cJSON *it;
    cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) return true;
    return false;
}

/* Bring the project's stored read state to `st`, writing only the
 * differences (red-team 008: rewriting every piece each job held locks long
 * enough to stall the UI's History saves). */
static bool state_save_db(Db *db, const char *pid, cJSON *st)
{
    cJSON *old = state_load_db(db, pid);
    if (!old) return false;
    bool ok = db_begin(db);
    cJSON *docs = cJSON_GetObjectItem(st, "docs"), *odocs = cJSON_GetObjectItem(old, "docs");
    cJSON *doc;
    cJSON_ArrayForEach(doc, docs)
    {
        if (!ok) break;
        cJSON *prev = cJSON_GetObjectItem(odocs, doc->string);
        size_t n = (size_t)cJSON_GetArraySize(doc), m = prev ? (size_t)cJSON_GetArraySize(prev) : 0;
        const char **add = calloc(n ? n : 1, sizeof(char *)), **del = calloc(m ? m : 1, sizeof(char *));
        size_t na = 0, nd = 0;
        cJSON *h;
        cJSON_ArrayForEach(h, doc) if (add && cJSON_IsString(h) && !(prev && in_array(prev, h->valuestring))) add[na++] = h->valuestring;
        if (prev) cJSON_ArrayForEach(h, prev) if (del && cJSON_IsString(h) && !in_array(doc, h->valuestring)) del[nd++] = h->valuestring;
        if (na || nd) {
            char did[40];
            if (store_document_id(pid, doc->string, true, did, sizeof(did)))
                ok = pieces_batch(db, true, did, add, na) && pieces_batch(db, false, did, del, nd);
        }
        free(add);
        free(del);
    }
    /* Documents gone from the state (deleted files): their pieces go. */
    cJSON_ArrayForEach(doc, odocs)
    {
        if (!ok) break;
        if (cJSON_GetObjectItem(docs, doc->string)) continue;
        char did[40];
        if (store_document_id(pid, doc->string, false, did, sizeof(did)))
            ok = db_run(db, "DELETE FROM observer_pieces WHERE document_id = ?", (DbArg[1]){ DB_S(did) }, 1, NULL);
    }
    cJSON *c, *ochats = cJSON_GetObjectItem(old, "chats");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(st, "chats"))
    {
        if (!ok) break;
        cJSON *n = cJSON_GetObjectItem(c, "n"), *off = cJSON_GetObjectItem(c, "off");
        double nv = cJSON_IsNumber(n) ? n->valuedouble : 0, ov = cJSON_IsNumber(off) ? off->valuedouble : 0;
        cJSON *was = cJSON_GetObjectItem(ochats, c->string);
        if (was && cJSON_GetNumberValue(cJSON_GetObjectItem(was, "n")) == nv &&
            cJSON_GetNumberValue(cJSON_GetObjectItem(was, "off")) == ov)
            continue; /* unchanged */
        DbArg a[4] = { DB_I((int64_t)nv), DB_I((int64_t)ov), DB_S(c->string), DB_S(pid) };
        ok = db_run(db,
                    "INSERT INTO observer_chats (conversation_id, msg, off)"
                    " SELECT id, ?, ? FROM conversations WHERE id = ? AND project_id = ?"
                    " ON DUPLICATE KEY UPDATE msg = VALUES(msg), off = VALUES(off)",
                    a, 4, NULL);
    }
    cJSON_Delete(old);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}

static cJSON *state_load(const char *project)
{
    char pid[64];
    Db *db;
    StoreMode mode = store_mode(project, pid, sizeof(pid), &db);
    if (mode == STORE_DB) return state_load_db(db, pid);
    if (mode == STORE_PAUSED) return NULL; /* nothing to do until the store is back */
    return state_load_files(project);
}

static cJSON *state_load_files(const char *project)
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

static bool state_save_files(const char *project, cJSON *st);

static bool state_save(const char *project, cJSON *st)
{
    char pid[64];
    Db *db;
    StoreMode mode = store_mode(project, pid, sizeof(pid), &db);
    if (mode == STORE_DB) return state_save_db(db, pid, st);
    if (mode == STORE_PAUSED) return false;
    return state_save_files(project, st);
}

static bool state_save_files(const char *project, cJSON *st)
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
        ok = fchmod(fd, 0644) == 0 && write(fd, s, n) == (ssize_t)n && fsync(fd) == 0;
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
    char *src;            /* document text, normalised as the editor loads it */
    char **msgs;          /* message texts (chat jobs), by index */
    size_t nmsgs;
    size_t msg_to, off_to; /* chat: read up to message msg_to, byte off_to */
    char *chunk;
    size_t chunk_len;
    Seg *segs;
    size_t nsegs, cap_segs;
    uint64_t *hashes;     /* document pieces read by this job */
    cJSON *current;       /* every piece of the document now (hex hashes), for pruning the index */
    size_t nhashes, cap_hashes;
} Job;

static void job_free(Job *j)
{
    free(j->src);
    for (size_t i = 0; i < j->nmsgs; i++) free(j->msgs[i]);
    free(j->msgs);
    free(j->chunk);
    free(j->segs);
    free(j->hashes);
    cJSON_Delete(j->current);
    memset(j, 0, sizeof(*j));
}

/* Add a piece (at most PIECE_MAX bytes) to the chunk; false if it doesn't fit. */
static bool chunk_add(Job *j, const char *label, const char *text, size_t src_off, size_t len, long message)
{
    size_t ll = label ? strlen(label) : 0;
    if (j->chunk_len && j->chunk_len + 2 + ll + len > CHUNK_MAX) return false;
    if (j->nsegs == j->cap_segs) {
        size_t cap = j->cap_segs ? j->cap_segs * 2 : 16;
        Seg *sg = realloc(j->segs, cap * sizeof(Seg));
        if (!sg) return false;
        j->segs = sg;
        j->cap_segs = cap;
    }
    char *c = realloc(j->chunk, j->chunk_len + ll + len + 3);
    if (!c) return false;
    j->chunk = c;
    if (j->chunk_len) {
        memcpy(j->chunk + j->chunk_len, "\n\n", 2);
        j->chunk_len += 2;
    }
    if (ll) memcpy(j->chunk + j->chunk_len, label, ll);
    j->chunk_len += ll;
    j->segs[j->nsegs++] = (Seg){ j->chunk_len, src_off, len, message };
    memcpy(j->chunk + j->chunk_len, text + src_off, len);
    j->chunk_len += len;
    j->chunk[j->chunk_len] = '\0';
    return true;
}

static bool add_hash(Job *j, uint64_t h)
{
    if (j->nhashes == j->cap_hashes) {
        size_t cap = j->cap_hashes ? j->cap_hashes * 2 : 32;
        uint64_t *hs = realloc(j->hashes, cap * sizeof(uint64_t));
        if (!hs) return false;
        j->hashes = hs;
        j->cap_hashes = cap;
    }
    j->hashes[j->nhashes++] = h;
    return true;
}

/* Length of the next piece of s[0, n): at most PIECE_MAX bytes, ending at a
 * sentence end or a space when possible, never inside a UTF-8 sequence. Long
 * paragraphs and messages are read piece by piece, each tracked on its own
 * (red-team 005: everything past 3,000 bytes used to be marked read unread). */
static size_t piece_len(const char *s, size_t n)
{
    if (n <= PIECE_MAX) return n;
    size_t cut = 0;
    for (size_t i = PIECE_MAX; i > PIECE_MAX / 2 && !cut; i--)
        if ((s[i - 1] == '.' || s[i - 1] == '?' || s[i - 1] == '!' || s[i - 1] == '\n') && s[i] == ' ') cut = i;
    for (size_t i = PIECE_MAX; i > PIECE_MAX / 2 && !cut; i--)
        if (s[i] == ' ') cut = i;
    if (!cut) {
        cut = PIECE_MAX;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    }
    return cut;
}

/* Read a document as the editor does (doc_io): drop a UTF-8 BOM, turn CRLF
 * and lone CR into LF, so offsets match what the author sees and what a
 * save writes (red-team 005: CRLF files had every offset shifted). */
static char *read_doc(const char *path, size_t *n)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > MAX_DOC_BYTES) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    size_t r = 0, w = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) r = 3;
    for (; r < got; r++) {
        if (buf[r] == '\r') {
            if (r + 1 < got && buf[r + 1] == '\n') r++;
            buf[w++] = '\n';
        } else {
            buf[w++] = buf[r];
        }
    }
    buf[w] = '\0';
    *n = w;
    return buf;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void set_chat_progress(cJSON *chats, const char *id, size_t n, size_t off)
{
    cJSON_DeleteItemFromObject(chats, id);
    cJSON *o = cJSON_AddObjectToObject(chats, id);
    cJSON_AddNumberToObject(o, "n", (double)n);
    cJSON_AddNumberToObject(o, "off", (double)off);
}

static bool in_list(char **names, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++)
        if (names[i] && !strcmp(names[i], s)) return true;
    return false;
}

/* The next unread piece of stored text, or false if all is read. Also
 * prunes state for paragraphs, documents and chats that no longer exist. */
static bool find_job(const char *project, cJSON *st, Job *j)
{
    memset(j, 0, sizeof(*j));
    cJSON *docs = cJSON_GetObjectItem(st, "docs"), *chats = cJSON_GetObjectItem(st, "chats");
    /* Documents, by name (no cap). */
    DIR *d = opendir(project);
    char **names = NULL;
    size_t nn = 0, cap = 0;
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if (!((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")) ||
              (n > 9 && !strcasecmp(ent->d_name + n - 9, ".markdown"))))
            continue;
        if (nn == cap) {
            cap = cap ? cap * 2 : 64;
            char **nm = realloc(names, cap * sizeof(char *));
            if (!nm) break;
            names = nm;
        }
        names[nn++] = strdup(ent->d_name);
    }
    if (d) closedir(d);
    if (nn) qsort(names, nn, sizeof(char *), cmp_names);
    /* Forget documents that are gone. */
    for (cJSON *it = docs ? docs->child : NULL, *next; it; it = next) {
        next = it->next;
        if (!in_list(names, nn, it->string)) cJSON_DeleteItemFromObject(docs, it->string);
    }
    bool found = false;
    for (size_t i = 0; i < nn && !found; i++) {
        if (!names[i]) continue;
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", project, names[i]);
        size_t len = 0;
        char *text = read_doc(path, &len);
        if (!text || strlen(text) != len || !valid_utf8(text, len)) { /* not a readable UTF-8 text file */
            free(text);
            continue;
        }
        cJSON *seen = cJSON_GetObjectItem(docs, names[i]);
        cJSON *kept = cJSON_CreateArray(); /* seen hashes still present (pruned) */
        size_t np = 0, pcap = 256;
        size_t *ps = malloc(pcap * sizeof(size_t)), *pl = malloc(pcap * sizeof(size_t));
        while (ps && pl) { /* all paragraphs, however many */
            np = obs_paragraphs(text, ps, pl, pcap);
            if (np < pcap) break;
            pcap *= 2;
            size_t *a = realloc(ps, pcap * sizeof(size_t)), *b = realloc(pl, pcap * sizeof(size_t));
            if (!a || !b) {
                free(a ? a : ps);
                free(b ? b : pl);
                ps = pl = NULL;
                break;
            }
            ps = a;
            pl = b;
        }
        bool full = false;
        for (size_t p = 0; ps && pl && p < np; p++) {
            for (size_t off = 0; off < pl[p];) {
                size_t plen = piece_len(text + ps[p] + off, pl[p] - off);
                while (off + plen < pl[p] && text[ps[p] + off + plen] == ' ') plen++; /* keep spaces with the piece */
                uint64_t h = fnv(text + ps[p] + off, plen);
                if (seen && hash_seen(seen, h)) {
                    char hex[17];
                    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
                    if (!hash_seen(kept, h)) cJSON_AddItemToArray(kept, cJSON_CreateString(hex));
                } else if (!full) {
                    if (chunk_add(j, NULL, text, ps[p] + off, plen, -1)) add_hash(j, h);
                    else full = true;
                }
                off += plen;
            }
        }
        free(ps);
        free(pl);
        if (j->nsegs) {
            /* Every piece the document has now: what was read before plus
             * this job's (the semantic index drops the rest). */
            j->current = cJSON_Duplicate(kept, true);
            for (size_t h = 0; j->current && h < j->nhashes; h++) {
                char hex[17];
                snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)j->hashes[h]);
                cJSON_AddItemToArray(j->current, cJSON_CreateString(hex));
            }
        }
        cJSON_DeleteItemFromObject(docs, names[i]);
        cJSON_AddItemToObject(docs, names[i], kept);
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
    free(names);
    if (found) return true;

    /* Chats: what follows the position already read. */
    ChatSummary *list = NULL;
    size_t nc = chat_store_list(project, &list);
    for (cJSON *it = chats ? chats->child : NULL, *next; it; it = next) { /* forget deleted chats */
        next = it->next;
        bool present = false;
        for (size_t i = 0; i < nc && !present; i++) present = !strcmp(list[i].id, it->string);
        if (!present) cJSON_DeleteItemFromObject(chats, it->string);
    }
    for (size_t i = 0; i < nc && !found; i++) {
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load(project, list[i].id, &msgs, &n)) continue;
        cJSON *pos = cJSON_GetObjectItem(chats, list[i].id);
        size_t from = 0, from_off = 0;
        if (cJSON_IsNumber(pos) && pos->valuedouble >= 0) from = (size_t)pos->valuedouble; /* older state */
        if (cJSON_IsObject(pos)) {
            cJSON *a = cJSON_GetObjectItem(pos, "n"), *b = cJSON_GetObjectItem(pos, "off");
            if (cJSON_IsNumber(a) && a->valuedouble >= 0) from = (size_t)a->valuedouble;
            if (cJSON_IsNumber(b) && b->valuedouble >= 0) from_off = (size_t)b->valuedouble;
        }
        if (from < n) {
            j->msgs = calloc(n, sizeof(char *));
            j->nmsgs = j->msgs ? n : 0;
            for (size_t k = 0; k < j->nmsgs; k++) j->msgs[k] = strdup(msgs[k].content ? msgs[k].content : "");
            size_t k = from, off = from_off;
            bool full = false;
            for (; k < j->nmsgs && !full; k++, off = 0) {
                /* Failed replies, and AI rewrites (which may have been
                 * rejected), are not story text. */
                bool skip = msgs[k].error || !j->msgs[k] || !j->msgs[k][0] || (msgs[k].role == CHAT_ASSISTANT && msgs[k].is_edit);
                if (skip) continue;
                const char *label = msgs[k].role == CHAT_USER ? "AUTHOR: " : "ASSISTANT: ";
                size_t mlen = strlen(j->msgs[k]);
                if (off > mlen) off = mlen;
                while (off < mlen) {
                    size_t plen = piece_len(j->msgs[k] + off, mlen - off);
                    if (!chunk_add(j, off == 0 ? label : NULL, j->msgs[k], off, plen, (long)k)) {
                        full = true;
                        break;
                    }
                    off += plen;
                }
                if (full) break;
            }
            j->msg_to = k;
            j->off_to = full ? off : 0;
            snprintf(j->name, sizeof(j->name), "%s", list[i].id);
            found = true;
            if (!j->nsegs) { /* only failed/empty/rewrite messages: just mark them read */
                set_chat_progress(chats, list[i].id, j->msg_to, j->off_to);
                job_free(j);
                found = false;
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

/* Enough source text to support a finding: an entity's own name, or a
 * phrase of at least 3 words and 12 bytes. Measured red-team 005: a prompt
 * planted in a document made the model cite "the" as evidence for "Ingrid
 * is the murderer". */
static bool strong_evidence(const char *type, const char *subject, const char *quote)
{
    size_t n = strlen(quote);
    int words = 0;
    bool in = false;
    for (const unsigned char *p = (const unsigned char *)quote; *p; p++) {
        bool w = word_byte(*p);
        if (w && !in) words++;
        in = w;
    }
    if (n >= 12 && words >= 3) return true;
    return !strcmp(type, "entity") && strlen(subject) >= 3 && strcasestr(quote, subject) != NULL;
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
        /* Model text, clipped on character boundaries (a split character
         * is invalid UTF-8, which the store refuses). */
        if (cJSON_IsString(sub)) utf8_clip(c->info.subject, sizeof(c->info.subject), sub->valuestring, SIZE_MAX);
        if (cJSON_IsString(rel)) utf8_clip(c->info.relation, sizeof(c->info.relation), rel->valuestring, SIZE_MAX);
        if (cJSON_IsString(obj)) utf8_clip(c->info.object, sizeof(c->info.object), obj->valuestring, SIZE_MAX);
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
        c->keep = c->desc && c->src.quote && strong_evidence(t, c->info.subject, c->src.quote);
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
    const char *p = check;
    for (; *p && k + 16 < sizeof(buf);) {
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
    /* never end inside a character */
    /* stopped inside a character (its bytes are copied one for one): back
     * up to its first byte */
    while (k > 0 && ((unsigned char)*p & 0xC0) == 0x80) {
        k--;
        p--;
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
    utf8_clip(out, outlen, buf, SIZE_MAX);
}

/* Returns true if the job completed (its text is now read). */
static bool run_job(Observer *o, const char *project, uint64_t gen, Job *j, cJSON *st, bool *io_out)
{
    bool io_failed = false;
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

    /* Projects in the store get semantic retrieval when the index is up;
     * canon and notes are synced into it first (cheap when unchanged). */
    char sem_pid[64] = "";
    bool use_sem = store_mode(project, sem_pid, sizeof(sem_pid), NULL) == STORE_DB && semantic_ready();
    if (use_sem) {
        char serr[200];
        use_sem = semantic_sync_records(sem_pid, serr, sizeof(serr));
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
        /* Every statement is checked, including ones that duplicate an
         * existing finding ("Zoë" is known, but "Zoë's brown eyes" may still
         * contradict canon). A question asserts nothing. */
        if (!strcmp(cands[i].type, "question")) continue;
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
        /* Semantic candidates (projects in the store with an index): the
         * entries nearest in meaning, which shared words can miss ("the old
         * woman" vs canon about "Grandmother Signe"). Lexical ones too. */
        size_t cand[4] = { SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX };
        int ncand = 0;
        if (use_sem) {
            SemHit *hits = NULL;
            size_t nh = semantic_canon_for(sem_pid, stmt, 2, &hits);
            for (size_t k = 0; k < nh && ncand < 2; k++)
                for (size_t c = 0; c < ncanon; c++)
                    if (!strcmp(canon[c].id, hits[k].record_id) && !strcmp(record_current(&canon[c])->status, "committed")) {
                        cand[ncand++] = c;
                        break;
                    }
            semantic_hits_free(hits, nh);
        }
        for (int b = 0; b < 2 && ncand < 3; b++) {
            bool dup = false;
            for (int k = 0; k < ncand; k++) dup = dup || cand[k] == best[b];
            if (best[b] != SIZE_MAX && !dup) cand[ncand++] = best[b];
        }
        for (int b = 0; b < ncand && pairs < MAX_JUDGE_PAIRS; b++) {
            best[0] = cand[b];
            if (best[0] == SIZE_MAX) continue;
            pairs++;
            const RecordVersion *v = record_current(&canon[best[0]]);
            char check[300];
            bool conflict = backend_judge(o, v->content, stmt, check, sizeof(check));
            if (atomic_load(&o->cancel)) {
                cancelled = true;
                break;
            }
            for (size_t q = 0; conflict && q < nconf; q++) /* same canon, same passage: once */
                if (!strcmp(conflicts[q].canon_id, canon[best[0]].id) && cands[conflicts[q].cand].src.start == cands[i].src.start)
                    conflict = false;
            for (size_t e = 0; conflict && e < ne; e++) { /* already flagged earlier */
                const RecordVersion *ev = record_current(&existing[e]);
                /* Same canon entry *with the same text* (a revised entry is
                 * checked afresh), same passage. */
                conflict = !(!strcmp(ev->finding.type, "conflict") && !strcmp(ev->finding.related, canon[best[0]].id) &&
                             ev->finding.related_text && !strcmp(ev->finding.related_text, v->content) &&
                             ev->nsources && ev->sources[0].quote && !strcmp(ev->sources[0].quote, cands[i].src.quote));
            }
            if (conflict && nconf < MAX_JUDGE_PAIRS) {
                Conflict *cf = &conflicts[nconf++];
                cf->cand = i;
                snprintf(cf->canon_id, sizeof(cf->canon_id), "%s", canon[best[0]].id);
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
            if (!records_create_ex(project, REC_FINDING, id, "new", cands[i].desc, &cands[i].src, 1, &cands[i].info))
                io_failed = true;
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
            if (!records_create_ex(project, REC_FINDING, id, "new", conflicts[c].check, &cd->src, 1, &fi)) io_failed = true;
        }
        pthread_mutex_unlock(&g_findings_mu);
        /* Index what was read (documents as draft text, chats as chat), and
         * drop the document's points for pieces it no longer has. */
        if (use_sem) {
            char did[40] = "";
            if (j->is_doc) store_document_id(sem_pid, j->name, true, did, sizeof(did));
            char rev[160] = "";
            if (j->is_doc) revisions_snapshot(project, j->path, j->src, rev, sizeof(rev));
            for (size_t q = 0; q < j->nsegs; q++) {
                const Seg *sg = &j->segs[q];
                char *piece = dupn(j->chunk + sg->chunk_off, sg->len);
                if (!piece) continue;
                const char *base = j->is_doc ? j->src : j->msgs[sg->message];
                size_t start = cp_count(base, sg->src_off), len = cp_count(piece, sg->len);
                if (j->is_doc) semantic_index_text(sem_pid, "draft", did, rev[0] ? rev : NULL, NULL, 0, start, len, piece);
                else semantic_index_text(sem_pid, "chat", NULL, NULL, j->name, sg->message, start, len, piece);
                free(piece);
            }
            if (j->is_doc && did[0] && j->current) {
                size_t n = (size_t)cJSON_GetArraySize(j->current), k = 0;
                const char **hs = calloc(n ? n : 1, sizeof(char *));
                cJSON *it;
                cJSON_ArrayForEach(it, j->current) if (hs && cJSON_IsString(it)) hs[k++] = it->valuestring;
                if (hs) semantic_prune_doc(sem_pid, did, hs, k);
                free(hs);
            }
        }
        /* Mark the text read, unless a finding couldn't be saved: then it is
         * read again later rather than silently missing (red-team 008). */
        if (io_failed) {
        } else if (j->is_doc) {
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
            set_chat_progress(cJSON_GetObjectItem(st, "chats"), j->name, j->msg_to, j->off_to);
        }
        if (!io_failed && !state_save(project, st)) io_failed = true;
        ok = !io_failed;
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
    if (io_out) *io_out = io_failed;
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
            bool ready = (o->poked || o->rebuild) && o->project[0] && o->enabled && t >= o->retry_at;
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
        bool rebuild = o->rebuild; /* read under the lock */
        bool was_poked = o->poked;
        o->rebuild = false;
        o->poked = false;
        o->running = true;
        atomic_store(&o->cancel, false);
        pthread_mutex_unlock(&o->mu);

        if (rebuild) { /* the index is derived: drop it and rebuild it from MariaDB */
            char pid[64], err[200] = "";
            long n = -1;
            set_status(o, "Rebuilding the semantic index\xE2\x80\xA6");
            if (store_mode(project, pid, sizeof(pid), NULL) == STORE_DB) n = semantic_rebuild(pid, err, sizeof(err));
            else snprintf(err, sizeof(err), "this project isn't in the project store");
            char msg[300];
            if (n >= 0) snprintf(msg, sizeof(msg), "Index rebuilt: %ld passages", n);
            else snprintf(msg, sizeof(msg), "Couldn't rebuild the index: %s", err);
            pthread_mutex_lock(&o->mu);
            snprintf(o->status, sizeof(o->status), "%s", msg);
            o->running = false;
            o->poked = was_poked; /* only work that was already waiting (the result stays visible) */
            o->version++;
            pthread_mutex_unlock(&o->mu);
            continue;
        }
        cJSON *st = state_load(project);
        if (!st) { /* the project store is down: wait, and say so */
            pthread_mutex_lock(&o->mu);
            o->running = false;
            o->poked = true;
            o->retry_at = now_s() + 30.0;
            snprintf(o->status, sizeof(o->status), "Paused: the project store is offline");
            pthread_mutex_unlock(&o->mu);
            continue;
        }
        /* Renamed documents keep what was already read: apply this project's
         * renames, in order; other projects' wait until they're open
         * (red-team 006: they used to be applied to whatever was open). */
        pthread_mutex_lock(&o->mu);
        cJSON *sdocs = cJSON_GetObjectItem(st, "docs");
        size_t keep = 0;
        for (size_t r = 0; r < o->nrenames; r++) {
            if (strcmp(o->renames[r].project, project) != 0) {
                o->renames[keep++] = o->renames[r];
                continue;
            }
            cJSON *seen = cJSON_DetachItemFromObject(sdocs, o->renames[r].from);
            if (seen) {
                cJSON_DeleteItemFromObject(sdocs, o->renames[r].to);
                cJSON_AddItemToObject(sdocs, o->renames[r].to, seen);
            }
        }
        o->nrenames = keep;
        pthread_mutex_unlock(&o->mu);
        Job j;
        if (!find_job(project, st, &j)) {
            bool saved = state_save(project, st); /* pruned state; blank text marked read */
            cJSON_Delete(st);
            pthread_mutex_lock(&o->mu);
            if (gen == o->gen) snprintf(o->status, sizeof(o->status), "%s", saved ? "Up to date" : "Can't write .scribodo/observer/state.json");
            o->running = false;
            pthread_mutex_unlock(&o->mu);
            continue;
        }
        char msg[300];
        snprintf(msg, sizeof(msg), "Reading %s\xE2\x80\xA6", j.is_doc ? j.name : "the chat");
        set_status(o, msg);
        bool io = false;
        pthread_mutex_lock(&o->mu);
        o->attempts++;
        pthread_mutex_unlock(&o->mu);
        bool done = run_job(o, project, gen, &j, st, &io);
        job_free(&j);
        cJSON_Delete(st);
        pthread_mutex_lock(&o->mu);
        o->running = false;
        o->poked = true; /* more may be waiting (or this one, again) */
        if (done) {
            o->version++;
            o->jobs_done++;
            o->io_error = false;
        } else if (io && gen == o->gen) {
            /* Findings were written but progress couldn't be: back off a
             * minute (saves don't cut it short) rather than re-asking the
             * model every few seconds (red-team 005). */
            o->version++;
            o->io_error = true;
            o->retry_at = now_s() + 60.0;
            snprintf(o->status, sizeof(o->status), "Can't write .scribodo/observer/state.json \xE2\x80\x94 retrying in a minute");
        } else if (!atomic_load(&o->cancel) && gen == o->gen) {
            o->retry_at = now_s() + RETRY_DELAY;
            snprintf(o->status, sizeof(o->status), "%s",
                     o->server && llm_server_state(o->server, NULL, 0) != LLM_READY ? "Waiting for the model\xE2\x80\xA6"
                                                                                   : "Couldn't read that; will retry");
        }
        pthread_mutex_unlock(&o->mu);
    }
    store_thread_done(); /* this thread's database connection, if any */
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
    free(o->renames);
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
        o->io_error = false;
        snprintf(o->status, sizeof(o->status), "%s", o->project[0] ? "Starting\xE2\x80\xA6" : "Open a document to start");
    }
    o->poked = true;
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void observer_rename_doc(Observer *o, const char *project_dir, const char *from, const char *to)
{
    pthread_mutex_lock(&o->mu);
    struct PendingRename *r = realloc(o->renames, (o->nrenames + 1) * sizeof(*r));
    if (r) {
        o->renames = r;
        snprintf(r[o->nrenames].project, sizeof(r[0].project), "%s", project_dir);
        snprintf(r[o->nrenames].from, sizeof(r[0].from), "%s", from);
        snprintf(r[o->nrenames].to, sizeof(r[0].to), "%s", to);
        o->nrenames++;
    }
    o->poked = true;
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void observer_request_rebuild(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    o->rebuild = true;
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void observer_poke(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    o->poked = true;
    if (!o->io_error) o->retry_at = 0;
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

unsigned observer_attempts(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    unsigned v = o->attempts;
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
