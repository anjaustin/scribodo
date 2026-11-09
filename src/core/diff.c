#include "diff.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t pos, len;
    uint64_t hash;
} Tok;

static bool is_word_byte(unsigned char c)
{
    /* ASCII letters/digits/apostrophe, and every byte of a multibyte UTF-8
     * sequence (letters in other scripts stay inside words). */
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'' || c >= 0x80;
}

static bool is_space_byte(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* U+2019 (’) and U+2014 (—) etc. are multibyte and would glue onto words;
 * treat common punctuation sequences as their own tokens. */
static size_t punct_len(const unsigned char *p, size_t avail)
{
    static const char *const seqs[] = {
        "\xE2\x80\x94", "\xE2\x80\x93", "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98",
        "\xE2\x80\xA6", "\xC2\xAB", "\xC2\xBB",
    };
    for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        size_t n = strlen(seqs[i]);
        if (n <= avail && !memcmp(p, seqs[i], n)) return n; /* never read past the text */
    }
    return 0;
}

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

static Tok *tokenize(const char *s, size_t *count)
{
    size_t n = strlen(s), cap = n / 3 + 8, k = 0;
    /* Keep the empty-input allocation initialized too. Emitted tokens are
     * fully assigned below; unused capacity is not part of the token count. */
    Tok *t = calloc(cap, sizeof(Tok));
    if (!t) return NULL;
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        size_t start = i, pl;
        if ((pl = punct_len(u + i, n - i)) != 0) {
            i += pl;
        } else if (is_space_byte(u[i])) {
            while (i < n && is_space_byte(u[i])) i++;
        } else if (is_word_byte(u[i])) {
            while (i < n && is_word_byte(u[i]) && !punct_len(u + i, n - i)) i++;
        } else {
            i++;
        }
        if (k == cap) {
            cap *= 2;
            Tok *nt = realloc(t, cap * sizeof(Tok));
            if (!nt) {
                free(t);
                return NULL;
            }
            t = nt;
        }
        t[k++] = (Tok){ start, i - start, fnv(s + start, i - start) };
    }
    *count = k;
    return t;
}

static bool tok_eq(const char *a, const Tok *x, const char *b, const Tok *y)
{
    return x->hash == y->hash && x->len == y->len && !memcmp(a + x->pos, b + y->pos, x->len);
}

static bool push(Diff *d, size_t *cap, DiffKind kind, size_t pos, size_t len)
{
    if (len == 0) return true;
    if (d->count && d->ops[d->count - 1].kind == kind && d->ops[d->count - 1].pos + d->ops[d->count - 1].len == pos) {
        d->ops[d->count - 1].len += len;
    } else {
        if (d->count == *cap) {
            *cap = *cap ? *cap * 2 : 16;
            DiffOp *o = realloc(d->ops, *cap * sizeof(DiffOp));
            if (!o) return false;
            d->ops = o;
        }
        d->ops[d->count++] = (DiffOp){ kind, pos, len };
    }
    if (kind == DIFF_DELETE) d->deleted_bytes += len;
    if (kind == DIFF_INSERT) d->inserted_bytes += len;
    return true;
}

void diff_free(Diff *d)
{
    free(d->ops);
    memset(d, 0, sizeof(*d));
}

/* Fallback: replace everything. */
static bool whole(Diff *d, const char *a, const char *b)
{
    size_t cap = 0;
    return push(d, &cap, DIFF_DELETE, 0, strlen(a)) && push(d, &cap, DIFF_INSERT, 0, strlen(b));
}

