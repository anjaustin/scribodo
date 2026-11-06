/* Unit tests for the editor core: UTF-8, gap buffer, undo, wrap. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/diff.h"
#include "../src/core/text_buffer.h"
#include "../src/core/undo.h"
#include "../src/core/utf8.h"
#include "../src/core/wrap.h"
#include "../src/app/frame_view_model.h"
#include "test_util.h"

/* Fill a buffer from a UTF-8 literal. */
static void tb_from(TextBuffer *tb, const char *s)
{
    size_t len = strlen(s);
    uint32_t *cps = malloc((len + 1) * sizeof(uint32_t));
    size_t n = utf8_decode((const uint8_t *)s, len, cps, NULL);
    tb_set(tb, cps, n);
    free(cps);
}

/* Compare buffer contents to a UTF-8 literal. */
static int tb_equals(const TextBuffer *tb, const char *s)
{
    size_t len = strlen(s);
    uint32_t *cps = malloc((len + 1) * sizeof(uint32_t));
    size_t n = utf8_decode((const uint8_t *)s, len, cps, NULL);
    int ok = n == tb_len(tb);
    for (size_t i = 0; ok && i < n; i++) ok = tb_get(tb, i) == cps[i];
    free(cps);
    return ok;
}

static void test_utf8_basic(void)
{
    const char *s = "Zoë — “quoted” Ωμέγα Привет 日本 😀";
    size_t len = strlen(s);
    uint32_t cps[128];
    size_t bad = 99;
    size_t n = utf8_decode((const uint8_t *)s, len, cps, &bad);
    CHECK(bad == 0);
    CHECK(cps[0] == 'Z' && cps[2] == 0x00EB && cps[4] == 0x2014);
    CHECK(cps[n - 1] == 0x1F600);

    uint8_t out[256];
    CHECK(utf8_encoded_len(cps, n) == len);
    size_t m = utf8_encode(cps, n, out);
    CHECK(m == len && memcmp(out, s, len) == 0);
}

static void test_utf8_invalid(void)
{
    struct { const char *bytes; size_t len; size_t expect_bad; } cases[] = {
        { "\xC0\xAF", 2, 2 },         /* overlong '/' */
        { "\xED\xA0\x80", 3, 3 },     /* UTF-16 surrogate */
        { "\xF4\x90\x80\x80", 4, 4 }, /* > U+10FFFF */
        { "\xE2\x82", 2, 2 },         /* truncated sequence */
        { "\x80", 1, 1 },             /* stray continuation byte */
        { "ok\xFF", 3, 1 },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        uint32_t cps[8];
        size_t bad = 0;
        size_t n = utf8_decode((const uint8_t *)cases[k].bytes, cases[k].len, cps, &bad);
        CHECK(bad == cases[k].expect_bad);
        CHECK(n >= 1 && cps[n - 1] == UTF8_REPLACEMENT);
    }
}

/* The NEON fast paths must match the scalar reference on arbitrary input. */
static void test_utf8_simd_matches_scalar(void)
{
    static const char *pieces[] = { "a", "The quick brown fox ", "é", "—", "“", "”", "日", "😀", "\n", "\xFF", "\xE2\x82" };
    const size_t npieces = sizeof(pieces) / sizeof(pieces[0]);
    uint8_t *src = malloc(1 << 16);
    uint32_t *a = malloc((1 << 16) * sizeof(uint32_t));
    uint32_t *b = malloc((1 << 16) * sizeof(uint32_t));
    uint8_t *ea = malloc(1 << 18), *eb = malloc(1 << 18);

    for (int iter = 0; iter < 200; iter++) {
        size_t len = 0;
        size_t target = rnd() % 4000;
        while (len < target) {
            const char *p = pieces[rnd() % npieces];
            size_t pl = strlen(p);
            memcpy(src + len, p, pl);
            len += pl;
        }
        size_t bad_a = 0, bad_b = 0;
        size_t na = utf8_decode(src, len, a, &bad_a);
        size_t nb = utf8_decode_scalar(src, len, b, &bad_b);
        CHECK(na == nb && bad_a == bad_b);
        CHECK(memcmp(a, b, na * sizeof(uint32_t)) == 0);

        size_t ma = utf8_encode(a, na, ea);
        size_t mb = utf8_encode_scalar(a, na, eb);
        CHECK(ma == mb && ma == utf8_encoded_len(a, na));
        CHECK(memcmp(ea, eb, ma) == 0);
    }
    free(src); free(a); free(b); free(ea); free(eb);
}

