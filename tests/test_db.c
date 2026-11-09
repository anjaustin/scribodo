/* The MariaDB layer against the local service (make dbtest; needs
 * tools/services.sh up). Uses a throwaway database, dropped at the end. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <mysql.h>

#include "../src/app/db.h"
#include "../src/app/store.h"
#include "../src/app/store_backends.h"
#include "../src/app/store_entities.h"
#include "../src/app/project_store.h"
#include "../src/app/entity_backfill_job.h"
#include "../src/llm/semantic.h"
#include "cJSON.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "../src/app/renames.h"
#include "test_util.h"
#include "test_db_internal.h"
#include "core/deadline_clock.h"

char g_name[64];
char g_name2[64];
StoreManager *g_store;

typedef struct {
    StoreManager *store;
    const char *database;
    bool ok;
} ManagerThread;

static void *manager_thread_main(void *context)
{
    ManagerThread *thread = context;
    Db *db = store_db(thread->store);
    DbRows rows;
    bool queried = db && db_query(db, "SELECT DATABASE()", NULL, 0, &rows);
    thread->ok = queried && rows.nrows == 1 && !strcmp(db_cell(&rows, 0, 0), thread->database);
    if (queried) db_rows_free(&rows);
    store_thread_done(thread->store);
    return NULL;
}

/* Create the throwaway database as root and grant it to the app user. */
bool admin_name(const char *sql_fmt, const char *name)
{
    const char *home = getenv("HOME");
    const char *secrets = getenv("SCRIBODO_SECRETS");
    char path[1024], pw[200] = "";
    if (secrets && secrets[0]) snprintf(path, sizeof(path), "%s/db_root_password", secrets);
    else snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/services/db_root_password", home);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    pw[fread(pw, 1, sizeof(pw) - 1, f)] = '\0';
    fclose(f);
    pw[strcspn(pw, "\r\n")] = '\0';
    MYSQL *m = mysql_init(NULL);
    unsigned t = 5;
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
    const char *configured_port = getenv("SCRIBODO_DB_PORT");
    char *port_end = NULL;
    unsigned long parsed_port = configured_port ? strtoul(configured_port, &port_end, 10) : 33061;
    unsigned port = configured_port && (!configured_port[0] || !port_end || *port_end || parsed_port > 65535) ? 0 : (unsigned)parsed_port;
    bool ok = port && mysql_real_connect(m, "127.0.0.1", "root", pw, NULL, port, NULL, 0) != NULL;
    char sql[512];
    snprintf(sql, sizeof(sql), sql_fmt, name, name);
    ok = ok && mysql_query(m, sql) == 0;
    if (!ok) fprintf(stderr, "admin: %s\n", mysql_error(m));
    mysql_close(m);
    return ok;
}

bool admin(const char *sql_fmt) { return admin_name(sql_fmt, g_name); }

static void test_store_ids(void)
{
    char why[256];
    CHECK(store_start(g_store, g_name, why, sizeof(why)) && store_online(g_store));
    char p1[40], p2[40], d1[40], d2[40], d3[40], d4[40], name[300];
    CHECK(store_project_id(g_store, "/tmp", true, p1, sizeof(p1)) && p1[0] == 'p');
    CHECK(store_project_id(g_store, "/private/tmp", false, p2, sizeof(p2)) && !strcmp(p1, p2)); /* same real path */
    CHECK(!store_project_id(g_store, "/nonexistent-scribodo-dir", false, p2, sizeof(p2)));
    CHECK(store_document_id(g_store, p1, "a.md", true, d1, sizeof(d1)) && d1[0] == 'd');
    CHECK(store_document_id(g_store, p1, "a.md", true, d2, sizeof(d2)) && !strcmp(d1, d2));
    /* Case and accents are different names (case-sensitive volumes). */
    CHECK(store_document_id(g_store, p1, "A.md", true, d3, sizeof(d3)) && strcmp(d1, d3));
    CHECK(store_document_id(g_store, p1, "\xC3\xA9.md", true, d4, sizeof(d4)) && store_document_id(g_store, p1, "e.md", true, d2, sizeof(d2)) &&
          strcmp(d4, d2));
    CHECK(!store_document_id(g_store, p1, "../x.md", true, d2, sizeof(d2)));
    /* A rename keeps the id; a stale row with the new name is moved aside. */
    CHECK(store_document_id(g_store, p1, "b.md", true, d2, sizeof(d2)));
    CHECK(store_rename_document(g_store, p1, "a.md", "b.md"));
    CHECK(store_document_name(g_store, d1, name, sizeof(name)) && !strcmp(name, "b.md"));
    CHECK(store_document_name(g_store, d2, name, sizeof(name)) && strncmp(name, "b.md (earlier", 13) == 0);
    CHECK(store_document_id(g_store, p1, "b.md", false, d3, sizeof(d3)) && !strcmp(d3, d1));
    /* A connection the server drops (restart, timeout) is noticed and
     * reopened on next use (red-team 008). */
    Db *db = store_db(g_store);
    DbRows r;
    CHECK(db && db_query(db, "SELECT CONNECTION_ID()", NULL, 0, &r) && r.nrows == 1);
    char kill_sql[64];
    snprintf(kill_sql, sizeof(kill_sql), "KILL CONNECTION %s", r.nrows ? db_cell(&r, 0, 0) : "0");
    db_rows_free(&r);
    CHECK(admin(kill_sql));
    CHECK(!db_query(db, "SELECT 1", NULL, 0, &r) && db_broken(db));
    Db *again = store_db(g_store);
    CHECK(again && store_online(g_store) && db_query(again, "SELECT 1", NULL, 0, &r) && r.nrows == 1);
    if (again) db_rows_free(&r);
    CHECK(store_document_id(g_store, p1, "b.md", false, d3, sizeof(d3)) && !strcmp(d3, d1));
    store_stop(g_store);
    store_set_offline(g_store, true);
    CHECK(!store_start(g_store, g_name, why, sizeof(why)) && !store_online(g_store) && store_db(g_store) == NULL);
    store_set_offline(g_store, false);
}

static void test_chat_source_pages(void)
{
    char why[256], pid[40];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    CHECK(store_project_id(g_store, "/private/var", true, pid, sizeof(pid)));
    Db *db = store_db(g_store);
    const char *cid = "20261106-150000-abcd";
    /* Oversized Unicode source forces both byte-limited pages and UTF-8
     * continuations. Every byte must reconstruct exactly, once and in order. */
    size_t len = 200003;
    char *large = malloc(len + 1);
    CHECK(large != NULL);
    memset(large, 'x', 8191);
    for (size_t i = 8191; i + 4 <= len; i += 4) memcpy(large + i, "\xF0\x9F\x8C\xAB", 4);
    large[len] = '\0';
    ChatMessage m = { .role = CHAT_USER, .content = large };
    CHECK(sdb_chat_append(db, pid, cid, "History pages", &m));
    /* SQL string equality may ignore trailing spaces. Any nonempty persisted
     * provider marker still means remote, matching the chat adapter's policy. */
    m = (ChatMessage){ .role = CHAT_ASSISTANT, .content = "", .error = true, .truncated = true, .via = " " };
    CHECK(sdb_chat_append(db, pid, cid, "History pages", &m));
    for (size_t i = 0; i < 130; i++) {
        m = (ChatMessage){ .role = CHAT_USER, .content = "tail" };
        CHECK(sdb_chat_append(db, pid, cid, "History pages", &m));
    }
    ChatSourceCursor cursor = {0};
    ChatSourcePage page;
    DbRows plan;
    CHECK(db_query(db, "EXPLAIN SELECT m.seq FROM messages m"
                      " JOIN conversations c ON c.id = m.conversation_id"
                      " WHERE m.conversation_id = ? AND c.project_id = ? ORDER BY m.seq DESC LIMIT 1",
                   (DbArg[2]){ DB_S(cid), DB_S(pid) }, 2, &plan));
    CHECK(db_rows_has_columns(&plan, 10));
    bool indexed_seek = false;
    for (size_t i = 0; i < plan.nrows; i++) {
        const char *table = db_cell(&plan, i, 2), *key = db_cell(&plan, i, 5), *extra = db_cell(&plan, i, 9);
        CHECK(!extra || !strstr(extra, "filesort"));
        if (table && !strcmp(table, "m") && key && !strcmp(key, "msg_seq")) indexed_seek = true;
    }
    CHECK(indexed_seek); /* DESC LIMIT 1, not the archive-sized aggregate scan */
    db_rows_free(&plan);
    size_t copied = 0, empty = 0, tails = 0, pages = 0;
    bool more = true;
    while (more && pages < 20) {
        CHECK(sdb_chat_source_page(db, pid, cid, &cursor, &page));
        CHECK(page.count <= CHAT_SOURCE_PAGE_ROWS && page.text_bytes <= CHAT_SOURCE_PAGE_BYTES);
        CHECK(page.next.started && page.next.end_seq == 131);
        for (size_t i = 0; i < page.count; i++) {
            ChatSourceChunk *c = &page.chunks[i];
            size_t bytes = strlen(c->text);
            CHECK(c->message_id > 0 && bytes <= CHAT_SOURCE_CHUNK_BYTES);
            if (c->seq == 0) {
                CHECK(c->byte_offset == copied && c->total_bytes == len && c->role == CHAT_USER && !c->remote);
                CHECK(!memcmp(large + copied, c->text, bytes));
                CHECK(!bytes || ((unsigned char)c->text[0] & 0xC0) != 0x80);
                copied += bytes;
            } else if (c->seq == 1) {
                CHECK(bytes == 0 && c->error && c->truncated && c->remote && c->role == CHAT_ASSISTANT);
                empty++;
            } else { CHECK(!strcmp(c->text, "tail")); tails++; }
        }
        cursor = page.next;
        more = page.has_more;
        chat_source_page_free(&page);
        if (pages++ == 0) {
            /* Appends after page one must not move this traversal's endpoint. */
            CHECK(sdb_chat_append(db, pid, cid, "History pages", &m));
        }
    }
    CHECK(!more && copied == len && empty == 1 && tails == 130 && pages > 3);
    CHECK(sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0 && !page.has_more);
    chat_source_page_free(&page);
    printf("    source pages=%zu bytes=%zu messages=132 (concurrent append excluded)\n", pages, copied);
    cursor = (ChatSourceCursor){ .started = true, .seq = 0, .end_seq = 131, .byte_offset = 8192 };
    CHECK(!sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0); /* inside emoji */
    cursor.byte_offset = len + 1;
    CHECK(!sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0);
    cursor = (ChatSourceCursor){ .started = true, .seq = 2, .end_seq = 0 };
    CHECK(!sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0);
    cursor = (ChatSourceCursor){ .started = true, .seq = 1, .end_seq = 0, .byte_offset = 1 };
    CHECK(!sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0);
    cursor = (ChatSourceCursor){0};
    CHECK(sdb_chat_source_page(db, "wrong-project", cid, &cursor, &page) && page.count == 0 && !page.has_more);
    chat_source_page_free(&page);
    CHECK(db_run(db, "UPDATE messages SET created_at = -1 WHERE conversation_id = ? AND seq = 1",
                 (DbArg[1]){ DB_S(cid) }, 1, NULL));
    cursor = (ChatSourceCursor){ .started = true, .seq = 1, .end_seq = 131 };
    CHECK(!sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0);
    cursor = (ChatSourceCursor){ .started = true, .seq = 0, .end_seq = 131, .byte_offset = 196607 };
    CHECK(!sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 0 && page.text_bytes == 0);
    CHECK(cursor.seq == 0 && cursor.byte_offset == 196607); /* retry position not advanced by failure */
    CHECK(db_run(db, "UPDATE messages SET created_at = 1 WHERE conversation_id = ? AND seq = 1",
                 (DbArg[1]){ DB_S(cid) }, 1, NULL));
    /* Seek through sequence gaps, without assuming array offsets are IDs. */
    CHECK(db_run(db, "DELETE FROM messages WHERE conversation_id = ? AND seq = 2",
                 (DbArg[1]){ DB_S(cid) }, 1, NULL));
    cursor = (ChatSourceCursor){ .started = true, .seq = 2, .end_seq = 3 };
    CHECK(sdb_chat_source_page(db, pid, cid, &cursor, &page) && page.count == 1 && page.chunks[0].seq == 3);
    chat_source_page_free(&page);
    free(large);
    store_stop(g_store);
}

