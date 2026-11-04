/* Headless tests of the real editor (stb_textedit integration, undo, save)
 * linked against tests/raylib_stubs.c. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/ui/editor.h"
#include "../src/ui/review_view.h"
#include "test_util.h"

extern double g_stub_time;

static SFont g_font;
static char g_err[512];
static char g_dir[512];

static void init_font(void)
{
    memset(&g_font, 0, sizeof(g_font));
    g_font.size = 16;
    g_font.line_height = 20;
    g_font.dpi = 1;
    g_font.tofu_advance = 10;
    g_font.advance = calloc(SFONT_LUT_SIZE, sizeof(float));
    for (uint32_t c = 0x20; c < SFONT_LUT_SIZE; c++) g_font.advance[c] = c < 0x80 ? 8.0f : 10.0f;
    for (uint32_t c = 0x300; c <= 0x36F; c++) g_font.advance[c] = 0.0f;
    g_font.advance['\t'] = 32.0f;
}

static Editor *new_editor(const char *initial)
{
    Editor *e = editor_create(&g_font);
    char path[1024];
    snprintf(path, sizeof(path), "%s/doc-%u.md", g_dir, rnd());
    FILE *f = fopen(path, "wb");
    fputs(initial, f);
    fclose(f);
    editor_load_file(e, path, g_err, sizeof(g_err));
    /* One frame with a 400px-wide pane to establish wrap geometry. */
    editor_update(e, (Rectangle){ 0, 0, 464, 600 }, false);
    return e;
}

static int text_is(Editor *e, const char *expect)
{
    char *t = editor_text_utf8(e);
    int ok = t && strcmp(t, expect) == 0;
    if (!ok) fprintf(stderr, "    got \"%s\"\n    want \"%s\"\n", t ? t : "(null)", expect);
    free(t);
    return ok;
}

static void type(Editor *e, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        uint32_t cp;
        int n = 1;
        if (*p < 0x80) cp = *p;
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else { cp = *p & 0x07; n = 4; }
        for (int i = 1; i < n; i++) cp = (cp << 6) | (p[i] & 0x3F);
        p += n;
        editor_input_char(e, cp);
        g_stub_time += 0.05;
    }
}

static void test_typing_undo_is_per_word(void)
{
    Editor *e = new_editor("");
    type(e, "The fog came");
    CHECK(text_is(e, "The fog came"));
    editor_undo(e);
    CHECK(text_is(e, "The fog "));
    editor_undo(e);
    CHECK(text_is(e, "The "));
    editor_redo(e);
    editor_redo(e);
    CHECK(text_is(e, "The fog came"));
    editor_destroy(e);
}

static void test_word_motion_and_delete(void)
{
    Editor *e = new_editor("she didn’t — go");
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    editor_input_key(e, EDITOR_KEY_WORD_LEFT, false); /* before "go" */
    editor_input_key(e, EDITOR_KEY_WORD_LEFT, false); /* skips the em dash, before "didn’t" */
    size_t a, c;
    editor_get_selection(e, &a, &c);
    CHECK(c == 4);
    editor_input_key(e, EDITOR_KEY_DELETE_WORD, false);
    CHECK(text_is(e, "she  — go"));
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    editor_input_key(e, EDITOR_KEY_BACKSPACE_WORD, false);
    CHECK(text_is(e, "she  — "));
    editor_undo(e);
    editor_undo(e);
    CHECK(text_is(e, "she didn’t — go"));
    editor_destroy(e);
}

static void test_selection_replace_and_paste(void)
{
    Editor *e = new_editor("the cat sat");
    editor_set_selection(e, 4, 7);
    type(e, "d");
    type(e, "og");
    CHECK(text_is(e, "the dog sat"));
    editor_undo(e); /* "og" */
    editor_undo(e); /* "d" replacing "cat" */
    CHECK(text_is(e, "the cat sat"));

    /* Paste normalises CRLF and strips control characters. */
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    CHECK(editor_paste_utf8(e, "\r\nnew\x01 line\r"));
    CHECK(text_is(e, "the cat sat\nnew line\n"));
    editor_undo(e);
    CHECK(text_is(e, "the cat sat"));

    editor_select_all(e);
    char *cut = NULL;
    CHECK(editor_cut_selection(e, &cut) && cut && strcmp(cut, "the cat sat") == 0);
    free(cut);
    CHECK(text_is(e, ""));
    editor_undo(e);
    CHECK(text_is(e, "the cat sat"));
    editor_destroy(e);
}