/* Random edits against a plain array reference implementation. */
static void test_utf8_clip(void)
{
    char out[8];
    /* "aé€😀" = 1 + 2 + 3 + 4 bytes */
    const char *s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    CHECK(utf8_clip(out, sizeof(out), s, 2) == 1 && !strcmp(out, "a"));             /* inside é */
    CHECK(utf8_clip(out, sizeof(out), s, 3) == 3 && !strcmp(out, "a\xC3\xA9"));      /* after é */
    CHECK(utf8_clip(out, sizeof(out), s, 5) == 3);                                   /* inside € */
    CHECK(utf8_clip(out, sizeof(out), s, 100) == 6 && !strcmp(out, "a\xC3\xA9\xE2\x82\xAC")); /* dst: 7 bytes */
    CHECK(utf8_clip(out, sizeof(out), "abc", 100) == 3 && !strcmp(out, "abc"));
    CHECK(utf8_clip(out, 1, "abc", 100) == 0 && out[0] == '\0');
    char big[16];
    CHECK(utf8_clip(big, sizeof(big), s, 9) == 6);                                   /* inside 😀 */
    CHECK(utf8_clip(big, sizeof(big), s, 10) == 10 && !strcmp(big, s));
}

static void test_gap_buffer_random(void)
{
    TextBuffer tb;
    tb_init(&tb);
    size_t ref_cap = 1 << 20, ref_len = 0;
    uint32_t *ref = malloc(ref_cap * sizeof(uint32_t));
    uint32_t chunk[600];

    for (int iter = 0; iter < 5000; iter++) {
        if (ref_len == 0 || rnd() % 3) {
            size_t pos = ref_len ? rnd() % (ref_len + 1) : 0;
            size_t n = 1 + rnd() % 500;
            for (size_t i = 0; i < n; i++) chunk[i] = 0x20 + rnd() % 0x3000;
            CHECK(tb_insert(&tb, pos, chunk, n));
            memmove(ref + pos + n, ref + pos, (ref_len - pos) * sizeof(uint32_t));
            memcpy(ref + pos, chunk, n * sizeof(uint32_t));
            ref_len += n;
        } else {
            size_t pos = rnd() % ref_len;
            size_t n = 1 + rnd() % 300;
            if (n > ref_len - pos) n = ref_len - pos;
            tb_delete(&tb, pos, n);
            memmove(ref + pos, ref + pos + n, (ref_len - pos - n) * sizeof(uint32_t));
            ref_len -= n;
        }
        if (iter % 250 == 0) {
            CHECK(tb_len(&tb) == ref_len);
            int same = 1;
            for (size_t i = 0; same && i < ref_len; i++) same = tb_get(&tb, i) == ref[i];
            CHECK(same);
            if (ref_len > 10) {
                size_t pos = rnd() % (ref_len - 10);
                uint32_t out[10];
                tb_copy(&tb, pos, 10, out);
                CHECK(memcmp(out, ref + pos, sizeof(out)) == 0);
            }
        }
    }
    CHECK(tb_len(&tb) == ref_len);
    free(ref);
    tb_free(&tb);
}

/* Helpers that mutate the buffer and record into the log, as the editor does. */
static void ed_insert(TextBuffer *tb, UndoLog *log, size_t pos, const char *s)
{
    uint32_t cps[256];
    size_t n = utf8_decode((const uint8_t *)s, strlen(s), cps, NULL);
    tb_insert(tb, pos, cps, n);
    undo_record_insert(log, pos, cps, n);
}

static void ed_delete(TextBuffer *tb, UndoLog *log, size_t pos, size_t n)
{
    uint32_t cps[256];
    tb_copy(tb, pos, n, cps);
    undo_record_delete(log, pos, cps, n);
    tb_delete(tb, pos, n);
}

static UndoCursor cur(int c) { return (UndoCursor){ c, c, c }; }

