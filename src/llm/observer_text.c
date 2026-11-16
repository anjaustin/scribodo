#define _DARWIN_C_SOURCE
#include "observer.h"
#include "observer_text_internal.h"
#include <stdint.h>
#include <stdio.h>
#include "core/utf8.h"
#include "observer_unicode_word.h"

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

/* A title or honorific is not a name two texts share: "Mr. Darcy" and "Mr. Wickham" have "Mr" in common and nothing
 * else, and the canon candidate step sent such pairs to the judge (journal/observer-cycle2_quality.md, step 4). Skipped
 * in tokens() only when the caller asks (obs_overlap's ignore_titles), which the observer does for its canon candidate
 * steps under the conflict-gate experiment: measured with the value-verified judge on both planted novels (step 5), the
 * off-fault conflicts rose and a Mars place fault whose only shared word was "Princess" was lost. */
bool obs_title(const char *w, size_t n)
{
    static const char *const titles[] = {
        "mr", "mrs", "ms", "miss", "mister", "master", "lady", "lord", "sir", "dame", "madam", "madame", "dr",
        "doctor", "captain", "capt", "colonel", "col", "major", "general", "gen", "lieutenant", "lt", "sergeant",
        "sgt", "reverend", "rev", "professor", "prof", "king", "queen", "prince", "princess", "duke", "duchess",
        "count", "countess", "baron", "baroness", "earl", "saint", "st",
    };
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        if (strlen(titles[i]) == n && !strncasecmp(titles[i], w, n)) return true;
    return false;
}

typedef struct {
    char w[48];
    bool name;
} Tok;

static size_t tokens(const char *s, Tok *out, size_t max, bool ignore_titles)
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
        if (n >= sizeof(out[0].w) || obs_stopword(s + a, n) || (ignore_titles && obs_title(s + a, n))) continue;
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

int obs_overlap(const char *a, const char *b, bool ignore_titles)
{
    Tok ta[64], tb[64];
    size_t na = tokens(a, ta, 64, ignore_titles), nb = tokens(b, tb, 64, ignore_titles);
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

/* The statement a judge call sees: the description and the located quote, the quote clipped to 300 characters (the
 * evidence schema's maxLength) by characters, never by bytes. Clipping by "%.300s" cut inside a multi-byte character
 * and the request was invalid JSON the server refused, so the statement was never checked (red-team 2026-11-14; the
 * recorded Mars runs hold a 305-byte quote ending in an em dash). */
void obs_statement(const char *desc, const char *quote, char *out, size_t cap)
{
    char q[1205];
    const char *s = quote ? quote : "";
    size_t n = strlen(s), k = 0, chars = 0;
    while (k < n && chars < 300) { /* 300 characters, whole ones */
        uint32_t cp;
        int len = utf8_decode_one((const uint8_t *)s + k, n - k, &cp);
        if (len <= 0 || k + (size_t)len >= sizeof(q)) break;
        k += (size_t)len;
        chars++;
    }
    memcpy(q, s, k);
    q[k] = '\0';
    snprintf(out, cap, "%s (\xE2\x80\x9C%s\xE2\x80\x9D)", desc ? desc : "", q);
}

/* The judge's two values, each located in its own text (obs_locate: exact, then ignoring quote style and emphasis)
 * and different from each other; a verdict of "conflict" is accepted only with them. A judge that calls "Dejah
 * Thoris wears silks" a conflict with "Dejah Thoris is the Princess of Helium" cannot name a value of the canon that
 * the new text contradicts, and software refuses it here rather than a prompt asking it not to
 * (journal/observer-cycle2_quality.md, steps 3 and 4: 85 of 92 conflicts were such additions). */
bool obs_values_in_texts(const char *canon, const char *statement, const char *canon_value, const char *new_value)
{
    if (!canon || !statement || !canon_value || !new_value || !canon_value[0] || !new_value[0]) return false;
    if (!strcasecmp(canon_value, new_value)) return false;
    size_t start, len;
    return obs_locate(canon, strlen(canon), canon_value, &start, &len) &&
           obs_locate(statement, strlen(statement), new_value, &start, &len);
}

/* ---- sentence pairing (SCRIBODO_OBSERVER_SENTENCES; journal/observer-cycle2_quality.md, steps 7 and 8) ---------- */

static bool is_closing_quote(const unsigned char *u, size_t n, size_t *w)
{
    if (n >= 1 && (u[0] == '"' || u[0] == '\'' || u[0] == ')')) { *w = 1; return true; }
    if (n >= 3 && u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x9D || u[2] == 0x99)) { *w = 3; return true; } /* ” ’ */
    return false;
}

