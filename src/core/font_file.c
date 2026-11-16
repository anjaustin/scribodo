/* Font file reading. See font_file.h. Every read goes through rd16/rd32, which check the buffer. */
#include "core/font_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define MAX_TABLES 512

static bool rd16(const uint8_t *d, size_t len, size_t off, uint16_t *v)
{
    if (off > len || len - off < 2) return false;
    *v = (uint16_t)((d[off] << 8) | d[off + 1]);
    return true;
}

static bool rd32(const uint8_t *d, size_t len, size_t off, uint32_t *v)
{
    if (off > len || len - off < 4) return false;
    *v = ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) | ((uint32_t)d[off + 2] << 8) | d[off + 3];
    return true;
}

static bool is_sfnt_version(uint32_t v) { return v == 0x00010000u || v == TAG('O', 'T', 'T', 'O') || v == TAG('t', 'r', 'u', 'e'); }

/* The face's table directory: its offset in the file and its table count. */
static bool face_dir(const uint8_t *d, size_t len, int index, size_t *dir, uint16_t *ntables)
{
    uint32_t v;
    if (index < 0 || !rd32(d, len, 0, &v)) return false;
    size_t off = 0;
    if (v == TAG('t', 't', 'c', 'f')) {
        uint32_t n, o;
        if (!rd32(d, len, 8, &n) || n == 0 || n > FONT_FILE_MAX_FACES || (uint32_t)index >= n) return false;
        if (!rd32(d, len, 12 + 4 * (size_t)index, &o)) return false;
        off = o;
        if (!rd32(d, len, off, &v)) return false;
    } else if (index != 0) {
        return false;
    }
    uint16_t nt;
    if (!is_sfnt_version(v) || !rd16(d, len, off + 4, &nt) || nt == 0 || nt > MAX_TABLES) return false;
    if (off + 12 > len || (len - off - 12) / 16 < nt) return false;
    *dir = off;
    *ntables = nt;
    return true;
}

static bool find_table(const uint8_t *d, size_t len, size_t dir, uint16_t nt, uint32_t tag, size_t *toff, size_t *tlen)
{
    for (uint16_t i = 0; i < nt; i++) {
        size_t rec = dir + 12 + 16 * (size_t)i;
        uint32_t t, o, l;
        if (!rd32(d, len, rec, &t) || !rd32(d, len, rec + 8, &o) || !rd32(d, len, rec + 12, &l)) return false;
        if (t != tag) continue;
        if (o > len || l > len - o) return false;
        *toff = o;
        *tlen = l;
        return true;
    }
    return false;
}

static bool face_table(const uint8_t *d, size_t len, int index, uint32_t tag, size_t *toff, size_t *tlen)
{
    size_t dir;
    uint16_t nt;
    return face_dir(d, len, index, &dir, &nt) && find_table(d, len, dir, nt, tag, toff, tlen);
}

int font_file_face_count(const uint8_t *data, size_t len)
{
    uint32_t v, n;
    if (!data || !rd32(data, len, 0, &v)) return 0;
    if (v != TAG('t', 't', 'c', 'f')) {
        size_t dir;
        uint16_t nt;
        return face_dir(data, len, 0, &dir, &nt) ? 1 : 0;
    }
    if (!rd32(data, len, 8, &n) || n == 0 || n > FONT_FILE_MAX_FACES) return 0;
    return (int)n;
}

/* --- names ------------------------------------------------------------------------------------------------------ */

static size_t put_utf8(char *out, size_t cap, size_t at, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) b[0] = (char)cp, n = 1;
    else if (cp < 0x800) b[0] = (char)(0xC0 | (cp >> 6)), b[1] = (char)(0x80 | (cp & 0x3F)), n = 2;
    else if (cp < 0x10000)
        b[0] = (char)(0xE0 | (cp >> 12)), b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), b[2] = (char)(0x80 | (cp & 0x3F)), n = 3;
    else
        b[0] = (char)(0xF0 | (cp >> 18)), b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)), b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)),
        b[3] = (char)(0x80 | (cp & 0x3F)), n = 4;
    if (at + n >= cap) return at; /* whole characters only, and room for the terminator */
    memcpy(out + at, b, n);
    return at + n;
}

/* A name record's string as UTF-8: UTF-16BE on the Unicode and Windows platforms, Mac Roman's ASCII range on the
 * Macintosh platform (other bytes become '?'). Control characters are dropped. */
