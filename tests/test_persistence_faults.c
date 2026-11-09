#define _DARWIN_C_SOURCE
#include "fault_alloc.h"
#include "test_util.h"
#include "test_state_fixture.h"
#include "app/chat_store.h"
#include "app/observer_store_internal.h"
#include "llm/chat_history.h"
#include "app/records.h"
#include "app/revisions.h"
#include <sys/stat.h>
#include <unistd.h>

static char project[700], doc[800];
static StoreManager *store;

static void allocation_case(long i, long count)
{
    if (i < count) fault_alloc_once_after(i);
    else fault_alloc_after(i - count);
}

static void record_faults(void)
{
    Record *records = NULL;
    size_t n = 0;
    fault_alloc_after(-1);
    CHECK(records_list_checked(store, project, REC_NOTE, &records, &n) && n == 1);
    long count = fault_alloc_count();
    records_free(records, n);
    CHECK(count > 10);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = records_list_checked(store, project, REC_NOTE, &records, &n);
        fault_alloc_after(-1);
        CHECK(!ok && !records && n == 0);
        records_free(records, n);
    }
}

static void revision_faults(void)
{
    Revision *revs = NULL;
    size_t n = 0;
    fault_alloc_after(-1);
    CHECK(revisions_list_checked(store, project, doc, &revs, &n) && n == 1);
    long count = fault_alloc_count();
    free(revs);
    CHECK(count > 10);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = revisions_list_checked(store, project, doc, &revs, &n);
        fault_alloc_after(-1);
        CHECK(!ok && !revs && n == 0);
        free(revs);
    }
}

static void chat_faults(void)
{
    ChatMessage *messages = NULL;
    size_t n = 0;
    fault_alloc_after(-1);
    CHECK(chat_store_load(store, project, "abcd", &messages, &n) && n == 1);
    long count = fault_alloc_count();
    chat_store_free_messages(messages, n);
    CHECK(count > 10);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = chat_store_load(store, project, "abcd", &messages, &n);
        fault_alloc_after(-1);
        CHECK(!ok && !messages && n == 0);
        chat_store_free_messages(messages, n);
    }
    ChatSummary *chats = NULL;
    fault_alloc_after(-1);
    CHECK(chat_store_list_checked(store, project, &chats, &n) && n == 1);
    count = fault_alloc_count();
    free(chats);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = chat_store_list_checked(store, project, &chats, &n);
        fault_alloc_after(-1);
        CHECK(!ok && !chats && n == 0);
        free(chats);
    }
}

static void request_history_faults(void)
{
    ChatMessage messages[] = { { .role = CHAT_USER, .content = "Question" },
                               { .role = CHAT_ASSISTANT, .content = "Answer" },
                               { .role = CHAT_USER, .content = "Follow-up" } };
    ChatRequestHistory history;
    fault_alloc_after(-1);
    CHECK(chat_request_history_copy(messages, 3, false, &history) && history.count == 3);
    long count = fault_alloc_count();
    chat_request_history_free(&history);
    CHECK(count == 4); /* one bounded array and three independent bodies */
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = chat_request_history_copy(messages, 3, false, &history);
        fault_alloc_after(-1);
        CHECK(!ok && !history.messages && history.count == 0 && history.content_bytes == 0);
        chat_request_history_free(&history);
    }
}

static void packed_history_faults(void)
{
    const char request[] = "{\"messages\":[{\"role\":\"system\",\"content\":\"persona\"},"
        "{\"role\":\"user\",\"content\":\"First option\"},{\"role\":\"user\",\"content\":\"Not decided\"},"
        "{\"role\":\"user\",\"content\":\"CURRENT request\"}],\"max_tokens\":1024}";
    fault_alloc_after(-1);
    char *expected = chat_pack_history_request(request);
    long count = fault_alloc_count();
    CHECK(expected && count > 10);
    if (!expected) return;
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        char *packed = chat_pack_history_request(request);
        fault_alloc_after(-1);
        /* cJSON printing may recover from a failed optional shrink. Any
         * successful result must still be the complete, unchanged packet. */
        CHECK(!packed || !strcmp(packed, expected));
        free(packed);
    }
    free(expected);
}