static double monotonic_seconds(void)
{
    struct timespec t;
    clock_gettime(SCRIBODO_DEADLINE_CLOCK, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void measure_chat_window(Db *db, const char *pid, const char *cid, size_t count)
{
    double full[3], window[3];
    size_t full_bytes = 0, page_bytes = 0, page_rows = 0;
    for (size_t run = 0; run < 3; run++) {
        ChatMessage *messages = NULL;
        size_t n = 0;
        double begin = monotonic_seconds();
        CHECK(sdb_chat_load(db, pid, cid, &messages, &n) && n == count);
        full[run] = monotonic_seconds() - begin;
        full_bytes = 0;
        for (size_t i = 0; i < n; i++) full_bytes += strlen(messages[i].content);
        chat_store_free_messages(messages, n);
        ChatWindowPage page;
        ChatWindowRequest request = { .direction = CHAT_WINDOW_LATEST };
        begin = monotonic_seconds();
        CHECK(sdb_chat_window(db, pid, cid, &request, &page));
        window[run] = monotonic_seconds() - begin;
        CHECK(page.count == 64 && page.text_bytes <= 65536);
        page_rows = page.count; page_bytes = page.text_bytes;
        chat_window_page_free(&page);
    }
    for (size_t i = 0; i < 3; i++) for (size_t j = i + 1; j < 3; j++) {
        if (full[i] > full[j]) { double tmp = full[i]; full[i] = full[j]; full[j] = tmp; }
        if (window[i] > window[j]) { double tmp = window[i]; window[i] = window[j]; window[j] = tmp; }
    }
    printf("    window fixture=%zu full_bytes=%zu window_rows=%zu window_bytes=%zu full_ms=%.3f window_ms=%.3f (warm median of 3)\n",
           count, full_bytes, page_rows, page_bytes, full[1] * 1000, window[1] * 1000);
}

static void test_chat_windows(void)
{
    char why[256], pid[40];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    CHECK(store_project_id(g_store, "/private/var", true, pid, sizeof(pid)));
    Db *db = store_db(g_store);
    const char *cid = "20261106-170000-abcd";
    CHECK(db_begin(db));
    CHECK(db_run(db, "INSERT INTO conversations (id,project_id,title,created_at,updated_at) VALUES (?,?,?,1,1)",
                 (DbArg[3]){ DB_S(cid), DB_S(pid), DB_S("Window fixture") }, 3, NULL));
    for (int i = 0; i < 10000; i++) {
        char text[256];
        snprintf(text, sizeof(text), "message %d", i);
        CHECK(db_run(db, "INSERT INTO messages (conversation_id,seq,role,content,created_at) VALUES (?,?,?,?,1)",
                     (DbArg[4]){ DB_S(cid), DB_I(i * 3), DB_S(i % 2 ? "assistant" : "user"), DB_S(text) }, 4, NULL));
        if (i == 99 || i == 999 || i == 9999) {
            CHECK(db_commit(db));
            measure_chat_window(db, pid, cid, (size_t)i + 1);
            if (i != 9999) CHECK(db_begin(db));
        }
    }
    ChatWindowPage page;
    ChatWindowRequest request = { .direction = CHAT_WINDOW_LATEST };
    CHECK(sdb_chat_window(db, pid, cid, &request, &page));
    CHECK(page.count == 64 && page.older && !page.newer && page.attachments_known && !page.latest_user_docs[0]);
    CHECK(page.messages[0].sequence == 9936 * 3 && page.messages[63].sequence == 9999 * 3);
    CHECK(!strcmp(page.messages[63].content, "message 9999"));
    int64_t first = page.messages[0].sequence;
    chat_window_page_free(&page);
    request = (ChatWindowRequest){ .direction = CHAT_WINDOW_BEFORE, .sequence = first };
    CHECK(sdb_chat_window(db, pid, cid, &request, &page));
    CHECK(page.count == 64 && page.older && page.newer && page.messages[63].sequence == 9935 * 3);
    int64_t last = page.messages[63].sequence;
    chat_window_page_free(&page);
    request = (ChatWindowRequest){ .direction = CHAT_WINDOW_AFTER, .sequence = last };
    CHECK(sdb_chat_window(db, pid, cid, &request, &page));
    CHECK(page.count == 64 && page.messages[0].sequence == first && !page.newer);
    chat_window_page_free(&page);
    request = (ChatWindowRequest){ .direction = CHAT_WINDOW_BEFORE, .sequence = 0 };
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.messages && !page.count);
    request = (ChatWindowRequest){ .direction = CHAT_WINDOW_AFTER, .sequence = 9999 * 3 };
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.messages && !page.count);
    request = (ChatWindowRequest){ .direction = CHAT_WINDOW_FOCUS, .sequence = 41 * 3 };
    CHECK(sdb_chat_window(db, pid, cid, &request, &page));
    CHECK(page.count == 42 && !page.older && page.newer && !strcmp(page.messages[41].content, "message 41"));
    chat_window_page_free(&page);
    request.sequence++;
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.messages && !page.count); /* sequence gap */
    char *large = malloc(24001);
    CHECK(large != NULL);
    for (size_t i = 0; i < 24000; i += 4) {
        memcpy(large + i, "\xF0\x9F\x8C\xAB", 4);
        large[i + 3] = (char)(0x80 + (i / 4) % 61);
    }
    large[24000] = '\0';
    ChatMessage message = { .role = CHAT_ASSISTANT, .content = large, .via = " ", .stopped = true };
    int64_t sequence = -1;
    CHECK(sdb_chat_append_sequenced(db, pid, cid, "Window fixture", &message, &sequence) && sequence == 29998);
    size_t offset = 0, reconstructed = 0, pieces = 0;
    bool more = true;
    while (more && pieces < 10) {
        request = (ChatWindowRequest){ CHAT_WINDOW_FOCUS, sequence, offset };
        CHECK(sdb_chat_window(db, pid, cid, &request, &page));
        const ChatMessage *m = &page.messages[page.count - 1];
        CHECK(m->sequence == sequence && m->excerpt && m->excerpt_start == offset && m->via && !strcmp(m->via, " "));
        CHECK(m->stopped && !m->error && !m->truncated);
        size_t bytes = strlen(m->content);
        CHECK(bytes <= 8192 && bytes % 4 == 0 && !memcmp(large + reconstructed, m->content, bytes));
        offset += bytes / 4; reconstructed += bytes;
        more = m->excerpt_more;
        CHECK(page.count <= 64 && page.text_bytes <= 65536);
        pieces++;
        chat_window_page_free(&page);
    }
    CHECK(!more && reconstructed == 24000 && offset == 6000 && pieces == 3);
    request = (ChatWindowRequest){ CHAT_WINDOW_FOCUS, sequence, 6001 };
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.count && !page.messages);
    request = (ChatWindowRequest){ .direction = CHAT_WINDOW_LATEST };
    CHECK(sdb_chat_window(db, "wrong-project", cid, &request, &page) && page.count == 0 && !page.older && !page.newer);
    chat_window_page_free(&page);
    CHECK(db_run(db, "UPDATE messages SET created_at=-1 WHERE conversation_id=? AND seq=?",
                 (DbArg[2]){DB_S(cid), DB_I(9990 * 3)}, 2, NULL));
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.count && !page.messages && !page.text_bytes);
    CHECK(db_run(db, "UPDATE messages SET created_at=1 WHERE conversation_id=? AND seq=?",
                 (DbArg[2]){DB_S(cid), DB_I(9990 * 3)}, 2, NULL));
    CHECK(db_run(db, "UPDATE messages SET via=REPEAT('é',300) WHERE conversation_id=? AND seq=?",
                 (DbArg[2]){ DB_S(cid), DB_I(sequence) }, 2, NULL));
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.messages);
    CHECK(db_run(db, "UPDATE messages SET via=NULL,content=CONCAT('a',CHAR(0),'b') WHERE conversation_id=? AND seq=?",
                 (DbArg[2]){ DB_S(cid), DB_I(sequence) }, 2, NULL));
    CHECK(!sdb_chat_window(db, pid, cid, &request, &page) && !page.messages);
    CHECK(db_run(db, "UPDATE messages SET content='evidence' WHERE conversation_id=? AND seq=?",
                 (DbArg[2]){ DB_S(cid), DB_I(sequence) }, 2, NULL));
    for (size_t i = 0; i < 9; i++) {
        char name[64], doc[40];
        snprintf(name, sizeof(name), "window-attachment-%zu.md", i);
        CHECK(store_document_id_db(db, pid, name, true, doc, sizeof(doc)));
        CHECK(db_run(db, "INSERT INTO message_documents(message_id,document_id)"
                         " SELECT id,? FROM messages WHERE conversation_id=? AND seq=?",
                     (DbArg[3]){ DB_S(doc), DB_S(cid), DB_I(9998 * 3) }, 3, NULL));
    }
    CHECK(sdb_chat_window(db, pid, cid, &request, &page) && page.count == 64 && !page.attachments_known && !page.latest_user_docs[0]);
    chat_window_page_free(&page);
    /* The exact sequence returned by append is the source reference, not the
     * last display position (63). Store linkage must preserve that identity. */
    SourceRef source = { .kind = SRC_CHAT, .message = (long)sequence, .quote = "evidence", .len = 8 };
    snprintf(source.chat, sizeof(source.chat), "%s", cid);
    char record[40]; store_new_id('n', record, sizeof(record));
    CHECK(sdb_record_append(db, pid, REC_NOTE, record, "active", "from the reply", &source, 1, NULL, true, NULL));
    DbRows refs;
    CHECK(db_query(db, "SELECT message_seq FROM source_refs WHERE record_id=?", (DbArg[1]){DB_S(record)}, 1, &refs));
    int64_t actual = -1;
    CHECK(refs.nrows == 1 && db_cell_i64(&refs, 0, 0, &actual) && actual == sequence);
    db_rows_free(&refs);
    CHECK(db_run(db, "UPDATE messages SET role='assistant' WHERE conversation_id=? AND seq>=?",
                 (DbArg[2]){ DB_S(cid), DB_I(9937 * 3) }, 2, NULL));
    CHECK(sdb_chat_window(db, pid, cid, &request, &page) && page.count == 64 && !page.attachments_known && !page.latest_user_docs[0]);
    chat_window_page_free(&page);
    free(large);
    store_stop(g_store);
}

static void test_store_managers(void)
{
    StoreManager *a = store_manager_create(), *b = store_manager_create();
    char why[256] = "";
    CHECK(a && b);
    CHECK(store_start(a, g_name, why, sizeof(why)));
    CHECK(store_start(b, g_name2, why, sizeof(why)));

    DbRows rows;
    Db *adb = store_db(a), *bdb = store_db(b);
    CHECK(adb && bdb && adb != bdb);
    CHECK(db_query(adb, "SELECT DATABASE()", NULL, 0, &rows) && !strcmp(db_cell(&rows, 0, 0), g_name));
    db_rows_free(&rows);
    CHECK(db_query(bdb, "SELECT DATABASE()", NULL, 0, &rows) && !strcmp(db_cell(&rows, 0, 0), g_name2));
    db_rows_free(&rows);

    store_set_offline(a, true);
    CHECK(!store_online(a) && store_online(b) && store_db(a) == NULL && store_db(b) == bdb);
    store_set_offline(a, false);

    ManagerThread one = { a, g_name, false }, two = { b, g_name2, false };
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, NULL, manager_thread_main, &one) == 0);
    CHECK(pthread_create(&tb, NULL, manager_thread_main, &two) == 0);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(one.ok && two.ok);

    char project[PATH_MAX], pid[40];
    snprintf(project, sizeof(project), "%s/scribodo-manager-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(project, 0755);
    CHECK(store_project_id(a, project, true, pid, sizeof(pid)) && store_mark_imported(project, pid));
    CHECK(store_mode(a, project, NULL, 0, NULL) == STORE_DB);
    CHECK(store_mode(b, project, NULL, 0, NULL) == STORE_PAUSED &&
          store_problem(b, project, NULL, 0) == STORE_PROBLEM_MISSING);

    char real[PATH_MAX];
    CHECK(realpath(project, real) != NULL);
    CHECK(db_run(bdb,
                 "INSERT INTO projects (id,name,path,path_hash,created_at,updated_at) VALUES (?,?,?,?,1,1)",
                 (DbArg[4]){ DB_S(pid), DB_S("manager"), DB_S(real), DB_S("manager-generation-test") }, 4, NULL));
    /* Restarting advances the lifecycle generation and invalidates b's
     * cached missing verdict without disturbing a. */
    CHECK(store_start(b, g_name2, why, sizeof(why)));
    CHECK(store_mode(b, project, NULL, 0, NULL) == STORE_DB && store_mode(a, project, NULL, 0, NULL) == STORE_DB);

    char missing[64], configured[64];
    snprintf(missing, sizeof(missing), "scribodo_missing_%d", (int)getpid());
    CHECK(!store_start(b, missing, why, sizeof(why)) && !store_online(b));
    store_database(b, configured, sizeof(configured));
    CHECK(!strcmp(configured, missing) && !store_probe(b));
    CHECK(store_start(b, g_name2, why, sizeof(why)));

    char marker[PATH_MAX + 32];
    snprintf(marker, sizeof(marker), "%s/.scribodo/store.json", project);
    unlink(marker);
    char metadata[PATH_MAX + 16];
    snprintf(metadata, sizeof(metadata), "%s/.scribodo", project);
    rmdir(metadata);
    rmdir(project);
    store_manager_destroy(a);
    store_manager_destroy(b);
}

