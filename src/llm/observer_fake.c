#define _DARWIN_C_SOURCE
#include "observer.h"
#include "observer_text_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

#include "cJSON.h"

static char *dupn(const char *s, size_t n)
{
    char *copy = malloc(n + 1);
    if (copy) {
        memcpy(copy, s, n);
        copy[n] = '\0';
    }
    return copy;
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
    while (ns > s && obs_word_byte((unsigned char)ns[-1])) ns--;
    if (p - ns < 2 || p - ns > 40) return false;
    snprintf(name, 48, "%.*s", (int)(p - ns), ns);
    const char *a = p + plen;
    int an = 0;
    while (a[an] && obs_word_byte((unsigned char)a[an])) an++;
    if (!an || an > 40) return false;
    snprintf(attr, 48, "%.*s", an, a);
    const char *v = a + an;
    if (!strncmp(v, " is ", 4)) v += 4;
    else if (!strncmp(v, " are ", 5)) v += 5;
    else return false;
    int vn = 0;
    while (v[vn] && obs_word_byte((unsigned char)v[vn])) vn++;
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

/* "Zoë is Mara's sister." -> Zoë, "sister of", Mara (the fake model's one
 * relationship form). */
static bool rel_pattern(const char *sent, char *a, char *rel, char *b)
{
    char tail[8] = "";
    if (sscanf(sent, "%47[^ ] is %47[^' ]'s %47[a-z]%7s", a, b, rel, tail) != 4 || strcmp(tail, ".") != 0) return false;
    if (!obs_upper_start((const unsigned char *)a) || !obs_upper_start((const unsigned char *)b)) return false;
    size_t n = strlen(rel);
    if (n + 4 < 48) memcpy(rel + n, " of", 4);
    return true;
}

char *obs_fake_extract(const char *chunk)
{
    /* Test hook: SCRIBODO_FAKE_EXTRACT_MS holds the fake backend inside a job for that many milliseconds, so a test can
     * make chat active while a job is running (the observer's wait mode, 2026-11-14). Unset in the product. */
    const char *slow = getenv("SCRIBODO_FAKE_EXTRACT_MS");
    if (slow && atoi(slow) > 0) usleep((useconds_t)atoi(slow) * 1000);
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
        char nm[48], at[48], va[48], ra[48], rr[48], rb[48];
        if (sent[strlen(sent) - 1] == '?') add_finding(arr, "question", sent, sent, "");
        else if (rel_pattern(sent, ra, rr, rb)) {
            add_finding(arr, "relationship", sent, sent, ra);
            cJSON *last = cJSON_GetArrayItem(arr, cJSON_GetArraySize(arr) - 1);
            cJSON_AddStringToObject(last, "relation", rr);
            cJSON_AddStringToObject(last, "object", rb);
        }
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
        while (i < n && !obs_word_byte(u[i])) i++;
        size_t a = i;
        while (i < n && obs_word_byte(u[i])) i++;
        size_t len = i - a;
        if (len < 3 || len >= 48 || !obs_upper_start(u + a)) continue;
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
        if (obs_stopword(chunk + a, len) || (initial && starter) || caps) continue;
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