static void chat_load_faults(void)
{
    ChatMessage original = { .role = CHAT_USER, .content = "Keep this conversation" };
    ChatMessage replacement[] = {
        { .role = CHAT_USER, .content = "A question", .docs = "chapter.md", .via = "Provider", .ts = 123 },
        { .role = CHAT_ASSISTANT, .content = "A partial answer", .via = "Provider", .ts = 124,
          .truncated = true, .stopped = true, .is_edit = true }
    };
    ChatSession *s = chat_create(NULL);
    CHECK(s && chat_load_messages(s, &original, 1));
    fault_alloc_after(-1);
    CHECK(chat_load_messages(s, replacement, 2));
    long count = fault_alloc_count();
    chat_lock(s);
    CHECK(chat_message(s, 0)->ts == 123 && chat_message(s, 1)->ts == 124);
    CHECK(chat_message(s, 1)->truncated && chat_message(s, 1)->stopped && chat_message(s, 1)->is_edit);
    chat_unlock(s);
    for (long i = 0; i < count * 2; i++) {
        CHECK(chat_load_messages(s, &original, 1));
        allocation_case(i, count);
        bool ok = chat_load_messages(s, replacement, 2);
        fault_alloc_after(-1);
        CHECK(!ok);
        chat_lock(s);
        CHECK(chat_count(s) == 1 && !strcmp(chat_message(s, 0)->content, original.content));
        chat_unlock(s);
    }
    chat_destroy(s);
}

static void chat_send_faults(void)
{
    /* Only UI-thread preparation allocations are injected. The fake worker has
     * its own thread-local allocator state. Each failed send must be atomic. */
    ChatDoc docs[] = { { .title = "one.md", .text = "first document" }, { .title = "two.md", .text = "second document" } };
    ChatContext contexts[] = { {0}, { .doc_title = "one.md", .doc_text = "a document" },
                               { .docs = docs, .ndocs = 2 } };
    for (size_t k = 0; k < 4; k++) {
        size_t context_index = k < 3 ? k : 0;
        RemoteConfig remote = { .name = "Test provider", .base_url = "http://127.0.0.1:1/v1",
                                .host = "127.0.0.1", .model = "test", .context_tokens = 32768 };
        ChatSession *s = chat_create(NULL);
        if (k == 3) CHECK(chat_select_remote(s, &remote));
        char why[256];
        fault_alloc_after(-1);
        CHECK(chat_send(s, "question", &contexts[context_index], why, sizeof(why)));
        long count = fault_alloc_count();
        chat_cancel(s);
        chat_destroy(s);
        for (long i = 0; i < count * 2; i++) {
            s = chat_create(NULL);
            if (k == 3) CHECK(chat_select_remote(s, &remote));
            allocation_case(i, count);
            bool ok = chat_send(s, "question", &contexts[context_index], why, sizeof(why));
            fault_alloc_after(-1);
            CHECK(!ok && why[0] && !chat_busy(s));
            chat_lock(s);
            CHECK(chat_count(s) == 0);
            chat_unlock(s);
            chat_destroy(s);
        }
    }
}

static void window_shrink_faults(void)
{
    ChatMessage *rows = calloc(1024, sizeof(*rows));
    CHECK(rows != NULL);
    for (size_t i = 0; i < 1024; i++) rows[i] = (ChatMessage){ .role = i % 2 ? CHAT_ASSISTANT : CHAT_USER,
        .content = "saved", .sequence_known = true, .sequence = (int64_t)i };
    for (int persistent = 0; persistent < 2; persistent++) {
        ChatSession *s = chat_create(NULL);
        CHECK(chat_load_messages(s, rows, 1024)); chat_enable_window(s);
        if (persistent) fault_alloc_after(0); else fault_alloc_once_after(0);
        CHECK(chat_trim_saved(s));
        chat_lock(s);
        CHECK(chat_count(s) == 64 && chat_find_sequence(s, 1023) == 63 && chat_allocated_slots(s) == 1024);
        CHECK(!strcmp(chat_message(s, 0)->content, "saved"));
        chat_unlock(s);
        if (persistent) {
            CHECK(!chat_trim_saved(s));
            chat_lock(s); CHECK(chat_allocated_slots(s) == 1024); chat_unlock(s);
        }
        fault_alloc_after(-1);
        CHECK(!chat_trim_saved(s)); /* same text/version, refused capacity shrink is retried */
        chat_lock(s);
        CHECK(chat_allocated_slots(s) <= 128 && chat_count(s) == 64 && chat_find_sequence(s, 1023) == 63);
        chat_unlock(s);
        chat_destroy(s);
    }
    free(rows);
}

