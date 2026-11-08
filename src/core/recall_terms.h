/* Deterministic query terms for the recall view (MEMORY-015 S1).
 *
 * Terms are split on ASCII whitespace and punctuation, ASCII-lowercased,
 * at least two bytes long, deduplicated and kept in order of first
 * appearance. Non-ASCII bytes are kept as written, so a non-ASCII letter
 * matches only its own case. Matching is substring matching: "key" matches
 * "keys". There is no stemming, no synonym table and no model anywhere. */
#ifndef SCRIBODO_RECALL_TERMS_H
#define SCRIBODO_RECALL_TERMS_H

#include <stddef.h>

#define RECALL_TERMS_MAX 16u
#define RECALL_TERM_BYTES 64u

typedef struct {
    char text[RECALL_TERM_BYTES];
} RecallTerm;

/* Fills out[] and returns how many terms were produced (0 for no query). */
unsigned recall_terms(const char *query, RecallTerm out[RECALL_TERMS_MAX]);

/* How many distinct terms occur in text as ASCII-case-insensitive substrings. */
unsigned recall_match(const char *text, const RecallTerm *terms, unsigned count);

#endif
