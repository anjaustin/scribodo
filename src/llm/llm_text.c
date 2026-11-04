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
    size_t vl = 0, vc = 64, rl = 0, rc = 64;
    char *v = malloc(vc), *r = malloc(rc);
    if (v) v[0] = '\0';
    if (r) r[0] = '\0';
    const char *p = s ? s : "";
    for (;;) {
        const char *open = strstr(p, "<think>");
        if (!open) {
            append(&v, &vl, &vc, p, strlen(p));
            break;
        }
        append(&v, &vl, &vc, p, (size_t)(open - p));
        const char *body = open + 7;
        const char *close = strstr(body, "</think>");
        if (!close) { /* still streaming, or unterminated */
            append(&r, &rl, &rc, body, strlen(body));
            break;
        }
        append(&r, &rl, &rc, body, (size_t)(close - body));
        p = close + 8;
    }
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