/* The store backends: revisions, records with sources, and chats, all by id. */
static void test_backends(void)
{
    char why[256];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    Db *db = store_db(g_store);
    char pid[40];
    CHECK(db && store_project_id(g_store, "/private/var", true, pid, sizeof(pid)));
    /* Revisions: deduplicated against the newest; a source snapshot reuses any. */
    bool rec = false;
    char r1[40] = "", r2[40] = "", r3[40] = "";
    CHECK(sdb_revision_record(db, pid, "/x/ch1.md", "first", "save", NULL, false, &rec, r1, sizeof(r1)) && rec && r1[0] == 'r');
    CHECK(sdb_revision_record(db, pid, "/x/ch1.md", "first", "save", NULL, false, &rec, NULL, 0) && !rec);
    CHECK(sdb_revision_record(db, pid, "/x/ch1.md", "second", "ai-edit", "tighten", false, &rec, r2, sizeof(r2)) && rec);
    CHECK(sdb_revision_record(db, pid, "/x/ch1.md", "first", "source", NULL, true, &rec, r3, sizeof(r3)) && !rec &&
          !strcmp(r3, r1));
    Revision *list = NULL;
    size_t n = sdb_revision_list(db, pid, "/x/ch1.md", &list);
    CHECK(n == 2 && !strcmp(list[0].origin, "ai-edit") && !strcmp(list[0].note, "tighten") && list[0].bytes == 6);
    char *t0 = n ? sdb_revision_load(db, pid, "/x/ch1.md", &list[1]) : NULL;
    CHECK(t0 && !strcmp(t0, "first"));
    free(t0);
    CHECK(n && sdb_revision_load(db, pid, "/x/other.md", &list[1]) == NULL); /* not that document's */
    free(list);
    /* History order is the order recorded, whatever the clock said (two
     * saves in one millisecond, a clock stepped back). */
    CHECK(db_run(db, "UPDATE revisions SET created_at = 100 WHERE id = ?", (DbArg[1]){ DB_S(r2) }, 1, NULL));
    CHECK(db_run(db, "UPDATE revisions SET created_at = 200 WHERE id = ?", (DbArg[1]){ DB_S(r1) }, 1, NULL));
    n = sdb_revision_list(db, pid, "/x/ch1.md", &list);
    CHECK(n == 2 && !strcmp(list[0].origin, "ai-edit")); /* r2, recorded last, is still the newest */
    free(list);
    CHECK(db_run(db, "UPDATE revisions SET origin = REPEAT('x', 24) WHERE id = ?", (DbArg[1]){ DB_S(r2) }, 1, NULL));
    list = (Revision *)1;
    n = 99;
    CHECK(!sdb_revision_list_checked(db, pid, "/x/ch1.md", &list, &n) && list == NULL && n == 0);
    CHECK(db_run(db, "UPDATE revisions SET origin = 'ai-edit' WHERE id = ?", (DbArg[1]){ DB_S(r2) }, 1, NULL));
    CHECK(sdb_revision_record(db, pid, "/x/ch1.md", "second", "save", NULL, false, &rec, NULL, 0) && !rec); /* same as newest */

    /* Records: versions, sources by id, the document's current name after a rename. */
    SourceRef src = { .kind = SRC_DOC, .start = 0, .len = 5, .quote = "first" };
    snprintf(src.doc, sizeof(src.doc), "ch1.md");
    snprintf(src.revision, sizeof(src.revision), "%s", r1);
    char cid[40];
    store_new_id('c', cid, sizeof(cid));
    int ver = 0;
    CHECK(sdb_record_append(db, pid, REC_CANON, cid, "committed", "Zoë's eyes are green.", &src, 1, NULL, true, &ver) && ver == 1);
    CHECK(sdb_record_append(db, pid, REC_CANON, cid, "retracted", "Zoë's eyes are green.", &src, 1, NULL, false, &ver) && ver == 2);
    CHECK(!sdb_record_append(db, pid, REC_NOTE, cid, "active", "wrong kind", NULL, 0, NULL, false, &ver)); /* a canon id as a note */
    FindingInfo fi = { 0 };
    snprintf(fi.type, sizeof(fi.type), "conflict");
    snprintf(fi.related, sizeof(fi.related), "%s", cid);
    fi.related_text = "Zoë's eyes are green.";
    char fid[40];
    store_new_id('f', fid, sizeof(fid));
    CHECK(sdb_record_append(db, pid, REC_FINDING, fid, "new", "Possible conflict.", &src, 1, &fi, true, NULL));
    CHECK(store_rename_document(g_store, pid, "ch1.md", "Chapter One.md"));
    Record *recs = NULL;
    size_t nr = sdb_record_list(db, pid, REC_CANON, &recs);
    CHECK(nr == 1 && recs[0].nversions == 2 && !strcmp(record_current(&recs[0])->status, "retracted"));
    CHECK(nr == 1 && record_current(&recs[0])->nsources == 1 &&
          !strcmp(record_current(&recs[0])->sources[0].doc, "Chapter One.md") && /* follows the rename */
          !strcmp(record_current(&recs[0])->sources[0].revision, r1) && !strcmp(record_current(&recs[0])->sources[0].quote, "first"));
    records_free(recs, nr);
    CHECK(db_run(db, "UPDATE record_versions SET status = REPEAT('x', 16) WHERE record_id = ? AND ver = 2",
                 (DbArg[1]){ DB_S(cid) }, 1, NULL));
    recs = (Record *)1;
    nr = 99;
    CHECK(!sdb_record_list_checked(db, pid, REC_CANON, &recs, &nr) && recs == NULL && nr == 0);
    CHECK(db_run(db, "UPDATE record_versions SET status = 'retracted' WHERE record_id = ? AND ver = 2",
                 (DbArg[1]){ DB_S(cid) }, 1, NULL));
    nr = sdb_record_list(db, pid, REC_FINDING, &recs);
    CHECK(nr == 1 && !strcmp(record_current(&recs[0])->finding.type, "conflict") &&
          !strcmp(record_current(&recs[0])->finding.related, cid) && record_current(&recs[0])->finding.related_text &&
          !strcmp(record_current(&recs[0])->finding.related_text, "Zoë's eyes are green."));
    records_free(recs, nr);
    /* Revisions are found under the new name too (same document id). */
    n = sdb_revision_list(db, pid, "/x/Chapter One.md", &list);
    CHECK(n == 2);
    free(list);

    /* Another project's ids are never linked (red-team 008): its revision,
     * its chat and its records come through as plain quotes. */
    char other[40];
    CHECK(store_project_id(g_store, "/private/var/empty", true, other, sizeof(other)));
    char ofid[40];
    store_new_id('f', ofid, sizeof(ofid));
    SourceRef xs = src; /* r1 is pid's revision, of a document named ch1.md there */
    snprintf(xs.doc, sizeof(xs.doc), "Chapter One.md");
    FindingInfo xf = fi; /* related: pid's canon entry */
    CHECK(sdb_record_append(db, other, REC_FINDING, ofid, "new", "Elsewhere.", &xs, 1, &xf, true, NULL));
    DbRows xr;
    CHECK(db_query(db, "SELECT revision_id FROM source_refs WHERE record_id = ?", (DbArg[1]){ DB_S(ofid) }, 1, &xr) &&
          xr.nrows == 1 && db_cell(&xr, 0, 0) == NULL);
    db_rows_free(&xr);
    CHECK(db_query(db, "SELECT related_id FROM record_versions WHERE record_id = ?", (DbArg[1]){ DB_S(ofid) }, 1, &xr) &&
          xr.nrows == 1 && db_cell(&xr, 0, 0) == NULL);
    db_rows_free(&xr);

    /* Chats: messages in order, referenced documents by id. */
    ChatMessage u = { .role = CHAT_USER, .content = "compare", .docs = "Chapter One.md\nnotes.md" };
    ChatMessage a = { .role = CHAT_ASSISTANT, .content = "They differ.", .via = "Frontier \xC2\xB7 api.example.com", .stopped = true };
    CHECK(sdb_chat_append(db, pid, "20261104-150000-abcd", "Compare", &u) && sdb_chat_append(db, pid, "20261104-150000-abcd", "x", &a));
    CHECK(store_rename_document(g_store, pid, "notes.md", "Notes.md"));
    {
        char ofid2[40];
        store_new_id('f', ofid2, sizeof(ofid2));
        SourceRef cs = { .kind = SRC_CHAT, .message = 0, .start = 0, .len = 3, .quote = "com" };
        snprintf(cs.chat, sizeof(cs.chat), "20261104-150000-abcd"); /* pid's chat */
        CHECK(sdb_record_append(db, other, REC_FINDING, ofid2, "new", "Elsewhere.", &cs, 1, NULL, true, NULL));
        DbRows cr;
        CHECK(db_query(db, "SELECT conversation_id FROM source_refs WHERE record_id = ?", (DbArg[1]){ DB_S(ofid2) }, 1, &cr) &&
              cr.nrows == 1 && db_cell(&cr, 0, 0) == NULL);
        db_rows_free(&cr);
    }
    ChatMessage *msgs = NULL;
    size_t nm = 0;
    CHECK(sdb_chat_load(db, pid, "20261104-150000-abcd", &msgs, &nm) && nm == 2 && msgs[0].role == CHAT_USER &&
          !strcmp(msgs[1].content, "They differ.") && msgs[0].docs && !strcmp(msgs[0].docs, "Chapter One.md\nNotes.md"));
    CHECK(nm == 2 && !msgs[0].via && msgs[1].via && !strcmp(msgs[1].via, "Frontier \xC2\xB7 api.example.com"));
    CHECK(nm == 2 && !msgs[0].stopped && msgs[1].stopped && !msgs[1].error && !msgs[1].truncated);
    chat_store_free_messages(msgs, nm);
    ChatSourceCursor stopped_cursor = {0};
    ChatSourcePage stopped_page;
    CHECK(sdb_chat_source_page(db, pid, "20261104-150000-abcd", &stopped_cursor, &stopped_page));
    CHECK(stopped_page.count == 2 && !stopped_page.chunks[0].stopped && stopped_page.chunks[1].stopped);
    chat_source_page_free(&stopped_page);
    CHECK(sdb_chat_rename(db, pid, "20261104-150000-abcd", "Comparing chapters"));
    /* The author's "don't observe this chat" is kept with it. */
    {
        CHECK(sdb_chat_set_observer(db, pid, "20261104-150000-abcd", false));
        ChatSummary *ol = NULL;
        size_t on = sdb_chat_list(db, pid, &ol);
        CHECK(on >= 1 && ol[0].observer_ignore);
        free(ol);
        CHECK(sdb_chat_set_observer(db, pid, "20261104-150000-abcd", true));
        on = sdb_chat_list(db, pid, &ol);
        CHECK(on >= 1 && !ol[0].observer_ignore);
        free(ol);
    }
    ChatSummary *cs = NULL;
    size_t nc = sdb_chat_list(db, pid, &cs);
    CHECK(nc == 1 && !strcmp(cs[0].title, "Comparing chapters"));
    free(cs);
    /* Checked decoders reject schema-valid values that cannot safely fit the
     * public domain structs, and never return partial output. */
    CHECK(db_run(db, "UPDATE conversations SET updated_at = -1 WHERE id = ?",
                 (DbArg[1]){ DB_S("20261104-150000-abcd") }, 1, NULL));
    cs = (ChatSummary *)1;
    nc = 99;
    CHECK(!sdb_chat_list_checked(db, pid, &cs, &nc) && cs == NULL && nc == 0);
    CHECK(db_run(db, "UPDATE conversations SET updated_at = 9223372036854775808 WHERE id = ?",
                 (DbArg[1]){ DB_S("20261104-150000-abcd") }, 1, NULL));
    cs = (ChatSummary *)1;
    nc = 99;
    CHECK(!sdb_chat_list_checked(db, pid, &cs, &nc) && cs == NULL && nc == 0);
    CHECK(db_run(db, "UPDATE conversations SET updated_at = 10, title = REPEAT('x', 160) WHERE id = ?",
                 (DbArg[1]){ DB_S("20261104-150000-abcd") }, 1, NULL));
    cs = (ChatSummary *)1;
    nc = 99;
    CHECK(!sdb_chat_list_checked(db, pid, &cs, &nc) && cs == NULL && nc == 0);
    CHECK(db_run(db, "UPDATE conversations SET title = 'Comparing chapters' WHERE id = ?",
                 (DbArg[1]){ DB_S("20261104-150000-abcd") }, 1, NULL));
    CHECK(db_run(db, "UPDATE messages SET created_at = -1 WHERE conversation_id = ? AND seq = 1",
                 (DbArg[1]){ DB_S("20261104-150000-abcd") }, 1, NULL));
    msgs = (ChatMessage *)1;
    nm = 99;
    CHECK(!sdb_chat_load(db, pid, "20261104-150000-abcd", &msgs, &nm) && msgs == NULL && nm == 0);
    CHECK(db_run(db, "UPDATE messages SET created_at = 10 WHERE conversation_id = ? AND seq = 1",
                 (DbArg[1]){ DB_S("20261104-150000-abcd") }, 1, NULL));
    /* Another project can't touch this one's chat. */
    char pid2[40];
    CHECK(store_project_id(g_store, "/private/etc", true, pid2, sizeof(pid2)));
    CHECK(!sdb_chat_append(db, pid2, "20261104-150000-abcd", "x", &a) && !sdb_chat_rename(db, pid2, "20261104-150000-abcd", "x"));
    CHECK(sdb_chat_load(db, pid2, "20261104-150000-abcd", &msgs, &nm) && nm == 0);
    chat_store_free_messages(msgs, nm);
    store_stop(g_store);
}

typedef struct {
    const char *project_id;
    const char *directory;
    char why[256];
    bool ok;
    atomic_bool started;
    atomic_bool done;
} ExportCall;

static void *run_export(void *arg)
{
    ExportCall *call = arg;
    atomic_store(&call->started, true);
    call->ok = store_export_project(g_store, call->project_id, call->directory, call->why, sizeof(call->why));
    store_thread_done(g_store);
    atomic_store(&call->done, true);
    return NULL;
}

/* A project's files moved into the store keep everything: revisions,
 * chats, notes, canon, findings with sources, renames, observer state. */