static void test_undo_typing_merges(void)
{
    TextBuffer tb; tb_init(&tb);
    UndoLog log; undo_init(&log);
    const char *word = "Hello";
    double t = 0;
    for (int i = 0; i < 5; i++) {
        char ch[2] = { word[i], 0 };
        undo_begin(&log, cur(i), UNDO_MERGE_TYPING, true, t);
        ed_insert(&tb, &log, (size_t)i, ch);
        undo_end(&log, cur(i + 1), t);
        t += 0.1;
    }
    CHECK(tb_equals(&tb, "Hello"));
    CHECK(log.count == 1);

    UndoCursor c;
    CHECK(undo_undo(&log, &tb, &c));
    CHECK(tb_equals(&tb, "") && c.cursor == 0);
    CHECK(undo_redo(&log, &tb, &c));
    CHECK(tb_equals(&tb, "Hello") && c.cursor == 5);
    undo_free(&log); tb_free(&tb);
}

static void test_undo_merge_breaks(void)
{
    TextBuffer tb; tb_init(&tb);
    UndoLog log; undo_init(&log);

    undo_begin(&log, cur(0), UNDO_MERGE_TYPING, true, 0.0);
    ed_insert(&tb, &log, 0, "a");
    undo_end(&log, cur(1), 0.0);

    /* Pause longer than the merge window. */
    undo_begin(&log, cur(1), UNDO_MERGE_TYPING, true, 5.0);
    ed_insert(&tb, &log, 1, "b");
    undo_end(&log, cur(2), 5.0);
    CHECK(log.count == 2);

    /* Cursor moved elsewhere before typing. */
    undo_begin(&log, cur(0), UNDO_MERGE_TYPING, true, 5.1);
    ed_insert(&tb, &log, 0, "c");
    undo_end(&log, cur(1), 5.1);
    CHECK(log.count == 3);
    CHECK(tb_equals(&tb, "cab"));

    /* Caller requested a new group (word boundary). */
    undo_begin(&log, cur(1), UNDO_MERGE_TYPING, false, 5.2);
    ed_insert(&tb, &log, 1, "d");
    undo_end(&log, cur(2), 5.2);
    CHECK(log.count == 4);
    undo_free(&log); tb_free(&tb);
}

static void test_undo_backspace_and_delete_merge(void)
{
    TextBuffer tb; tb_init(&tb);
    UndoLog log; undo_init(&log);
    tb_from(&tb, "abcdef");

    /* Backspace three times from the end. */
    for (int i = 0; i < 3; i++) {
        int c = 6 - i;
        undo_begin(&log, cur(c), UNDO_MERGE_BACKSPACE, true, 0.0);
        ed_delete(&tb, &log, (size_t)(c - 1), 1);
        undo_end(&log, cur(c - 1), 0.0);
    }
    CHECK(tb_equals(&tb, "abc") && log.count == 1);

    /* Forward-delete twice at position 0. */
    for (int i = 0; i < 2; i++) {
        undo_begin(&log, cur(0), UNDO_MERGE_DELETE, true, 0.0);
        ed_delete(&tb, &log, 0, 1);
        undo_end(&log, cur(0), 0.0);
    }
    CHECK(tb_equals(&tb, "c") && log.count == 2);

    UndoCursor c;
    CHECK(undo_undo(&log, &tb, &c) && tb_equals(&tb, "abc"));
    CHECK(undo_undo(&log, &tb, &c) && tb_equals(&tb, "abcdef") && c.cursor == 6);
    CHECK(!undo_undo(&log, &tb, &c));
    CHECK(undo_redo(&log, &tb, &c) && tb_equals(&tb, "abc"));
    CHECK(undo_redo(&log, &tb, &c) && tb_equals(&tb, "c"));
    undo_free(&log); tb_free(&tb);
}

static void test_undo_replace_selection_and_redo_discard(void)
{
    TextBuffer tb; tb_init(&tb);
    UndoLog log; undo_init(&log);
    tb_from(&tb, "the cat sat");

    /* Replace "cat" with "dog" as one action (delete + insert). */
    UndoCursor before = { 7, 4, 7 };
    undo_begin(&log, before, UNDO_MERGE_NONE, false, 0.0);
    ed_delete(&tb, &log, 4, 3);
    ed_insert(&tb, &log, 4, "dog");
    undo_end(&log, cur(7), 0.0);
    CHECK(tb_equals(&tb, "the dog sat"));

    UndoCursor c;
    CHECK(undo_undo(&log, &tb, &c));
    CHECK(tb_equals(&tb, "the cat sat"));
    CHECK(c.sel_start == 4 && c.sel_end == 7);

    /* Navigation (no ops) must not discard redo. */
    undo_begin(&log, cur(0), UNDO_MERGE_NONE, false, 1.0);
    undo_end(&log, cur(3), 1.0);
    CHECK(undo_can_redo(&log));

    /* A real edit does discard redo. */
    undo_begin(&log, cur(0), UNDO_MERGE_NONE, false, 1.0);
    ed_insert(&tb, &log, 0, "§");
    undo_end(&log, cur(1), 1.0);
    CHECK(!undo_can_redo(&log));
    CHECK(tb_equals(&tb, "§the cat sat"));
    undo_free(&log); tb_free(&tb);
}

