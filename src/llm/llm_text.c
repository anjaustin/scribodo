#include "llm_text.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define ZWNJ "\xE2\x80\x8C" /* U+200C */

/* Length of the dangerous sequence starting at p (which points at '<'), or 0. */
static size_t dangerous_at(const char *p)
{
    if (p[0] != '<') return 0;
    if (p[1] == '|') return 2;
    if (strncmp(p + 1, "think", 5) == 0) return 6;
    if (strncmp(p + 1, "/think", 6) == 0) return 7;
    return 0;
}

bool llm_needs_neutralize(const char *s)
{
    for (const char *p = s ? strchr(s, '<') : NULL; p; p = strchr(p + 1, '<'))
        if (dangerous_at(p)) return true;
    return false;
}

char *llm_neutralize(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s), extra = 0;
    for (const char *p = strchr(s, '<'); p; p = strchr(p + 1, '<'))
        if (dangerous_at(p)) extra += sizeof(ZWNJ) - 1;
    char *out = malloc(n + extra + 1);
    if (!out) return NULL;
    char *o = out;
    for (const char *p = s; *p; p++) {
        *o++ = *p;
        if (*p == '<' && dangerous_at(p)) {
            memcpy(o, ZWNJ, sizeof(ZWNJ) - 1);
            o += sizeof(ZWNJ) - 1;
        }
    }
    *o = '\0';
    return out;
}

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (!*buf) return;
    if (*len + n + 1 > *cap) {
        size_t c = *cap ? *cap : 64;
        while (*len + n + 1 > c) c *= 2;
        char *b = realloc(*buf, c);
        if (!b) {
            free(*buf);
            *buf = NULL;
            return;
        }
        *buf = b;
        *cap = c;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

/* `s` starts with the word `w` (case-insensitive), not just its letters:
 * "Sure," matches "sure" but "Surely" does not. */
static bool starts_word_ci(const char *s, const char *w)
{
    for (; *w; s++, w++)
        if ((*s | 0x20) != (*w | 0x20)) return false;
    unsigned char c = (unsigned char)*s;
    return !((c | 0x20) >= 'a' && (c | 0x20) <= 'z') && c != '\'' && c < 0x80;
}

/* A model preamble such as "Here is the revised passage:" or "Sure:" —
 * a short first line ending in ':' that opens with a stock word, and that
 * is not the start of the passage itself (red-team 004: "Surely he knew:"
 * and "Hereafter:" were stripped as preambles). */
static bool is_preamble(const char *line, size_t len, const char *passage)
{
    if (len < 2 || len > 90 || line[len - 1] != ':') return false;
    static const char *const words[] = { "here", "sure", "certainly", "okay", "ok", "revised", "rewritten", "edited" };
    bool stock = false;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]) && !stock; i++) stock = starts_word_ci(line, words[i]);
    if (!stock) return false;
    while (passage && (*passage == ' ' || *passage == '\n' || *passage == '\r' || *passage == '\t')) passage++;
    return !(passage && !strncmp(passage, line, len));
}

/* `q` occurs in s[from, to). */
static bool contains(const char *s, size_t from, size_t to, const char *q)
{
    size_t ql = strlen(q);
    for (size_t i = from; i + ql <= to; i++)
        if (!memcmp(s + i, q, ql)) return true;
    return false;
}

/* ---- quote style ---------------------------------------------------------------- */

#define LQ2 "\xE2\x80\x9C" /* “ */
#define RQ2 "\xE2\x80\x9D" /* ” */
#define LQ1 "\xE2\x80\x98" /* ‘ */
#define RQ1 "\xE2\x80\x99" /* ’ */

/* The passage is written with typographic quotes and no straight ones. */
static bool uses_curly_quotes(const char *s)
{
    if (strchr(s, '"') || strchr(s, '\'')) return false;
    return strstr(s, LQ2) || strstr(s, RQ2) || strstr(s, LQ1) || strstr(s, RQ1);
}

/* Codepoint ending just before s[i] (0 at the start). */
static unsigned prev_cp(const char *s, size_t i)
{
    if (i == 0) return 0;
    size_t j = i - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    const unsigned char *u = (const unsigned char *)s + j;
    if (u[0] < 0x80) return u[0];
    if ((u[0] & 0xE0) == 0xC0 && i - j >= 2) return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
    if ((u[0] & 0xF0) == 0xE0 && i - j >= 3) return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
    return 0x10000; /* other scripts / emoji: treat as a letter */
}

/* A quote here opens (start of text, after space, an opening bracket or
 * quote, or a dash); otherwise it closes. */
static bool opens_after(unsigned c)
{
    return c == 0 || c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '(' || c == '[' || c == '{' ||
           c == 0x2014 || c == 0x2013 || c == 0x201C || c == 0x2018 || c == 0x00A0;
}

/* Straight quotes -> typographic ones, so a rewrite keeps the manuscript's
 * style (red-team 004 #17: LFM3 sometimes returns "You're" for “You’re”).
 * Known limit: a leading apostrophe ('tis, '90s) becomes ‘ rather than ’. */
char *llm_curl_quotes(const char *s)
{
    size_t n = strlen(s), k = 0;
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        const char *rep = NULL;
        if (c == '"' || c == '\'') {
            /* An apostrophe (don't, Zoë's, dogs') follows a letter, so it
             * closes: ’. */
            if (opens_after(prev_cp(s, i))) rep = c == '"' ? LQ2 : LQ1;
            else rep = c == '"' ? RQ2 : RQ1;
        }
        if (rep) {
            memcpy(out + k, rep, 3);
            k += 3;
        } else {
            out[k++] = c;
        }
    }
    out[k] = '\0';
    return out;
}

