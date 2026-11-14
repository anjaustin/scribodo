#define _DARWIN_C_SOURCE
#include "observer.h"
#include "observer_text_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static size_t cp_count(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
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
    bool starts_word = obs_word_byte((unsigned char)h[at]), ends_word = obs_word_byte((unsigned char)h[at + len - 1]);
    if (starts_word && at > 0 && obs_word_byte((unsigned char)h[at - 1]) && ((unsigned char)h[at] & 0xC0) != 0x80) return false;
    if (starts_word && at > 0 && ((unsigned char)h[at] & 0xC0) == 0x80) return false; /* mid-character */
    if (ends_word && at + len < hn && obs_word_byte((unsigned char)h[at + len]) && ((unsigned char)h[at + len] & 0xC0) != 0x80)
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

bool obs_word_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80;
}

bool obs_upper_start(const unsigned char *w)
{
    return (w[0] >= 'A' && w[0] <= 'Z') || (w[0] == 0xC3 && w[1] >= 0x80 && w[1] <= 0x9E) ||
           (w[0] == 0xC5 && (w[1] & 1) == 0 && w[1] >= 0x80);
}

bool obs_stopword(const char *w, size_t n)
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
        while (u[i] && !obs_word_byte(u[i])) i++;
        size_t a = i;
        while (u[i] && (obs_word_byte(u[i]) || (u[i] == '\'' && obs_word_byte(u[i + 1])) ||
                        (u[i] == 0xE2 && u[i + 1] == 0x80 && u[i + 2] == 0x99)))
            i += (u[i] == 0xE2) ? 3 : 1;
        size_t n = i - a;
        if (n == 0) continue;
        /* strip a possessive ('s / ’s) */
        if (n > 2 && u[a + n - 1] == 's' && u[a + n - 2] == '\'') n -= 2;
        else if (n > 4 && u[a + n - 1] == 's' && u[a + n - 2] == 0x99 && u[a + n - 3] == 0x80) n -= 4;
        if (n >= sizeof(out[0].w) || obs_stopword(s + a, n)) continue;
        bool name = obs_upper_start(u + a);
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

/* Cuts s at the end of its first sentence: a period that ends the text or is followed by a space and a capital,
 * unless the period closes an initial ("James K. Powell") or a title ("Mr. Bennet"), which the real model's
 * explanations carry: a conflict's content read "Canon states Captain James K." before this, and "the new text
 * introduces 'Mrs." when the title opened a quotation (journal/observer-cycle2_quality.md, steps 3 and 4). */
void obs_first_sentence(char *s)
{
    static const char *const titles[] = { "Mr", "Mrs", "Ms", "Dr", "St", "Capt", "Col", "Lt", "Gen", "Prof", "Sgt",
                                          "Jr", "Sr", "Rev", "Hon" };
    for (char *end = s; (end = strchr(end, '.')) != NULL; end++) {
        if (!(end[1] == '\0' || (end[1] == ' ' && end[2] >= 'A' && end[2] <= 'Z'))) continue;
        const char *w = end; /* the word before the period, without an opening quote, bracket or asterisk */
        while (w > s && w[-1] != ' ') w--;
        while (w < end && !((*w >= 'A' && *w <= 'Z') || (*w >= 'a' && *w <= 'z') || (*w >= '0' && *w <= '9'))) w++;
        size_t wl = (size_t)(end - w);
        bool abbrev = wl == 1 && *w >= 'A' && *w <= 'Z';
        for (size_t i = 0; !abbrev && i < sizeof(titles) / sizeof(titles[0]); i++)
            abbrev = wl == strlen(titles[i]) && !strncmp(w, titles[i], wl);
        if (abbrev && end[1] != '\0') continue;
        end[1] = '\0';
        return;
    }
}