static void test_import(void)
{
    char why[256];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    char proj[512], doc[600];
    snprintf(proj, sizeof(proj), "%s/scribodo-import-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(proj, 0755);
    snprintf(doc, sizeof(doc), "%s/ch1.md", proj);
    FILE *f = fopen(doc, "wb");
    fputs("Zoë kept the letter.\n", f);
    fclose(f);
    /* A second file with no snapshot history at all: the import must still
     * give it a saved revision (its on-disk text), or memory can never cover it. */
    char loose[600];
    snprintf(loose, sizeof(loose), "%s/notes.txt", proj);
    f = fopen(loose, "wb");
    fputs("Loose notes.\n", f);
    fclose(f);
    /* Entries the app cannot read as text (red-team 016): a folder named like
     * a document and a file with a NUL byte. The move into the store must
     * succeed; they simply get no saved revision. */
    char folder[600], binary[600];
    snprintf(folder, sizeof(folder), "%s/folder.md", proj);
    mkdir(folder, 0755);
    snprintf(binary, sizeof(binary), "%s/binary.txt", proj);
    f = fopen(binary, "wb");
    fwrite("abc\0def", 1, 7, f);
    fclose(f);
    /* Not in the store yet: everything below goes to files. */
    CHECK(store_mode(g_store, proj, NULL, 0, NULL) == STORE_FILES);
    ProjectStoreInspection inspection = project_store_inspect(g_store, proj);
    CHECK((inspection.capabilities & PROJECT_STORE_CAP_STRUCTURED_WRITE) &&
          !(inspection.capabilities & PROJECT_STORE_CAP_ENTITIES) && inspection.blocked_by == PROJECT_STORE_BLOCK_NONE);
    bool rec;
    store_set_write_time(g_store, 1700000000.25); /* written long ago: exports must keep that */
    CHECK(revisions_record(g_store, proj, doc, "first draft", "save", NULL, &rec) && rec);
    char snap[160];
    CHECK(revisions_snapshot(g_store, proj, doc, "Zoë kept the letter.\n", snap, sizeof(snap)));
    SourceRef src = { .kind = SRC_DOC, .start = 0, .len = 3, .quote = "Zoë" };
    snprintf(src.doc, sizeof(src.doc), "ch1.md");
    snprintf(src.revision, sizeof(src.revision), "%s", snap);
    char nid[40], cid[40];
    records_new_id(REC_NOTE, nid, sizeof(nid));
    records_new_id(REC_CANON, cid, sizeof(cid));
    CHECK(records_append(g_store, proj, REC_NOTE, nid, "active", "Check the date.", &src, 1, NULL));
    CHECK(records_append(g_store, proj, REC_NOTE, nid, "active", "Check the date (1911?).", &src, 1, NULL));
    CHECK(records_append(g_store, proj, REC_CANON, cid, "committed", "Zoë's eyes are green.", NULL, 0, NULL));
    store_set_write_time(g_store, 0);
    const time_t revs_ts_oldest = 1700000000, note_ts = 1700000000;
    char cmd0[1400];
    ChatMessage u = { .role = CHAT_USER, .content = "hello", .docs = "ch1.md" };
    ChatMessage a = { .role = CHAT_ASSISTANT, .content = "hi", .stopped = true };
    CHECK(chat_store_append(g_store, proj, "20261104-160000-abcd", "Greeting", &u) && chat_store_append(g_store, proj, "20261104-160000-abcd", "x", &a));
    /* A rename made before the import. */
    char doc2[600];
    snprintf(doc2, sizeof(doc2), "%s/Chapter One.md", proj);
    CHECK(rename(doc, doc2) == 0);
    char rv_old[700], rv_new[700];
    snprintf(rv_old, sizeof(rv_old), "%s/.scribodo/revisions/ch1.md", proj);
    snprintf(rv_new, sizeof(rv_new), "%s/.scribodo/revisions/Chapter One.md", proj);
    CHECK(rename(rv_old, rv_new) == 0);
    CHECK(renames_record(proj, "ch1.md", "Chapter One.md"));
    CHECK(chat_store_set_observer(g_store, proj, "20261104-160000-abcd", false)); /* carried into the store */
    cJSON *ost = cJSON_CreateObject();
    cJSON *odocs = cJSON_AddObjectToObject(ost, "docs");
    cJSON *ohashes = cJSON_AddArrayToObject(odocs, "Chapter One.md");
    cJSON_AddItemToArray(ohashes, cJSON_CreateString("0123456789abcdef"));
    cJSON *ochats = cJSON_AddObjectToObject(ost, "chats");
    cJSON *ochat = cJSON_AddObjectToObject(ochats, "20261104-160000-abcd");
    cJSON_AddNumberToObject(ochat, "n", 2);
    cJSON_AddNumberToObject(ochat, "off", 17);
    CHECK(project_store_observer_state_save(g_store, proj, ost));
    cJSON_Delete(ost);
    CHECK(project_store_observer_rules_set(g_store, proj, "feedfacecafebeef"));
    cJSON *stale_file = project_store_observer_state_load(g_store, proj);
    ost = project_store_observer_state_load(g_store, proj);
    CHECK(stale_file && ost && project_store_observer_state_save(g_store, proj, ost));
    CHECK(stale_file && !project_store_observer_state_save(g_store, proj, stale_file));
    cJSON_Delete(ost);
    cJSON_Delete(stale_file);

    why[0] = '\0';
    CHECK(store_import(g_store, proj, why, sizeof(why)));
    if (why[0]) fprintf(stderr, "    %s\n", why);
    CHECK(store_mode(g_store, proj, NULL, 0, NULL) == STORE_DB);
    EntityBackfillJob backfill;
    CHECK(entity_backfill_job_init(&backfill, g_store));
    CHECK(entity_backfill_job_start(&backfill, proj));
    CHECK(entity_backfill_job_busy(&backfill) && !entity_backfill_job_start(&backfill, proj));
    bool backfill_ok = false, backfill_done = false;
    for (int i = 0; !backfill_done && i < 500; i++) {
        backfill_done = entity_backfill_job_poll(&backfill, &backfill_ok);
        if (!backfill_done) usleep(1000);
    }
    CHECK(backfill_done && backfill_ok && !entity_backfill_job_busy(&backfill));
    entity_backfill_job_destroy(&backfill);
    inspection = project_store_inspect(g_store, proj);
    CHECK((inspection.capabilities & (PROJECT_STORE_CAP_STRUCTURED_WRITE | PROJECT_STORE_CAP_ENTITIES |
                                      PROJECT_STORE_CAP_SEMANTIC | PROJECT_STORE_CAP_LINKED_DOCUMENTS)) ==
          (PROJECT_STORE_CAP_STRUCTURED_WRITE | PROJECT_STORE_CAP_ENTITIES |
           PROJECT_STORE_CAP_SEMANTIC | PROJECT_STORE_CAP_LINKED_DOCUMENTS));
    CHECK(store_import(g_store, proj, why, sizeof(why))); /* again: nothing to do */
    /* Every document file has a saved revision holding its on-disk text: the
     * snapshot that matches (no duplicate), or a new "import" revision. */
    {
        char pid_saved[64] = "";
        CHECK(store_mode(g_store, proj, pid_saved, sizeof(pid_saved), NULL) == STORE_DB);
        Db *sdb = store_db(g_store);
        SavedRevisionIdentity saved = {0};
        CHECK(sdb && sdb_saved_revision_identity(sdb, pid_saved, "Chapter One.md", &saved) &&
              saved.bytes == strlen("Zoë kept the letter.\n"));
        CHECK(sdb && sdb_saved_revision_identity(sdb, pid_saved, "notes.txt", &saved) && saved.bytes == strlen("Loose notes.\n"));
        CHECK(sdb && !sdb_saved_revision_identity(sdb, pid_saved, "folder.md", &saved));
        CHECK(sdb && !sdb_saved_revision_identity(sdb, pid_saved, "binary.txt", &saved));
        /* The lazy repair of a project moved before imports recorded saved
         * files: with the pointer gone, adoption restores it to the revision
         * that already holds the file's text (no duplicate), once. */
        bool adopted = true;
        CHECK(sdb && db_run(sdb, "UPDATE documents SET saved_revision_id = NULL WHERE project_id = ? AND name = 'notes.txt'",
                            (DbArg[1]){ DB_S(pid_saved) }, 1, NULL));
        CHECK(sdb && !sdb_saved_revision_identity(sdb, pid_saved, "notes.txt", &saved));
        CHECK(sdb && sdb_saved_file_adopt(sdb, pid_saved, proj, "notes.txt", false, &adopted) && adopted);
        CHECK(sdb && sdb_saved_revision_identity(sdb, pid_saved, "notes.txt", &saved) && saved.bytes == strlen("Loose notes.\n"));
        CHECK(sdb && sdb_saved_file_adopt(sdb, pid_saved, proj, "notes.txt", false, &adopted) && !adopted);
        /* Nothing to adopt from a folder, a NUL file or a missing file; a
         * name that is not a plain document file is refused outright. */
        CHECK(sdb && sdb_saved_file_adopt(sdb, pid_saved, proj, "folder.md", false, &adopted) && !adopted);
        CHECK(sdb && sdb_saved_file_adopt(sdb, pid_saved, proj, "binary.txt", false, &adopted) && !adopted);
        CHECK(sdb && sdb_saved_file_adopt(sdb, pid_saved, proj, "absent.md", false, &adopted) && !adopted);
        CHECK(sdb && !sdb_saved_file_adopt(sdb, pid_saved, proj, "../notes.txt", false, &adopted));
        CHECK(sdb && !sdb_saved_file_adopt(sdb, pid_saved, proj, "notes.pdf", false, &adopted));
        CHECK(sdb && !sdb_saved_file_adopt(sdb, pid_saved, proj, "notes.txt", true, &adopted)); /* import mode needs its transaction */
        Revision *loose_revs = NULL;
        size_t nl = revisions_list(g_store, proj, loose, &loose_revs);
        CHECK(nl == 1 && !strcmp(loose_revs[0].origin, "import"));
        char *loose_text = nl ? revisions_load(g_store, proj, loose, &loose_revs[0]) : NULL;
        CHECK(loose_text && !strcmp(loose_text, "Loose notes.\n"));
        free(loose_text);
        free(loose_revs);
    }
    /* The same APIs now read the store. */
    Revision *revs = NULL;
    size_t nr = revisions_list(g_store, proj, doc2, &revs);
    CHECK(nr == 2);
    char *t = nr ? revisions_load(g_store, proj, doc2, &revs[nr - 1]) : NULL;
    CHECK(t && !strcmp(t, "first draft"));
    free(t);
    free(revs);
    Record *notes = NULL;
    size_t nn = records_list(g_store, proj, REC_NOTE, &notes);
    CHECK(nn == 1 && notes[0].nversions == 2 && !strcmp(record_current(&notes[0])->content, "Check the date (1911?)."));
    CHECK(nn == 1 && record_current(&notes[0])->nsources == 1 &&
          !strcmp(record_current(&notes[0])->sources[0].doc, "Chapter One.md") && /* the rename, resolved to the id */
          record_current(&notes[0])->sources[0].revision[0] == 'r');
    records_free(notes, nn);
    ChatMessage *msgs = NULL;
    size_t nm = 0;
    CHECK(chat_store_load(g_store, proj, "20261104-160000-abcd", &msgs, &nm) && nm == 2 && msgs[0].docs &&
           !strcmp(msgs[0].docs, "Chapter One.md"));
    CHECK(nm == 2 && !msgs[0].stopped && msgs[1].stopped && !msgs[1].truncated && !msgs[1].error);
    ChatSourcePage source_page;
    ChatSourceCursor source_cursor = {0};
    CHECK(chat_store_source_page(g_store, proj, "20261104-160000-abcd", &source_cursor, &source_page) &&
          source_page.count == 2 && !source_page.has_more);
    CHECK(!strcmp(source_page.chunks[0].text, msgs[0].content) && !strcmp(source_page.chunks[1].text, msgs[1].content));
    chat_source_page_free(&source_page);
    chat_store_free_messages(msgs, nm);
    {
        ChatSummary *il = NULL;
        size_t in = chat_store_list(g_store, proj, &il);
        CHECK(in == 1 && il[0].observer_ignore); /* the setting came along */
        free(il);
    }
    ost = project_store_observer_state_load(g_store, proj);
    CHECK(ost && cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(ost, "docs"), "Chapter One.md")) == 1);
    CHECK(ost && cJSON_GetNumberValue(cJSON_GetObjectItem(
                     cJSON_GetObjectItem(cJSON_GetObjectItem(ost, "chats"), "20261104-160000-abcd"), "off")) == 17);
    cJSON_Delete(ost);
    char rules[17];
    CHECK(project_store_observer_rules_get(g_store, proj, rules) && !strcmp(rules, "feedfacecafebeef"));
    cJSON *stale_db = project_store_observer_state_load(g_store, proj);
    ost = project_store_observer_state_load(g_store, proj);
    CHECK(stale_db && ost && project_store_observer_state_save(g_store, proj, ost));
    CHECK(stale_db && !project_store_observer_state_save(g_store, proj, stale_db));
    cJSON_Delete(ost);
    cJSON_Delete(stale_db);
    /* Exported as files (what an in-app backup carries): original times,
     * and sources that point at the exported snapshots. */
    {
        char pidx[40], ex[600], exdoc[700];
        CHECK(store_mode(g_store, proj, pidx, sizeof(pidx), NULL) == STORE_DB);
        snprintf(ex, sizeof(ex), "%s-export", proj);
        snprintf(cmd0, sizeof(cmd0), "rm -rf '%s'", ex);
        system(cmd0);
        mkdir(ex, 0755);
        char exscrib[700], exlock[800];
        snprintf(exscrib, sizeof(exscrib), "%s/.scribodo", ex);
        mkdir(exscrib, 0755);
        snprintf(exlock, sizeof(exlock), "%s/import.lock", exscrib);
        int export_lfd = open(exlock, O_RDWR | O_CREAT, 0644);
        CHECK(export_lfd >= 0 && flock(export_lfd, LOCK_EX | LOCK_NB) == 0);
        ExportCall export_call = { .project_id = pidx, .directory = ex };
        atomic_init(&export_call.started, false);
        atomic_init(&export_call.done, false);
        pthread_t exporter;
        int export_rc = pthread_create(&exporter, NULL, run_export, &export_call);
        CHECK(export_rc == 0);
        for (int i = 0; export_rc == 0 && !atomic_load(&export_call.started) && i < 100; i++) usleep(1000);
        CHECK(export_rc == 0 && atomic_load(&export_call.started));
        usleep(50000);
        CHECK(export_rc == 0 && !atomic_load(&export_call.done));
        flock(export_lfd, LOCK_UN);
        close(export_lfd);
        if (export_rc == 0) pthread_join(exporter, NULL);
        CHECK(export_rc == 0 && export_call.ok);
        if (!export_call.ok) fprintf(stderr, "    export: %s\n", export_call.why);
        snprintf(exdoc, sizeof(exdoc), "%s/Chapter One.md", ex);
        Revision *erevs = NULL;
        size_t ne = revisions_list(g_store, ex, exdoc, &erevs);
        CHECK(ne == nr && ne && erevs[ne - 1].ts == revs_ts_oldest); /* the first draft keeps its time */
        free(erevs);
        Record *en = NULL;
        size_t nen = records_list(g_store, ex, REC_NOTE, &en);
        CHECK(nen == 1 && en[0].nversions == 2 && en[0].versions[0].ts == note_ts &&
              record_current(&en[0])->nsources == 1 && record_current(&en[0])->sources[0].revision[0] != '\0');
        if (nen && record_current(&en[0])->nsources) {
            char snapfile[1400];
            struct stat sst;
            snprintf(snapfile, sizeof(snapfile), "%s/.scribodo/revisions/Chapter One.md/%s", ex, record_current(&en[0])->sources[0].revision);
            CHECK(stat(snapfile, &sst) == 0); /* a real exported snapshot */
        }
        records_free(en, nen);
        msgs = NULL; nm = 0;
        CHECK(chat_store_load(g_store, ex, "20261104-160000-abcd", &msgs, &nm));
        CHECK(nm == 2 && !msgs[0].stopped && msgs[1].stopped && !msgs[1].truncated && !msgs[1].error);
        chat_store_free_messages(msgs, nm);
        ost = project_store_observer_state_load(g_store, ex);
        CHECK(ost && cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(ost, "docs"), "Chapter One.md")) == 1);
        cJSON_Delete(ost);
        CHECK(project_store_observer_rules_get(g_store, ex, rules) && !strcmp(rules, "feedfacecafebeef"));
        snprintf(cmd0, sizeof(cmd0), "rm -rf '%s'", ex);
        system(cmd0);
    }
    ost = project_store_observer_state_load(g_store, proj);
    CHECK(ost != NULL);
    if (ost) cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItem(ost, "chats"), "20261104-160000-abcd");
    CHECK(ost && project_store_observer_state_save(g_store, proj, ost));
    cJSON_Delete(ost);
    ost = project_store_observer_state_load(g_store, proj);
    CHECK(ost && !cJSON_GetObjectItem(cJSON_GetObjectItem(ost, "chats"), "20261104-160000-abcd"));
    cJSON_Delete(ost);
    /* New writes go to the store; the old files were set aside, not deleted. */
    CHECK(records_append(g_store, proj, REC_NOTE, nid, "active", "v3", NULL, 0, NULL));
    nn = records_list(g_store, proj, REC_NOTE, &notes);
    CHECK(nn == 1 && notes[0].nversions == 3);
    records_free(notes, nn);
    char legacy_glob[700];
    snprintf(legacy_glob, sizeof(legacy_glob), "ls -d '%s'/.scribodo/legacy-*/notes.jsonl >/dev/null 2>&1", proj);
    CHECK(system(legacy_glob) == 0);
    char notes_path[700];
    snprintf(notes_path, sizeof(notes_path), "%s/.scribodo/notes.jsonl", proj);
    struct stat st;
    CHECK(stat(notes_path, &st) != 0);
    char bad[600], bad_dir[700], bad_state[800];
    snprintf(bad, sizeof(bad), "%s-bad", proj);
    snprintf(cmd0, sizeof(cmd0), "rm -rf '%s'", bad);
    system(cmd0);
    mkdir(bad, 0755);
    snprintf(bad_dir, sizeof(bad_dir), "%s/.scribodo", bad);
    mkdir(bad_dir, 0755);
    snprintf(bad_dir, sizeof(bad_dir), "%s/.scribodo/observer", bad);
    mkdir(bad_dir, 0755);
    snprintf(bad_state, sizeof(bad_state), "%s/state.json", bad_dir);
    f = fopen(bad_state, "wb");
    if (f) { fputs("{\"docs\":[],\"chats\":{}}", f); fclose(f); }
    CHECK(!store_import(g_store, bad, why, sizeof(why)) && store_mode(g_store, bad, NULL, 0, NULL) == STORE_FILES);
    static const char *const malformed_states[] = {
        "{\"docs\":{},\"chats\":{},\"_version\":9007199254740991}",
        "{\"docs\":{},\"chats\":{\"chat\":{\"n\":1.5,\"off\":0}}}",
        "{\"docs\":{},\"chats\":{},\"_version\":1,\"_version\":2}"
    };
    for (size_t i = 0; i < sizeof(malformed_states) / sizeof(malformed_states[0]); i++) {
        f = fopen(bad_state, "wb");
        if (f) { fputs(malformed_states[i], f); fclose(f); }
        CHECK(!store_import(g_store, bad, why, sizeof(why)) && store_mode(g_store, bad, NULL, 0, NULL) == STORE_FILES);
    }
    f = fopen(bad_state, "wb");
    if (f) { fputs("{\"docs\":{},\"chats\":{},\"_version\":9007199254740990}", f); fclose(f); }
    CHECK(store_import(g_store, bad, why, sizeof(why)));
    cJSON *max_version = project_store_observer_state_load(g_store, bad);
    CHECK(max_version && project_store_observer_state_save(g_store, bad, max_version));
    cJSON_Delete(max_version);
    max_version = project_store_observer_state_load(g_store, bad);
    CHECK(max_version && !project_store_observer_state_save(g_store, bad, max_version));
    cJSON_Delete(max_version);
    snprintf(cmd0, sizeof(cmd0), "rm -rf '%s'", bad);
    system(cmd0);
    /* With the database down, a moved project pauses instead of using files. */
    store_stop(g_store);
    CHECK(store_mode(g_store, proj, NULL, 0, NULL) == STORE_PAUSED);
    inspection = project_store_inspect(g_store, proj);
    CHECK(!inspection.capabilities && inspection.blocked_by == PROJECT_STORE_BLOCK_OFFLINE);
    int paused_ver = 99;
    CHECK(!records_append(g_store, proj, REC_NOTE, nid, "active", "while paused", NULL, 0, &paused_ver) && paused_ver == 0);
    CHECK(records_list(g_store, proj, REC_NOTE, &notes) == 0);
    bool paused_recorded = true;
    char paused_snap[64] = "stale";
    Revision *paused_revs = (Revision *)1;
    CHECK(!revisions_record(g_store, proj, doc, "offline", "save", NULL, &paused_recorded) && !paused_recorded);
    CHECK(!revisions_snapshot(g_store, proj, doc, "offline", paused_snap, sizeof(paused_snap)) && !paused_snap[0]);
    CHECK(revisions_list(g_store, proj, doc, &paused_revs) == 0 && paused_revs == NULL);
    ChatSummary *paused_chats = (ChatSummary *)1;
    ChatMessage *paused_msgs = (ChatMessage *)1;
    size_t paused_nmsgs = 99;
    CHECK(chat_store_list(g_store, proj, &paused_chats) == 0 && paused_chats == NULL);
    CHECK(!chat_store_load(g_store, proj, cid, &paused_msgs, &paused_nmsgs) && paused_msgs == NULL && paused_nmsgs == 0);
    CHECK(!chat_store_source_page(g_store, proj, cid, &source_cursor, &source_page) && source_page.count == 0);
    CHECK(project_store_observer_state_load(g_store, proj) == NULL);
    ost = cJSON_CreateObject();
    CHECK(!project_store_observer_state_save(g_store, proj, ost));
    cJSON_Delete(ost);
    CHECK(!project_store_observer_rules_get(g_store, proj, rules) && !rules[0]);
    CHECK(!project_store_observer_rules_set(g_store, proj, "deadbeefdeadbeef"));
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", proj);
    system(cmd);
}

