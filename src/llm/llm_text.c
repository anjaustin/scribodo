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