static void test_replace_range_single_undo(void)
{
    Editor *e = new_editor("Zoë stood at the top.");
    uint32_t repl[] = { 'w', 'a', 'i', 't', 'e', 'd' };
    CHECK(editor_replace_range(e, 4, 5, repl, 6));
    CHECK(text_is(e, "Zoë waited at the top."));
    editor_undo(e);
    CHECK(text_is(e, "Zoë stood at the top."));
    CHECK(!editor_replace_range(e, 30, 1, repl, 1)); /* out of range */
    editor_destroy(e);
}

static void test_dirty_tracking(void)
{
    Editor *e = new_editor("abc");
    CHECK(!editor_is_dirty(e));
    type(e, "x");
    CHECK(editor_is_dirty(e));
    editor_undo(e);
    CHECK(!editor_is_dirty(e)); /* back to the saved state */

    type(e, "y");
    CHECK(editor_save(e, g_err, sizeof(g_err)));
    CHECK(!editor_is_dirty(e));
    g_stub_time += 0.01; /* within the merge window */
    type(e, "z");
    CHECK(editor_is_dirty(e));
    /* Review finding R5: the save seals the undo group, so one undo returns
     * exactly to the saved text instead of jumping past it. */
    editor_undo(e);
    CHECK(text_is(e, "yabc")); /* cursor starts at 0 after load */
    CHECK(!editor_is_dirty(e));
    editor_destroy(e);
}

static void test_read_only_blocks_edits(void)
{
    Editor *e = editor_create(&g_font);
    char path[1024];
    snprintf(path, sizeof(path), "%s/invalid.md", g_dir);
    FILE *f = fopen(path, "wb");
    fputs("bad \xFF byte", f);
    fclose(f);
    CHECK(editor_load_file(e, path, g_err, sizeof(g_err)));
    CHECK(editor_is_read_only(e));
    size_t len = editor_length(e);
    type(e, "x");
    editor_input_key(e, EDITOR_KEY_BACKSPACE, false);
    CHECK(!editor_paste_utf8(e, "y"));
    uint32_t r[] = { 'q' };
    CHECK(!editor_replace_range(e, 0, 1, r, 1));
    CHECK(editor_length(e) == len);
    CHECK(!editor_save(e, g_err, sizeof(g_err)));
    f = fopen(path, "rb");
    char buf[32] = { 0 };
    CHECK(fread(buf, 1, sizeof(buf), f) == 10 && memcmp(buf, "bad \xFF byte", 10) == 0);
    fclose(f);
    editor_destroy(e);
}

static void write_file(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    fputs(s, f);
    fclose(f);
}

static char *read_file_str(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Count files in the test dir whose name contains `needle`, and return the
 * content of the last one found. */
static int find_conflict_copy(const char *needle, char *content, size_t cap)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "ls '%s' | grep -F '%s'", g_dir, needle);
    FILE *p = popen(cmd, "r");
    char line[600];
    int n = 0;
    while (fgets(line, sizeof(line), p)) {
        line[strcspn(line, "\n")] = '\0';
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", g_dir, line);
        char *c = read_file_str(path);
        if (c && content) snprintf(content, cap, "%s", c);
        n++;
    }
    pclose(p);
    return n;
}

static void test_save_detects_external_change(void)
{
    Editor *e = new_editor("draft one");
    type(e, "!");
    /* Another program rewrites the file underneath us. */
    write_file(editor_path(e), "someone else's text");
    CHECK(!editor_save(e, g_err, sizeof(g_err)));
    CHECK(editor_overwrite_armed(e) && editor_is_dirty(e));
    CHECK(strcmp(read_file_str(editor_path(e)), "someone else's text") == 0);
    CHECK(editor_save(e, g_err, sizeof(g_err))); /* confirmed overwrite */
    CHECK(!editor_overwrite_armed(e) && !editor_is_dirty(e));
    editor_destroy(e);
}

