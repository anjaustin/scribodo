#define _DARWIN_C_SOURCE
#include "fault_alloc.h"
#include "test_util.h"
#include "test_state_fixture.h"
#include "app/chat_store.h"
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
                         { "chat_allocations", chat_faults }, { "writer_allocations", writer_faults } };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    test_state_fixture_destroy(root, store);
    return rc;
}