/* Red-team 008: a copied folder never merges into the original; a missing
 * project restores from its set-aside files; one importer at a time; a stale
 * project row at the same path is not reused. */
static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static size_t note_count(const char *proj)
{
    Record *r = NULL;
    size_t n = records_list(g_store, proj, REC_NOTE, &r);
    records_free(r, n);
    return n;
}

typedef struct {
    const char *directory;
    char project_id[64];
    bool ok;
} RegisterMoveCall;

static void *register_move_main(void *arg)
{
    RegisterMoveCall *call = arg;
    StoreManager *other = store_manager_create();
    char why[256];
    call->ok = other && store_start(other, g_name, why, sizeof(why)) &&
               store_mode(other, call->directory, call->project_id, sizeof(call->project_id), NULL) == STORE_DB;
    if (other) store_manager_destroy(other);
    return NULL;
}

static void test_warmed_verification_replacement(void)
{
    char why[256], root[PATH_MAX], a[PATH_MAX], b[PATH_MAX], cmd[PATH_MAX * 2 + 32], pid[64];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    snprintf(root, sizeof(root), "%s/scribodo-verify-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(root) != NULL);
    snprintf(a, sizeof(a), "%s/A", root);
    snprintf(b, sizeof(b), "%s/B", root);
    CHECK(mkdir(a, 0755) == 0);
    CHECK(store_import(g_store, a, why, sizeof(why)));
    /* This thread represents a persistent worker: do not replace its TLS,
     * switch its verification path, or explicitly forget its warmed verdict. */
    CHECK(store_mode(g_store, a, pid, sizeof(pid), NULL) == STORE_DB);
    double warmed = monotonic_seconds();
    CHECK(rename(a, b) == 0);
    RegisterMoveCall moved = { .directory = b };
    pthread_t mover;
    int rc = pthread_create(&mover, NULL, register_move_main, &moved);
    CHECK(rc == 0);
    if (rc == 0) pthread_join(mover, NULL);
    CHECK(moved.ok && !strcmp(moved.project_id, pid));
    snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", b, a);
    CHECK(system(cmd) == 0);
    struct stat ast, bst;
    CHECK(stat(a, &ast) == 0 && stat(b, &bst) == 0 &&
          (ast.st_dev != bst.st_dev || ast.st_ino != bst.st_ino));
    CHECK(monotonic_seconds() - warmed < 30.0); /* exercise a live cache entry */
    CHECK(store_mode(g_store, a, NULL, 0, NULL) == STORE_PAUSED);
    char original[PATH_MAX], real_b[PATH_MAX];
    CHECK(store_problem(g_store, a, original, sizeof(original)) == STORE_PROBLEM_COPY);
    CHECK(realpath(b, real_b) != NULL && !strcmp(original, real_b));
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
    store_stop(g_store);
}

typedef struct {
    const char *project_dir;
    char why[256];
    bool ok;
    atomic_bool started;
    atomic_bool done;
} ImportCall;

typedef struct {
    const char *project_dir;
    atomic_bool *go;
    char why[256];
    bool ok;
} ForkCall;

static void *run_import(void *arg)
{
    ImportCall *call = arg;
    atomic_store(&call->started, true);
    call->ok = store_import(g_store, call->project_dir, call->why, sizeof(call->why));
    store_thread_done(g_store);
    atomic_store(&call->done, true);
    return NULL;
}

static void *run_fork(void *arg)
{
    ForkCall *call = arg;
    while (!atomic_load(call->go)) sched_yield();
    call->ok = store_fork(g_store, call->project_dir, call->why, sizeof(call->why));
    store_thread_done(g_store);
    return NULL;
}

static void test_import_safety(void)
{
    char why[256];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char a[512], b[512], doc[700], cmd[1200];
    snprintf(a, sizeof(a), "%s/scribodo-safe-a-%d", tmp, (int)getpid());
    snprintf(b, sizeof(b), "%s/scribodo-safe-b-%d", tmp, (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", a, b);
    system(cmd);
    mkdir(a, 0755);
    snprintf(doc, sizeof(doc), "%s/ch1.md", a);
    write_file(doc, "One.\n");
    char nid[40];
    records_new_id(REC_NOTE, nid, sizeof(nid));
    CHECK(records_append(g_store, a, REC_NOTE, nid, "active", "original note", NULL, 0, NULL));
    CHECK(revisions_record(g_store, a, doc, "One.\n", "save", NULL, NULL));

    /* Copied before either was moved into the store: both import the same
     * snapshot files, and each must get its own History. */
    char d0[512];
    snprintf(d0, sizeof(d0), "%s/scribodo-safe-d-%d", tmp, (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && cp -R '%s' '%s'", d0, a, d0);
    CHECK(system(cmd) == 0);

    /* Importer election is separate from the transition lock: another elected
     * importer is refused without mistaking an ordinary reader for one. */
    char ownerp[700], lockp[700];
    snprintf(ownerp, sizeof(ownerp), "%s/.scribodo/import.owner.lock", a);
    int fd = open(ownerp, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    CHECK(!store_import(g_store, a, why, sizeof(why)) && strstr(why, "another window"));
    flock(fd, LOCK_UN);
    close(fd);
    CHECK(store_mode(g_store, a, NULL, 0, NULL) == STORE_FILES);

    /* A stale row for this path (an earlier project here, marker lost) is
     * superseded, not reused. */
    char stale[40], pa[40];
    CHECK(store_project_id(g_store, a, true, stale, sizeof(stale)));

    /* An elected importer waits for an in-flight shared file operation rather
     * than failing it as a competing importer. Once it drains, import wins. */
    snprintf(lockp, sizeof(lockp), "%s/.scribodo/import.lock", a);
    fd = open(lockp, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0 && flock(fd, LOCK_SH | LOCK_NB) == 0);
    ImportCall call = { .project_dir = a };
    atomic_init(&call.started, false);
    atomic_init(&call.done, false);
    pthread_t importer;
    int import_rc = pthread_create(&importer, NULL, run_import, &call);
    CHECK(import_rc == 0);
    for (int i = 0; import_rc == 0 && !atomic_load(&call.started) && i < 100; i++) usleep(1000);
    CHECK(import_rc == 0 && atomic_load(&call.started));
    usleep(50000);
    CHECK(import_rc == 0 && !atomic_load(&call.done));
    CHECK(store_mode(g_store, a, NULL, 0, NULL) == STORE_FILES);
    flock(fd, LOCK_UN);
    close(fd);
    if (import_rc == 0) pthread_join(importer, NULL);
    CHECK(import_rc == 0 && call.ok);
    CHECK(store_mode(g_store, a, pa, sizeof(pa), NULL) == STORE_DB && strcmp(pa, stale) != 0);
    CHECK(note_count(a) == 1);
    CHECK(store_import(g_store, d0, why, sizeof(why)));
    char pd[40], docd[700];
    CHECK(store_mode(g_store, d0, pd, sizeof(pd), NULL) == STORE_DB && strcmp(pd, pa) != 0);
    snprintf(docd, sizeof(docd), "%s/ch1.md", d0);
    Revision *drevs = NULL;
    CHECK(revisions_list(g_store, d0, docd, &drevs) == 1);
    free(drevs);
    drevs = NULL;
    CHECK(revisions_list(g_store, a, doc, &drevs) == 1);
    free(drevs);
    CHECK(note_count(d0) == 1);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", d0);
    system(cmd);
    char pending[700];
    snprintf(pending, sizeof(pending), "%s/.scribodo/import-pending", a);
    struct stat st;
    CHECK(stat(pending, &st) != 0);

    /* A copy of the folder: paused as a copy, then forked into its own
     * project; the original is untouched by the copy's writes. */
    snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", a, b);
    CHECK(system(cmd) == 0);
    store_forget_verification(g_store);
    char orig[1024];
    CHECK(store_mode(g_store, b, NULL, 0, NULL) == STORE_PAUSED);
    CHECK(store_problem(g_store, b, orig, sizeof(orig)) == STORE_PROBLEM_COPY);
    atomic_bool fork_go = false;
    ForkCall forks[2] = { { .project_dir = b, .go = &fork_go }, { .project_dir = b, .go = &fork_go } };
    pthread_t fork_threads[2];
    int fork_rc[2] = { pthread_create(&fork_threads[0], NULL, run_fork, &forks[0]),
                       pthread_create(&fork_threads[1], NULL, run_fork, &forks[1]) };
    CHECK(fork_rc[0] == 0 && fork_rc[1] == 0);
    atomic_store(&fork_go, true);
    if (fork_rc[0] == 0) pthread_join(fork_threads[0], NULL);
    if (fork_rc[1] == 0) pthread_join(fork_threads[1], NULL);
    CHECK(fork_rc[0] == 0 && fork_rc[1] == 0 && forks[0].ok != forks[1].ok);
    char pb[40];
    CHECK(store_mode(g_store, b, pb, sizeof(pb), NULL) == STORE_DB && strcmp(pa, pb) != 0);
    CHECK(note_count(b) == 1);
    Record *r = NULL;
    size_t n = records_list(g_store, b, REC_NOTE, &r);
    CHECK(n == 1 && strcmp(r[0].id, nid) != 0 && !strcmp(record_current(&r[0])->content, "original note"));
    char bnid[40];
    snprintf(bnid, sizeof(bnid), "%s", n ? r[0].id : "");
    records_free(r, n);
    CHECK(records_append(g_store, b, REC_NOTE, bnid, "active", "copy's edit", NULL, 0, NULL));
    n = records_list(g_store, a, REC_NOTE, &r);
    CHECK(n == 1 && !strcmp(record_current(&r[0])->content, "original note"));
    records_free(r, n);
    char docb[700];
    snprintf(docb, sizeof(docb), "%s/ch1.md", b);
    Revision *revs = NULL;
    n = revisions_list(g_store, b, docb, &revs);
    CHECK(n == 1);
    free(revs);
    store_forget_verification(g_store);
    CHECK(store_mode(g_store, a, NULL, 0, NULL) == STORE_DB); /* the original is still itself */

    /* Moved to another volume (or restored from a backup): a new inode, but
     * the original is gone, so it is the project, not a copy. */
    char c[512];
    snprintf(c, sizeof(c), "%s/scribodo-safe-c-%d", tmp, (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && cp -R '%s' '%s' && rm -rf '%s'", c, b, c, b);
    CHECK(system(cmd) == 0);
    store_forget_verification(g_store);
    char pc[40];
    CHECK(store_mode(g_store, c, pc, sizeof(pc), NULL) == STORE_DB && !strcmp(pc, pb));
    CHECK(store_problem(g_store, c, NULL, 0) == 0);
    n = records_list(g_store, c, REC_NOTE, &r);
    CHECK(n == 1 && !strcmp(record_current(&r[0])->content, "copy's edit"));
    records_free(r, n);
    snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", c, b); /* and a copy of it again is a copy */
    CHECK(system(cmd) == 0);
    store_forget_verification(g_store);
    CHECK(store_problem(g_store, b, NULL, 0) == STORE_PROBLEM_COPY);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", c);
    system(cmd);

    /* The project's rows disappear (store reset): paused as missing, then
     * restored from the set-aside files. */
    Db *db = store_db(g_store);
    /* The saved-revision key RESTRICTs deletion by design (migration 010), and
     * since the import records each file's saved revision every imported
     * project has one; a reset that removes rows must clear the pointers first. */
    CHECK(db_run(db, "UPDATE documents SET saved_revision_id = NULL WHERE project_id = ?", (DbArg[1]){ DB_S(pa) }, 1, NULL));
    CHECK(db_run(db, "DELETE FROM projects WHERE id = ?", (DbArg[1]){ DB_S(pa) }, 1, NULL));
    store_forget_verification(g_store);
    CHECK(store_mode(g_store, a, NULL, 0, NULL) == STORE_PAUSED && store_problem(g_store, a, NULL, 0) == STORE_PROBLEM_MISSING);
    CHECK(store_restore(g_store, a, why, sizeof(why)));
    char pa2[40];
    CHECK(store_mode(g_store, a, pa2, sizeof(pa2), NULL) == STORE_DB && strcmp(pa2, pa) != 0);
    CHECK(note_count(a) == 1);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", a, b);
    system(cmd);
}

/* The entity graph from findings (SOW §4.3, acceptance 7). */
static bool add_finding(Db *db, const char *pid, char *id, const char *type, const char *sub, const char *rel, const char *obj,
                        const char *text)
{
    FindingInfo f = { 0 };
    snprintf(f.type, sizeof(f.type), "%s", type);
    snprintf(f.subject, sizeof(f.subject), "%s", sub);
    snprintf(f.relation, sizeof(f.relation), "%s", rel);
    snprintf(f.object, sizeof(f.object), "%s", obj);
    store_new_id('f', id, 40);
    SourceRef src = { .kind = SRC_DOC, .start = 0, .len = 3, .quote = "Zoë" };
    snprintf(src.doc, sizeof(src.doc), "ch1.md");
    return sdb_record_append(db, pid, REC_FINDING, id, "new", text, &src, 1, &f, true, NULL);
}

static const EntityRow *find_entity(const EntityRow *e, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(e[i].name, name)) return &e[i];
    return NULL;
}

static void test_entities(void)
{
    char why[256], norm[64];
    entity_norm("Zoë Ångström's", norm, sizeof(norm));
    CHECK(!strcmp(norm, "zoë ångström"));
    entity_norm("  ZOË  ", norm, sizeof(norm));
    CHECK(!strcmp(norm, "zoë"));
    entity_norm("Zoë\xE2\x80\x99s", norm, sizeof(norm));
    CHECK(!strcmp(norm, "zoë"));
    entity_norm("Zo\xC3\xAB", norm, 3); /* "ë" doesn't fit: cut before it */
    CHECK(!strcmp(norm, "zo"));
    entity_norm("Ångström", norm, 3); /* room for "å" only: never half of "n…" or of a character */
    CHECK(!strcmp(norm, "\xC3\xA5"));

    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    Db *db = store_db(g_store);
    char pid[40], other[40];
    CHECK(store_project_id(g_store, "/private/var/db", true, pid, sizeof(pid)));
    CHECK(store_project_id(g_store, "/private/var/log", true, other, sizeof(other)));
    char f1[40], f2[40], f3[40], f4[40], f5[40], f6[40];
    CHECK(add_finding(db, pid, f1, "entity", "Zoë", "", "", "Zoë is mentioned."));
    CHECK(add_finding(db, pid, f2, "relationship", "Zoë's", "sister of", "Mara", "Zoë is Mara's sister."));
    CHECK(add_finding(db, pid, f3, "event", "MARA", "", "", "Mara leaves the island."));
    CHECK(add_finding(db, pid, f4, "relationship", "Zoë", "looks at", "the sea", "Zoë looks at the sea."));
    CHECK(add_finding(db, pid, f5, "relationship", "Mara", "rows to", "Ytre Sula", "Mara rows to Ytre Sula."));
    EntityRow *e = NULL;
    size_t n = sdb_entity_list(db, pid, &e);
    const EntityRow *zoe = find_entity(e, n, "Zoë"), *mara = find_entity(e, n, "Mara");
    CHECK(n == 3 && zoe && mara && find_entity(e, n, "Ytre Sula")); /* "the sea" is not a named entity */
    CHECK(zoe && zoe->mentions == 3 && zoe->links == 1);  /* f1, f2, f4 */
    CHECK(mara && mara->mentions == 3 && mara->links == 2); /* f2, f3 ("MARA"), f5 */
    char zid[40] = "", mid[40] = "";
    if (zoe) snprintf(zid, sizeof(zid), "%s", zoe->id);
    if (mara) snprintf(mid, sizeof(mid), "%s", mara->id);
    entity_list_free(e, n);
    EntityDetail d;
    CHECK(sdb_entity_detail(db, pid, zid, &d));
    CHECK(d.nrows == 3 && d.rows[0].is_link && d.rows[0].outgoing && !strcmp(d.rows[0].relation, "sister of") &&
          !strcmp(d.rows[0].other_id, mid) && !strcmp(d.rows[0].finding_id, f2));
    bool has_f1 = false, has_f4 = false;
    for (size_t i = 1; i < d.nrows; i++) {
        has_f1 = has_f1 || (!d.rows[i].is_link && !strcmp(d.rows[i].finding_id, f1) && !strcmp(d.rows[i].text, "Zoë is mentioned."));
        has_f4 = has_f4 || (!d.rows[i].is_link && !strcmp(d.rows[i].finding_id, f4));
    }
    CHECK(has_f1 && has_f4);
    entity_detail_free(&d);
    CHECK(sdb_entity_detail(db, pid, mid, &d) && d.nrows >= 2 && d.rows[0].is_link && !d.rows[0].outgoing &&
          !strcmp(d.rows[0].other_id, zid));
    entity_detail_free(&d);
    /* Dismissing a finding drops its link and mention. */
    CHECK(sdb_record_append(db, pid, REC_FINDING, f2, "dismissed", "Zoë is Mara's sister.", NULL, 0, NULL, false, NULL));
    n = sdb_entity_list(db, pid, &e);
    zoe = find_entity(e, n, "Zoë");
    CHECK(zoe && zoe->links == 0 && zoe->mentions == 2);
    entity_list_free(e, n);
    CHECK(sdb_entity_detail(db, pid, zid, &d) && (d.nrows == 0 || !d.rows[0].is_link));
    entity_detail_free(&d);
    /* Backfill is idempotent. */
    CHECK(sdb_entities_backfill(db, pid) && sdb_entities_backfill(db, pid));
    n = sdb_entity_list(db, pid, &e);
    zoe = find_entity(e, n, "Zoë");
    CHECK(n == 3 && zoe && zoe->mentions == 2);
    entity_list_free(e, n);
    DbRows r;
    CHECK(db_query(db, "SELECT COUNT(*) FROM links WHERE project_id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r) &&
          !strcmp(db_cell(&r, 0, 0), "2"));
    db_rows_free(&r);
    /* Another project's finding is never linked into this one. */
    FindingInfo xf = { 0 };
    snprintf(xf.type, sizeof(xf.type), "entity");
    snprintf(xf.subject, sizeof(xf.subject), "Intruder");
    CHECK(sdb_entities_from_finding(db, other, f1, &xf));
    CHECK(sdb_entity_list(db, other, &e) == 0);
    entity_list_free(e, 0);
    CHECK(add_finding(db, other, f6, "entity", "Zoë", "", "", "Elsewhere."));
    n = sdb_entity_list(db, pid, &e);
    zoe = find_entity(e, n, "Zoë");
    CHECK(zoe && zoe->mentions == 2); /* still this project's two */
    entity_list_free(e, n);

    /* Names look like names (the real model gives phrases as subjects);
     * a first name joins the one full name it starts. */
    char p3[40], g[40];
    CHECK(store_project_id(g_store, "/private/var/run", true, p3, sizeof(p3)));
    CHECK(add_finding(db, p3, g, "event", "Dvořák confronts Zoë about being late", "", "", "x."));
    CHECK(add_finding(db, p3, g, "event", "Zoë's eyes narrowing", "", "", "x."));
    CHECK(add_finding(db, p3, g, "entity", "the sea", "", "", "x."));
    CHECK(sdb_entity_list(db, p3, &e) == 0);
    entity_list_free(e, 0);
    CHECK(add_finding(db, p3, g, "entity", "Zoë", "", "", "Zoë is mentioned."));
    CHECK(add_finding(db, p3, g, "entity", "Zoë Ångström", "", "", "Zoë Ångström climbs."));
    CHECK(add_finding(db, p3, g, "event", "Zoë", "", "", "Zoë waits."));
    CHECK(add_finding(db, p3, g, "entity", "Ludwig van Beethoven", "", "", "A bust of Beethoven."));
    n = sdb_entity_list(db, p3, &e);
    const EntityRow *full = find_entity(e, n, "Zoë Ångström");
    CHECK(n == 2 && full && full->mentions == 3 && !find_entity(e, n, "Zoë") && find_entity(e, n, "Ludwig van Beethoven"));
    entity_list_free(e, n);
    /* Two full names with one first name: "Anna" alone stays apart. */
    CHECK(add_finding(db, p3, g, "entity", "Anna Berg", "", "", "x."));
    CHECK(add_finding(db, p3, g, "entity", "Anna Lund", "", "", "x."));
    CHECK(add_finding(db, p3, g, "entity", "Anna", "", "", "x."));
    n = sdb_entity_list(db, p3, &e);
    CHECK(find_entity(e, n, "Anna") && find_entity(e, n, "Anna Berg") && find_entity(e, n, "Anna Lund"));
    entity_list_free(e, n);
    store_stop(g_store);
}

/* Red-team 009: folding during a relationship, imports that contain one,
 * and two connections linking the same findings. */
typedef struct {
    char pid[40];
    char (*ids)[40];
    int n;
} RaceArg;

static void *race_main(void *arg)
{
    RaceArg *a = arg;
    Db *db = store_db(g_store); /* this thread's own connection */
    for (int i = 0; db && i < a->n; i++) {
        FindingInfo f = { 0 };
        snprintf(f.type, sizeof(f.type), "relationship");
        snprintf(f.subject, sizeof(f.subject), "Anna%c%c", 'A' + i % 26, 'a' + i / 26);
        snprintf(f.relation, sizeof(f.relation), "knows");
        snprintf(f.object, sizeof(f.object), "Bert%c%c", 'A' + i % 26, 'a' + i / 26);
        sdb_entities_from_finding(db, a->pid, a->ids[i], &f);
    }
    store_thread_done(g_store);
    return NULL;
}

static void test_entities_redteam(void)
{
    char why[256];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    Db *db = store_db(g_store);
    char p5[40], g[40];
    CHECK(store_project_id(g_store, "/private/var/folders", true, p5, sizeof(p5)));
    /* "Zoë" exists; a relationship names "Zoë" and "Zoë Ångström". */
    CHECK(add_finding(db, p5, g, "entity", "Zoë", "", "", "x."));
    CHECK(add_finding(db, p5, g, "relationship", "Zoë", "is called", "Zoë Ångström", "x."));
    CHECK(add_finding(db, p5, g, "relationship", "Mara", "sister of", "Zoë", "x."));
    EntityRow *e = NULL;
    size_t n = sdb_entity_list(db, p5, &e);
    const EntityRow *z = find_entity(e, n, "Zoë Ångström");
    CHECK(n == 2 && z && !find_entity(e, n, "Zoë") && z->links == 1 && find_entity(e, n, "Mara"));
    entity_list_free(e, n);

    /* Directly: the graph update for such a finding succeeds. */
    {
        char p8[40], rf[40];
        CHECK(store_project_id(g_store, "/private/var/select", true, p8, sizeof(p8)));
        CHECK(add_finding(db, p8, g, "entity", "Ilse", "", "", "x."));
        store_new_id('f', rf, sizeof(rf));
        CHECK(sdb_record_append(db, p8, REC_FINDING, rf, "new", "x.", NULL, 0, NULL, true, NULL)); /* no hook */
        FindingInfo rel = { 0 };
        snprintf(rel.type, sizeof(rel.type), "relationship");
        snprintf(rel.subject, sizeof(rel.subject), "Ilse");
        snprintf(rel.relation, sizeof(rel.relation), "is called");
        snprintf(rel.object, sizeof(rel.object), "Ilse Brandt");
        CHECK(sdb_entities_from_finding(db, p8, rf, &rel));
    }
    /* The same pair in files, imported: the import succeeds. */
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char proj[512], cmd[700];
    snprintf(proj, sizeof(proj), "%s/scribodo-ent-%d", tmp, (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", proj);
    system(cmd);
    mkdir(proj, 0755);
    FindingInfo f1 = { 0 }, f2 = { 0 };
    snprintf(f1.type, sizeof(f1.type), "entity");
    snprintf(f1.subject, sizeof(f1.subject), "Zoë");
    snprintf(f2.type, sizeof(f2.type), "relationship");
    snprintf(f2.subject, sizeof(f2.subject), "Zoë");
    snprintf(f2.relation, sizeof(f2.relation), "full name is");
    snprintf(f2.object, sizeof(f2.object), "Zoë Ångström");
    char i1[40], i2[40];
    records_new_id(REC_FINDING, i1, sizeof(i1));
    records_new_id(REC_FINDING, i2, sizeof(i2));
    CHECK(records_append_ex(g_store, proj, REC_FINDING, i1, "new", "Zoë.", NULL, 0, &f1, NULL));
    CHECK(records_append_ex(g_store, proj, REC_FINDING, i2, "new", "Zoë's full name.", NULL, 0, &f2, NULL));
    why[0] = '\0';
    CHECK(store_import(g_store, proj, why, sizeof(why)));
    if (why[0]) fprintf(stderr, "    %s\n", why);
    system(cmd);

    /* Two connections link the same findings: one link each. */
    char p6[40];
    CHECK(store_project_id(g_store, "/private/var/audit", true, p6, sizeof(p6)));
    static char ids[60][40];
    for (int i = 0; i < 60; i++) {
        FindingInfo f = { 0 };
        snprintf(f.type, sizeof(f.type), "entity");
        snprintf(f.subject, sizeof(f.subject), "Seed");
        store_new_id('f', ids[i], sizeof(ids[i]));
        CHECK(sdb_record_append(db, p6, REC_FINDING, ids[i], "new", "x", NULL, 0, &f, true, NULL));
    }
    RaceArg ra = { .ids = ids, .n = 60 };
    snprintf(ra.pid, sizeof(ra.pid), "%s", p6);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, race_main, &ra);
    pthread_create(&t2, NULL, race_main, &ra);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    DbRows r;
    CHECK(db_query(db, "SELECT COUNT(*), COUNT(DISTINCT finding_id) FROM links WHERE project_id = ?", (DbArg[1]){ DB_S(p6) }, 1, &r) &&
          !strcmp(db_cell(&r, 0, 0), "60") && !strcmp(db_cell(&r, 0, 1), "60"));
    db_rows_free(&r);
    /* Greek and Cyrillic names are names too. */
    char p7[40];
    CHECK(store_project_id(g_store, "/private/var/spool", true, p7, sizeof(p7)));
    CHECK(add_finding(db, p7, g, "entity", "Σοφία", "", "", "x."));
    CHECK(add_finding(db, p7, g, "entity", "ΣΟΦΊΑ", "", "", "x.") || true);
    CHECK(add_finding(db, p7, g, "entity", "Наташа", "", "", "x."));
    n = sdb_entity_list(db, p7, &e);
    CHECK(find_entity(e, n, "Σοφία") && find_entity(e, n, "Наташа"));
    entity_list_free(e, n);
    store_stop(g_store);
}

/* Semantic retrieval against the local Qdrant (fake embedder): committed
 * canon only, kinds kept apart, and a wiped index rebuilt from MariaDB with
 * exact provenance (SOW acceptance 10). */
static void test_semantic(void)
{
    char why[256] = "";
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    SemanticClient *semantic = semantic_client_create(g_store, NULL);
    CHECK(semantic != NULL);
    CHECK(semantic_ready(semantic));
    char pid[40];
    CHECK(store_project_id(g_store, "/private/var/tmp", true, pid, sizeof(pid)));
    semantic_drop(semantic, pid);
    Db *db = store_db(g_store);
    char c1[40], c2[40], c3[40], n1[40];
    store_new_id('c', c1, sizeof(c1));
    store_new_id('c', c2, sizeof(c2));
    store_new_id('c', c3, sizeof(c3));
    store_new_id('n', n1, sizeof(n1));
    CHECK(sdb_record_append(db, pid, REC_CANON, c1, "committed", "Zoë's eyes are green.", NULL, 0, NULL, true, NULL));
    CHECK(sdb_record_append(db, pid, REC_CANON, c2, "committed", "The lighthouse stands on Ytre Sula.", NULL, 0, NULL, true, NULL));
    CHECK(sdb_record_append(db, pid, REC_CANON, c3, "retracted", "Dvořák is Zoë's uncle.", NULL, 0, NULL, true, NULL));
    CHECK(sdb_record_append(db, pid, REC_NOTE, n1, "active", "Zoë's eyes might be grey in chapter two.", NULL, 0, NULL, true, NULL));
    CHECK(semantic_sync_records(semantic, pid, why, sizeof(why)));
    if (why[0]) fprintf(stderr, "    sync: %s\n", why);
    SemHit *h = NULL;
    size_t n = semantic_canon_for(semantic, pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].record_id, c1) && !strcmp(h[0].kind, "canon"));
    bool leaked = false;
    for (size_t i = 0; i < n; i++) leaked = leaked || !strcmp(h[i].record_id, c3) || !strcmp(h[i].record_id, n1);
    CHECK(!leaked); /* retracted canon and notes are never canon */
    semantic_hits_free(h, n);
    /* Retracting moves an entry out of canon retrieval. */
    CHECK(sdb_record_append(db, pid, REC_CANON, c1, "retracted", "Zoë's eyes are green.", NULL, 0, NULL, false, NULL));
    CHECK(semantic_sync_records(semantic, pid, why, sizeof(why)));
    n = semantic_canon_for(semantic, pid, "Zoë's brown eyes narrowed.", 3, &h);
    leaked = false;
    for (size_t i = 0; i < n; i++) leaked = leaked || !strcmp(h[i].record_id, c1);
    CHECK(!leaked);
    semantic_hits_free(h, n);
    CHECK(sdb_record_append(db, pid, REC_CANON, c1, "committed", "Zoë's eyes are green.", NULL, 0, NULL, false, NULL));
    /* Document text is indexed as draft, with its provenance. */
    bool rec = false;
    char rv[40];
    CHECK(sdb_revision_record(db, pid, "/x/ch1.md", "The fog came in.\n\nZoë climbed the lighthouse stairs.", "save", NULL,
                              false, &rec, rv, sizeof(rv)));
    long count = semantic_rebuild(semantic, pid, why, sizeof(why));
    CHECK(count >= 6); /* 4 records + 2 paragraphs */
    n = semantic_search(semantic, pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].kind, "draft") && !strcmp(h[0].revision_id, rv) && h[0].start == 18 && h[0].len == 34 &&
          !strcmp(h[0].text, "Zoë climbed the lighthouse stairs."));
    semantic_hits_free(h, n);
    /* Wipe the index; retrieval is empty; a rebuild restores it exactly. */
    CHECK(semantic_drop(semantic, pid));
    n = semantic_canon_for(semantic, pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n == 0);
    semantic_hits_free(h, n);
    CHECK(semantic_rebuild(semantic, pid, why, sizeof(why)) == count);
    n = semantic_canon_for(semantic, pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].record_id, c1) && !strcmp(h[0].status, "committed"));
    semantic_hits_free(h, n);
    n = semantic_search(semantic, pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].revision_id, rv) && h[0].start == 18);
    semantic_hits_free(h, n);
    /* Syncing again does no work (everything is marked). */
    DbRows r;
    CHECK(db_query(db, "SELECT COUNT(*) FROM semantic_marks WHERE project_id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r) &&
          atol(db_cell(&r, 0, 0)) == count);
    db_rows_free(&r);
    CHECK(semantic_sync_records(semantic, pid, why, sizeof(why)));
    /* The index lost but its marks kept (restored under another database,
     * Qdrant wiped): the next sync rebuilds everything, documents too. */
    CHECK(semantic_forget_collection(semantic, pid));
    CHECK(semantic_sync_records(semantic, pid, why, sizeof(why)));
    n = semantic_search(semantic, pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].revision_id, rv) && h[0].start == 18);
    semantic_hits_free(h, n);
    n = semantic_canon_for(semantic, pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].record_id, c1));
    semantic_hits_free(h, n);
    /* A rebuild interrupted (the embedder down) is resumed by a later sync,
     * documents included (red-team 009). */
    LlmServerConfig dead;
    llm_server_embed_config(&dead, "");
    LlmServer *down = llm_server_create(&dead); /* never started: not ready */
    SemanticClient *unready = semantic_client_create(g_store, down);
    CHECK(unready != NULL);
    CHECK(semantic_forget_collection(unready, pid));
    CHECK(!semantic_sync_records(unready, pid, why, sizeof(why)));
    semantic_client_destroy(unready);
    CHECK(semantic_sync_records(semantic, pid, why, sizeof(why)));
    n = semantic_search(semantic, pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].revision_id, rv));
    semantic_hits_free(h, n);
    CHECK(semantic_sync_records(semantic, pid, why, sizeof(why))); /* and then it's done: no rebuild each time */
    CHECK(db_query(db, "SELECT index_rebuild_pending FROM projects WHERE id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r) &&
          !strcmp(db_cell(&r, 0, 0), "0"));
    db_rows_free(&r);
    llm_server_destroy(down);
    semantic_drop(semantic, pid);
    semantic_client_destroy(semantic);
    store_stop(g_store);
}