/* Review finding R1: the overwrite arm must not outlive the situation the
 * user was warned about, and refusing must never lose either version. */
static void test_overwrite_arm_is_narrow(void)
{
    Editor *e = new_editor("mine");
    char tag[64];
    snprintf(tag, sizeof(tag), "Scribodo conflict");
    int copies_before = find_conflict_copy(tag, NULL, 0);
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    type(e, " v2");
    write_file(editor_path(e), "theirs #1");
    CHECK(!editor_save(e, g_err, sizeof(g_err)));

    /* Our text was preserved in a conflict copy beside the file. */
    char copy[256] = "";
    CHECK(find_conflict_copy(tag, copy, sizeof(copy)) == copies_before + 1);
    CHECK(strcmp(copy, "mine v2") == 0);

    /* Editing after the warning disarms: the next save is checked again. */
    type(e, "!");
    CHECK(!editor_overwrite_armed(e));
    g_stub_time += 1.0; /* new second, so the conflict copy gets a fresh name */
    CHECK(!editor_save(e, g_err, sizeof(g_err)));
    CHECK(strcmp(read_file_str(editor_path(e)), "theirs #1") == 0);

    /* A second external change after arming is still caught. */
    CHECK(editor_overwrite_armed(e));
    write_file(editor_path(e), "theirs #2, different size");
    CHECK(!editor_save(e, g_err, sizeof(g_err)));
    CHECK(strcmp(read_file_str(editor_path(e)), "theirs #2, different size") == 0);

    /* The arm expires. */
    CHECK(editor_overwrite_armed(e));
    g_stub_time += 31.0;
    CHECK(!editor_overwrite_armed(e));
    CHECK(!editor_save(e, g_err, sizeof(g_err)));
    CHECK(strcmp(read_file_str(editor_path(e)), "theirs #2, different size") == 0);
    editor_destroy(e);
}

/* Review finding R2: write-protected files open read-only and stay intact. */
static void test_write_protected_opens_read_only(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/protected.md", g_dir);
    write_file(path, "do not touch");
    chmod(path, 0444);
    Editor *e = editor_create(&g_font);
    CHECK(editor_load_file(e, path, g_err, sizeof(g_err)));
    CHECK(editor_is_read_only(e));
    type(e, "x");
    CHECK(editor_length(e) == 12);
    CHECK(!editor_save(e, g_err, sizeof(g_err)));
    CHECK(strcmp(read_file_str(path), "do not touch") == 0);
    chmod(path, 0644);
    editor_destroy(e);
}

/* Review finding R3: no document (or nowhere to save) means no typing. */
static void test_no_document_is_read_only(void)
{
    Editor *e = editor_create(&g_font);
    CHECK(editor_is_read_only(e) && !editor_path(e));
    type(e, "lost words");
    CHECK(editor_length(e) == 0 && !editor_is_dirty(e));
    editor_destroy(e);

    if (geteuid() != 0) {
        char dir[1024], path[1100];
        snprintf(dir, sizeof(dir), "%s/nocreate", g_dir);
        mkdir(dir, 0555);
        snprintf(path, sizeof(path), "%s/new.md", dir);
        e = editor_create(&g_font);
        CHECK(editor_load_file(e, path, g_err, sizeof(g_err)));
        CHECK(editor_is_read_only(e));
        editor_destroy(e);
        chmod(dir, 0755);
    }
}