static bool is_opening_quote(const unsigned char *u, size_t n, size_t *w)
{
    if (n >= 1 && (u[0] == '"' || u[0] == '\'' || u[0] == '(')) { *w = 1; return true; }
    if (n >= 3 && u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x9C || u[2] == 0x98)) { *w = 3; return true; } /* “ ‘ */
    return false;
}

/* tools/observer_plant.py ABBREVIATIONS: a period after one of these, or after a single capital (an initial), ends no
 * sentence. */
static bool abbreviation(const char *w, size_t n)
{
    static const char *const ab[] = { "mr", "mrs", "ms", "dr", "st", "capt", "col", "lt", "gen", "prof", "sgt", "jr", "sr",
                                      "rev", "hon" };
    if (n == 1) return w[0] >= 'A' && w[0] <= 'Z';
    for (size_t i = 0; i < sizeof(ab) / sizeof(ab[0]); i++)
        if (strlen(ab[i]) == n && !strncasecmp(ab[i], w, n)) return true;
    return false;
}

size_t obs_sentence_end(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (u[i] == '\n') return i;
        if (u[i] != '.' && u[i] != '!' && u[i] != '?') continue;
        size_t j = i + 1, w;
        while (j < n && is_closing_quote(u + j, n - j, &w)) j += w;
        if (j >= n || !(u[j] == ' ' || u[j] == '\t')) continue;
        size_t k = j;
        while (k < n && (u[k] == ' ' || u[k] == '\t')) k++;
        if (k < n && is_opening_quote(u + k, n - k, &w)) k += w;
        if (k >= n || u[k] < 'A' || u[k] > 'Z') continue;
        if (u[i] == '.') {
            size_t a = i;
            while (a > 0 && ((u[a - 1] >= 'A' && u[a - 1] <= 'Z') || (u[a - 1] >= 'a' && u[a - 1] <= 'z'))) a--;
            if (a < i && abbreviation(s + a, i - a)) continue;
        }
        return j;
    }
    return n;
}

bool obs_next_sentence(const char *s, size_t n, size_t *pos, size_t *start, size_t *len)
{
    while (*pos < n) {
        while (*pos < n && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == '\n' || s[*pos] == '\r')) (*pos)++;
        if (*pos >= n) break;
        size_t e = obs_sentence_end(s + *pos, n - *pos), l = e;
        while (l > 0 && (s[*pos + l - 1] == ' ' || s[*pos + l - 1] == '\t' || s[*pos + l - 1] == '\r')) l--;
        size_t at = *pos;
        *pos += e ? e : 1;
        if (l == 0) continue;
        *start = at;
        *len = l;
        return true;
    }
    return false;
}

/* The attribute vocabularies of tools/observer_judge_score.py (frozen 2026-11-14 in step 6), word for word. */
static const char *const k_number_words[] = { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
    "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "first", "second", "third", "fourth",
    "fifth", "eldest", "youngest", "twice", "once", NULL };
static const char *const k_kin_words[] = { "father", "mother", "son", "daughter", "brother", "sister", "aunt", "uncle", "niece",
    "nephew", "cousin", "grandfather", "grandmother", "granddaughter", "grandson", "wife", "husband", "widow", "widower",
    "parent", "parents", "child", "children", "sons", "daughters", "brothers", "sisters", "grandparent", NULL };
static const char *const k_color_words[] = { "black", "white", "red", "blue", "green", "brown", "golden", "gold", "grey",
    "gray", "yellow", "crimson", "scarlet", "silver", "dark", "fair", "pale", "auburn", "copper", "purple", "violet", NULL };
static const char *const k_time_words[] = { "january", "february", "march", "april", "may", "june", "july", "august",
    "september", "october", "november", "december", "before", "after", "earlier", "later", "years", "year", "days", "day",
    "months", "month", "weeks", "week", "age", "aged", "old", NULL };
