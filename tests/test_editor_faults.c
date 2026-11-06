/* Allocation faults must not turn a refused replacement into a deletion. */
#define _DARWIN_C_SOURCE
#include "fault_alloc.h"
#include "test_util.h"
#include "ui/editor.h"

static SFont font;
static uint32_t replacement[4096];
static char replacement_utf8[4097];

static Editor *fixture(void)
{
    Editor *e = editor_create(&font);
    if (!e) return NULL;
    editor_set_scratch(e, "");
    if (!editor_set_text_utf8(e, "original passage")) {
        editor_destroy(e);
        return NULL;
    }
    editor_set_selection(e, 16, 16);
    editor_input_char(e, '!');
    editor_undo(e); /* A refused edit must preserve the redo branch too. */
    editor_set_selection(e, 0, 16);
    editor_anchor_set(e, 0, 16);
    return e;
}

static bool replace(Editor *e, int mode)
{
    if (mode == 0) return editor_replace_range(e, 0, 16, replacement, 4096);
    if (mode == 1) return editor_replace_range_utf8(e, 0, 16, replacement_utf8);
    return editor_paste_utf8(e, replacement_utf8);
}

static void replacement_faults(void)
{
    for (int mode = 0; mode < 3; mode++) {
        fault_alloc_after(-1);
        Editor *e = fixture();
        CHECK(e);
        if (!e) return;
        if (mode < 2) editor_set_selection(e, 3, 7); /* Different from the target range. */
        fault_alloc_after(-1);
        CHECK(replace(e, mode));
        long count = fault_alloc_count();
        fault_alloc_after(-1);
        editor_destroy(e);
        int refused = 0;
        for (long i = 0; i < count * 2; i++) {
            e = fixture();
            CHECK(e);
            if (!e) return;
            if (mode < 2) editor_set_selection(e, 3, 7);
            uint64_t id = editor_content_id(e);
            bool dirty = editor_is_dirty(e);
            if (i < count) fault_alloc_once_after(i);
            else fault_alloc_after(i - count);
            bool ok = replace(e, mode);
            fault_alloc_after(-1);
            char *text = editor_text_utf8(e);
            if (!ok) {
                refused++;
                CHECK(text && !strcmp(text, "original passage"));
                CHECK(editor_content_id(e) == id && editor_is_dirty(e) == dirty);
                size_t a, c, start, len;
                editor_get_selection(e, &a, &c);
                CHECK(mode < 2 ? a == 3 && c == 7 : a == 0 && c == 16);
                CHECK(editor_anchor_get(e, &start, &len) && start == 0 && len == 16);
                editor_redo(e);
                char *redone = editor_text_utf8(e);
                CHECK(redone && !strcmp(redone, "original passage!"));
                free(redone);
            } else {
                CHECK(text && !strcmp(text, replacement_utf8));
            }
            CHECK(editor_check_invariants(e));
            free(text);
            editor_destroy(e);
        }
        CHECK(refused > 0);
    }
}

static void typing_growth_fault(void)
{
    Editor *e = fixture();
    CHECK(e);
    if (!e) return;
    CHECK(editor_replace_range(e, 16, 0, replacement, 1024)); /* Exhaust the gap. */
    uint64_t id = editor_content_id(e);
    char *before = editor_text_utf8(e);
    fault_alloc_after(0);
    editor_input_char(e, 'z');
    fault_alloc_after(-1);
    char *after = editor_text_utf8(e);
    CHECK(before && after && !strcmp(before, after) && editor_content_id(e) == id);
    CHECK(editor_take_notice(e));
    editor_undo(e);
    char *undone = editor_text_utf8(e);
    CHECK(undone && !strcmp(undone, "original passage"));
    free(before);
    free(after);
    free(undone);
    editor_destroy(e);
}

int main(void)
{
    font.size = 16;
    font.line_height = 20;
    font.dpi = 1;
    font.tofu_advance = 10;
    font.advance = calloc(SFONT_LUT_SIZE, sizeof(float));
    if (!font.advance) return 1;
    for (size_t i = 0; i < SFONT_LUT_SIZE; i++) font.advance[i] = 8;
    for (size_t i = 0; i < 4096; i++) replacement[i] = replacement_utf8[i] = 'x';
    const TestCase tests[] = { { "replacement_allocations", replacement_faults },
                              { "typing_growth_allocation", typing_growth_fault } };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    free(font.advance);
    return rc;
}