/* Review findings R6/R7: motion and line commands work on visual lines. */
static void test_visual_line_motion(void)
{
    /* Wrap width 400px, 8px ASCII advance -> 50 chars per row. */
    Editor *e = new_editor("First paragraph here.\nSecond");
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    editor_input_key(e, EDITOR_KEY_UP, false);
    size_t a, c;
    editor_get_selection(e, &a, &c);
    CHECK(c == 6); /* same x (6 chars in) on the previous line, not 22 */
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    editor_input_key(e, EDITOR_KEY_UP, true);
    editor_get_selection(e, &a, &c);
    CHECK(a == 28 && c == 6);
    editor_destroy(e);

    /* A three-row paragraph: ⌘⌫ deletes only the last visual row. */
    const char *para =
        "aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa "  /* row 0: 50 chars */
        "bbbb bbbb bbbb bbbb bbbb bbbb bbbb bbbb bbbb bbbb "  /* row 1 */
        "cccc cccc";                                          /* row 2 */
    e = new_editor(para);
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    editor_input_key(e, EDITOR_KEY_BACKSPACE_LINE, false);
    CHECK(editor_length(e) == 100);
    editor_undo(e);

    /* Line start/end stay on the visual row. */
    editor_set_selection(e, 60, 60);
    editor_input_key(e, EDITOR_KEY_LINE_START, false);
    editor_get_selection(e, &a, &c);
    CHECK(c == 50);
    editor_input_key(e, EDITOR_KEY_LINE_END, false);
    editor_get_selection(e, &a, &c);
    CHECK(c == 99); /* before the wrap boundary, still on row 1 */

    /* Clicking past the end of a wrapped row stays on that row. */
    CHECK(editor_hit_test(e, 1000.0f, 25.0f) == 99);
    CHECK(editor_hit_test(e, 1000.0f, 45.0f) == 109); /* last row: true end */
    CHECK(editor_hit_test(e, 0.0f, -5.0f) == 0);
    CHECK(editor_hit_test(e, 0.0f, 10000.0f) == 109);

    /* Page Down moves by visual rows and lands on the last row. */
    editor_set_selection(e, 0, 0);
    editor_input_key(e, EDITOR_KEY_PAGE_DOWN, false);
    editor_get_selection(e, &a, &c);
    CHECK(c == 109);
    editor_destroy(e);
}

/* Review finding R8: at the size limit, replacing a selection must not
 * delete it and then fail to insert. */
static void test_size_limit_keeps_selection(void)
{
    Editor *e = new_editor("0123456789");
    editor_set_max_chars(e, 10);
    editor_set_selection(e, 2, 5);
    /* 5 chars over a 3-char selection: fits alone, but the result (12)
     * exceeds the limit, so the selection must survive. */
    CHECK(!editor_paste_utf8(e, "abcde"));
    CHECK(text_is(e, "0123456789"));
    CHECK(editor_take_notice(e) != NULL);
    CHECK(!editor_paste_utf8(e, "much longer than the whole limit"));
    CHECK(text_is(e, "0123456789"));
    editor_input_key(e, EDITOR_KEY_TEXT_END, false);
    type(e, "x"); /* at the limit: refused, nothing deleted */
    CHECK(text_is(e, "0123456789"));
    editor_set_selection(e, 0, 1);
    type(e, "Z"); /* same size: allowed */
    CHECK(text_is(e, "Z123456789"));
    editor_destroy(e);
}

/* Ranges used by reviewed AI edits. */
static void test_ranges(void)
{
    Editor *e = new_editor("Zoë stood on the stairs. Zoë waited.");
    char *r = editor_range_utf8(e, 4, 5);
    CHECK(r && !strcmp(r, "stood"));
    free(r);
    CHECK(editor_range_utf8(e, 30, 100) == NULL);
    size_t first = 99;
    CHECK(editor_find_utf8(e, "Zoë", &first) == 2 && first == 0);
    CHECK(editor_find_utf8(e, "waited", &first) == 1 && first == 29);
    CHECK(editor_find_utf8(e, "absent", &first) == 0);
    CHECK(editor_replace_range_utf8(e, 4, 5, "paused\r\nthen"));
    CHECK(text_is(e, "Zoë paused\nthen on the stairs. Zoë waited."));
    editor_undo(e); /* one undo step */
    CHECK(text_is(e, "Zoë stood on the stairs. Zoë waited."));
    CHECK(!editor_replace_range_utf8(e, 999, 1, "x"));
    editor_destroy(e);
}