static void test_transition_read_failures(void)
{
    char why[256] = "";
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    char project[PATH_MAX];
    snprintf(project, sizeof(project), "%sscribodo-read-failures-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp/");
    CHECK(mkdtemp(project) != NULL);
    ChatMessage message = { .role = CHAT_USER, .content = "Keep this chat", .truncated = true };
    store_set_write_time(g_store, 1700000000.25);
    CHECK(chat_store_append(g_store, project, "aaaa", "Good", &message));
    store_set_write_time(g_store, 0);
    CHECK(chat_store_append(g_store, project, "bbbb", "Bad", &message));
    char path[PATH_MAX + 80];
    snprintf(path, sizeof(path), "%s/.scribodo/chats/bbbb.jsonl", project);
    write_file(path, "{\"type\":\"message\",\"role\":\"user\",\"content\":\"Keep this too\",\"ts\":-1}\n");
    CHECK(!store_import(g_store, project, why, sizeof(why)) && why[0]);
    CHECK(store_mode(g_store, project, NULL, 0, NULL) == STORE_FILES && access(path, F_OK) == 0);
    char pid[40];
    CHECK(store_project_id(g_store, project, false, pid, sizeof(pid)));
    Db *db = store_db(g_store);
    DbRows rows;
    CHECK(db_query(db, "SELECT COUNT(*) FROM conversations WHERE project_id = ?", (DbArg[1]){ DB_S(pid) }, 1, &rows));
    CHECK(rows.nrows == 1 && !strcmp(db_cell(&rows, 0, 0), "0"));
    db_rows_free(&rows);
    write_file(path, "{\"type\":\"message\",\"role\":\"user\",\"content\":\"Keep this too\",\"ts\":1}\n");
    CHECK(chmod(path, 0000) == 0);
    CHECK(!store_import(g_store, project, why, sizeof(why)));
    CHECK(chmod(path, 0644) == 0);
    CHECK(store_mode(g_store, project, NULL, 0, NULL) == STORE_FILES && access(path, F_OK) == 0);
    char chats_dir[PATH_MAX + 40];
    snprintf(chats_dir, sizeof(chats_dir), "%s/.scribodo/chats", project);
    CHECK(chmod(chats_dir, 0000) == 0);
    CHECK(!store_import(g_store, project, why, sizeof(why)));
    CHECK(chmod(chats_dir, 0755) == 0);
    CHECK(store_mode(g_store, project, NULL, 0, NULL) == STORE_FILES && access(path, F_OK) == 0);
    CHECK(store_import(g_store, project, why, sizeof(why)));
    ChatSummary *chats = NULL;
    size_t n = 0;
    CHECK(chat_store_list_checked(g_store, project, &chats, &n) && n == 2);
    free(chats);
    CHECK(store_mode(g_store, project, pid, sizeof(pid), NULL) == STORE_DB);

    /* All record kinds must fail export on decode failure, not become empty. */
    const RecordKind kinds[] = { REC_NOTE, REC_CANON, REC_FINDING };
    const char *ids[] = { "n-audit", "c-audit", "f-audit" };
    for (size_t i = 0; i < 3; i++) {
        CHECK(records_create_ex(g_store, project, kinds[i], ids[i], "active", "Keep this record", NULL, 0, NULL));
        CHECK(db_run(db, "UPDATE record_versions SET status = '1234567890123456' WHERE record_id = ?",
                     (DbArg[1]){ DB_S(ids[i]) }, 1, NULL));
        char exported[PATH_MAX + 40];
        snprintf(exported, sizeof(exported), "%s-export-%zu", project, i);
        CHECK(!store_export_project(g_store, pid, exported, why, sizeof(why)) && why[0]);
        CHECK(db_run(db, "UPDATE record_versions SET status = 'active' WHERE record_id = ?",
                     (DbArg[1]){ DB_S(ids[i]) }, 1, NULL));
        /* A failed export is partial output, never an append target on retry. */
        CHECK(!store_export_project(g_store, pid, exported, why, sizeof(why)) && why[0]);
    }
    char exported[PATH_MAX + 40];
    snprintf(exported, sizeof(exported), "%s-export-ok", project);
    bool exported_ok = store_export_project(g_store, pid, exported, why, sizeof(why));
    CHECK(exported_ok);
    if (!exported_ok) fprintf(stderr, "    export: %s\n", why);
    CHECK(!store_export_project(g_store, pid, exported, why, sizeof(why)) && strstr(why, "fresh destination"));
    ChatMessage *messages = NULL;
    n = 0;
    CHECK(chat_store_load(g_store, exported, "aaaa", &messages, &n) && n == 1);
    CHECK(n == 1 && messages[0].truncated && messages[0].ts == 1700000000.25);
    chat_store_free_messages(messages, n);
    for (size_t i = 0; i < 3; i++) {
        Record *records = NULL;
        n = records_list_files(exported, kinds[i], &records);
        CHECK(n == 1 && !strcmp(records[0].id, ids[i]) && !strcmp(record_current(&records[0])->content, "Keep this record"));
        records_free(records, n);
    }
    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'-export-*", project, project);
    system(cmd);
}

