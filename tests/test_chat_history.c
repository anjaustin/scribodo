#define _DARWIN_C_SOURCE
#include "llm/chat_history.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

static void history_properties(void)
{
    size_t sizes[] = { 0, 1, 31, 2048, 4096, 8192, 65536, 65537 };
    char *texts[8];
    for (size_t i = 0; i < 8; i++) {
        texts[i] = malloc(sizes[i] + 1);
        CHECK(texts[i] != NULL);
        if (!texts[i]) return;
        memset(texts[i], (int)('a' + i), sizes[i]);
        texts[i][sizes[i]] = '\0';
    }
    for (size_t run = 0; run < 1000; run++) {
        size_t n = 1 + rnd() % 600;
        bool remote = run % 2;
        ChatMessage *messages = calloc(n, sizeof(*messages));
        CHECK(messages != NULL);
        if (!messages) break;
        for (size_t i = 0; i < n; i++) {
            messages[i] = (ChatMessage){ .role = rnd() % 2 ? CHAT_USER : CHAT_ASSISTANT,
                                         .content = texts[rnd() % 8], .via = rnd() % 3 ? "remote" : NULL,
                                         .error = rnd() % 11 == 0, .ts = (double)i };
        }
        messages[n - 1].role = CHAT_USER;
        messages[n - 1].via = remote ? "remote" : NULL;
        messages[n - 1].error = false;
        ChatRequestHistory h;
        bool ok = chat_request_history_copy(messages, n, remote, &h);
        CHECK(ok == (strlen(messages[n - 1].content) <= CHAT_HISTORY_MAX_BYTES));
        if (ok) {
            CHECK(h.count > 0 && h.count <= CHAT_HISTORY_MAX_MESSAGES);
            CHECK(h.scanned > 0 && h.scanned <= CHAT_HISTORY_MAX_SCAN);
            CHECK(h.content_bytes <= CHAT_HISTORY_MAX_BYTES);
            CHECK(h.messages[0].role == CHAT_USER);
            size_t bytes = 0, previous = 0;
            for (size_t i = 0; i < h.count; i++) {
                const ChatMessage *m = &h.messages[i];
                size_t source = (size_t)m->ts;
                CHECK(source < n && (!i || source > previous));
                if (source >= n) break;
                CHECK(!m->error && (!remote || messages[source].via));
                CHECK(!m->docs && !m->via && m->content != messages[source].content);
                CHECK(!strcmp(m->content, messages[source].content));
                bytes += strlen(m->content);
                previous = source;
            }
            CHECK(previous == n - 1); /* current request always survives */
            CHECK(bytes == h.content_bytes);
        } else CHECK(!h.count && !h.messages && !h.content_bytes);
        chat_request_history_free(&h);
        free(messages);
    }
    for (size_t i = 0; i < 8; i++) free(texts[i]);
}

static void mandatory_request(void)
{
    ChatMessage m[] = { { .role = CHAT_USER, .content = "local secret" },
                        { .role = CHAT_USER, .content = "current", .via = "remote" } };
    ChatRequestHistory h;
    CHECK(chat_request_history_copy(m, 2, true, &h) && h.count == 1 && !strcmp(h.messages[0].content, "current"));
    chat_request_history_free(&h);
    m[1].via = NULL;
    CHECK(!chat_request_history_copy(m, 2, true, &h) && !h.messages);
    m[1].error = true;
    CHECK(!chat_request_history_copy(m, 2, false, &h) && !h.messages);
    m[1].error = false;
    m[1].streaming = true;
    CHECK(!chat_request_history_copy(m, 2, false, &h) && !h.messages);
    m[1].streaming = false;
    m[1].role = CHAT_ASSISTANT;
    CHECK(!chat_request_history_copy(m, 2, false, &h) && !h.messages);
    ChatMessage sparse[600];
    for (size_t i = 0; i < 600; i++) sparse[i] = (ChatMessage){ .role = CHAT_USER, .content = "private local text" };
    sparse[599].via = "remote";
    CHECK(chat_request_history_copy(sparse, 600, true, &h));
    CHECK(h.count == 1 && h.scanned == CHAT_HISTORY_MAX_SCAN && h.limited);
    chat_request_history_free(&h);
}

int main(void)
{
    TestCase tests[] = { { "history_properties", history_properties }, { "mandatory_request", mandatory_request } };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