bool diff_words(const char *a, const char *b, size_t max_edit, Diff *d)
{
    memset(d, 0, sizeof(*d));
    size_t na = 0, nb = 0;
    Tok *ta = tokenize(a, &na), *tb = tokenize(b, &nb);
    if (!ta || !tb) {
        free(ta);
        free(tb);
        return false;
    }
    if (max_edit == 0) max_edit = 2000;
    long n = (long)na, m = (long)nb;
    long maxd = n + m;
    if ((size_t)maxd > max_edit) maxd = (long)max_edit;
    long off = maxd + 1;
    long *v = calloc((size_t)(2 * maxd + 3), sizeof(long));
    /* trace[d] = the band v[-(d+1)..d+1] as it was at the START of step d,
     * which is what backtracking step d needs: O(D^2) memory. */
    long **trace = calloc((size_t)maxd + 1, sizeof(long *));
    bool ok = v && trace;
    long found_d = -1;
    for (long dd = 0; ok && dd <= maxd && found_d < 0; dd++) {
        trace[dd] = malloc((size_t)(2 * dd + 3) * sizeof(long));
        if (!trace[dd]) {
            ok = false;
            break;
        }
        memcpy(trace[dd], v + off - (dd + 1), (size_t)(2 * dd + 3) * sizeof(long));
        for (long k = -dd; k <= dd; k += 2) {
            long x;
            if (k == -dd || (k != dd && v[off + k - 1] < v[off + k + 1])) x = v[off + k + 1];
            else x = v[off + k - 1] + 1;
            long y = x - k;
            while (x < n && y < m && tok_eq(a, &ta[x], b, &tb[y])) {
                x++;
                y++;
            }
            v[off + k] = x;
            if (x >= n && y >= m) {
                found_d = dd;
                break;
            }
        }
    }

    bool result = ok;
    if (ok && found_d < 0) {
        result = whole(d, a, b); /* too different to diff cheaply: show as a replacement */
    } else if (ok) {
#define TV(dd, k) trace[dd][(k) + (dd) + 1]
        size_t cap_rev = (size_t)(found_d + n + m + 1);
        DiffOp *rev = malloc(cap_rev * sizeof(DiffOp));
        size_t nr = 0;
        long x = n, y = m;
        for (long dd = found_d; dd >= 0 && rev; dd--) {
            long k = x - y, pk, px, py;
            if (dd == 0) {
                px = py = 0;
            } else {
                if (k == -dd || (k != dd && TV(dd, k - 1) < TV(dd, k + 1))) pk = k + 1;
                else pk = k - 1;
                px = TV(dd, pk);
                py = px - pk;
            }
            while (x > px && y > py) { /* diagonal: equal tokens */
                x--;
                y--;
                rev[nr++] = (DiffOp){ DIFF_EQUAL, ta[x].pos, ta[x].len };
            }
            if (dd > 0) {
                if (x == px) { /* down: token inserted from b */
                    y--;
                    rev[nr++] = (DiffOp){ DIFF_INSERT, tb[y].pos, tb[y].len };
                } else { /* right: token deleted from a */
                    x--;
                    rev[nr++] = (DiffOp){ DIFF_DELETE, ta[x].pos, ta[x].len };
                }
            }
        }
#undef TV
        result = rev != NULL;
        /* Emit forward, merging runs; within a change, deletes before inserts. */
        size_t cap = 0;
        size_t i = nr;
        while (result && i > 0) {
            if (rev[i - 1].kind == DIFF_EQUAL) {
                result = push(d, &cap, DIFF_EQUAL, rev[i - 1].pos, rev[i - 1].len);
                i--;
                continue;
            }
            size_t j = i;
            while (j > 0 && rev[j - 1].kind != DIFF_EQUAL) j--;
            for (size_t q = i; q > j && result; q--)
                if (rev[q - 1].kind == DIFF_DELETE) result = push(d, &cap, DIFF_DELETE, rev[q - 1].pos, rev[q - 1].len);
            for (size_t q = i; q > j && result; q--)
                if (rev[q - 1].kind == DIFF_INSERT) result = push(d, &cap, DIFF_INSERT, rev[q - 1].pos, rev[q - 1].len);
            i = j;
        }
        free(rev);
    }
    for (long dd = 0; trace && dd <= maxd; dd++) free(trace[dd]);
    free(trace);
    free(v);
    free(ta);
    free(tb);
    return result;
}
