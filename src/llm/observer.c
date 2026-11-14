#define _DARWIN_C_SOURCE
#include "observer.h"
#include "observer_text_internal.h"
#include "observer_model_internal.h"
#include "observer_job_internal.h"

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
#include "core/json.h"
#include "llm/http.h"
#include "llm/llm_text.h"
#include "core/deadline_clock.h"

#define CHUNK_MAX 3000        /* bytes of new text per extraction request */
#define PIECE_MAX 2000        /* longest piece of one paragraph or message */
#define IDLE_DELAY 1.5        /* seconds of chat idleness before observer work */
#define IDLE_MAX 120.0        /* the idleness required after repeated cancellations by chat (wait mode) */
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
    GenerativeGate *generative_gate; /* borrowed; changed only with worker idle */
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
    double idle_required;    /* idleness a job needs: IDLE_DELAY, doubled each time chat cancels a running job, up
                              * to IDLE_MAX; back to IDLE_DELAY when a job completes (wait mode, 2026-11-14) */
    bool cancelled_by_chat;  /* the running job was cancelled by chat becoming active */
    unsigned chat_cancels;   /* jobs cancelled by chat, for tests and the status line */
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
    clock_gettime(SCRIBODO_DEADLINE_CLOCK, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_status(Observer *o, const char *s)
{
    pthread_mutex_lock(&o->mu);
    snprintf(o->status, sizeof(o->status), "%s", s);
    pthread_mutex_unlock(&o->mu);
}

/* ---- text helpers ------------------------------------------------------------------ */

static size_t cp_count(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
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

static bool backend_admit(Observer *o, GenerativeTicket *ticket)
{
    if (atomic_load(&o->cancel)) return false;
    if (!o->generative_gate) return true;
    /* Never route background generation to a remote endpoint. The observer
     * model uses only this local server, and readiness is checked pre-claim. */
    if (o->server && llm_server_state(o->server, NULL, 0) != LLM_READY) return false;
    if (generative_gate_try_background(o->generative_gate, ticket, &o->cancel)) return true;
    atomic_store(&o->cancel, true); /* retry this chunk, not an empty success */
    return false;
}

static bool backend_release(Observer *o, GenerativeTicket *ticket)
{
    if (!o->generative_gate) return !atomic_load(&o->cancel);
    bool valid = generative_gate_ticket_valid(ticket);
    generative_gate_finish(ticket);
    if (!valid) atomic_store(&o->cancel, true);
    return valid;
}

static char *backend_extract(Observer *o, const char *chunk, bool is_chat)
{
    GenerativeTicket ticket = { 0 };
    if (!backend_admit(o, &ticket)) return NULL;
    ObserverModel model = { o->server, &o->cancel };
    char *out = observer_model_extract(&model, chunk, is_chat);
    if (!backend_release(o, &ticket)) {
        free(out);
        return NULL;
    }
    return out;
}

static bool backend_judge(Observer *o, const char *canon, const char *statement, char *check, size_t checklen)
{
    GenerativeTicket ticket = { 0 };
    if (!backend_admit(o, &ticket)) return false;
    ObserverModel model = { o->server, &o->cancel };
    bool conflict = observer_model_judge(&model, canon, statement, check, checklen);
    return backend_release(o, &ticket) && conflict;
}

static bool generation_current(Observer *o, uint64_t fence)
{
    bool ok = !atomic_load(&o->cancel) && generative_gate_fence_valid(o->generative_gate, fence);
    if (!ok) atomic_store(&o->cancel, true);
    return ok;
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
    size_t n = 0, changed = 0;
    if (!records_list_checked(store, project, REC_FINDING, &recs, &n)) {
        findings_unlock(lock);
        return 0;
    }
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
static size_t parse_candidates(const ObserverJob *j, const char *json, const char *revision, Cand *out, size_t max)
{
    cJSON *root = json_parse(json);
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
        const ObserverSegment *seg = NULL;
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

/* The judge's explanation, readable on its own: its first sentence (obs_first_sentence, so "Captain James K. Powell"
 * is not cut at the initial), with the prompt's CANON/NEW labels in plain words. */
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
    obs_first_sentence(buf); /* a period after an initial or a title does not end it */
    if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 32);
    utf8_clip(out, outlen, buf, SIZE_MAX);
}

/* ---- one extraction job, in stages ----------------------------------------------------
 * run_job was one 285-line function (AUDIT-003 F5). It is now the stages below over a JobRun: extract, load the
 * records, deduplicate, judge against canon and notes, snapshot, commit under the findings lock, index, mark read.
 * The order, the locks, the generation checks and every condition are the ones the single function had; the
 * headless replay (tools/observer_replay.c) is byte-identical on three novels (journal/observer-cycle1_split.md). */

typedef struct {
    size_t cand;
    char canon_id[40];
    char *canon_text;
    char check[300];
} Conflict;

typedef struct {
    Observer *o;
    const char *project;
    uint64_t gen, fence;
    ObserverJob *j;
    cJSON *st;
    Cand cands[16];
    size_t nc;
    Record *existing, *canon, *notes;
    size_t ne, ncanon, nnotes;
    char sem_pid[64];
    bool use_sem;
    Conflict conflicts[MAX_JUDGE_PAIRS];
    size_t nconf, pairs;
    bool cancelled, io_failed, ok;
} JobRun;

/* A conflict finding already recorded for this canon entry with this text, from this passage (a revised entry is
 * checked afresh). The judge stage and the commit stage ask the same question of two lists. */
static bool conflict_recorded(const Record *existing, size_t ne, const char *canon_id, const char *canon_text, const char *quote)
{
    for (size_t e = 0; e < ne; e++) {
        const RecordVersion *ev = record_current(&existing[e]);
        if (!strcmp(ev->finding.type, "conflict") && !strcmp(ev->finding.related, canon_id) && ev->finding.related_text &&
            canon_text && !strcmp(ev->finding.related_text, canon_text) && ev->nsources && ev->sources[0].quote && quote &&
            !strcmp(ev->sources[0].quote, quote))
            return true;
    }
    return false;
}

/* Stage 1: the backend reads the chunk; its findings become candidates. False when the backend gave nothing. */
static bool job_extract(JobRun *r)
{
    char *json = backend_extract(r->o, r->j->chunk, !r->j->is_doc);
    if (!json) return false;
    r->nc = parse_candidates(r->j, json, "", r->cands, 16);
    free(json);
    return true;
}

/* Stage 2: the stored findings (under the findings lock), canon and notes. False, with io_failed, when a list
 * could not be read. */
static bool job_load_records(JobRun *r)
{
    int findings_fd = findings_lock(r->project);
    bool lists_ok = findings_fd >= 0 && records_list_checked(r->o->store, r->project, REC_FINDING, &r->existing, &r->ne);
    findings_unlock(findings_fd);
    lists_ok = lists_ok && records_list_checked(r->o->store, r->project, REC_CANON, &r->canon, &r->ncanon) &&
               records_list_checked(r->o->store, r->project, REC_NOTE, &r->notes, &r->nnotes);
    if (!lists_ok) r->io_failed = true;
    return lists_ok;
}

/* Stage 3: deduplicate against stored findings and within this batch. */
static void job_dedupe(JobRun *r)
{
    for (size_t i = 0; i < r->nc; i++) {
        Cand *c = &r->cands[i];
        char key[400], other[400];
        finding_key(c->type, c->info.subject, c->info.relation, c->info.object, c->desc, key, sizeof(key));
        for (size_t e = 0; e < r->ne && c->keep; e++) {
            const RecordVersion *v = record_current(&r->existing[e]);
            finding_key(v->finding.type, v->finding.subject, v->finding.relation, v->finding.object, v->content, other, sizeof(other));
            if (!strcmp(key, other)) c->keep = false;
        }
        for (size_t p = 0; p < i && c->keep; p++) {
            if (!r->cands[p].keep) continue;
            finding_key(r->cands[p].type, r->cands[p].info.subject, r->cands[p].info.relation, r->cands[p].info.object,
                        r->cands[p].desc, other, sizeof(other));
            if (!strcmp(key, other)) c->keep = false;
        }
    }
}

/* Stage 4: projects in the store get semantic retrieval when the index is up; canon and notes are synced into it
 * first (cheap when unchanged). */
static void job_semantic(JobRun *r)
{
    r->use_sem = r->o->semantic &&
                 project_store_feature_key(r->o->store, r->project, PROJECT_STORE_CAP_SEMANTIC, r->sem_pid, sizeof(r->sem_pid)) &&
                 semantic_ready(r->o->semantic);
    if (r->use_sem) {
        char serr[200];
        r->use_sem = semantic_sync_records(r->o->semantic, r->sem_pid, serr, sizeof(serr));
    }
}

/* The canon entries one statement is judged against: up to two nearest in meaning (projects with an index), then the
 * best two by shared words, three at most, each once. */
static int job_canon_candidates(JobRun *r, const char *stmt, size_t cand[4])
{
    size_t best[2] = { SIZE_MAX, SIZE_MAX };
    int score[2] = { 1, 1 };
    for (size_t c = 0; c < r->ncanon; c++) {
        const RecordVersion *v = record_current(&r->canon[c]);
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
    /* Semantic candidates: the entries nearest in meaning, which shared words can miss ("the old woman" vs canon
     * about "Grandmother Signe"). Lexical ones too. */
    int ncand = 0;
    for (int k = 0; k < 4; k++) cand[k] = SIZE_MAX;
    if (r->use_sem) {
        SemHit *hits = NULL;
        size_t nh = semantic_canon_for(r->o->semantic, r->sem_pid, stmt, 2, &hits);
        for (size_t k = 0; k < nh && ncand < 2; k++)
            for (size_t c = 0; c < r->ncanon; c++)
                if (!strcmp(r->canon[c].id, hits[k].record_id) && !strcmp(record_current(&r->canon[c])->status, "committed")) {
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
    return ncand;
}

/* One statement against one canon entry: the backend's verdict, once per canon entry and passage, and not when the
 * same conflict is already recorded. Returns false when the job was cancelled meanwhile. */
static bool job_judge_pair(JobRun *r, size_t i, size_t c, const char *stmt)
{
    r->pairs++;
    const RecordVersion *v = record_current(&r->canon[c]);
    char check[300];
    bool conflict = backend_judge(r->o, v->content, stmt, check, sizeof(check));
    if (atomic_load(&r->o->cancel)) return false;
    for (size_t q = 0; conflict && q < r->nconf; q++) /* same canon, same passage: once */
        if (!strcmp(r->conflicts[q].canon_id, r->canon[c].id) && r->cands[r->conflicts[q].cand].src.start == r->cands[i].src.start)
            conflict = false;
    if (conflict && conflict_recorded(r->existing, r->ne, r->canon[c].id, v->content, r->cands[i].src.quote)) conflict = false;
    if (conflict && r->nconf < MAX_JUDGE_PAIRS) {
        Conflict *cf = &r->conflicts[r->nconf++];
        cf->cand = i;
        snprintf(cf->canon_id, sizeof(cf->canon_id), "%s", r->canon[c].id);
        cf->canon_text = strdup(v->content);
        plain_check(check, cf->check, sizeof(cf->check));
    }
    return true;
}

/* A related note (no model call): the first live note sharing three words with the statement. */
static void job_note_overlap(JobRun *r, size_t i, const char *stmt)
{
    for (size_t n = 0; n < r->nnotes; n++) {
        const RecordVersion *v = record_current(&r->notes[n]);
        if (!strcmp(v->status, "deleted")) continue;
        if (obs_overlap(stmt, v->content) >= 3) {
            snprintf(r->cands[i].info.related, sizeof(r->cands[i].info.related), "%s", r->notes[n].id);
            r->cands[i].info.related_text = strdup(v->content);
            break;
        }
    }
}

/* Stage 5: compare every statement with committed canon (possible conflicts) and notes (overlaps). Every statement
 * is checked, including ones that duplicate an existing finding ("Zoë" is known, but "Zoë's brown eyes" may still
 * contradict canon). A question asserts nothing. */
static void job_judge(JobRun *r)
{
    for (size_t i = 0; i < r->nc && !r->cancelled; i++) {
        if (!strcmp(r->cands[i].type, "question")) continue;
        char stmt[600];
        snprintf(stmt, sizeof(stmt), "%s (\xE2\x80\x9C%.300s\xE2\x80\x9D)", r->cands[i].desc, r->cands[i].src.quote);
        size_t cand[4];
        int ncand = job_canon_candidates(r, stmt, cand);
        for (int b = 0; b < ncand && r->pairs < MAX_JUDGE_PAIRS; b++) {
            if (cand[b] == SIZE_MAX) continue;
            if (!job_judge_pair(r, i, cand[b], stmt)) {
                r->cancelled = true;
                break;
            }
        }
        job_note_overlap(r, i, stmt);
    }
}

/* Stage 6: document sources point at an immutable snapshot of the text that was read (usually the save's own
 * revision, reused), only taken when there is something to point at; then the project must be unchanged. */
static void job_snapshot(JobRun *r)
{
    r->ok = !r->cancelled && generation_current(r->o, r->fence);
    bool any = r->nconf > 0;
    for (size_t i = 0; i < r->nc; i++) any = any || r->cands[i].keep;
    if (r->ok && any && r->j->is_doc) {
        char revision[160] = "";
        r->ok = revisions_snapshot(r->o->store, r->project, r->j->path, r->j->src, revision, sizeof(revision));
        for (size_t i = 0; i < r->nc; i++) snprintf(r->cands[i].src.revision, sizeof(r->cands[i].src.revision), "%s", revision);
    }
    pthread_mutex_lock(&r->o->mu);
    r->ok = r->ok && r->gen == r->o->gen && generation_current(r->o, r->fence); /* project unchanged */
    pthread_mutex_unlock(&r->o->mu);
}

/* Under the final write lock: model work happened outside it, so the stored findings are read again and the
 * candidates and conflicts another process may have created meanwhile are dropped. */
static void job_revalidate(JobRun *r)
{
    records_free(r->existing, r->ne);
    r->existing = NULL;
    if (!records_list_checked(r->o->store, r->project, REC_FINDING, &r->existing, &r->ne)) r->io_failed = true;
    for (size_t i = 0; i < r->nc; i++)
        if (!r->io_failed && r->cands[i].keep && candidate_already_exists(&r->cands[i], r->existing, r->ne)) r->cands[i].keep = false;
    for (size_t c = 0; !r->io_failed && c < r->nconf; c++) {
        const Cand *candidate = &r->cands[r->conflicts[c].cand];
        if (conflict_recorded(r->existing, r->ne, r->conflicts[c].canon_id, r->conflicts[c].canon_text, candidate->src.quote))
            r->conflicts[c].cand = SIZE_MAX;
    }
}

/* The kept candidates and the conflicts become findings, each a new record; stops at a generation change. Each id is
 * drawn against the findings re-read under the lock, so it is never one another process (or a log written before the
 * per-second count) already holds: records_create_ex writes without reading, and a reused id would hide the earlier
 * finding as a version of the new one (red-team 2026-11-12, journal/redteam-loose_ends.md). */
static void job_create(JobRun *r, int findings_fd)
{
    for (size_t i = 0; !r->io_failed && findings_fd >= 0 && i < r->nc; i++) {
        if (!generation_current(r->o, r->fence)) { r->ok = false; break; }
        if (!r->cands[i].keep) continue;
        char id[40];
        records_new_id_unused(REC_FINDING, r->existing, r->ne, id, sizeof(id));
        if (!records_create_ex(r->o->store, r->project, REC_FINDING, id, "new", r->cands[i].desc, &r->cands[i].src, 1, &r->cands[i].info))
            r->io_failed = true;
    }
    for (size_t c = 0; !r->io_failed && findings_fd >= 0 && c < r->nconf; c++) {
        if (!generation_current(r->o, r->fence)) { r->ok = false; break; }
        if (r->conflicts[c].cand == SIZE_MAX) continue;
        Cand *cd = &r->cands[r->conflicts[c].cand];
        FindingInfo fi = { 0 };
        snprintf(fi.type, sizeof(fi.type), "conflict");
        snprintf(fi.subject, sizeof(fi.subject), "%s", cd->info.subject);
        snprintf(fi.related, sizeof(fi.related), "%s", r->conflicts[c].canon_id);
        fi.related_text = r->conflicts[c].canon_text;
        char id[40];
        records_new_id_unused(REC_FINDING, r->existing, r->ne, id, sizeof(id));
        if (!records_create_ex(r->o->store, r->project, REC_FINDING, id, "new", r->conflicts[c].check, &cd->src, 1, &fi)) r->io_failed = true;
    }
}

/* Index what was read (documents as draft text, chats as chat), and drop the document's points for pieces it no
 * longer has. */
static void job_index(JobRun *r)
{
    const ObserverJob *j = r->j;
    char did[40] = "";
    if (j->is_doc) store_document_id(r->o->store, r->sem_pid, j->name, true, did, sizeof(did));
    char rev[160] = "";
    if (j->is_doc) revisions_snapshot(r->o->store, r->project, j->path, j->src, rev, sizeof(rev));
    for (size_t q = 0; q < j->nsegs; q++) {
        const ObserverSegment *sg = &j->segs[q];
        char *piece = dupn(j->chunk + sg->chunk_off, sg->len);
        if (!piece) continue;
        const char *base = j->is_doc ? j->src : j->msgs[sg->message];
        size_t start = cp_count(base, sg->src_off), len = cp_count(piece, sg->len);
        if (j->is_doc) semantic_index_text(r->o->semantic, r->sem_pid, "draft", did, rev[0] ? rev : NULL, NULL, 0, start, len, piece);
        else semantic_index_text(r->o->semantic, r->sem_pid, "chat", NULL, NULL, j->name, sg->message, start, len, piece);
        free(piece);
    }
    if (j->is_doc && did[0] && j->current) {
        size_t n = (size_t)cJSON_GetArraySize(j->current), k = 0;
        const char **hs = calloc(n ? n : 1, sizeof(char *));
        cJSON *it;
        cJSON_ArrayForEach(it, j->current) if (hs && cJSON_IsString(it)) hs[k++] = it->valuestring;
        if (hs) semantic_prune_doc(r->o->semantic, r->sem_pid, did, hs, k);
        free(hs);
    }
}

/* Mark the text read in the state: a document's piece hashes, or the chat's position. */
static void job_mark_read(JobRun *r)
{
    const ObserverJob *j = r->j;
    if (j->is_doc) {
        cJSON *docs = cJSON_GetObjectItem(r->st, "docs");
        cJSON *arr = cJSON_GetObjectItem(docs, j->name);
        if (!cJSON_IsArray(arr)) {
            cJSON_DeleteItemFromObject(docs, j->name);
            arr = cJSON_AddArrayToObject(docs, j->name);
        }
        if (!arr) r->io_failed = true;
        for (size_t h = 0; !r->io_failed && h < j->nhashes; h++) {
            char hex[17];
            snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)j->hashes[h]);
            cJSON *value = cJSON_CreateString(hex);
            if (!value || !cJSON_AddItemToArray(arr, value)) {
                cJSON_Delete(value);
                r->io_failed = true;
            }
        }
    } else {
        if (!observer_job_set_chat_progress(cJSON_GetObjectItem(r->st, "chats"), j->name, j->msg_to, j->off_to)) r->io_failed = true;
    }
}

/* Stage 7: the findings are written under the lock, the index updated, the text marked read and the state saved,
 * unless a finding couldn't be saved: then the text is read again later rather than silently missing (red-team 008). */
static void job_commit(JobRun *r)
{
    int findings_fd = findings_lock(r->project);
    if (findings_fd < 0) r->io_failed = true;
    if (findings_fd >= 0) job_revalidate(r);
    job_create(r, findings_fd);
    findings_unlock(findings_fd);
    if (r->ok && generation_current(r->o, r->fence) && r->use_sem) job_index(r);
    if (!(r->io_failed || !r->ok || !generation_current(r->o, r->fence))) job_mark_read(r);
    r->ok = r->ok && !r->io_failed && generation_current(r->o, r->fence);
    if (r->ok && !observer_job_state_save(r->o->store, r->project, r->st)) r->io_failed = true;
    r->ok = r->ok && !r->io_failed && generation_current(r->o, r->fence);
}

static void job_end(JobRun *r)
{
    for (size_t i = 0; i < r->nc; i++) {
        free(r->cands[i].desc);
        free(r->cands[i].src.quote);
        free(r->cands[i].info.related_text);
    }
    for (size_t c = 0; c < r->nconf; c++) free(r->conflicts[c].canon_text);
    records_free(r->existing, r->ne);
    records_free(r->canon, r->ncanon);
    records_free(r->notes, r->nnotes);
}

/* Returns true if the job completed (its text is now read). */
static bool run_job(Observer *o, const char *project, uint64_t gen, ObserverJob *j, cJSON *st, bool *io_out)
{
    JobRun r = { .o = o, .project = project, .gen = gen, .fence = generative_gate_fence(o->generative_gate), .j = j, .st = st };
    if (!job_extract(&r)) return false;
    if (job_load_records(&r)) {
        job_dedupe(&r);
        job_semantic(&r);
        job_judge(&r);
        job_snapshot(&r);
        if (r.ok) job_commit(&r);
    }
    job_end(&r);
    if (io_out) *io_out = r.io_failed;
    return r.ok;
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
            bool idle = !atomic_load(&o->chat_active) && t - o->idle_since >= o->idle_required;
            bool ready = (o->poked || o->rebuild) && o->project[0] && o->enabled && t >= o->retry_at;
            if (ready && idle) break;
            if (ready && !idle && !waiting_shown) {
                /* Wait mode: after chat has cancelled a running job, the observer asks for more idleness before it
                 * starts again (doubling to IDLE_MAX), so a chat that sends every minute no longer costs a model call
                 * that is cancelled twelve seconds in and a job that restarts from its beginning each time
                 * (journal/gap003-cycle1_series.md, "the observer starves under continuous chat"). */
                if (o->idle_required > IDLE_DELAY)
                    snprintf(o->status, sizeof(o->status), "Waiting for the chat to settle (%.0f s)\xE2\x80\xA6", o->idle_required);
                else
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
        cJSON *st = observer_job_state_load(o->store, project);
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
        ObserverJob j;
        bool discovery_error = false;
        if (!observer_job_find(o->store, project, st, &j, &discovery_error)) {
            bool saved = !discovery_error && observer_job_state_save(o->store, project, st); /* pruned state; blank text marked read */
            cJSON_Delete(st);
            pthread_mutex_lock(&o->mu);
            if (gen == o->gen)
                snprintf(o->status, sizeof(o->status), "%s",
                         saved ? "Up to date" : discovery_error ? "Can't read observer inputs — retrying" : "Can't write .scribodo/observer/state.json");
            if (discovery_error) {
                o->poked = true;
                o->retry_at = now_s() + RETRY_DELAY;
            }
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
        observer_job_free(&j);
        cJSON_Delete(st);
        pthread_mutex_lock(&o->mu);
        o->running = false;
        o->poked = true; /* more may be waiting (or this one, again) */
        if (done) {
            o->version++;
            o->jobs_done++;
            o->io_error = false;
            o->idle_required = IDLE_DELAY; /* a job got through: the chat has settled */
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
        } else if (o->generative_gate && gen == o->gen) {
            /* Admission is polled on this worker, never on the UI thread.
             * Avoid a hot retry loop while foreground intent remains queued. */
            o->retry_at = now_s() + 0.2;
        }
        if (o->cancelled_by_chat) {
            o->cancelled_by_chat = false;
            o->chat_cancels++;
            o->idle_required = o->idle_required * 2 > IDLE_MAX ? IDLE_MAX : o->idle_required * 2;
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
    o->idle_required = IDLE_DELAY;
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

bool observer_set_generative_gate(Observer *o, GenerativeGate *gate)
{
    if (!o) return false;
    pthread_mutex_lock(&o->mu);
    bool idle = !o->running && !o->stop;
    if (idle) o->generative_gate = gate;
    pthread_mutex_unlock(&o->mu);
    return idle;
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
        if (o->running) o->cancelled_by_chat = true; /* counted when the job unwinds */
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

double observer_idle_required(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    double d = o->idle_required;
    pthread_mutex_unlock(&o->mu);
    return d;
}

unsigned observer_chat_cancels(Observer *o)
{
    pthread_mutex_lock(&o->mu);
    unsigned n = o->chat_cancels;
    pthread_mutex_unlock(&o->mu);
    return n;
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
    size_t n = 0;
    if (!records_list_checked(o->store, project_dir, REC_FINDING, &list, &n)) {
        findings_unlock(lock);
        return false;
    }
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