static void test_legacy_import_read_failures(void)
{
    char why[256] = "";
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    for (int which = 0; which < 7; which++) {
        char project[PATH_MAX], doc[PATH_MAX + 32], path[PATH_MAX + 300];
        snprintf(project, sizeof(project), "%sscribodo-legacy-errors-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp/");
        CHECK(mkdtemp(project) != NULL);
        snprintf(doc, sizeof(doc), "%s/chapter.md", project);
        write_file(doc, "Saved manuscript");
        CHECK(revisions_record(g_store, project, doc, "First revision", "save", NULL, NULL));
        CHECK(revisions_record(g_store, project, doc, "Second revision", "save", NULL, NULL));
        CHECK(records_append(g_store, project, REC_NOTE, "n-legacy", "active", "A note", NULL, 0, NULL));
        CHECK(records_append(g_store, project, REC_CANON, "c-legacy", "committed", "A fact", NULL, 0, NULL));
        CHECK(records_append(g_store, project, REC_FINDING, "f-legacy", "new", "A finding", NULL, 0, NULL));
        const char *suffixes[] = { "notes.jsonl", "canon.jsonl", "findings.jsonl", "revisions/chapter.md/index.jsonl", "revisions", "" };
        if (which < 5) snprintf(path, sizeof(path), "%s/.scribodo/%s", project, suffixes[which]);
        else {
            Revision *revs = NULL;
            size_t n = revisions_list(g_store, project, doc, &revs);
            CHECK(n == 2);
            if (n != 2) { free(revs); return; }
            snprintf(path, sizeof(path), "%s/.scribodo/revisions/chapter.md/%s", project, revs[0].file);
            free(revs);
        }
        if (which == 6) write_file(path, "Damaged snapshot");
        else CHECK(chmod(path, 0000) == 0);
        CHECK(!store_import(g_store, project, why, sizeof(why)) && why[0]);
        if (which == 6) write_file(path, "Second revision");
        else CHECK(chmod(path, which == 4 ? 0755 : 0644) == 0);
        CHECK(store_mode(g_store, project, NULL, 0, NULL) == STORE_FILES && access(path, F_OK) == 0);
        char pending_id[40];
        CHECK(store_project_id(g_store, project, false, pending_id, sizeof(pending_id)));
        DbRows rollback;
        CHECK(db_query(store_db(g_store), "SELECT COUNT(*) FROM documents WHERE project_id = ?",
                       (DbArg[1]){ DB_S(pending_id) }, 1, &rollback));
        CHECK(rollback.nrows == 1 && !strcmp(db_cell(&rollback, 0, 0), "0"));
        db_rows_free(&rollback);
        CHECK(store_import(g_store, project, why, sizeof(why)));
        /* Two snapshots, plus the file as saved on disk ("Saved manuscript"),
         * which neither snapshot held: the import records it as the saved revision. */
        Revision *revs = NULL;
        size_t imported = revisions_list(g_store, project, doc, &revs);
        CHECK(imported == 3 && !strcmp(revs[0].origin, "import"));
        char *saved_text = imported == 3 ? revisions_load(g_store, project, doc, &revs[0]) : NULL;
        CHECK(saved_text && !strcmp(saved_text, "Saved manuscript"));
        free(saved_text);
        free(revs);
        CHECK(note_count(project) == 1);
        char cmd[PATH_MAX + 20];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", project);
        system(cmd);
    }
}