char *llm_clean_edit(const char *reply, const char *passage)
{
    char *visible = NULL;
    llm_split_thinking(reply ? reply : "", &visible, NULL);
    if (!visible) return NULL;
    char *p = visible;
    /* A leading preamble line ending in ':' (e.g. "Here is the revised passage:"). */
    char *nl = strchr(p, '\n');
    if (nl && nl > p && is_preamble(p, (size_t)(nl - p), passage)) p = nl + 1;
    while (*p == '\n' || *p == ' ' || *p == '\r' || *p == '\t') p++;
    /* A wrapping code fence. */
    if (!strncmp(p, "```", 3)) {
        char *eol = strchr(p, '\n');
        char *close = eol ? strstr(eol + 1, "```") : NULL;
        if (eol && close) {
            *close = '\0';
            p = eol + 1;
        }
    }
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\n' || p[n - 1] == ' ' || p[n - 1] == '\r' || p[n - 1] == '\t')) p[--n] = '\0';
    /* Quotes wrapping the whole reply that the passage didn't have, and only
     * when no quote mark occurs inside: "Run," she said. "Now." is dialogue,
     * not a wrapped reply (red-team 004). */
    static const char *const quotes[][2] = { { "\"", "\"" }, { "\xE2\x80\x9C", "\xE2\x80\x9D" } };
    for (size_t q = 0; q < 2; q++) {
        size_t lo = strlen(quotes[q][0]), lc = strlen(quotes[q][1]);
        bool had = passage && !strncmp(passage, quotes[q][0], lo);
        if (!had && n >= lo + lc && !strncmp(p, quotes[q][0], lo) && !strcmp(p + n - lc, quotes[q][1]) &&
            !contains(p, lo, n - lc, quotes[q][0]) && !contains(p, lo, n - lc, quotes[q][1])) {
            p[n - lc] = '\0';
            p += lo;
            n -= lo + lc;
        }
    }
    /* Keep the passage's own leading and trailing whitespace exactly, so the
     * paragraph breaks around a selection survive the replacement (red-team
     * 004: a selection starting at a blank line was merged into the
     * previous paragraph, an invisible change in the diff). */
    const char *pp = passage ? passage : "";
    size_t pl = strlen(pp), lead = 0, trail = 0;
    while (lead < pl && (pp[lead] == ' ' || pp[lead] == '\n' || pp[lead] == '\r' || pp[lead] == '\t')) lead++;
    while (trail < pl - lead && (pp[pl - 1 - trail] == ' ' || pp[pl - 1 - trail] == '\n' ||
                                 pp[pl - 1 - trail] == '\r' || pp[pl - 1 - trail] == '\t'))
        trail++;
    /* Keep the manuscript's quote style: a passage with typographic quotes
     * and no straight ones gets its rewrite's straight quotes curled. */
    char *curled = NULL;
    if (uses_curly_quotes(pp) && (strchr(p, '"') || strchr(p, '\''))) {
        curled = llm_curl_quotes(p);
        if (curled) p = curled;
    }
    size_t body = strlen(p);
    char *out = malloc(lead + body + trail + 1);
    if (out) {
        memcpy(out, pp, lead);
        memcpy(out + lead, p, body);
        memcpy(out + lead + body, pp + pl - trail, trail);
        out[lead + body + trail] = '\0';
        /* A reply that is only whitespace is no proposal at all. */
        if (body == 0) out[0] = '\0';
    }
    free(curled);
    free(visible);
    return out;
}

void llm_split_thinking(const char *s, char **visible, char **reasoning)
{
    /* Rules (red-team 003):
     *  - A reply that *starts* with <think> is reasoning up to </think>, or to
     *    the end while it is still streaming.
     *  - An orphan </think> (no opening tag: the model reasoned straight into
     *    the content) means everything before it was reasoning.
     *  - Any other <think> is ordinary text and stays visible, so a literal
     *    tag in the middle of a real answer can never hide the rest of it. */
    size_t vl = 0, vc = 64, rl = 0, rc = 64;
    char *v = malloc(vc), *r = malloc(rc);
    if (v) v[0] = '\0';
    if (r) r[0] = '\0';
    const char *p = s ? s : "";
    const char *lead = p;
    while (*lead == ' ' || *lead == '\n' || *lead == '\r' || *lead == '\t') lead++;
    if (!strncmp(lead, "<think>", 7)) {
        const char *body = lead + 7;
        const char *close = strstr(body, "</think>");
        if (!close) { /* still streaming, or unterminated */
            append(&r, &rl, &rc, body, strlen(body));
            p = body + strlen(body);
        } else {
            append(&r, &rl, &rc, body, (size_t)(close - body));
            p = close + 8;
        }
    } else {
        const char *close = strstr(p, "</think>");
        if (close) {
            append(&r, &rl, &rc, p, (size_t)(close - p));
            p = close + 8;
        }
    }
    append(&v, &vl, &vc, p, strlen(p));
    /* Replies usually start with a newline after </think>; drop leading
     * whitespace so the visible text starts cleanly. */
    if (v) {
        size_t k = 0;
        while (v[k] == '\n' || v[k] == ' ' || v[k] == '\r' || v[k] == '\t') k++;
        if (k) memmove(v, v + k, strlen(v + k) + 1);
    }
    if (visible) *visible = v; else free(v);
    if (reasoning) *reasoning = r; else free(r);
}
