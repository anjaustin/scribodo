#define _DARWIN_C_SOURCE
#include "app/chat_store.h"
#include "app/chat_store_internal.h"
#include "app/store_backends.h"
#include "llm/chat_history.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void file_metadata(void)
{
    char dir[1024], path[1200];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/provenance-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    char *sources = malloc(CHAT_MEMORY_SOURCES_BYTES + 2);
    CHECK(sources != NULL);
    if (!sources) return;
    memset(sources, 's', CHAT_MEMORY_SOURCES_BYTES + 1);
    sources[CHAT_MEMORY_SOURCES_BYTES] = '\0';
    ChatMessage m = { .role = CHAT_USER, .content = "body", .memory_sources = sources };
    CHECK(chat_store_append_files(NULL, dir, "abc", "test", &m));
    m.memory_sources = NULL; /* old files omit the field entirely */
    CHECK(chat_store_append_files(NULL, dir, "abc", "test", &m));
    m.memory_sources = "";
    CHECK(chat_store_append_files(NULL, dir, "abc", "test", &m));
    ChatMessage *loaded = NULL;
    size_t count = 0;
    CHECK(chat_store_load_files(dir, "abc", &loaded, &count) && count == 3);
    if (count == 3) {
        CHECK(loaded[0].memory_sources && !strcmp(loaded[0].memory_sources, sources));
        CHECK(!loaded[1].memory_sources);
        CHECK(loaded[2].memory_sources && !loaded[2].memory_sources[0]);
    }
    chat_store_free_messages(loaded, count);
    sources[CHAT_MEMORY_SOURCES_BYTES] = 's';
    sources[CHAT_MEMORY_SOURCES_BYTES + 1] = '\0';
    m.memory_sources = sources;
    CHECK(!chat_store_append_files(NULL, dir, "abc", "test", &m));
    snprintf(path, sizeof(path), "%s/.scribodo/chats/abc.jsonl", dir);
    FILE *f = fopen(path, "a");
    CHECK(f != NULL);
    if (f) {
        fprintf(f, "{\"type\":\"message\",\"role\":\"assistant\",\"content\":\"x\",\"memory_sources\":\"%s\"}\n", sources);
        fclose(f);
        CHECK(!chat_store_load_files(dir, "abc", &loaded, &count) && !loaded && !count);
    }
    /* A recognized non-string metadata field must also fail closed. */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("{\"type\":\"message\",\"role\":\"user\",\"content\":\"x\",\"memory_sources\":42}\n", f);
        fclose(f);
        CHECK(!chat_store_load_files(dir, "abc", &loaded, &count) && !loaded && !count);
    }
    unlink(path);
    snprintf(path, sizeof(path), "%s/.scribodo/chats", dir); rmdir(path);
    snprintf(path, sizeof(path), "%s/.scribodo", dir); rmdir(path);
    rmdir(dir);
    free(sources);
}

static void request_metadata_excluded(void)
{
    char *sources = strdup("private as-of quote and historical source ID");
    CHECK(sources != NULL);
    if (!sources) return;
    ChatMessage m[] = {
        { .role = CHAT_USER, .content = "earlier", .via = "remote", .memory_sources = sources },
        { .role = CHAT_ASSISTANT, .content = "reply", .via = "remote", .memory_sources = sources },
        { .role = CHAT_USER, .content = "current", .via = "remote", .memory_sources = sources }
    };
    for (int remote = 0; remote < 2; remote++) {
        ChatRequestHistory h;
        CHECK(chat_request_history_copy(m, 3, remote, &h) && h.count == 3);
        for (size_t i = 0; i < h.count; i++) {
            CHECK(!h.messages[i].memory_sources);
            CHECK(!strcmp(h.messages[i].content, m[i].content));
        }
        chat_request_history_free(&h);
        CHECK(!strcmp(sources, "private as-of quote and historical source ID"));
    }
    free(sources);
}

/* Optional integration mode uses its own disposable database, never user data. */
static void db_metadata(void)
{
    char name[64], command[256], error[512] = "";
    snprintf(name, sizeof(name), "scribodo_test_provenance_%ld", (long)getpid());
    snprintf(command, sizeof(command), "tools/services.sh testdb create %s", name);
    CHECK(system(command) == 0);
    DbConfig config;
    bool configured = db_config_default(&config, error, sizeof(error));
    snprintf(config.database, sizeof(config.database), "%s", name);
    Db *db = configured ? db_open(&config, error, sizeof(error)) : NULL;
    CHECK(db != NULL);
    if (db) {
        bool migrated = db_migrate(db, error, sizeof(error));
        CHECK(migrated);
        if (migrated) {
            CHECK(db_run(db, "INSERT INTO projects(id,name,path,path_hash,created_at,updated_at)"
                             " VALUES('provenance','test','/provenance-test','provenance-test',1,1)", NULL, 0, NULL));
            ChatMessage m = { .role = CHAT_USER, .content = "body", .memory_sources = "as-of evidence" };
            CHECK(sdb_chat_append(db, "provenance", "abc", "test", &m));
            ChatWindowRequest request = { .direction = CHAT_WINDOW_LATEST };
            ChatWindowPage page;
            CHECK(sdb_chat_window(db, "provenance", "abc", &request, &page) && page.count == 1);
            if (page.count == 1) CHECK(page.messages[0].memory_sources && !strcmp(page.messages[0].memory_sources, m.memory_sources));
            chat_window_page_free(&page);
            CHECK(db_run(db, "UPDATE messages SET memory_sources=NULL", NULL, 0, NULL));
            CHECK(sdb_chat_window(db, "provenance", "abc", &request, &page) && page.count == 1);
            if (page.count == 1) CHECK(!page.messages[0].memory_sources);
            chat_window_page_free(&page);
            CHECK(db_run(db, "UPDATE messages SET memory_sources=REPEAT('x',16384)", NULL, 0, NULL));
            CHECK(sdb_chat_window(db, "provenance", "abc", &request, &page) && page.count == 1 && page.text_bytes == 4);
            if (page.count == 1) CHECK(page.messages[0].memory_sources && strlen(page.messages[0].memory_sources) == CHAT_MEMORY_SOURCES_BYTES);
            chat_window_page_free(&page);
            /* Exercise the read-side guard even when schema constraints would
             * normally reject an overlength legacy/corrupt stored value. */
            CHECK(db_run(db, "SET SESSION check_constraint_checks=OFF", NULL, 0, NULL));
            CHECK(db_run(db, "UPDATE messages SET memory_sources=REPEAT('x',16385)", NULL, 0, NULL));
            CHECK(!sdb_chat_window(db, "provenance", "abc", &request, &page) && !page.messages && !page.count);
            chat_window_page_free(&page);
            CHECK(db_run(db, "UPDATE messages SET memory_sources=CONCAT('x',CHAR(0),'y')", NULL, 0, NULL));
            CHECK(!sdb_chat_window(db, "provenance", "abc", &request, &page) && !page.messages && !page.count);
            chat_window_page_free(&page);
        }
        db_close(db);
    }
    snprintf(command, sizeof(command), "tools/services.sh testdb drop %s", name);
    CHECK(system(command) == 0);
}

int main(int argc, char **argv)
{
    TestCase tests[] = { { "file provenance", file_metadata }, { "request privacy", request_metadata_excluded } };
    if (argc > 1 && !strcmp(argv[1], "--db")) db_metadata();
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
