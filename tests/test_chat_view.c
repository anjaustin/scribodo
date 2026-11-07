#define _DARWIN_C_SOURCE
#include "app/chat_store.h"
#include "ui/chat_view.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef CHAT_VIEW_FAULT_TEST
#include "fault_alloc.h"
#endif

extern Vector2 g_stub_mouse;
extern float g_stub_wheel;
extern bool g_stub_mouse_pressed;

static void store_finished(ChatSession *chat, size_t index, void *context)
{
    chat_mark_saved(chat, index, (int64_t)index);
    (*(int *)context)++;
}

static void concurrent_presentation(void)
{
    SFont font = { .size = 16, .line_height = 20, .tofu_advance = 8 };
    font.advance = malloc(SFONT_LUT_SIZE * sizeof(float));
    CHECK(font.advance != NULL);
    for (size_t i = 0; i < SFONT_LUT_SIZE; i++) font.advance[i] = 8;
    ChatSession *chat = chat_create(NULL);
    ChatView *view = chat_view_create(&font, &font, chat);
    ChatMessage rows[] = { { .role = CHAT_USER, .content = "older question", .sequence_known = true, .sequence = 0 },
                           { .role = CHAT_ASSISTANT, .content = "older reply", .sequence_known = true, .sequence = 1 } };
    CHECK(chat_load_window(chat, rows, 2, false, false));
    int done = 0; chat_on_message_done(chat, store_finished, &done);
    char why[256];
    CHECK(chat_send(chat, "worker", NULL, why, sizeof(why)));
    for (size_t frame = 0; frame < 4000 && (chat_busy(chat) || done < 2); frame++) {
        chat_poll(chat);
        chat_view_update(view, (Rectangle){0,0,400,202}, (Rectangle){0,202,400,100}, false);
        chat_view_draw(view, (Rectangle){0,0,400,202}, (Rectangle){0,202,400,100}, false);
        usleep(1000);
    }
    chat_poll(chat);
    CHECK(!chat_busy(chat) && done == 2);
    chat_lock(chat);
    CHECK(chat_count(chat) == 4 && chat_message(chat, 3)->sequence_known && !chat_message(chat, 3)->error);
    chat_unlock(chat);
    chat_view_destroy(view); chat_destroy(chat); free(font.advance);
}

static void presented_actions(void)
{
    SFont font = { .size = 16, .line_height = 20, .tofu_advance = 8 };
    font.advance = malloc(SFONT_LUT_SIZE * sizeof(float));
    CHECK(font.advance != NULL);
    for (size_t i = 0; i < SFONT_LUT_SIZE; i++) font.advance[i] = 8;
    ChatSession *chat = chat_create(NULL);
    ChatView *view = chat_view_create(&font, &font, chat);
    ChatMessage rows[64];
    char text[64][32];
    for (size_t i = 0; i < 64; i++) {
        snprintf(text[i], sizeof(text[i]), "message %04zu", i);
        rows[i] = (ChatMessage){ .role = i % 2 ? CHAT_ASSISTANT : CHAT_USER, .content = text[i],
                                .sequence_known = true, .sequence = (int64_t)i };
    }
    Rectangle list = {0,0,400,202}, input = {0,202,400,100};
    CHECK(chat_load_window(chat, rows, 64, false, false));
    chat_view_update(view, list, input, false); chat_view_draw(view, list, input, false);
    Rectangle shown_note = chat_view_last_note_rect(view, false);
    CHECK(shown_note.width > 0);
    for (size_t i = 0; i < 64; i++) {
        rows[i].sequence += 2;
        snprintf(text[i], sizeof(text[i]), "message %04zu", i + 2);
    }
    CHECK(chat_load_window(chat, rows, 64, true, false));
    chat_view_history_changed(view); /* the same reset used after automatic eviction */
    g_stub_mouse = (Vector2){ shown_note.x + shown_note.width / 2, shown_note.y + shown_note.height / 2 };
    g_stub_mouse_pressed = true;
    chat_view_update(view, list, input, false);
    bool canon = false;
    long record = chat_view_take_record(view, &canon);
    CHECK(record == -1); /* do not act on a newly positioned, not-yet-drawn source */
    g_stub_mouse_pressed = false;
    chat_view_draw(view, list, input, false);
    shown_note = chat_view_last_note_rect(view, false);
    g_stub_mouse = (Vector2){ shown_note.x + shown_note.width / 2, shown_note.y + shown_note.height / 2 };
    g_stub_mouse_pressed = true;
    chat_view_update(view, list, input, false);
    CHECK(chat_view_take_record(view, &canon) == 63); /* fresh, stable presentation works */
    g_stub_mouse_pressed = false;
    chat_view_set_can_apply(view, false);
    chat_view_update(view, list, input, false); chat_view_draw(view, list, input, false);
    shown_note = chat_view_last_note_rect(view, false);
    chat_view_set_can_apply(view, true); /* old Note pixels are now Apply pixels */
    g_stub_mouse = (Vector2){ shown_note.x + shown_note.width / 2, shown_note.y + shown_note.height / 2 };
    g_stub_mouse_pressed = true;
    chat_view_update(view, list, input, false);
    CHECK(chat_view_take_apply(view) == -1);
    g_stub_mouse_pressed = false;
    chat_view_draw(view, list, input, false);
    shown_note = chat_view_last_note_rect(view, false);
    g_stub_mouse = (Vector2){ shown_note.x + shown_note.width / 2, shown_note.y + shown_note.height / 2 };
    g_stub_mouse_pressed = true; g_stub_wheel = 5.4f;
    chat_view_update(view, list, input, false);
    CHECK(chat_view_take_record(view, &canon) == -1); /* wheel+click cannot select a different row */
    g_stub_mouse_pressed = false; g_stub_wheel = 0;
    g_stub_mouse = (Vector2){-1000, -1000};
    chat_view_destroy(view); chat_destroy(chat); free(font.advance);
}