static void decode_name(const uint8_t *s, size_t n, bool utf16, char *out, size_t cap)
{
    size_t at = 0;
    if (utf16) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            uint32_t cp = (uint32_t)((s[i] << 8) | s[i + 1]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
                uint32_t lo = (uint32_t)((s[i + 2] << 8) | s[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            if (cp < 0x20 || cp == 0x7F) continue;
            at = put_utf8(out, cap, at, cp);
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            uint32_t cp = s[i] < 0x80 ? s[i] : '?';
            if (cp < 0x20 || cp == 0x7F) continue;
            at = put_utf8(out, cap, at, cp);
        }
    }
    out[at] = '\0';
}

/* The best record for name ID `id`: Windows English first, then any Windows or Unicode record, then Macintosh Roman. */
static bool name_string(const uint8_t *d, size_t len, size_t toff, size_t tlen, uint16_t id, char *out, size_t cap)
{
    const uint8_t *t = d + toff;
    uint16_t count, store;
    if (!rd16(t, tlen, 2, &count) || !rd16(t, tlen, 4, &store)) return false;
    int best = 0;
    size_t best_off = 0, best_len = 0;
    bool best_utf16 = false;
    for (uint16_t i = 0; i < count; i++) {
        size_t r = 6 + 12 * (size_t)i;
        uint16_t pid, eid, lang, nid, n, o;
        if (!rd16(t, tlen, r, &pid) || !rd16(t, tlen, r + 2, &eid) || !rd16(t, tlen, r + 4, &lang) ||
            !rd16(t, tlen, r + 6, &nid) || !rd16(t, tlen, r + 8, &n) || !rd16(t, tlen, r + 10, &o))
            return false;
        if (nid != id || n == 0) continue;
        int score = 0;
        bool utf16 = false;
        if (pid == 3 && (eid == 1 || eid == 10)) score = lang == 0x0409 ? 4 : 3, utf16 = true;
        else if (pid == 0) score = 3, utf16 = true;
        else if (pid == 1 && eid == 0) score = lang == 0 ? 2 : 1;
        if (score <= best) continue;
        size_t at = (size_t)store + o;
        if (at > tlen || n > tlen - at) continue;
        best = score, best_off = at, best_len = n, best_utf16 = utf16;
    }
    if (!best) return false;
    decode_name(t + best_off, best_len, best_utf16, out, cap);
    return out[0] != '\0';
}

bool font_file_face_name(const uint8_t *data, size_t len, int index, FontFaceName *out)
{
    if (!data || !out) return false;
    memset(out, 0, sizeof(*out));
    size_t toff, tlen;
    if (!face_table(data, len, index, TAG('n', 'a', 'm', 'e'), &toff, &tlen)) return false;
    if (!name_string(data, len, toff, tlen, 16, out->family, sizeof(out->family)) &&
        !name_string(data, len, toff, tlen, 1, out->family, sizeof(out->family)))
        return false;
    if (!name_string(data, len, toff, tlen, 17, out->style, sizeof(out->style)) &&
        !name_string(data, len, toff, tlen, 2, out->style, sizeof(out->style)))
        snprintf(out->style, sizeof(out->style), "Regular");
    return true;
}

/* --- cmap ------------------------------------------------------------------------------------------------------- */

static bool cmap4_has(const uint8_t *t, size_t n, uint32_t cp)
{
    uint16_t segx2;
    if (cp > 0xFFFF || !rd16(t, n, 6, &segx2) || segx2 == 0 || (segx2 & 1)) return false;
    size_t ends = 14, starts = 16 + segx2, deltas = 16 + 2 * (size_t)segx2, ranges = 16 + 3 * (size_t)segx2;
    for (size_t s = 0; s < segx2 / 2u; s++) {
        uint16_t end, start, delta, range;
        if (!rd16(t, n, ends + 2 * s, &end) || !rd16(t, n, starts + 2 * s, &start) || !rd16(t, n, deltas + 2 * s, &delta) ||
            !rd16(t, n, ranges + 2 * s, &range))
            return false;
        if (cp > end) continue;
        if (cp < start) return false;
        if (range == 0) return ((cp + delta) & 0xFFFF) != 0;
        uint16_t g;
        if (!rd16(t, n, ranges + 2 * s + range + 2 * (cp - start), &g)) return false;
        return g != 0;
    }
    return false;
}

static bool cmap12_has(const uint8_t *t, size_t n, uint32_t cp)
{
    uint32_t groups;
    if (!rd32(t, n, 12, &groups) || groups > (n - 16) / 12) return false;
    for (uint32_t i = 0; i < groups; i++) {
        uint32_t a, b, g;
        if (!rd32(t, n, 16 + 12 * (size_t)i, &a) || !rd32(t, n, 20 + 12 * (size_t)i, &b) || !rd32(t, n, 24 + 12 * (size_t)i, &g))
            return false;
        if (cp >= a && cp <= b) return g + (cp - a) != 0;
    }
    return false;
}

bool font_file_has_codepoint(const uint8_t *data, size_t len, int index, uint32_t cp)
{
    size_t toff, tlen;
    if (!data || !face_table(data, len, index, TAG('c', 'm', 'a', 'p'), &toff, &tlen)) return false;
    const uint8_t *t = data + toff;
    uint16_t nsub;
    if (!rd16(t, tlen, 2, &nsub)) return false;
    size_t best = 0;
    uint16_t best_fmt = 0;
    for (uint16_t i = 0; i < nsub; i++) {
        uint16_t pid, eid, fmt;
        uint32_t o;
        if (!rd16(t, tlen, 4 + 8 * (size_t)i, &pid) || !rd16(t, tlen, 6 + 8 * (size_t)i, &eid) ||
            !rd32(t, tlen, 8 + 8 * (size_t)i, &o) || !rd16(t, tlen, o, &fmt))
            continue;
        bool unicode = pid == 0 || (pid == 3 && (eid == 1 || eid == 10));
        if (!unicode || (fmt != 4 && fmt != 12)) continue;
        if (!best_fmt || (fmt == 12 && best_fmt == 4)) best = o, best_fmt = fmt;
    }
    if (!best_fmt) return false;
    const uint8_t *s = t + best;
    size_t n = tlen - best;
    if (best_fmt == 12) {
        uint32_t l;
        if (!rd32(s, n, 4, &l) || l > n) return false;
        return cmap12_has(s, l, cp);
    }
    uint16_t l;
    if (!rd16(s, n, 2, &l) || l > n) return false;
    return cmap4_has(s, l, cp);
}

bool font_file_has_outlines(const uint8_t *data, size_t len, int index)
{
    size_t o, l;
    if (!data) return false;
    if (face_table(data, len, index, TAG('s', 'b', 'i', 'x'), &o, &l) || face_table(data, len, index, TAG('C', 'B', 'D', 'T'), &o, &l))
        return false;
    return (face_table(data, len, index, TAG('g', 'l', 'y', 'f'), &o, &l) && l > 0) ||
           (face_table(data, len, index, TAG('C', 'F', 'F', ' '), &o, &l) && l > 0);
}

/* --- extraction ------------------------------------------------------------------------------------------------- */

uint8_t *font_file_extract_face(const uint8_t *data, size_t len, int index, size_t *out_len)
{
    size_t dir;
    uint16_t nt;
    if (!data || !out_len || !face_dir(data, len, index, &dir, &nt)) return NULL;
    size_t total = 12 + 16 * (size_t)nt;
    /* A face's tables are distinct ranges of the file, so its copy is never much larger than the file: tables that
     * claim to overlap would let a 1 MB file demand gigabytes (red team, 2026-11-16). */
    size_t cap = len + 12 + 19 * (size_t)nt;
    for (uint16_t i = 0; i < nt; i++) {
        uint32_t o, l;
        size_t rec = dir + 12 + 16 * (size_t)i;
        if (!rd32(data, len, rec + 8, &o) || !rd32(data, len, rec + 12, &l) || o > len || l > len - o) return NULL;
        size_t padded = ((size_t)l + 3) & ~(size_t)3;
        if (padded > UINT32_MAX - total) return NULL; /* the directory holds 32-bit offsets */
        total += padded;
        if (total > cap) return NULL;
    }
    uint8_t *out = calloc(1, total);
    if (!out) return NULL;
    memcpy(out, data + dir, 12); /* sfnt version, table count, search fields */
    size_t at = 12 + 16 * (size_t)nt;
    for (uint16_t i = 0; i < nt; i++) {
        size_t rec = dir + 12 + 16 * (size_t)i;
        uint32_t o, l;
        (void)rd32(data, len, rec + 8, &o);
        (void)rd32(data, len, rec + 12, &l);
        uint8_t *nr = out + 12 + 16 * (size_t)i;
        memcpy(nr, data + rec, 8); /* tag, checksum */
        nr[8] = (uint8_t)(at >> 24), nr[9] = (uint8_t)(at >> 16), nr[10] = (uint8_t)(at >> 8), nr[11] = (uint8_t)at;
        memcpy(nr + 12, data + rec + 12, 4);
        memcpy(out + at, data + o, l);
        at += ((size_t)l + 3) & ~(size_t)3;
    }
    *out_len = total;
    return out;
}