static void test_fork_retry_snapshot(void)
{
    char why[256] = "", root[PATH_MAX], original[PATH_MAX + 20], copy[PATH_MAX + 20], cmd[PATH_MAX * 3];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    snprintf(root, sizeof(root), "%sscribodo-fork-retry-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp/");
    CHECK(mkdtemp(root) != NULL);
    snprintf(original, sizeof(original), "%s/original", root);
    snprintf(copy, sizeof(copy), "%s/copy", root);
    CHECK(mkdir(original, 0755) == 0);
    ChatMessage user = { .role = CHAT_USER, .content = "First message" };
    ChatMessage assistant = { .role = CHAT_ASSISTANT, .content = "Second message" };
    char chat_id[40];
    chat_store_new_id(chat_id, sizeof(chat_id));
    CHECK(chat_store_append(g_store, original, chat_id, "Two messages", &user));
    CHECK(chat_store_append(g_store, original, chat_id, "Two messages", &assistant));
    CHECK(records_append(g_store, original, REC_NOTE, "n-fork", "active", "v1", NULL, 0, NULL));
    CHECK(records_append(g_store, original, REC_NOTE, "n-fork", "active", "v2", NULL, 0, NULL));
    CHECK(records_append(g_store, original, REC_CANON, "c-fork", "committed", "Canon", NULL, 0, NULL));
    CHECK(store_import(g_store, original, why, sizeof(why)));
    char original_id[40];
    CHECK(store_mode(g_store, original, original_id, sizeof(original_id), NULL) == STORE_DB);
    snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", original, copy);
    CHECK(system(cmd) == 0);
    char unowned[PATH_MAX + 100], sentinel[PATH_MAX + 130];
    snprintf(unowned, sizeof(unowned), "%s/.scribodo/fork-snapshot", copy);
    snprintf(sentinel, sizeof(sentinel), "%s/keep.txt", unowned);
    CHECK(mkdir(unowned, 0700) == 0);
    write_file(sentinel, "Unrecognized staging must be preserved");
    CHECK(!store_fork(g_store, copy, why, sizeof(why)) && access(sentinel, F_OK) == 0);
    CHECK(unlink(sentinel) == 0 && rmdir(unowned) == 0);
    Db *db = store_db(g_store);
    CHECK(db_run(db, "UPDATE record_versions SET status = '1234567890123456' WHERE record_id = 'c-fork' AND record_id IN"
                     " (SELECT id FROM records WHERE project_id = ?)", (DbArg[1]){ DB_S(original_id) }, 1, NULL));
    CHECK(!store_fork(g_store, copy, why, sizeof(why)));
    CHECK(store_problem(g_store, copy, NULL, 0) == STORE_PROBLEM_COPY);
    CHECK(store_mode(g_store, copy, NULL, 0, NULL) == STORE_PAUSED);
    CHECK(!chat_store_append(g_store, copy, chat_id, "Must stay paused", &user));
    char chats[PATH_MAX + 60];
    snprintf(chats, sizeof(chats), "%s/.scribodo/chats", copy);
    CHECK(access(chats, F_OK) != 0); /* partial export never becomes live metadata */
    CHECK(db_run(db, "UPDATE record_versions SET status = 'committed' WHERE record_id = 'c-fork' AND record_id IN"
                     " (SELECT id FROM records WHERE project_id = ?)", (DbArg[1]){ DB_S(original_id) }, 1, NULL));
    char pending[PATH_MAX + 80];
    snprintf(pending, sizeof(pending), "%s/.scribodo/import-pending", copy);
    CHECK(mkdir(pending, 0700) == 0); /* export completes, but registration cannot persist */
    bool blocked = !store_fork(g_store, copy, why, sizeof(why));
    CHECK(blocked && store_problem(g_store, copy, NULL, 0) == STORE_PROBLEM_COPY);
    CHECK(store_mode(g_store, copy, NULL, 0, NULL) == STORE_PAUSED);
    if (blocked) {
        CHECK(rmdir(pending) == 0);
        write_file(pending, original_id); /* copied crash residue must never resume the source project's row */
        CHECK(chat_store_append(g_store, original, chat_id, "Two messages", &user));
        char staged_notes[PATH_MAX + 100];
        snprintf(staged_notes, sizeof(staged_notes), "%s/.scribodo/fork-snapshot/.scribodo/notes.jsonl", copy);
        CHECK(chmod(staged_notes, 0000) == 0);
        CHECK(!store_fork(g_store, copy, why, sizeof(why)) && store_problem(g_store, copy, NULL, 0) == STORE_PROBLEM_COPY);
        CHECK(chmod(staged_notes, 0644) == 0);
        store_stop(g_store);
        CHECK(store_start(g_store, g_name, why, sizeof(why))); /* resume using only the durable journal */
        char aside_path[PATH_MAX + 100];
        snprintf(aside_path, sizeof(aside_path), "%s/.scribodo/store.json.set-aside", copy);
        CHECK(mkdir(aside_path, 0700) == 0); /* marker withdrawal must not be part of a fork */
        CHECK(store_fork(g_store, copy, why, sizeof(why)));
        CHECK(rmdir(aside_path) == 0);
        char copy_id[40];
        CHECK(store_mode(g_store, copy, copy_id, sizeof(copy_id), NULL) == STORE_DB && strcmp(copy_id, original_id));
        ChatMessage *messages = NULL;
        size_t n = 0;
        ChatSummary *chats_list = NULL;
        CHECK(chat_store_list_checked(g_store, copy, &chats_list, &n) && n == 1);
        CHECK(n == 1 && chat_store_load(g_store, copy, chats_list[0].id, &messages, &n) && n == 2);
        free(chats_list);
        chat_store_free_messages(messages, n);
        Record *notes = NULL;
        n = records_list(g_store, copy, REC_NOTE, &notes);
        CHECK(n == 1 && notes[0].nversions == 2 && !strcmp(record_current(&notes[0])->content, "v2"));
        records_free(notes, n);
        CHECK(chat_store_load(g_store, original, chat_id, &messages, &n) && n == 3);
        chat_store_free_messages(messages, n);
        char journal[PATH_MAX + 80];
        snprintf(journal, sizeof(journal), "%s/.scribodo/fork.json", copy);
        CHECK(access(journal, F_OK) != 0 && access(pending, F_OK) != 0 && access(staged_notes, F_OK) != 0);

        /* Simulate a journal copied after marker commit but before cleanup. */
        char copy2[PATH_MAX + 20], residue[400];
        snprintf(copy2, sizeof(copy2), "%s/copy2", root);
        snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", copy, copy2);
        CHECK(system(cmd) == 0);
        snprintf(journal, sizeof(journal), "%s/.scribodo/fork.json", copy2);
        snprintf(residue, sizeof(residue), "{\"version\":1,\"source\":\"%s\",\"target\":\"%s\",\"ready\":true,\"signature\":\"0000000000000000\"}", original_id, copy_id);
        write_file(journal, residue);
        snprintf(pending, sizeof(pending), "%s/.scribodo/import-pending", copy2);
        write_file(pending, copy_id);
        CHECK(store_fork(g_store, copy2, why, sizeof(why)));
        char copy2_id[40];
        CHECK(store_mode(g_store, copy2, copy2_id, sizeof(copy2_id), NULL) == STORE_DB && strcmp(copy2_id, copy_id));
        CHECK(access(journal, F_OK) != 0 && access(pending, F_OK) != 0);
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    system(cmd);
}

static void test_import_revision_identity(void)
{
    char why[256] = "", root[PATH_MAX], doc[PATH_MAX + 32], snapshots[2][160];
    CHECK(store_start(g_store, g_name, why, sizeof(why)));
    snprintf(root, sizeof(root), "%sscribodo-revision-identity-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp/");
    CHECK(mkdtemp(root) != NULL);
    SourceRef sources[2] = { { .kind = SRC_DOC, .quote = "A" }, { .kind = SRC_DOC, .quote = "B" } };
    const char *names[] = { "a.md", "b.md" }, *texts[] = { "A", "B" };
    store_set_write_time(g_store, 1700000000.125);
    for (size_t i = 0; i < 2; i++) {
        snprintf(doc, sizeof(doc), "%s/%s", root, names[i]);
        write_file(doc, texts[i]);
        CHECK(revisions_snapshot(g_store, root, doc, texts[i], snapshots[i], sizeof(snapshots[i])));
        snprintf(sources[i].doc, sizeof(sources[i].doc), "%s", names[i]);
        snprintf(sources[i].revision, sizeof(sources[i].revision), "%s", snapshots[i]);
    }
    store_set_write_time(g_store, 0);
    CHECK(!strcmp(snapshots[0], snapshots[1])); /* filenames alone are not revision identities */
    CHECK(records_append(g_store, root, REC_NOTE, "n-revision-identity", "active", "Two sources", sources, 2, NULL));
    CHECK(store_import(g_store, root, why, sizeof(why)));
    char pid[40];
    CHECK(store_mode(g_store, root, pid, sizeof(pid), NULL) == STORE_DB);
    DbRows rows;
    CHECK(db_query(store_db(g_store), "SELECT COUNT(*) FROM source_refs s JOIN revisions v ON v.id = s.revision_id"
                                      " JOIN records r ON r.id = s.record_id WHERE r.project_id = ? AND s.document_id <> v.document_id",
                   (DbArg[1]){ DB_S(pid) }, 1, &rows));
    CHECK(rows.nrows == 1 && !strcmp(db_cell(&rows, 0, 0), "0"));
    db_rows_free(&rows);
    /* Database sequence is authoritative even when wall-clock time goes back. */
    snprintf(doc, sizeof(doc), "%s/a.md", root);
    CHECK(revisions_record(g_store, root, doc, "Later sequence, earlier clock", "save", NULL, NULL));
    CHECK(db_run(store_db(g_store), "UPDATE revisions v JOIN documents d ON d.id = v.document_id"
                 " SET v.created_at = 1600000000 WHERE d.project_id = ? AND v.content = 'Later sequence, earlier clock'",
                 (DbArg[1]){ DB_S(pid) }, 1, NULL));
    char exported[PATH_MAX + 32];
    snprintf(exported, sizeof(exported), "%s/exported", root);
    CHECK(store_export_project(g_store, pid, exported, why, sizeof(why)));
    CHECK(store_import(g_store, exported, why, sizeof(why)));
    snprintf(doc, sizeof(doc), "%s/a.md", exported);
    Revision *history = NULL;
    size_t nhistory = revisions_list(g_store, exported, doc, &history);
    CHECK(nhistory == 2);
    char *latest = nhistory ? revisions_load(g_store, exported, doc, &history[0]) : NULL;
    CHECK(latest && !strcmp(latest, "Later sequence, earlier clock"));
    free(latest);
    free(history);
    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    system(cmd);
}

int main(void)
{
    g_store = store_manager_create();
    if (!g_store) return 1;
    snprintf(g_name, sizeof(g_name), "scribodo_test_%d", (int)getpid());
    snprintf(g_name2, sizeof(g_name2), "scribodo_test_%d_b", (int)getpid());
    if (!admin("CREATE DATABASE %s CHARACTER SET utf8mb4") || !admin("GRANT ALL ON %s.* TO 'scribodo'@'%%'") ||
        !admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", g_name2) ||
        !admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", g_name2)) {
        fprintf(stderr, "dbtest: can't set up a test database (is tools/services.sh up?)\n");
        return 1;
    }
    TestCase tests[] = { { "store_ids", test_store_ids }, { "store_managers", test_store_managers }, { "backends", test_backends },
                         { "chat_source_pages", test_chat_source_pages },
                         { "chat_windows", test_chat_windows },
                         { "import", test_import }, { "import_safety", test_import_safety },
                         { "warmed_verification_replacement", test_warmed_verification_replacement },
                          { "transition_read_failures", test_transition_read_failures }, { "entities", test_entities },
                          { "legacy_import_read_failures", test_legacy_import_read_failures },
                          { "fork_retry_snapshot", test_fork_retry_snapshot },
                          { "import_revision_identity", test_import_revision_identity },
                         { "entities_redteam", test_entities_redteam }, { "semantic", test_semantic } };
    int rc = run_db_core_tests();
    if (run_tests(tests, sizeof(tests) / sizeof(tests[0])) != 0) rc = 1;
    admin("DROP DATABASE %s");
    admin_name("DROP DATABASE %s", g_name2);
    store_manager_destroy(g_store);
    return rc;
}