/* Red-team 004: the passage an AI edit targets is tracked through every
 * change (including undo/redo), never re-found by offset or by text. */
static bool anchor_is(Editor *e, size_t start, size_t len)
{
    size_t s = 0, l = 0;
    return editor_anchor_get(e, &s, &l) && s == start && l == len;
}

static void test_anchor(void)
{
    Editor *e = new_editor("Ab cd. Ab cd.");
    CHECK(editor_anchor_set(e, 7, 5));          /* the second "Ab cd" */
    CHECK(!editor_anchor_set(e, 10, 50));       /* out of range: refused */
    CHECK(anchor_is(e, 7, 5));                  /* ...and the old anchor kept */
    editor_set_selection(e, 0, 0);
    type(e, "X");                               /* before: shifts */
    CHECK(anchor_is(e, 8, 5));
    editor_undo(e);
    CHECK(anchor_is(e, 7, 5));
    editor_redo(e);
    CHECK(anchor_is(e, 8, 5));
    editor_undo(e);
    /* Delete the first copy entirely: the anchor follows the second, so a
     * text search can't pick the wrong one. */
    editor_set_selection(e, 0, 7);
    editor_input_key(e, EDITOR_KEY_BACKSPACE, false);
    CHECK(text_is(e, "Ab cd."));
    CHECK(anchor_is(e, 0, 5));
    editor_undo(e);
    CHECK(anchor_is(e, 7, 5));
    editor_set_selection(e, 12, 12);
    type(e, "!");                               /* right after the passage: untouched */
    CHECK(anchor_is(e, 7, 5));
    editor_set_selection(e, 7, 7);
    type(e, "Q");                               /* at its start: shifts */
    CHECK(anchor_is(e, 8, 5));
    editor_set_selection(e, 10, 10);
    type(e, "z");                               /* inside: invalid */
    CHECK(!anchor_is(e, 8, 5) && editor_anchor_active(e));
    editor_undo(e);                             /* stays invalid: never guess */
    CHECK(!anchor_is(e, 8, 5));

    /* An insertion point: typing exactly there invalidates it; deleting
     * across it too; text before shifts it. */
    CHECK(editor_anchor_set(e, 3, 0));
    editor_set_selection(e, 0, 0);
    type(e, "Y");
    CHECK(anchor_is(e, 4, 0));
    editor_set_selection(e, 4, 4);
    type(e, "w");
    CHECK(!anchor_is(e, 4, 0));
    CHECK(editor_anchor_set(e, 3, 0));
    editor_set_selection(e, 2, 5);
    editor_input_key(e, EDITOR_KEY_BACKSPACE, false);
    CHECK(!anchor_is(e, 2, 0) && !anchor_is(e, 3, 0));
    CHECK(editor_anchor_set(e, 3, 0));
    editor_set_selection(e, 3, 6);              /* deleting after it: untouched */
    editor_input_key(e, EDITOR_KEY_BACKSPACE, false);
    CHECK(anchor_is(e, 3, 0));

    /* Applying through the range API, and loading, drop it. */
    CHECK(editor_anchor_set(e, 0, 2));
    CHECK(editor_replace_range_utf8(e, 0, 2, "Hi"));
    CHECK(!editor_anchor_active(e));
    editor_destroy(e);
}

/* Red-team 004: proposals that probably aren't what the author asked for
 * carry a warning in the review panel. */
