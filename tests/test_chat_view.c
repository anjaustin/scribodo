#define _DARWIN_C_SOURCE
#include "app/chat_store.h"
#include "ui/chat_view.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#ifdef CHAT_VIEW_FAULT_TEST
#include "fault_alloc.h"
#endif

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
    TestCase tests[] = { { "window_layouts", window_layouts },
#ifdef CHAT_VIEW_FAULT_TEST
                         { "layout_allocations", layout_allocations },
#endif
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
