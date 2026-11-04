#include "llm_text.h"

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

static bool starts_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if ((*s | 0x20) != (*prefix | 0x20)) return false;
    return true;
}

char *llm_clean_edit(const char *reply, const char *passage)
{
    char *visible = NULL;
    llm_split_thinking(reply ? reply : "", &visible, NULL);
    if (!visible) return NULL;
    char *p = visible;
    /* A leading preamble line ending in ':' (e.g. "Here is the revised passage:"). */
    char *nl = strchr(p, '\n');
    if (nl && nl > p && nl[-1] == ':' && (starts_ci(p, "here") || starts_ci(p, "sure") || starts_ci(p, "revised") ||
                                          starts_ci(p, "rewritten") || starts_ci(p, "certainly") || starts_ci(p, "okay"))) {
        p = nl + 1;
    }
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
    /* Wrapping quotes the passage didn't have. */
    static const char *const quotes[][2] = { { "\"", "\"" }, { "\xE2\x80\x9C", "\xE2\x80\x9D" } };
    for (size_t q = 0; q < 2; q++) {
        size_t lo = strlen(quotes[q][0]), lc = strlen(quotes[q][1]);
        bool had = passage && !strncmp(passage, quotes[q][0], lo);
        if (!had && n >= lo + lc && !strncmp(p, quotes[q][0], lo) && !strcmp(p + n - lc, quotes[q][1])) {
            p[n - lc] = '\0';
            p += lo;
            n -= lo + lc;
        }
    }
    /* Keep the passage's trailing newline, if it had one, so paragraph
     * breaks around the selection survive the replacement. */
    size_t pl = passage ? strlen(passage) : 0;
    bool trailing_nl = pl && passage[pl - 1] == '\n';
    char *out = malloc(strlen(p) + 2);
    if (out) {
        strcpy(out, p);
        if (trailing_nl) strcat(out, "\n");
    }
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