static void request_builder_faults(void)
{
    ChatMessage message = {.role=CHAT_USER, .content="KEEP the author's exact request: Zoë chooses amber."};
    ChatContext context = {.doc_title="chapter.md", .doc_text="KEEP the attached passage exactly."};
    for (unsigned edit = 0; edit < 2; edit++) {
        size_t first = 1;
        fault_alloc_after(-1);
        char *control = edit ? chat_build_edit_request("KEEP this instruction", "KEEP this selected passage", &context,
                                                       32768, NULL, NULL, NULL)
                             : chat_build_request_selected(&message, 1, &context, NULL, 32768, NULL, NULL, NULL, &first);
        long allocations = fault_alloc_count();
        fault_alloc_after(-1);
        CHECK(control && allocations > 0 && (edit || first == 0));
        if (!control) continue;
        long partial = 0, missing_author = 0, missing_context = 0;
        for (long i = 0; i < allocations * 2; i++) {
            first = 1;
            allocation_case(i, allocations);
            char *request = edit ? chat_build_edit_request("KEEP this instruction", "KEEP this selected passage", &context,
                                                           32768, NULL, NULL, NULL)
                                 : chat_build_request_selected(&message, 1, &context, NULL, 32768, NULL, NULL, NULL, &first);
            long failed = fault_alloc_failures();
            fault_alloc_after(-1);
            if (request && strcmp(request, control)) {
                partial++;
                if (!strstr(request, edit ? "KEEP this selected passage" : message.content)) missing_author++;
                if (!strstr(request, context.doc_text)) missing_context++;
            }
            CHECK(failed > 0);
            CHECK(!request || !strcmp(request, control)); /* no incomplete request may be published */
            if (!edit) CHECK(first == (request ? 0u : 1u));
            free(request);
        }
        printf("    request builder edit=%u measured_allocations=%ld partial_requests=%ld missing_author_text=%ld missing_document_context=%ld (once + persistent)\n",
               edit, allocations, partial, missing_author, missing_context);
        free(control);
    }
}

static void writer_faults(void)
{
    SourceRef source = { .kind = SRC_DOC, .quote = "A quote" };
    FindingInfo info = { .type = "entity", .subject = "Mara", .relation = "knows", .object = "Zoë",
                         .related = "n-faults", .related_text = "A related assertion" };
    snprintf(source.doc, sizeof(source.doc), "chapter.md");
    char path[1000];
    snprintf(path, sizeof(path), "%s/.scribodo/findings.jsonl", project);
    fault_alloc_after(-1);
    CHECK(records_create_ex(store, project, REC_FINDING, "f-write", "new", "A finding", &source, 1, &info));
    long count = fault_alloc_count();
    CHECK(unlink(path) == 0);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = records_create_ex(store, project, REC_FINDING, "f-write", "new", "A finding", &source, 1, &info);
        fault_alloc_after(-1);
        CHECK(!ok && access(path, F_OK) != 0);
    }
    char write_doc[900];
    snprintf(write_doc, sizeof(write_doc), "%s/write.md", project);
    snprintf(path, sizeof(path), "%s/.scribodo/revisions/write.md/index.jsonl", project);
    fault_alloc_after(-1);
    CHECK(revisions_record(store, project, write_doc, "Snapshot", "save", "Reason", NULL));
    count = fault_alloc_count();
    CHECK(unlink(path) == 0);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool recorded = true;
        bool ok = revisions_record(store, project, write_doc, "Snapshot", "save", "Reason", &recorded);
        fault_alloc_after(-1);
        CHECK(!ok && !recorded && access(path, F_OK) != 0);
    }
    ChatMessage message = { .role = CHAT_USER, .content = "Reply", .docs = "chapter.md", .via = "Provider",
                            .error = true, .is_edit = true, .truncated = true };
    snprintf(path, sizeof(path), "%s/.scribodo/chats/aaaa.jsonl", project);
    fault_alloc_after(-1);
    CHECK(chat_store_append(store, project, "aaaa", "All fields", &message));
    count = fault_alloc_count();
    CHECK(unlink(path) == 0);
    for (long i = 0; i < count * 2; i++) {
        allocation_case(i, count);
        bool ok = chat_store_append(store, project, "aaaa", "All fields", &message);
        fault_alloc_after(-1);
        CHECK(!ok);
        if (access(path, F_OK) == 0) {
            ChatMessage *messages = NULL;
            size_t n = 0;
            CHECK(chat_store_load(store, project, "aaaa", &messages, &n) && n == 0);
            chat_store_free_messages(messages, n);
            CHECK(unlink(path) == 0);
        }
    }
}

