#define _DARWIN_C_SOURCE
#include "observer.h"
#include "observer_text_internal.h"

#include "core/utf8.h"

#include <ctype.h>
#include <sys/time.h>
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
#include <sys/file.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "app/chat_store.h"
#include "app/project_store.h"
#include "app/store.h"
#include "app/store_entities.h"
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

/* Finding read/modify/write sequences are serialized per project and across
 * processes; unrelated projects never share a process-global mutex. */
static int findings_lock(const char *project)
{
    char dir[PATH_MAX + 32], path[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.scribodo", project ? project : "");
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(path, sizeof(path), "%s/findings.lock", dir);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void findings_unlock(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

struct Observer {
    StoreManager *store;
    LlmServer *server;
    SemanticClient *semantic;
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
    char author_name[64], assistant_name[64]; /* who is who: never findings in chats */
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

/* A chat is a conversation about the story, not the story: only the
 * story-world facts in it are findings (the author's decision, user
 * testing 2026-11-04). */
static const char *k_extract_chat =
    "\nThis passage is a conversation between the author (AUTHOR) and their writing assistant (ASSISTANT). Extract "
    "only facts about the author's story world: its characters, places, objects, events and open questions. Ignore "
    "the conversation itself: greetings, the names the author and the assistant use for themselves or each other, "
    "what the assistant offers, asks or suggests, and talk about the writing process. If the conversation says "
    "nothing about the story world, return no findings.";

static char *backend_extract(Observer *o, const char *chunk, bool is_chat)
{
    if (!o->server) {
        usleep(20000);
        return atomic_load(&o->cancel) ? NULL : obs_fake_extract(chunk);
    }
    size_t n = strlen(chunk) + 16;
    char *user = malloc(n);
    if (!user) return NULL;
    snprintf(user, n, "PASSAGE:\n%s", chunk);
    char sys[2400];
    snprintf(sys, sizeof(sys), "%s%s", k_extract_system, is_chat ? k_extract_chat : "");
    char *out = complete(o, sys, user, extract_schema(), 1400);
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

static cJSON *state_load(StoreManager *store, const char *project)
{
    return project_store_observer_state_load(store, project);
}

static bool state_save(StoreManager *store, const char *project, cJSON *state)
{
    return project_store_observer_state_save(store, project, state);
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
    char author[64], assistant[64]; /* who is who, when the job started */
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
static bool find_job(StoreManager *store, const char *project, cJSON *st, Job *j)
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
    size_t nc = chat_store_list(store, project, &list);
    for (cJSON *it = chats ? chats->child : NULL, *next; it; it = next) { /* forget deleted chats */
        next = it->next;
        bool present = false;
        for (size_t i = 0; i < nc && !present; i++) present = !strcmp(list[i].id, it->string);
        if (!present) cJSON_DeleteItemFromObject(chats, it->string);
    }
    for (size_t i = 0; i < nc && !found; i++) {
        if (list[i].observer_ignore) continue; /* the author said not to read it */
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load(store, project, list[i].id, &msgs, &n)) continue;
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
        if (obs_word_byte(*p)) {
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

static bool candidate_already_exists(const Cand *candidate, const Record *existing, size_t count)
{
    char key[400], other[400];
    finding_key(candidate->type, candidate->info.subject, candidate->info.relation, candidate->info.object,
                candidate->desc, key, sizeof(key));
    for (size_t i = 0; i < count; i++) {
        const RecordVersion *version = record_current(&existing[i]);
        finding_key(version->finding.type, version->finding.subject, version->finding.relation,
                    version->finding.object, version->content, other, sizeof(other));
        if (!strcmp(key, other)) return true;
    }
    return false;
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
        bool w = obs_word_byte(*p);
        if (w && !in) words++;
        in = w;
    }
    if (n >= 12 && words >= 3) return true;
    return !strcmp(type, "entity") && strlen(subject) >= 3 && strcasestr(quote, subject) != NULL;
}

/* ---- the observer's rules for chats, versioned ------------------------------------
 * Bump OBS_RULES_VERSION whenever what counts as a chat finding changes:
 * existing findings are then re-checked automatically (obs_refilter), so
 * the software repairs what it recorded under older rules (user testing). */
#define OBS_RULES_VERSION 2

static bool same_name(const char *text, const char *name)
{
    if (!text || !name || !name[0]) return false;
    char a[256], b[256];
    entity_norm(text, a, sizeof(a));
    entity_norm(name, b, sizeof(b));
    if (!a[0] || !b[0]) return false;
    if (!strcmp(a, b)) return true;
    size_t fw = strcspn(a, " "); /* "Tripp Flynn" vs "Tripp" */
    return strlen(b) == fw && !strncmp(a, b, fw);
}

/* "tripp" in "Tripp proposes 'Scribe'..." as a whole word. */
static bool mentions_name(const char *text, const char *name)
{
    if (!text || !name || !name[0]) return false;
    size_t n = strlen(name);
    for (const char *p = text; (p = strcasestr(p, name)) != NULL; p += n) {
        bool start = p == text || !(isalnum((unsigned char)p[-1]) || ((unsigned char)p[-1] & 0x80));
        bool end = !(isalnum((unsigned char)p[n]) || ((unsigned char)p[n] & 0x80));
        if (start && end) return true;
    }
    return false;
}

/* A subject that looks like a name: capitalised, one to four words. */
static bool named_subject(const char *subject)
{
    while (*subject == ' ') subject++;
    if (!obs_upper_start((const unsigned char *)subject) || strlen(subject) < 2) return false;
    int words = 0;
    for (const char *p = subject; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        words++;
        while (*p && *p != ' ') p++;
    }
    return words <= 4;
}

/* A capitalised word that isn't the first: "Should Ingrid know…" names
 * someone; "What happens next?" doesn't. */
static bool names_someone(const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    bool first = true;
    while (*p) {
        while (*p && (*p == ' ' || ispunct(*p))) p++;
        if (!*p) break;
        if (!first && obs_upper_start(p)) return true;
        first = false;
        while (*p && *p != ' ') p++;
    }
    return false;
}

bool obs_chat_finding_allowed(const FindingInfo *f, const char *desc, const char *author, const char *assistant)
{
    const char *subject = f ? f->subject : "";
    if (obs_about_the_conversation(subject, desc)) return false;
    /* In a chat, a finding is about something named in the story (a
     * character, place, object): talk about the work ("drafts, notes, and
     * chats") or the session ("first scene / character notes") isn't. */
    if (!named_subject(subject) && !(f && !strcmp(f->type, "question") && names_someone(desc))) return false;
    /* The two people in the conversation are never story findings. */
    const char *names[2] = { author, assistant };
    for (int k = 0; k < 2; k++) {
        if (!names[k] || !names[k][0]) continue;
        if (same_name(subject, names[k]) || (f && same_name(f->object, names[k]))) return false;
        if (mentions_name(desc, names[k])) return false;
    }
    return true;
}

static void rules_signature(const char *author, const char *assistant, char out[17])
{
    char a[256], b[256], key[600];
    entity_norm(author ? author : "", a, sizeof(a));
    entity_norm(assistant ? assistant : "", b, sizeof(b));
    snprintf(key, sizeof(key), "rules=%d|%s|%s", OBS_RULES_VERSION, a, b);
    uint64_t h = 1469598103934665603ull;
    for (const char *p = key; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

static bool rules_get(StoreManager *store, const char *project, char out[17])
{
    return project_store_observer_rules_get(store, project, out);
}

static bool rules_set(StoreManager *store, const char *project, const char *sig)
{
    return project_store_observer_rules_set(store, project, sig);
}

/* Re-check every chat finding against the current rules: ones that no
 * longer qualify are marked "filtered" (kept, hidden, out of the graph);
 * filtered ones that qualify again are restored. Decisions the author made
 * (saved, committed, dismissed) are never touched. Returns how many
 * changed. */
size_t obs_refilter(StoreManager *store, const char *project, const char *author, const char *assistant)
{
    int lock = findings_lock(project);
    if (lock < 0) return 0;
    Record *recs = NULL;
    size_t n = records_list(store, project, REC_FINDING, &recs), changed = 0;
    for (size_t i = 0; i < n; i++) {
        const RecordVersion *v = record_current(&recs[i]);
        bool from_chat = v->nsources && v->sources[0].kind == SRC_CHAT;
        if (!from_chat) continue;
        bool is_new = !strcmp(v->status, "new"), is_filtered = !strcmp(v->status, "filtered");
        if (!is_new && !is_filtered) continue;
        bool allowed = obs_chat_finding_allowed(&v->finding, v->content, author, assistant);
        const char *to = is_new && !allowed ? "filtered" : is_filtered && allowed ? "new" : NULL;
        if (!to) continue;
        if (records_append_ex(store, project, REC_FINDING, recs[i].id, to, v->content, v->sources, v->nsources, &v->finding, NULL))
            changed++;
    }
    records_free(recs, n);
    findings_unlock(lock);
    return changed;
}

/* Does a chat finding describe the conversation rather than the story
 * ("Tripp asks the assistant…", "the name 'Maz' is…", "a greeting…")? */
bool obs_about_the_conversation(const char *subject, const char *desc)
{
    static const char *const marks[] = { "assistant", "the author", "the user", "scribodo", "writing partner",
                                         "conversation", "greeting", "the name '", "the name \"", "the name \xE2\x80\x98",
                                         "the name \xE2\x80\x9C", "chatbot", "the ai " };
    const char *fields[2] = { subject ? subject : "", desc ? desc : "" };
    for (int f = 0; f < 2; f++)
        for (size_t k = 0; k < sizeof(marks) / sizeof(marks[0]); k++)
            if (strcasestr(fields[f], marks[k])) return true;
    return false;
}
#define about_the_conversation obs_about_the_conversation

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
        if (!strcmp(t, "relationship") && (!obs_upper_start((const unsigned char *)c->info.subject) ||
                                           !obs_upper_start((const unsigned char *)c->info.object)))
            continue;
        /* A description that never names its subject ("A man who is
         * hunched over the lens") gets the name in front. */
        if (!strcmp(t, "entity") && c->info.subject[0] && obs_upper_start((const unsigned char *)c->info.subject) &&
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
        /* In a chat, a finding about the conversation itself is dropped,
         * whatever the model says. */
        if (c->keep && !j->is_doc && !obs_chat_finding_allowed(&c->info, c->desc, j->author, j->assistant)) c->keep = false;
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
    char *json = backend_extract(o, j->chunk, !j->is_doc);
    if (!json) return false;
    Cand cands[16];
    size_t nc = parse_candidates(j, json, "", cands, 16);
    free(json);

    /* Deduplicate against stored findings (and within this batch). */
    int findings_fd = findings_lock(project);
    Record *existing = NULL, *canon = NULL, *notes = NULL;
    size_t ne = findings_fd >= 0 ? records_list(o->store, project, REC_FINDING, &existing) : 0;
    findings_unlock(findings_fd);
    size_t ncanon = records_list(o->store, project, REC_CANON, &canon), nnotes = records_list(o->store, project, REC_NOTE, &notes);
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
    bool use_sem = o->semantic && project_store_feature_key(o->store, project, PROJECT_STORE_CAP_SEMANTIC, sem_pid, sizeof(sem_pid)) &&
                   semantic_ready(o->semantic);
    if (use_sem) {
        char serr[200];
        use_sem = semantic_sync_records(o->semantic, sem_pid, serr, sizeof(serr));
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
            size_t nh = semantic_canon_for(o->semantic, sem_pid, stmt, 2, &hits);
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
        ok = revisions_snapshot(o->store, project, j->path, j->src, revision, sizeof(revision));
        for (size_t i = 0; i < nc; i++) snprintf(cands[i].src.revision, sizeof(cands[i].src.revision), "%s", revision);
    }
    pthread_mutex_lock(&o->mu);
    ok = ok && gen == o->gen; /* project unchanged */
    pthread_mutex_unlock(&o->mu);
    if (ok) {
        findings_fd = findings_lock(project);
        if (findings_fd < 0) io_failed = true;
        if (findings_fd >= 0) {
            /* Model work happens outside the lock. Revalidate under the final
             * write lock so another process cannot create the same finding
             * between our first deduplication read and append. */
            records_free(existing, ne);
            existing = NULL;
            ne = records_list(o->store, project, REC_FINDING, &existing);
            for (size_t i = 0; i < nc; i++)
                if (cands[i].keep && candidate_already_exists(&cands[i], existing, ne)) cands[i].keep = false;
            for (size_t c = 0; c < nconf; c++) {
                Cand *candidate = &cands[conflicts[c].cand];
                for (size_t e = 0; e < ne; e++) {
                    const RecordVersion *version = record_current(&existing[e]);
                    bool duplicate = !strcmp(version->finding.type, "conflict") &&
                                     !strcmp(version->finding.related, conflicts[c].canon_id) &&
                                     version->finding.related_text && conflicts[c].canon_text &&
                                     !strcmp(version->finding.related_text, conflicts[c].canon_text) &&
                                     version->nsources && version->sources[0].quote && candidate->src.quote &&
                                     !strcmp(version->sources[0].quote, candidate->src.quote);
                    if (duplicate) {
                        conflicts[c].cand = SIZE_MAX;
                        break;
                    }
                }
            }
        }
        for (size_t i = 0; findings_fd >= 0 && i < nc; i++) {
            if (!cands[i].keep) continue;
            char id[40];
            records_new_id(REC_FINDING, id, sizeof(id));
            if (!records_create_ex(o->store, project, REC_FINDING, id, "new", cands[i].desc, &cands[i].src, 1, &cands[i].info))
                io_failed = true;
        }
        for (size_t c = 0; findings_fd >= 0 && c < nconf; c++) {
            if (conflicts[c].cand == SIZE_MAX) continue;
            Cand *cd = &cands[conflicts[c].cand];
            FindingInfo fi = { 0 };
            snprintf(fi.type, sizeof(fi.type), "conflict");
            snprintf(fi.subject, sizeof(fi.subject), "%s", cd->info.subject);
            snprintf(fi.related, sizeof(fi.related), "%s", conflicts[c].canon_id);
            fi.related_text = conflicts[c].canon_text;
            char id[40];
            records_new_id(REC_FINDING, id, sizeof(id));
            if (!records_create_ex(o->store, project, REC_FINDING, id, "new", conflicts[c].check, &cd->src, 1, &fi)) io_failed = true;
        }
        findings_unlock(findings_fd);
        /* Index what was read (documents as draft text, chats as chat), and
         * drop the document's points for pieces it no longer has. */
        if (use_sem) {
            char did[40] = "";
            if (j->is_doc) store_document_id(o->store, sem_pid, j->name, true, did, sizeof(did));
            char rev[160] = "";
            if (j->is_doc) revisions_snapshot(o->store, project, j->path, j->src, rev, sizeof(rev));
            for (size_t q = 0; q < j->nsegs; q++) {
                const Seg *sg = &j->segs[q];
                char *piece = dupn(j->chunk + sg->chunk_off, sg->len);
                if (!piece) continue;
                const char *base = j->is_doc ? j->src : j->msgs[sg->message];
                size_t start = cp_count(base, sg->src_off), len = cp_count(piece, sg->len);
                if (j->is_doc) semantic_index_text(o->semantic, sem_pid, "draft", did, rev[0] ? rev : NULL, NULL, 0, start, len, piece);
                else semantic_index_text(o->semantic, sem_pid, "chat", NULL, NULL, j->name, sg->message, start, len, piece);
                free(piece);
            }
            if (j->is_doc && did[0] && j->current) {
                size_t n = (size_t)cJSON_GetArraySize(j->current), k = 0;
                const char **hs = calloc(n ? n : 1, sizeof(char *));
                cJSON *it;
                cJSON_ArrayForEach(it, j->current) if (hs && cJSON_IsString(it)) hs[k++] = it->valuestring;
                if (hs) semantic_prune_doc(o->semantic, sem_pid, did, hs, k);
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
        if (!io_failed && !state_save(o->store, project, st)) io_failed = true;
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
            if (project_store_feature_key(o->store, project, PROJECT_STORE_CAP_SEMANTIC, pid, sizeof(pid)))
                n = semantic_rebuild(o->semantic, pid, err, sizeof(err));
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
        cJSON *st = state_load(o->store, project);
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
        char author[64], assistant[64];
        snprintf(author, sizeof(author), "%s", o->author_name);
        snprintf(assistant, sizeof(assistant), "%s", o->assistant_name);
        pthread_mutex_unlock(&o->mu);
        /* The rules (or the names) changed since this project's findings were
         * made: re-check them, so the software repairs what it recorded. */
        char sig[17], had[17];
        rules_signature(author, assistant, sig);
        if (rules_get(o->store, project, had) && strcmp(had, sig) != 0) {
            size_t changed = obs_refilter(o->store, project, author, assistant);
            rules_set(o->store, project, sig);
            if (changed) {
                pthread_mutex_lock(&o->mu);
                o->version++; /* the findings list and the graph refresh */
                pthread_mutex_unlock(&o->mu);
            }
        }
        Job j;
        if (!find_job(o->store, project, st, &j)) {
            bool saved = state_save(o->store, project, st); /* pruned state; blank text marked read */
            cJSON_Delete(st);
            pthread_mutex_lock(&o->mu);
            if (gen == o->gen) snprintf(o->status, sizeof(o->status), "%s", saved ? "Up to date" : "Can't write .scribodo/observer/state.json");
            o->running = false;
            pthread_mutex_unlock(&o->mu);
            continue;
        }
        snprintf(j.author, sizeof(j.author), "%s", author);
        snprintf(j.assistant, sizeof(j.assistant), "%s", assistant);
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
    store_thread_done(o->store); /* this thread's database connection, if any */
    return NULL;
}

Observer *observer_create(StoreManager *store, LlmServer *server, SemanticClient *semantic)
{
    if (!store) return NULL;
    Observer *o = calloc(1, sizeof(Observer));
    if (!o) return NULL;
    o->store = store;
    o->server = server;
    o->semantic = semantic;
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
    if (!o) return false;
    int lock = findings_lock(project_dir);
    if (lock < 0) return false;
    Record *list = NULL;
    size_t n = records_list(o->store, project_dir, REC_FINDING, &list);
    bool ok = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].id, id)) continue;
        const RecordVersion *v = record_current(&list[i]);
        ok = records_append_ex(o->store, project_dir, REC_FINDING, id, status, v->content, v->sources, v->nsources,
                               &v->finding, NULL);
        break;
    }
    records_free(list, n);
    findings_unlock(lock);
    if (ok) {
        pthread_mutex_lock(&o->mu);
        o->version++;
        pthread_mutex_unlock(&o->mu);
    }
    return ok;
}

void observer_set_names(Observer *o, const char *author, const char *assistant)
{
    pthread_mutex_lock(&o->mu);
    snprintf(o->author_name, sizeof(o->author_name), "%s", author ? author : "");
    snprintf(o->assistant_name, sizeof(o->assistant_name), "%s", assistant ? assistant : "");
    o->poked = true; /* the rules' signature changed: re-check */
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}