static void test_review_warnings(void)
{
    ReviewView *v = review_create(&g_font, &g_font);
    const char *passage = "The fog came in off the fjord the way it always did in late October, quietly and completely.";
    CHECK(review_set(v, passage, "The fog rolled in off the fjord the way it always did in late October, quietly.", NULL, false));
    CHECK(review_warning(v)[0] == '\0');
    int removed = 0, added = 0;
    review_counts(v, &removed, &added);
    CHECK(removed >= 1 && added == 1);
    CHECK(review_set(v, passage, "The fog came in.", NULL, false)); /* cut mid-way */
    CHECK(strstr(review_warning(v), "much shorter") != NULL);
    CHECK(review_set(v, "Is she cold now?", "Is she cold now? Yes. The passage suggests she is very cold, given the fjord and the wind.", NULL, false));
    CHECK(strstr(review_warning(v), "much longer") != NULL);
    CHECK(review_set(v, passage, passage, NULL, true));
    CHECK(strstr(review_warning(v), "incomplete") != NULL);
    CHECK(review_set(v, "Hi.", "Hello.", NULL, false)); /* short passages: no ratio warnings */
    CHECK(review_warning(v)[0] == '\0');
    review_destroy(v);
}

/* ---- fuzz ------------------------------------------------------------------ */

/* Maps content id -> text seen with that id. Equal ids must mean equal text,
 * so revisiting a state via undo/redo must reproduce it exactly. */
typedef struct { uint64_t id; char *text; } Snap;
static Snap *g_snaps;
static size_t g_nsnaps, g_capsnaps;

static void snap_reset(void)
{
    for (size_t i = 0; i < g_nsnaps; i++) free(g_snaps[i].text);
    g_nsnaps = 0;
}

static int snap_check(Editor *e)
{
    uint64_t id = editor_content_id(e);
    char *t = editor_text_utf8(e);
    for (size_t i = 0; i < g_nsnaps; i++) {
        if (g_snaps[i].id == id) {
            int same = strcmp(g_snaps[i].text, t) == 0;
            free(t);
            return same;
        }
    }
    if (g_nsnaps == g_capsnaps) {
        g_capsnaps = g_capsnaps ? g_capsnaps * 2 : 1024;
        g_snaps = realloc(g_snaps, g_capsnaps * sizeof(Snap));
    }
    g_snaps[g_nsnaps++] = (Snap){ id, t };
    return 1;
}

static const uint32_t k_pool[] = {
    'a', 'b', 'e', 'z', 'Q', '1', ' ', ' ', ' ', '\n', '\t', '\'', '.', ',',
    0x00E9, 0x2014, 0x2019, 0x201C, 0x201D, 0x0301, 0x65E5, 0x1F600, 0x00A0,
};

static const char *k_long_para =
    "She remembered the summer she was nine, when the keeper\u2019s cottage still had its red door "
    "and her grandmother still kept bees in the walled garden behind it \u2014 the smell of tar and "
    "woodsmoke, and the beam sweeping the ceiling every eleven seconds.\n";

/* `grow` biases toward insertion so documents reach sizes where wrapping,
 * paging and large offsets are exercised. */
static void random_op(Editor *e, bool grow)
{
    size_t len = editor_length(e);
    if (grow && rnd() % 3 == 0) {
        if (rnd() % 8 == 0) editor_paste_utf8(e, k_long_para);
        else editor_input_char(e, k_pool[rnd() % (sizeof(k_pool) / sizeof(k_pool[0]))]);
        g_stub_time += 0.1;
        return;
    }
    switch (rnd() % 20) {
    case 0: case 1: case 2: case 3: case 4: case 5:
        editor_input_char(e, k_pool[rnd() % (sizeof(k_pool) / sizeof(k_pool[0]))]);
        break;
    case 6: case 7:
        editor_input_key(e, (EditorKey)(rnd() % EDITOR_KEY_COUNT), rnd() % 3 == 0);
        break;
    case 8: case 9:
        editor_undo(e);
        break;
    case 10:
        editor_redo(e);
        break;
    case 11: {
        static const char *pastes[] = { "pasted", "\r\nCRLF\r\n", "“quote”—dash", "", "\x01\x02", "日本語 text", NULL };
        const char *p = pastes[rnd() % 7];
        editor_paste_utf8(e, p ? p : k_long_para);
    } break;
    case 12: {
        char *cut = NULL;
        editor_cut_selection(e, &cut);
        free(cut);
    } break;
    case 13:
        editor_set_selection(e, len ? rnd() % (len + 1) : 0, len ? rnd() % (len + 1) : 0);
        break;
    case 14: {
        size_t pos = len ? rnd() % (len + 1) : 0;
        size_t del = len - pos ? rnd() % (len - pos + 1) : 0;
        uint32_t repl[3] = { 'R', 0x00E9, '\n' };
        editor_replace_range(e, pos, del % 8, repl, rnd() % 4);
    } break;
    case 15:
        editor_select_all(e);
        break;
    case 16:
        /* Resize: changes wrap width mid-session. */
        editor_update(e, (Rectangle){ 0, 0, (float)(80 + rnd() % 800), 300 }, false);
        break;
    default:
        editor_input_key(e, EDITOR_KEY_BACKSPACE + (EditorKey)(rnd() % 5), false);
        break;
    }
    /* Time passes irregularly: sometimes inside the merge window, sometimes not. */
    g_stub_time += (rnd() % 4 == 0) ? 2.5 : 0.1;
}

