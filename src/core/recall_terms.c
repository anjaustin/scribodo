#include "recall_terms.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool separator(unsigned char c)
{
    return c < 0x80 && (isspace(c) || ispunct(c));
}

unsigned recall_terms(const char *query, RecallTerm out[RECALL_TERMS_MAX])
{
    unsigned n = 0;
    if (!query || !out) return 0;
    const char *p = query;
    while (*p && n < RECALL_TERMS_MAX) {
        while (*p && separator((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && !separator((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len < 2 || len >= RECALL_TERM_BYTES) continue;
        RecallTerm term;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            term.text[i] = (char)(c < 0x80 ? tolower(c) : c);
        }
        term.text[len] = '\0';
        bool duplicate = false;
        for (unsigned i = 0; i < n && !duplicate; i++) duplicate = strcmp(out[i].text, term.text) == 0;
        if (!duplicate) out[n++] = term;
    }
    return n;
}

static bool contains_ci(const char *text, const char *term)
{
    size_t length = strlen(term);
    if (!length) return false;
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < length) {
            unsigned char a = (unsigned char)p[i], b = (unsigned char)term[i];
            if (!a) return false;
            if ((a < 0x80 ? (unsigned char)tolower(a) : a) != b) break;
            i++;
        }
        if (i == length) return true;
    }
    return false;
}

unsigned recall_match(const char *text, const RecallTerm *terms, unsigned count)
{
    unsigned matched = 0;
    if (!text || !terms) return 0;
    for (unsigned i = 0; i < count; i++)
        if (contains_ci(text, terms[i].text)) matched++;
    return matched;
}
