#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app/chat_store.h"
#include "app/revisions.h"
#include "app/store.h"
#include "llm/chat.h"

int main(void)
{
    char dir[1024];
    const char *base = getenv("TMPDIR");
    int written = snprintf(dir, sizeof(dir), "%s/scribodo-audit-XXXXXX", base && *base ? base : "/tmp");
    if (written < 0 || (size_t)written >= sizeof(dir) || !mkdtemp(dir)) return 1;
    StoreManager *store = store_manager_create();
    ChatSession *chat = chat_create(NULL);
    if (!store || !chat) return 2;
    ChatMessage msg = {.role = CHAT_USER, .content = "unsaved author text"};
    if (!chat_load_messages(chat, &msg, 1)) return 2;
    chat_mark_save_failed(chat, 0);
    printf("failed file message: save_failed=%d window.unsaved=%d\n",
           chat_message(chat, 0)->save_failed, chat_window_info(chat).unsaved);
    bool ok;
    size_t first = chat_store_resave_failed(store, chat, dir, "20261125-000000-abcd", "Audit", &ok);
    printf("first retry: n=%zu ok=%d save_failed=%d\n", first, ok, chat_message(chat, 0)->save_failed);
    size_t second = chat_store_resave_failed(store, chat, dir, "20261125-000000-abcd", "Audit", &ok);
    ChatMessage *loaded = NULL;
    size_t n = 0;
    if (!chat_store_load(store, dir, "20261125-000000-abcd", &loaded, &n)) return 3;
    printf("second retry: n=%zu ok=%d persisted_messages=%zu\n", second, ok, n);
    chat_store_free_messages(loaded, n);

    char a[256], b[256];
    memset(a, 'x', 199); memset(b, 'x', 199);
    strcpy(a + 199, "A.md"); strcpy(b + 199, "B.md");
    bool recorded = false;
    bool ra = revisions_record(store, dir, a, "document A private text", "save", "", &recorded);
    bool rb = revisions_record(store, dir, b, "document B private text", "save", "", &recorded);
    Revision *revs = NULL;
    size_t count = revisions_list(store, dir, a, &revs);
    printf("distinct names (%zu bytes): writes=%d,%d history_A=%zu\n", strlen(a), ra, rb, count);
    if (count) {
        char *text = revisions_load(store, dir, a, &revs[0]);
        printf("newest A revision contains: %s\n", text ? text : "(null)");
        free(text);
    }
    free(revs);
    chat_destroy(chat);
    store_manager_destroy(store);
    printf("isolated fixture retained at %s\n", dir);
    return 0;
}