static void chat_unterminated_list_faults(void)
{
    const char *cases[] = {
        "{\"type\":\"meta\",\"title\":\"No final newline\"}",
        "{\"type\":\"meta\",\"title\":\"Observer\"}\n{\"type\":\"observer\",\"read\":false}"
    };
    char path[900];
    snprintf(path, sizeof(path), "%s/.scribodo/chats/abcd.jsonl", project);
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        if (!f) return;
        CHECK(fputs(cases[c], f) >= 0);
        CHECK(fclose(f) == 0);
        ChatSummary *chats = NULL;
        size_t n = 0;
        fault_alloc_after(-1);
        CHECK(chat_store_list_checked(store, project, &chats, &n) && n == 1);
        long count = fault_alloc_count();
        if (chats && n == 1) {
            CHECK(!strcmp(chats[0].title, c == 0 ? "No final newline" : "Observer"));
            CHECK(chats[0].observer_ignore == (c == 1));
        }
        free(chats);
        for (long i = 0; i < count * 2; i++) {
            allocation_case(i, count);
            bool ok = chat_store_list_checked(store, project, &chats, &n);
            fault_alloc_after(-1);
            CHECK(!ok && !chats && n == 0);
            free(chats);
        }
    }
}

static void observer_rules_boundaries(void)
{
    const struct {
        const char *text;
        size_t len;
        bool valid;
    } cases[] = {
        { "", 0, true },
        { "0123456789abcdef", 16, true },
        { "ABCDEF0123456789", 16, true },
        { "0", 1, false },
        { "0123456789abcde", 15, false },
        { "0123456789abcdef0", 17, false },
        { "0123456789abcdef\n", 17, false },
        { "0123456789abcdeg", 16, false },
        { "01234567\0abcdef0", 16, false }
    };
    char path[900], out[17];
    snprintf(path, sizeof(path), "%s/.scribodo/observer-rules", project);
    CHECK(access(path, F_OK) != 0);
    CHECK(observer_rules_import_files(project, out) && !out[0]);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        if (!f) return;
        CHECK(fwrite(cases[i].text, 1, cases[i].len, f) == cases[i].len);
        CHECK(fclose(f) == 0);
        memset(out, 'x', sizeof(out));
        CHECK(observer_rules_import_files(project, out) == cases[i].valid);
        CHECK(out[16] == '\0' || cases[i].len < 16);
        if (cases[i].valid) CHECK(!strcmp(out, cases[i].text));
    }
    CHECK(unlink(path) == 0);
    CHECK(mkdir(path, 0755) == 0);
    CHECK(!observer_rules_import_files(project, out));
    CHECK(rmdir(path) == 0);
}

int main(void)
{
    char root[512];
    if (!test_state_fixture_init(root, sizeof(root), &store)) return 1;
    snprintf(project, sizeof(project), "%s/faults", root);
    mkdir(project, 0755);
    snprintf(doc, sizeof(doc), "%s/chapter.md", project);
    CHECK(revisions_record(store, project, doc, "Immutable text", "save", "A note", NULL));
    SourceRef source = { .kind = SRC_DOC, .quote = "Immutable" };
    snprintf(source.doc, sizeof(source.doc), "chapter.md");
    FindingInfo info = { .type = "entity", .related_text = "A related text" };
    CHECK(records_append_ex(store, project, REC_NOTE, "n-faults", "active", "Keep me", &source, 1, &info, NULL));
    ChatMessage message = { .role = CHAT_USER, .content = "Keep me too", .docs = "chapter.md", .via = "A provider" };
    CHECK(chat_store_append(store, project, "abcd", "Fault tests", &message));
    TestCase tests[] = { { "record_allocations", record_faults }, { "revision_allocations", revision_faults },
                        { "request_history_allocations", request_history_faults },
                        { "packed_history_allocations", packed_history_faults },
                        { "chat_load_allocations", chat_load_faults },
                        { "chat_send_allocations", chat_send_faults },
                        { "request_builder_allocations", request_builder_faults },
                        { "window_shrink_allocations", window_shrink_faults },
                          { "chat_allocations", chat_faults }, { "writer_allocations", writer_faults },
                          { "chat_unterminated_list_allocations", chat_unterminated_list_faults },
                          { "observer_rules_boundaries", observer_rules_boundaries } };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    test_state_fixture_destroy(root, store);
    return rc;
}