static void test_undo_state_ids(void)
{
    TextBuffer tb; tb_init(&tb);
    UndoLog log; undo_init(&log);
    CHECK(undo_state_id(&log) == 0);

    undo_begin(&log, cur(0), UNDO_MERGE_TYPING, true, 0.0);
    ed_insert(&tb, &log, 0, "a");
    undo_end(&log, cur(1), 0.0);
    uint64_t saved = undo_state_id(&log);

    /* Merging more typing into the same group must change the id. */
    undo_begin(&log, cur(1), UNDO_MERGE_TYPING, true, 0.1);
    ed_insert(&tb, &log, 1, "b");
    undo_end(&log, cur(2), 0.1);
    CHECK(log.count == 1);
    CHECK(undo_state_id(&log) != saved);

    UndoCursor c;
    undo_undo(&log, &tb, &c);
    CHECK(undo_state_id(&log) == 0);
    undo_free(&log); tb_free(&tb);
}

static float adv_fixed(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp == '\t' ? 40.0f : 10.0f;
}

static void test_wrap(void)
{
    TextBuffer tb; tb_init(&tb);
    WrapLayout l = { 0 };

    /* Width 100 = 10 chars per row. */
    tb_from(&tb, "aaaa bbbb cccc\nshort\n");
    CHECK(wrap_compute(&l, &tb, 100.0f, adv_fixed, NULL) == 0);
    /* "aaaa bbbb " | "cccc\n" | "short\n" | "" */
    CHECK(l.count == 4);
    CHECK(l.rows[0].start == 0 && l.rows[0].len == 10);
    CHECK(l.rows[1].start == 10 && l.rows[1].len == 5);
    CHECK(l.rows[2].start == 15 && l.rows[2].len == 6);
    CHECK(l.rows[3].start == 21 && l.rows[3].len == 0);

    /* Soft-wrap boundary belongs to the next row; end maps to the last row. */
    CHECK(wrap_row_for_index(&l, 9) == 0);
    CHECK(wrap_row_for_index(&l, 10) == 1);
    CHECK(wrap_row_for_index(&l, 21) == 3);

    /* An over-long word hard-breaks. */
    tb_from(&tb, "abcdefghijklmnopqrstuvwxy");
    wrap_compute(&l, &tb, 100.0f, adv_fixed, NULL);
    CHECK(l.count == 3 && l.rows[0].len == 10 && l.rows[2].len == 5);

    /* Empty text has one empty row. */
    tb_from(&tb, "");
    wrap_compute(&l, &tb, 100.0f, adv_fixed, NULL);
    CHECK(l.count == 1 && l.rows[0].len == 0);

    /* Every index is covered exactly once by rows. */
    tb_from(&tb, "Il était une fois — dans un pays lointain — une écrivaine.\n\nFin.");
    wrap_compute(&l, &tb, 75.0f, adv_fixed, NULL);
    size_t expect = 0;
    for (size_t r = 0; r < l.count; r++) {
        CHECK(l.rows[r].start == expect);
        expect += l.rows[r].len;
    }
    CHECK(expect == tb_len(&tb));

    wrap_free(&l);
    tb_free(&tb);
}

/* Rebuild old and new from a diff; both must match exactly. */
static int diff_roundtrip(const char *a, const char *b, const Diff *d)
{
    char ra[8192] = "", rb[8192] = "";
    size_t la = 0, lb = 0;
    for (size_t i = 0; i < d->count; i++) {
        const DiffOp *o = &d->ops[i];
        if (o->kind != DIFF_INSERT) { memcpy(ra + la, a + o->pos, o->len); la += o->len; }
        if (o->kind == DIFF_EQUAL)  { memcpy(rb + lb, a + o->pos, o->len); lb += o->len; }
        if (o->kind == DIFF_INSERT) { memcpy(rb + lb, b + o->pos, o->len); lb += o->len; }
    }
    ra[la] = rb[lb] = 0;
    return !strcmp(ra, a) && !strcmp(rb, b);
}