static const char *const k_place_titles[] = { "mr", "mrs", "ms", "miss", "lady", "lord", "sir", "dr", "captain", "colonel",
    "prince", "princess", "king", "queen", "the", "a", "an", "he", "she", "it", "they", "his", "her", "canon", "new", NULL };

/* A set of words, as Python's set of str: unbounded. A fixed table of 64 words of under 64 bytes each kept the first
 * 64 and dropped any longer word, where the Python keeps every one (AUDIT-004 F2; tests/fixtures/observer_rules).
 * Words are appended as found and made a set once, by set_finish (sort and drop repeats), so building and comparing
 * two sets is O(n log n): checking each word against the set as it was added was quadratic, 3.7 s for 40,000 numbers
 * a side (red-team 2026-11-15). failed marks an allocation failure: the caller cannot check and lets the other
 * evidence stand. */
typedef struct {
    char **w;
    size_t n, cap;
    bool failed;
} WordSet;

static void set_add(WordSet *s, const char *w, size_t n)
{
    if (s->failed) return;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char **grown = realloc(s->w, cap * sizeof(char *));
        if (!grown) { s->failed = true; return; }
        s->w = grown;
        s->cap = cap;
    }
    char *copy = malloc(n + 1);
    if (!copy) { s->failed = true; return; }
    memcpy(copy, w, n);
    copy[n] = '\0';
    s->w[s->n++] = copy;
}