static void test_fuzz_editor(void)
{
    int bad_invariants = 0, bad_snapshots = 0;
    size_t total_states = 0, max_len = 0;
    for (int seed = 0; seed < 40; seed++) {
        g_rng = 0x1234567ull + (uint64_t)seed * 0x9E3779B97F4A7C15ull;
        char seed_text[4096] = "";
        if (seed % 3 == 1) snprintf(seed_text, sizeof(seed_text), "Seed text with “quotes”.\nSecond line.\n");
        if (seed % 3 == 2)
            for (int k = 0; k < 8; k++) strncat(seed_text, k_long_para, sizeof(seed_text) - strlen(seed_text) - 1);
        bool grow = seed % 2 == 0;
        Editor *e = new_editor(seed_text);
        char *initial = editor_text_utf8(e);
        snap_reset();
        snap_check(e);
        for (int i = 0; i < 5000; i++) {
            random_op(e, grow);
            if (!editor_check_invariants(e)) bad_invariants++;
            if (!snap_check(e)) bad_snapshots++;
        }
        total_states += g_nsnaps;
        if (editor_length(e) > max_len) max_len = editor_length(e);
        /* Undo everything: must return exactly to the loaded text. */
        for (int i = 0; i < 100000 && editor_content_id(e) != 0; i++) editor_undo(e);
        CHECK(editor_content_id(e) == 0);
        CHECK(text_is(e, initial));
        CHECK(!editor_is_dirty(e));
        free(initial);
        editor_destroy(e);
    }
    CHECK(bad_invariants == 0);
    CHECK(bad_snapshots == 0);
    CHECK(total_states > 40 * 1000); /* the fuzzer really is editing */
    printf("    fuzz: 200000 ops, %zu distinct content states, max length %zu\n", total_states, max_len);
    if (bad_invariants || bad_snapshots)
        fprintf(stderr, "    invariant failures %d, snapshot mismatches %d\n", bad_invariants, bad_snapshots);
    snap_reset();
    free(g_snaps);
}

int main(void)
{
    init_font();
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/scribodo-editor-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    TestCase tests[] = {
        { "typing_undo_is_per_word", test_typing_undo_is_per_word },
        { "word_motion_and_delete", test_word_motion_and_delete },
        { "selection_replace_and_paste", test_selection_replace_and_paste },
        { "replace_range_single_undo", test_replace_range_single_undo },
        { "dirty_tracking", test_dirty_tracking },
        { "read_only_blocks_edits", test_read_only_blocks_edits },
        { "save_detects_external_change", test_save_detects_external_change },
        { "overwrite_arm_is_narrow", test_overwrite_arm_is_narrow },
        { "write_protected_opens_read_only", test_write_protected_opens_read_only },
        { "no_document_is_read_only", test_no_document_is_read_only },
        { "visual_line_motion", test_visual_line_motion },
        { "size_limit_keeps_selection", test_size_limit_keeps_selection },
        { "ranges", test_ranges },
        { "anchor", test_anchor },
        { "review_warnings", test_review_warnings },
        { "fuzz_editor", test_fuzz_editor },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", g_dir);
    free(g_font.advance);
    return rc;
}