static void window_layouts(void)
{
    SFont font = { .size = 16, .line_height = 20, .tofu_advance = 8 };
    font.advance = malloc(SFONT_LUT_SIZE * sizeof(float));
    CHECK(font.advance != NULL);
    for (size_t i = 0; i < SFONT_LUT_SIZE; i++) font.advance[i] = 8;
    ChatSession *chat = chat_create(NULL);
    ChatView *view = chat_view_create(&font, &font, chat);
    CHECK(view && chat);
    char text[1025]; memset(text, '\n', 1024); text[1024] = '\0';
    ChatMessage rows[64];
    for (size_t iteration = 0; iteration < 100; iteration++) {
        for (size_t i = 0; i < 64; i++)
            rows[i] = (ChatMessage){ .role = i % 2 ? CHAT_ASSISTANT : CHAT_USER, .content = text,
                                     .sequence_known = true, .sequence = (int64_t)(iteration * 64 + i) };
        CHECK(chat_load_window(chat, rows, 64, iteration != 0, false));
        chat_view_history_changed(view);
        CHECK(chat_view_cache_bytes(view) == 0);
        chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
        chat_view_draw(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
        CHECK(chat_view_cache_bytes(view) < 64 * 1024 * 1024);
    }
    char *large = malloc(CHAT_MAX_RESPONSE_BYTES + 1);
    CHECK(large != NULL);
    memset(large, '\n', CHAT_MAX_RESPONSE_BYTES); large[CHAT_MAX_RESPONSE_BYTES] = '\0';
    char saved_text[8193], current_text[65537];
    memset(saved_text, '\n', 8192); saved_text[8192] = '\0';
    memset(current_text, '\n', 65536); current_text[65536] = '\0';
    ChatMessage stress[66];
    for (size_t i = 0; i < 64; i++)
        stress[i] = (ChatMessage){ .role = i % 2 ? CHAT_ASSISTANT : CHAT_USER, .content = saved_text,
                                  .sequence_known = true, .sequence = (int64_t)i };
    stress[64] = (ChatMessage){ .role = CHAT_USER, .content = current_text };
    stress[65] = (ChatMessage){ .role = CHAT_ASSISTANT, .content = large, .streaming = true };
    CHECK(chat_load_messages(chat, stress, 66));
    chat_enable_window(chat);
    chat_view_history_changed(view);
    chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    chat_view_draw(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    size_t owned_text = 64 * 8192 + 65536 + CHAT_MAX_RESPONSE_BYTES;
    size_t cache_bytes = chat_view_cache_bytes(view);
    CHECK(cache_bytes + owned_text < 64 * 1024 * 1024);
    CHECK(chat_view_last_note_rect(view, false).width == 0 && chat_view_last_note_rect(view, true).width == 0);
    printf("    stress rows=66 owned_text=%zu cache_bytes=%zu combined=%zu\n", owned_text, cache_bytes, owned_text + cache_bytes);
    ChatMessage big = { .role = CHAT_ASSISTANT, .content = large };
    CHECK(chat_load_messages(chat, &big, 1));
    chat_view_history_changed(view);
    chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    chat_view_draw(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    CHECK(chat_view_cache_bytes(view) < 1024 * 1024);
    chat_lock(chat);
    CHECK(strlen(chat_message(chat, 0)->content) == CHAT_MAX_RESPONSE_BYTES);
    chat_unlock(chat);
    free(large);
    chat_clear(chat);
    chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    CHECK(chat_view_cache_bytes(view) == 0);
    chat_view_destroy(view); chat_destroy(chat); free(font.advance);
}

#ifdef CHAT_VIEW_FAULT_TEST
static void layout_allocations(void)
{
    SFont font = { .size = 16, .line_height = 20, .tofu_advance = 8 };
    font.advance = malloc(SFONT_LUT_SIZE * sizeof(float));
    CHECK(font.advance != NULL);
    for (size_t i = 0; i < SFONT_LUT_SIZE; i++) font.advance[i] = 8;
    ChatSession *chat = chat_create(NULL);
    ChatMessage message = { .role = CHAT_ASSISTANT, .content = "**A reply**\n\n- Zoë in the fog.\n" };
    CHECK(chat_load_messages(chat, &message, 1));
    ChatView *view = chat_view_create(&font, &font, chat);
    CHECK(view != NULL);
    fault_alloc_after(-1);
    chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    chat_view_draw(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
    long count = fault_alloc_count();
    CHECK(count > 3);
    chat_view_destroy(view);
    for (long i = 0; i < count * 2; i++) {
        view = chat_view_create(&font, &font, chat);
        CHECK(view != NULL);
        if (i < count) fault_alloc_once_after(i);
        else fault_alloc_after(i - count);
        chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
        chat_view_draw(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
        fault_alloc_after(-1);
        CHECK(chat_view_cache_bytes(view) < 1024 * 1024);
        chat_lock(chat);
        CHECK(chat_count(chat) == 1 && !strcmp(chat_message(chat, 0)->content, message.content));
        chat_unlock(chat);
        chat_view_update(view, (Rectangle){0,0,400,600}, (Rectangle){0,600,400,100}, false);
        chat_view_destroy(view);
    }
    chat_destroy(chat); free(font.advance);
}
#endif

int main(void)
{
    TestCase tests[] = { { "window_layouts", window_layouts }, { "presented_actions", presented_actions },
                         { "concurrent_presentation", concurrent_presentation },
#ifdef CHAT_VIEW_FAULT_TEST
                         { "layout_allocations", layout_allocations },
#endif
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