static int cmp_word(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Sorted, each word once. */
static void set_finish(WordSet *s)
{
    if (s->failed || s->n < 2) return;
    qsort(s->w, s->n, sizeof(char *), cmp_word);
    size_t k = 1;
    for (size_t i = 1; i < s->n; i++) {
        if (!strcmp(s->w[i], s->w[k - 1])) free(s->w[i]);
        else s->w[k++] = s->w[i];
    }
    s->n = k;
}

static void set_free(WordSet *s)
{
    for (size_t i = 0; i < s->n; i++) free(s->w[i]);
    free(s->w);
}

/* The entry of list equal to w[0, n) ignoring case, or NULL. */
static const char *list_entry(const char *const *list, const char *w, size_t n)
{
    for (size_t i = 0; list[i]; i++)
        if (strlen(list[i]) == n && !strncasecmp(list[i], w, n)) return list[i];
    return NULL;
}

static bool in_list(const char *const *list, const char *w, size_t n) { return list_entry(list, w, n) != NULL; }

static bool ascii_alpha(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* Python's re.findall(r"[A-Za-z]+|[0-9]+") filtered into a set, lower-cased: the words of one vocabulary, and with
 * numbers=true also every run of ASCII digits (of any length) and every number word. A vocabulary word is added as
 * the list's own lower-case entry, so no word is copied into a buffer of fixed size. */
static void vocab_set(const char *text, const char *const *list, bool numbers, WordSet *out)
{
    const unsigned char *u = (const unsigned char *)text;
    for (size_t i = 0; u[i];) {
        size_t a = i;
        if (ascii_alpha(u[i])) {
            while (ascii_alpha(u[i])) i++;
        } else if (ascii_digit(u[i])) {
            while (ascii_digit(u[i])) i++;
        } else {
            i++;
            continue;
        }
        size_t n = i - a;
        if (ascii_digit(u[a])) {
            if (numbers) set_add(out, text + a, n);
            continue;
        }
        const char *entry = list ? list_entry(list, text + a, n) : NULL;
        if (!entry && numbers) entry = list_entry(k_number_words, text + a, n);
        if (entry) set_add(out, entry, n);
    }
}

/* Python's \w at a byte: ASCII letters, digits and underscore, and for a multi-byte character the code points
 * Python's re matches with \w (str.isalnum() or "_"), from the generated table observer_unicode_word.h
 * (tools/gen_unicode_word.py). Before 2026-11-15 every multi-byte character outside three punctuation ranges counted as
 * a word character, so x, emoji and fullwidth punctuation hid a boundary the Python sees (red-team: 8 attribute and
 * 39 subject disagreements in 60,000 random pairs). Bytes that are not UTF-8 count as a word character (no boundary);
 * the Python never sees them, since it reads text as UTF-8. */
static bool unicode_word(unsigned cp)
{
    size_t lo = 0, hi = sizeof(k_unicode_word) / sizeof(k_unicode_word[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < k_unicode_word[mid][0]) hi = mid;
        else if (cp > k_unicode_word[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool py_word_at(const unsigned char *u, size_t i)
{
    if (u[i] < 0x80) return ascii_alpha(u[i]) || ascii_digit(u[i]) || u[i] == '_';
    while (i > 0 && (u[i] & 0xC0) == 0x80) i--; /* back to the character's first byte */
    unsigned cp;
    if ((u[i] & 0xE0) == 0xC0 && (u[i + 1] & 0xC0) == 0x80) cp = (u[i] & 0x1Fu) << 6 | (u[i + 1] & 0x3Fu);
    else if ((u[i] & 0xF0) == 0xE0 && (u[i + 1] & 0xC0) == 0x80 && (u[i + 2] & 0xC0) == 0x80)
        cp = (u[i] & 0x0Fu) << 12 | (u[i + 1] & 0x3Fu) << 6 | (u[i + 2] & 0x3Fu);
    else if ((u[i] & 0xF8) == 0xF0 && (u[i + 1] & 0xC0) == 0x80 && (u[i + 2] & 0xC0) == 0x80 && (u[i + 3] & 0xC0) == 0x80)
        cp = (u[i] & 0x07u) << 18 | (u[i + 1] & 0x3Fu) << 12 | (u[i + 2] & 0x3Fu) << 6 | (u[i + 3] & 0x3Fu);
    else return true;
    return unicode_word(cp);
}

/* Python's \s at u (str patterns: Unicode whitespace, U+0009 to U+000D, U+001C to U+001F, U+0020, U+0085, U+00A0,
 * U+1680, U+2000 to U+200A, U+2028, U+2029, U+202F, U+205F, U+3000): its length in bytes, or 0. The ASCII-only test
 * before 2026-11-15 read "of\u00A0Helium" as no place (AUDIT-004 F2). */
static size_t py_space_len(const unsigned char *u)
{
    if ((u[0] >= 0x09 && u[0] <= 0x0D) || (u[0] >= 0x1C && u[0] <= 0x20)) return 1;
    if (u[0] == 0xC2 && (u[1] == 0x85 || u[1] == 0xA0)) return 2;
    if (u[0] == 0xE1 && u[1] == 0x9A && u[2] == 0x80) return 3;                                   /* U+1680 */
    if (u[0] == 0xE2 && u[1] == 0x80 && ((u[2] >= 0x80 && u[2] <= 0x8A) || u[2] == 0xA8 || u[2] == 0xA9 || u[2] == 0xAF))
        return 3;                                                                                  /* U+2000-200A, 2028, 2029, 202F */
    if (u[0] == 0xE2 && u[1] == 0x81 && u[2] == 0x9F) return 3;                                   /* U+205F */
    if (u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80) return 3;                                   /* U+3000 */
    return 0;
}

/* A capitalised word [A-Z][a-z]+ at u: its length, or 0. */
static size_t cap_word(const unsigned char *u)
{
    if (!(u[0] >= 'A' && u[0] <= 'Z') || !(u[1] >= 'a' && u[1] <= 'z')) return 0;
    size_t n = 2;
    while (u[n] >= 'a' && u[n] <= 'z') n++;
    return n;
}

/* re.finditer(r"\b(of|in|from|at|to)\s+((?:[A-Z][a-z]+)(?:\s+[A-Z][a-z]+)*)"), the name kept unless its first word is
 * on the title list: the places and households a sentence puts someone in. The \b before the preposition is read with
 * py_word_at, so an em dash or a curly quote before it is a boundary, as in Python (a non-ASCII byte counted as a
 * letter until 2026-11-15). */
static void place_set(const char *text, WordSet *out)
{
    static const char *const preps[] = { "of", "in", "from", "at", "to" };
    const unsigned char *u = (const unsigned char *)text;
    for (size_t i = 0; u[i];) {
        size_t matched = 0;
        if (i == 0 || !py_word_at(u, i - 1))
            for (size_t p = 0; p < 5 && !matched; p++) {
                size_t pl = strlen(preps[p]), sp;
                if (strncmp((const char *)u + i, preps[p], pl)) continue;
                size_t k = i + pl;
                if (!py_space_len(u + k)) continue;
                while ((sp = py_space_len(u + k))) k += sp;
                size_t w = cap_word(u + k);
                if (!w) continue;
                size_t name = k, end = k + w;
                for (;;) {
                    size_t at = end;
                    while ((sp = py_space_len(u + at))) at += sp;
                    if (at == end) break;
                    size_t w2 = cap_word(u + at);
                    if (!w2) break;
                    end = at + w2;
                }
                if (!in_list(k_place_titles, text + name, w)) set_add(out, text + name, end - name);
                matched = end - i;
            }
        i += matched ? matched : 1;
    }
}

/* Two finished sets differ when both are non-empty and they are not equal. */
static bool sets_differ(const WordSet *a, const WordSet *b)
{
    if (!a->n || !b->n) return false;
    if (a->n != b->n) return true;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->w[i], b->w[i])) return true;
    return false;
}

/* The two sets compared, then freed; an allocation failure answers "differ" (cannot check: the judge's verdict stands
 * alone, as before). */
static bool sets_differ_free(WordSet *a, WordSet *b)
{
    set_finish(a);
    set_finish(b);
    bool d = a->failed || b->failed || sets_differ(a, b);
    set_free(a);
    set_free(b);
    return d;
}

bool obs_attributes_differ(const char *canon, const char *text)
{
    static const char *const *const lists[] = { NULL, k_kin_words, k_color_words, k_time_words };
    for (int k = 0; k < 4; k++) {
        WordSet a = { 0 }, b = { 0 };
        vocab_set(canon, lists[k], k == 0 || k == 3, &a);
        vocab_set(text, lists[k], k == 0 || k == 3, &b);
        if (sets_differ_free(&a, &b)) return true;
    }
    WordSet a = { 0 }, b = { 0 };
    place_set(canon, &a);
    place_set(text, &b);
    return sets_differ_free(&a, &b);
}

/* tools/observer_judge_score.py subjects(), frozen in step 6: who a sentence is about. The leading run of
 * re.findall(r"[A-Z][a-z]+|[A-Z]\.|[a-z]+") over the text before its first " — " (stopping at the first lower-case
 * word), kept when longer than two letters and not on the title list, plus every name before "'s". */
static void subject_set(const char *text, WordSet *out)
{
    const unsigned char *u = (const unsigned char *)text;
    const char *dash = strstr(text, " \xE2\x80\x94 ");
    size_t head = dash ? (size_t)(dash - text) : strlen(text);
    for (size_t i = 0; i < head;) {
        size_t w = cap_word(u + i);
        if (w && i + w > head) w = head - i; /* the regex sees only the head */
        if (w >= 2) {
            if (w > 2 && !in_list(k_place_titles, text + i, w)) set_add(out, text + i, w);
            i += w;
        } else if (u[i] >= 'A' && u[i] <= 'Z' && i + 1 < head && u[i + 1] == '.') {
            i += 2; /* an initial: part of the run, never a subject */
        } else if (u[i] >= 'a' && u[i] <= 'z') {
            break;
        } else {
            i++;
        }
    }
    for (size_t i = 0; u[i]; i++) {
        if (i > 0 && py_word_at(u, i - 1)) continue;
        size_t w = cap_word(u + i);
        if (!w || u[i + w] != '\'' || u[i + w + 1] != 's' || (u[i + w + 2] && py_word_at(u, i + w + 2))) continue;
        set_add(out, text + i, w);
    }
}

bool obs_same_subject(const char *canon, const char *text)
{
    WordSet a = { 0 }, b = { 0 };
    subject_set(canon, &a);
    subject_set(text, &b);
    set_finish(&a);
    set_finish(&b);
    bool shared = a.failed || b.failed; /* cannot check: the other co-signers stand */
    for (size_t i = 0, j = 0; !shared && i < a.n && j < b.n;) { /* a merge of two sorted sets */
        int c = strcmp(a.w[i], b.w[j]);
        if (c == 0) shared = true;
        else if (c < 0) i++;
        else j++;
    }
    set_free(&a);
    set_free(&b);
    return shared;
}