static void test_diff(void)
{
    Diff d;
    const char *a = "The fog came in off the fjord, quietly and completely.";
    const char *b = "The fog rolled in off the fjord — silently and completely.";
    CHECK(diff_words(a, b, 0, &d));
    CHECK(diff_roundtrip(a, b, &d));
    /* Word-level: "came" -> "rolled" is one delete + one insert of whole words. */
    int saw = 0;
    for (size_t i = 0; i < d.count; i++)
        if (d.ops[i].kind == DIFF_DELETE && d.ops[i].len == 4 && !strncmp(a + d.ops[i].pos, "came", 4) &&
            i + 1 < d.count && d.ops[i + 1].kind == DIFF_INSERT && !strncmp(b + d.ops[i + 1].pos, "rolled", 6))
            saw = 1;
    CHECK(saw);
    CHECK(d.deleted_bytes < strlen(a) / 2); /* not a whole replacement */
    diff_free(&d);

    CHECK(diff_words("", "new text", 0, &d) && d.count == 1 && d.ops[0].kind == DIFF_INSERT);
    diff_free(&d);
    CHECK(diff_words("same", "same", 0, &d) && d.count == 1 && d.ops[0].kind == DIFF_EQUAL);
    diff_free(&d);
    CHECK(diff_words("Zoë’s letter", "Zoë’s old letter", 0, &d) && diff_roundtrip("Zoë’s letter", "Zoë’s old letter", &d));
    CHECK(d.deleted_bytes == 0);
    diff_free(&d);

    /* Property: random word edits always round-trip, and the edit limit
     * falls back to a whole replacement rather than failing. */
    static const char *words[] = { "fog", "the", "lighthouse", "Zoë", "—", " ", "  ", "\n", ",", "keeper", "Ångström" };
    for (int it = 0; it < 300; it++) {
        char x[2048] = "", y[2048] = "";
        int nx = 5 + rnd() % 80, ny = 5 + rnd() % 80;
        for (int i = 0; i < nx; i++) strcat(x, words[rnd() % 11]);
        for (int i = 0; i < ny; i++) strcat(y, rnd() % 3 ? words[rnd() % 11] : "edited");
        CHECK(diff_words(x, y, it % 10 == 0 ? 3 : 0, &d));
        CHECK(diff_roundtrip(x, y, &d));
        for (size_t i = 1; i < d.count; i++) CHECK(!(d.ops[i - 1].kind == DIFF_INSERT && d.ops[i].kind == DIFF_DELETE));
        diff_free(&d);
    }
}

static void test_frame_view_model(void)
{
    FrameViewModel frame;
    frame_view_model_init(&frame);
    frame_view_model_begin(&frame);
    CHECK(frame.generation == 1 && !frame.sealed);
    const char *first = frame_view_model_copy(&frame, "first label");
    char large[5000];
    memset(large, 'x', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    const char *second = frame_view_model_copy(&frame, large);
    CHECK(!strcmp(first, "first label")); /* later block growth never invalidates earlier strings */
    CHECK(strlen(second) == sizeof(large) - 1);
    frame_view_model_seal(&frame);
    CHECK(frame.sealed);
    CHECK(!strcmp(frame_view_model_copy(&frame, "late mutation"), "[frame text unavailable]"));
    frame_view_model_begin(&frame);
    CHECK(frame.generation == 2 && !frame.sealed);
    CHECK(!strcmp(frame_view_model_copy(&frame, "next frame"), "next frame"));
    frame_view_model_destroy(&frame);
    frame_view_model_destroy(&frame);
}

int main(void)
{
    TestCase tests[] = {
        { "utf8_clip", test_utf8_clip },
        { "utf8_basic", test_utf8_basic },
        { "utf8_invalid", test_utf8_invalid },
        { "utf8_simd_matches_scalar", test_utf8_simd_matches_scalar },
        { "gap_buffer_random", test_gap_buffer_random },
        { "undo_typing_merges", test_undo_typing_merges },
        { "undo_merge_breaks", test_undo_merge_breaks },
        { "undo_backspace_and_delete_merge", test_undo_backspace_and_delete_merge },
        { "undo_replace_selection_and_redo_discard", test_undo_replace_selection_and_redo_discard },
        { "undo_state_ids", test_undo_state_ids },
        { "wrap", test_wrap },
        { "diff", test_diff },
        { "frame_view_model", test_frame_view_model },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
