/* GAP-004 interruption matrix: the process dies at every named point of a transition and the next process
 * must find the row's invariant. Rows here: the schema migration (after each statement, after each receipt)
 * and the document save (after the temp file is complete, after the rename).
 *
 * The driver forks and execs itself with SCRIBODO_CRASH_POINT="<point>#k" for k = 1, 2, ... on a fresh
 * throwaway database each time, until a child reports it finished without dying (the matrix is exhausted:
 * k exceeded the number of hits). For every death it checks the child died of SIGKILL, reads the schema
 * version the dead process left, runs db_migrate in this process, and compares the schema (every column,
 * index and constraint from information_schema) with a reference database migrated once without
 * interruption. One row per point and hit goes to build/crash/matrix.tsv.
 *
 * Limits: process death, not power loss; the database server stays up, so what it has committed is what a
 * durable server would have committed, and the filesystem keeps what the dead process had written. Rows
 * seeded before the migration are the replay test's job (tests/test_db_core.c); this binary holds the schema. */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <errno.h>

#include "app/db.h"
#include "app/chat_store.h"
#include "app/records.h"
#include "app/store.h"
#include "app/store_backends.h"
#include "core/crash_point.h"
#include "core/doc_io.h"
#include "core/file_text.h"
#include "core/utf8.h"
#include "test_util.h"

bool admin_name(const char *sql_fmt, const char *name);

#define LATEST_SCHEMA 15

static const char *const k_points[] = { "migration.statement", "migration.receipt" };

static Db *open_named(const char *name, char *err, size_t errlen)
{
    DbConfig c;
    if (!db_config_default(&c, err, errlen)) return NULL;
    snprintf(c.database, sizeof(c.database), "%s", name);
    return db_open(&c, err, errlen);
}

/* Every column, index and constraint of the database, one line each, in a fixed order. */
static char *schema_fingerprint(Db *db, const char *name)
{
    static const char *const queries[] = {
        "SELECT CONCAT_WS('|', 'column', table_name, column_name, ordinal_position, column_type, is_nullable, "
        "COALESCE(column_default, 'NULL'), column_key, extra, COALESCE(character_set_name, ''), COALESCE(collation_name, '')) "
        "FROM information_schema.columns WHERE table_schema = ? ORDER BY table_name, ordinal_position",
        "SELECT CONCAT_WS('|', 'index', table_name, index_name, seq_in_index, column_name, non_unique, index_type, COALESCE(sub_part, 0)) "
        "FROM information_schema.statistics WHERE table_schema = ? ORDER BY table_name, index_name, seq_in_index",
        "SELECT CONCAT_WS('|', 'constraint', table_name, constraint_name, constraint_type) "
        "FROM information_schema.table_constraints WHERE table_schema = ? ORDER BY table_name, constraint_name",
        "SELECT CONCAT_WS('|', 'check', table_name, constraint_name, check_clause) "
        "FROM information_schema.check_constraints WHERE constraint_schema = ? ORDER BY table_name, constraint_name",
        "SELECT CONCAT_WS('|', 'table', table_name, engine, table_collation) "
        "FROM information_schema.tables WHERE table_schema = ? ORDER BY table_name",
    };
    size_t cap = 65536, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        DbRows r;
        if (!db_query(db, queries[q], (DbArg[1]){ DB_S(name) }, 1, &r)) { free(out); return NULL; }
        for (size_t i = 0; i < r.nrows; i++) {
            const char *cell = db_cell(&r, i, 0);
            size_t need = len + strlen(cell) + 2;
            if (need > cap) {
                char *grown = realloc(out, need * 2);
                if (!grown) { db_rows_free(&r); free(out); return NULL; }
                out = grown; cap = need * 2;
            }
            len += (size_t)snprintf(out + len, cap - len, "%s\n", cell);
        }
        db_rows_free(&r);
    }
    return out;
}

static int child_main(const char *name)
{
    char err[512] = "";
    Db *db = open_named(name, err, sizeof(err));
    if (!db) { fprintf(stderr, "child: %s\n", err); return 2; }
    bool ok = db_migrate(db, err, sizeof(err));
    if (!ok) fprintf(stderr, "child: %s\n", err);
    printf("DONE ok=%d version=%d hits=%ld\n", ok, db_schema_version(db), crash_point_hits());
    fflush(stdout);
    db_close(db);
    return ok ? 0 : 2;
}

typedef struct { bool exited, killed; int code, sig; long hits; int version; } ChildResult;

/* Runs this binary as a child under one selected point (mode --child or --child-save); its stdout's DONE line is
 * parsed when it finishes. */
static bool run_child2(const char *self, const char *spec, const char *mode, const char *name, const char *extra, ChildResult *out)
{
    memset(out, 0, sizeof(*out));
    out->hits = -1; out->version = -1;
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("SCRIBODO_CRASH_POINT", spec, 1);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execl(self, self, mode, name, extra, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[512] = "";
    size_t got = 0;
    for (;;) {
        ssize_t n = read(pipefd[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (got >= sizeof(buf) - 1) break;
    }
    close(pipefd[0]);
    buf[got] = '\0';
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    out->exited = WIFEXITED(status); out->code = out->exited ? WEXITSTATUS(status) : -1;
    out->killed = WIFSIGNALED(status); out->sig = out->killed ? WTERMSIG(status) : 0;
    int ok = 0;
    if (out->exited) sscanf(buf, "DONE ok=%d version=%d hits=%ld", &ok, &out->version, &out->hits);
    return true;
}

static bool run_child(const char *self, const char *spec, const char *mode, const char *name, ChildResult *out)
{
    return run_child2(self, spec, mode, name, NULL, out);
}

static FILE *g_matrix;
static const char *g_self;
static char *g_reference;
static int g_rows;

static void run_point(const char *point)
{
    for (long k = 1; k < 10000; k++) {
        char name[64], spec[160], err[512] = "";
        snprintf(name, sizeof(name), "scribodo_crash_%d_%ld", (int)getpid(), k);
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", name) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", name));
        ChildResult r;
        CHECK(run_child(g_self, spec, "--child", name, &r));
        if (r.exited) {
            /* The matrix is exhausted: the child finished every point before hit k. */
            CHECK(r.code == 0 && r.version == LATEST_SCHEMA && r.hits == k - 1);
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t%d\t-\t%ld hits in all\n", point, k, r.version, r.hits);
            printf("%s: %ld points, every one recovered to schema %d\n", point, k - 1, LATEST_SCHEMA);
            admin_name("DROP DATABASE %s", name);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        Db *db = open_named(name, err, sizeof(err));
        CHECK(db != NULL);
        if (!db) { fprintf(stderr, "    %s\n", err); admin_name("DROP DATABASE %s", name); break; }
        int at_death = db_schema_version(db);
        /* A death after a statement leaves the migration unreceipted (schema below 15); a death after receipt k
         * leaves exactly k receipts, the fifteenth included. */
        if (!strcmp(point, "migration.receipt")) CHECK(at_death == k);
        else CHECK(at_death >= 0 && at_death < LATEST_SCHEMA);
        bool recovered = db_migrate(db, err, sizeof(err));
        if (!recovered) fprintf(stderr, "    %s#%ld: %s\n", point, k, err);
        CHECK(recovered && db_schema_version(db) == LATEST_SCHEMA);
        DbRows rows;
        bool receipts = db_query(db, "SELECT COUNT(*), COUNT(DISTINCT version), MIN(version), MAX(version) FROM schema_migrations", NULL, 0, &rows) &&
                        rows.nrows == 1 && !strcmp(db_cell(&rows, 0, 0), "15") && !strcmp(db_cell(&rows, 0, 1), "15") &&
                        !strcmp(db_cell(&rows, 0, 2), "1") && !strcmp(db_cell(&rows, 0, 3), "15");
        if (rows.nrows) db_rows_free(&rows);
        CHECK(receipts);
        char *fp = schema_fingerprint(db, name);
        bool same = fp && g_reference && !strcmp(fp, g_reference);
        CHECK(same);
        if (!same && fp && g_reference) {
            /* Show the first differing line of each. */
            const char *a = fp, *b = g_reference;
            while (*a && *b && *a == *b) { a++; b++; }
            fprintf(stderr, "    schema differs at: %.120s\n    reference:         %.120s\n", a, b);
        }
        CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == LATEST_SCHEMA); /* idempotent after recovery */
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\t%d\t%d\t%s\t%s\n", point, k, at_death, db_schema_version(db), receipts ? "15 receipts" : "receipts wrong",
                same ? "schema identical" : "schema differs");
        g_rows++;
        free(fp);
        db_close(db);
        admin_name("DROP DATABASE %s", name);
    }
}

/* ---- the store write row --------------------------------------------------- */

#define CONVERSATION "20261111-120000-crsh"

static bool seed_project(Db *db)
{
    DbArg p[6] = { DB_S("p-crash"), DB_S("Book"), DB_S("/crash/Book"), DB_S("crash-hash"), DB_D(1.0), DB_D(1.0) };
    return db_run(db, "INSERT INTO projects (id, name, path, path_hash, created_at, updated_at) VALUES (?,?,?,?,?,?)", p, 6, NULL);
}

static bool append_message(Db *db, const char *content, bool assistant)
{
    ChatMessage m = { .role = assistant ? CHAT_ASSISTANT : CHAT_USER, .content = (char *)content };
    return sdb_chat_append(db, "p-crash", CONVERSATION, "Crash chat", &m);
}

/* Two appends, each one transaction: the commit points are hit twice. */
static int child_append_main(const char *name)
{
    char err[512] = "";
    Db *db = open_named(name, err, sizeof(err));
    if (!db) { fprintf(stderr, "child: %s\n", err); return 2; }
    bool ok = db_migrate(db, err, sizeof(err)) && seed_project(db) && append_message(db, "First message", false) &&
              append_message(db, "Second message", true);
    printf("DONE ok=%d version=%d hits=%ld\n", ok, db_schema_version(db), crash_point_hits());
    fflush(stdout);
    db_close(db);
    return ok ? 0 : 2;
}

/* Messages of the crash conversation: count, and whether seq runs 0..count-1 without a gap or a duplicate. */
static bool message_state(Db *db, long *count, bool *contiguous, long *conversations)
{
    DbRows r;
    if (!db_query(db, "SELECT COUNT(*), COUNT(DISTINCT seq), COALESCE(MIN(seq), 0), COALESCE(MAX(seq), -1) FROM messages WHERE conversation_id = ?",
                  (DbArg[1]){ DB_S(CONVERSATION) }, 1, &r) || r.nrows != 1) return false;
    *count = atol(db_cell(&r, 0, 0));
    *contiguous = atol(db_cell(&r, 0, 1)) == *count && atol(db_cell(&r, 0, 2)) == 0 && atol(db_cell(&r, 0, 3)) == *count - 1;
    db_rows_free(&r);
    if (!db_query(db, "SELECT COUNT(*) FROM conversations WHERE id = ?", (DbArg[1]){ DB_S(CONVERSATION) }, 1, &r) || r.nrows != 1) return false;
    *conversations = atol(db_cell(&r, 0, 0));
    db_rows_free(&r);
    return true;
}

static void run_store_point(const char *point, bool after)
{
    for (long k = 1; k < 100; k++) {
        char name[64], spec[160], err[512] = "";
        snprintf(name, sizeof(name), "scribodo_crash_%d_s%ld", (int)getpid(), k);
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", name) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", name));
        ChildResult r;
        CHECK(run_child(g_self, spec, "--child-append", name, &r));
        if (r.exited) {
            CHECK(r.code == 0 && r.hits == 2 && k == 3); /* two appends, two commits, no other transaction on the path */
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t-\t-\t%ld hits in all\n", point, k, r.hits);
            printf("%s: %ld points\n", point, k - 1);
            admin_name("DROP DATABASE %s", name);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        Db *db = open_named(name, err, sizeof(err));
        CHECK(db != NULL);
        if (!db) { admin_name("DROP DATABASE %s", name); break; }
        long count = -1, conversations = -1;
        bool contiguous = false;
        CHECK(message_state(db, &count, &contiguous, &conversations));
        /* Death before commit k: k-1 messages; after commit k: k messages. Never a partial message, never a duplicate. */
        long expected = after ? k : k - 1;
        CHECK(count == expected && contiguous && conversations == (expected ? 1 : 0));
        /* The next process appends; the sequence continues from what was committed. */
        bool next = append_message(db, "Third message, after recovery", false);
        CHECK(next);
        long count2 = -1, conv2 = -1;
        bool contiguous2 = false;
        CHECK(message_state(db, &count2, &contiguous2, &conv2) && count2 == expected + 1 && contiguous2 && conv2 == 1);
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\t%ld messages, %s\tnext append ok, %ld\tconversation rows %ld\t%s\n", point, k, count,
                contiguous ? "seq contiguous" : "SEQ BROKEN", count2, conversations, count == expected && contiguous ? "no partial, no duplicate" : "INVARIANT BROKEN");
        g_rows++;
        db_close(db);
        admin_name("DROP DATABASE %s", name);
    }
}

static void test_store_points(void)
{
    run_store_point("store.before_commit", false);
    run_store_point("store.after_commit", true);
}

/* ---- the legacy import row ------------------------------------------------- */

/* A legacy file-store project: one document, a chat of two messages, two note versions and a canon record. */
static bool make_legacy_project(StoreManager *manager, const char *dir, char *chat_id, size_t idlen)
{
    char doc[1100];
    snprintf(doc, sizeof(doc), "%s/ch1.md", dir);
    FILE *f = fopen(doc, "wb");
    if (!f) return false;
    fputs("# Chapter one\n\nText.\n", f);
    fclose(f);
    chat_store_new_id(chat_id, idlen);
    ChatMessage user = { .role = CHAT_USER, .content = "First message" };
    ChatMessage assistant = { .role = CHAT_ASSISTANT, .content = "Second message" };
    return chat_store_append(manager, dir, chat_id, "Two messages", &user) &&
           chat_store_append(manager, dir, chat_id, "Two messages", &assistant) &&
           records_append(manager, dir, REC_NOTE, "n-crash", "active", "v1", NULL, 0, NULL) &&
           records_append(manager, dir, REC_NOTE, "n-crash", "active", "v2", NULL, 0, NULL) &&
           records_append(manager, dir, REC_CANON, "c-crash", "committed", "Canon", NULL, 0, NULL);
}

static int child_import_main(const char *name, const char *dir)
{
    char why[256] = "";
    StoreManager *manager = store_manager_create();
    bool ok = manager && store_start(manager, name, why, sizeof(why));
    if (ok) ok = store_import(manager, dir, why, sizeof(why));
    if (!ok) fprintf(stderr, "child: %s\n", why);
    printf("DONE ok=%d version=0 hits=%ld\n", ok, crash_point_hits());
    fflush(stdout);
    if (manager) store_manager_destroy(manager);
    return ok ? 0 : 2;
}

static int count_entries(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (!strncmp(ent->d_name, prefix, strlen(prefix))) n++;
    closedir(d);
    return n;
}

static bool exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* What the next process finds and does: the mode, a second import, the data once, the files set aside once. */
static void run_import_point(const char *point)
{
    const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    for (long k = 1; k < 100; k++) {
        char name[64], spec[160], why[256] = "", dir[1024], chat_id[40], scribodo[1100], pending[1200], marker[1200];
        snprintf(name, sizeof(name), "scribodo_crash_%d_i%ld", (int)getpid(), k);
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        snprintf(dir, sizeof(dir), "%s/scribodo-crash-import-XXXXXX", base);
        CHECK(mkdtemp(dir) != NULL);
        snprintf(scribodo, sizeof(scribodo), "%s/.scribodo", dir);
        snprintf(pending, sizeof(pending), "%s/import-pending", scribodo);
        snprintf(marker, sizeof(marker), "%s/store.json", scribodo);
        CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", name) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", name));
        StoreManager *manager = store_manager_create();
        CHECK(manager && store_start(manager, name, why, sizeof(why)));
        CHECK(make_legacy_project(manager, dir, chat_id, sizeof(chat_id)));
        CHECK(store_mode(manager, dir, NULL, 0, NULL) == STORE_FILES);
        ChildResult r;
        CHECK(run_child2(g_self, spec, "--child-import", name, dir, &r));
        if (r.exited) {
            CHECK(r.code == 0 && r.hits == k - 1);
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t-\t-\t%ld hits in all\n", point, k, r.hits);
            printf("%s: %ld points\n", point, k - 1);
            store_manager_destroy(manager);
            admin_name("DROP DATABASE %s", name);
            char cmd[1200]; snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir); CHECK(system(cmd) == 0);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        /* The next process: a fresh manager on the same database and folder. */
        store_manager_destroy(manager);
        manager = store_manager_create();
        CHECK(manager && store_start(manager, name, why, sizeof(why)));
        StoreMode at_death = store_mode(manager, dir, NULL, 0, NULL);
        bool marker_at_death = exists(marker), pending_at_death = exists(pending);
        int legacy_at_death = count_entries(scribodo, "legacy-");
        /* The marker decides authority: no marker, the files (resumable); a marker, the store. Never paused. */
        CHECK(at_death == (marker_at_death ? STORE_DB : STORE_FILES));
        bool again = store_import(manager, dir, why, sizeof(why));
        if (!again) fprintf(stderr, "    %s#%ld: %s\n", point, k, why);
        CHECK(again);
        char pid[40] = "";
        CHECK(store_mode(manager, dir, pid, sizeof(pid), NULL) == STORE_DB && pid[0] == 'p');
        ChatMessage *msgs = NULL;
        size_t nm = 0;
        bool loaded = chat_store_load(manager, dir, chat_id, &msgs, &nm);
        CHECK(loaded && nm == 2 && msgs && !strcmp(msgs[0].content, "First message") && !strcmp(msgs[1].content, "Second message"));
        if (msgs) chat_store_free_messages(msgs, nm);
        Record *notes = NULL, *canon = NULL;
        size_t nn = records_list(manager, dir, REC_NOTE, &notes), nc = records_list(manager, dir, REC_CANON, &canon);
        CHECK(nn == 1 && notes && notes[0].nversions == 2 && nc == 1);
        records_free(notes, nn); records_free(canon, nc);
        /* Every legacy item is out of .scribodo/ (in one stamped archive, or two when the death split the move), the
         * witness is gone, and nothing legacy is left live. */
        int legacy_after = count_entries(scribodo, "legacy-");
        char chats_dir[1200]; snprintf(chats_dir, sizeof(chats_dir), "%s/chats", scribodo);
        char notes_file[1200]; snprintf(notes_file, sizeof(notes_file), "%s/notes.jsonl", scribodo);
        bool finished = legacy_after >= 1 && legacy_after <= 2 && !exists(pending) && !exists(chats_dir) && !exists(notes_file);
        CHECK(finished);
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\tmode %s, marker %d, witness %d, legacy dirs %d\treimport ok, mode db, legacy dirs %d\t2 messages, 1 note (2 versions), 1 canon\t%s\n",
                point, k, at_death == STORE_DB ? "db" : at_death == STORE_FILES ? "files" : "PAUSED", marker_at_death, pending_at_death, legacy_at_death,
                legacy_after, finished ? "authority held, data once, move finished" : "INVARIANT BROKEN");
        g_rows++;
        store_manager_destroy(manager);
        admin_name("DROP DATABASE %s", name);
        char cmd[1200]; snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir); CHECK(system(cmd) == 0);
    }
}

/* ---- the fork row ---------------------------------------------------------- */

static int child_fork_main(const char *name, const char *dir)
{
    char why[256] = "";
    StoreManager *manager = store_manager_create();
    bool ok = manager && store_start(manager, name, why, sizeof(why));
    if (ok) ok = store_fork(manager, dir, why, sizeof(why));
    if (!ok) fprintf(stderr, "child: %s\n", why);
    printf("DONE ok=%d version=0 hits=%ld\n", ok, crash_point_hits());
    fflush(stdout);
    if (manager) store_manager_destroy(manager);
    return ok ? 0 : 2;
}

/* An imported project and a copy of its folder; the child forks the copy and dies. The next process does what the
 * app does with the folder it finds: a copy is forked again, a project in the store is opened (store_finish_import). */
static void run_fork_point(const char *point)
{
    const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    for (long k = 1; k < 100; k++) {
        char name[64], spec[160], why[256] = "", root[1024], original[1100], copy[1100], chat_id[40], cmd[2400];
        snprintf(name, sizeof(name), "scribodo_crash_%d_f%ld", (int)getpid(), k);
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        snprintf(root, sizeof(root), "%s/scribodo-crash-fork-XXXXXX", base);
        CHECK(mkdtemp(root) != NULL);
        snprintf(original, sizeof(original), "%s/original", root);
        snprintf(copy, sizeof(copy), "%s/copy", root);
        CHECK(mkdir(original, 0755) == 0);
        CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", name) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", name));
        StoreManager *manager = store_manager_create();
        CHECK(manager && store_start(manager, name, why, sizeof(why)));
        CHECK(make_legacy_project(manager, original, chat_id, sizeof(chat_id)));
        CHECK(store_import(manager, original, why, sizeof(why)));
        char original_id[40] = "";
        CHECK(store_mode(manager, original, original_id, sizeof(original_id), NULL) == STORE_DB);
        snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", original, copy);
        CHECK(system(cmd) == 0);
        CHECK(store_problem(manager, copy, NULL, 0) == STORE_PROBLEM_COPY);
        ChildResult r;
        CHECK(run_child2(g_self, spec, "--child-fork", name, copy, &r));
        if (r.exited) {
            CHECK(r.code == 0 && r.hits == k - 1);
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t-\t-\t%ld hits in all\n", point, k, r.hits);
            printf("%s: %ld points\n", point, k - 1);
            store_manager_destroy(manager);
            admin_name("DROP DATABASE %s", name);
            snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root); CHECK(system(cmd) == 0);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        store_manager_destroy(manager);
        manager = store_manager_create();
        CHECK(manager && store_start(manager, name, why, sizeof(why)));
        /* What the next process finds. */
        char scribodo[1200], stage[1300], pending[1300], forkjson[1300];
        snprintf(scribodo, sizeof(scribodo), "%s/.scribodo", copy);
        snprintf(stage, sizeof(stage), "%s/fork-snapshot", scribodo);
        snprintf(pending, sizeof(pending), "%s/import-pending", scribodo);
        snprintf(forkjson, sizeof(forkjson), "%s/fork.json", scribodo);
        bool is_copy = store_problem(manager, copy, NULL, 0) == STORE_PROBLEM_COPY;
        StoreMode mode_at_death = store_mode(manager, copy, NULL, 0, NULL);
        bool stage_at_death = exists(stage), pending_at_death = exists(pending), fork_at_death = exists(forkjson);
        /* A copy stays a copy until its marker is replaced; then it is a project in the store. Never a plain file store. */
        CHECK(is_copy ? mode_at_death == STORE_PAUSED : mode_at_death == STORE_DB);
        /* What the app would do next. */
        bool acted;
        if (is_copy) acted = store_fork(manager, copy, why, sizeof(why));
        else acted = store_finish_import(manager, copy), acted = true;
        if (!acted) fprintf(stderr, "    %s#%ld: %s\n", point, k, why);
        CHECK(acted);
        char copy_id[40] = "";
        CHECK(store_mode(manager, copy, copy_id, sizeof(copy_id), NULL) == STORE_DB && copy_id[0] == 'p' && strcmp(copy_id, original_id));
        /* The copy's chat has a rebased id (the original project owns the old one): list, then load the one chat. */
        ChatSummary *list = NULL;
        size_t nl = chat_store_list(manager, copy, &list);
        ChatMessage *msgs = NULL;
        size_t nm = 0;
        CHECK(nl == 1 && list && chat_store_load(manager, copy, list[0].id, &msgs, &nm) && nm == 2);
        free(list);
        if (msgs) chat_store_free_messages(msgs, nm);
        msgs = NULL; nm = 0;
        CHECK(chat_store_load(manager, original, chat_id, &msgs, &nm) && nm == 2); /* the original is untouched */
        if (msgs) chat_store_free_messages(msgs, nm);
        char oid2[40] = "";
        CHECK(store_mode(manager, original, oid2, sizeof(oid2), NULL) == STORE_DB && !strcmp(oid2, original_id));
        /* The fork's journal is closed: no staging, no witness, no fork.json. */
        bool closed = !exists(stage) && !exists(pending) && !exists(forkjson);
        CHECK(closed);
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\t%s, stage %d, witness %d, fork.json %d\t%s, mode db, new project\tcopy 2 messages, original 2 messages\t%s\n",
                point, k, is_copy ? "copy (paused)" : mode_at_death == STORE_DB ? "db" : "OTHER", stage_at_death, pending_at_death, fork_at_death,
                is_copy ? "fork again ok" : "open finished", closed ? "data once, original intact, journal closed" : "JOURNAL LEFT OPEN");
        g_rows++;
        store_manager_destroy(manager);
        admin_name("DROP DATABASE %s", name);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root); CHECK(system(cmd) == 0);
    }
}

static void test_fork_points(void)
{
    static const char *const points[] = { "fork.journal_opened", "fork.exported", "import.registered", "import.committed",
                                          "import.marked", "import.set_aside", "fork.imported" };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) run_fork_point(points[i]);
}

static void test_import_points(void)
{
    run_import_point("import.registered");
    run_import_point("import.committed");
    run_import_point("import.marked");
    run_import_point("import.aside_item");
    run_import_point("import.set_aside");
}

/* ---- the document save row ------------------------------------------------ */

static const char k_original[] = "The original text, saved whole before the crash.\n";
static const char k_replacement[] = "The replacement text; longer than the original so a mix would be visible, and whole.\n";
static const char k_third[] = "A third save after recovery.\n";

static size_t to_cps(const char *text, uint32_t *out, size_t cap)
{
    size_t n = strlen(text);
    if (n > cap) return 0;
    size_t invalid = 0;
    return utf8_decode((const uint8_t *)text, n, out, &invalid);
}

static bool save_text(const char *path, const char *text, char *err, size_t errlen)
{
    uint32_t cps[512];
    size_t n = to_cps(text, cps, sizeof(cps) / sizeof(cps[0]));
    return n && doc_save(path, cps, n, false, false, NULL, NULL, err, errlen) == DOC_OK;
}

/* The app's small-file writer (core/file_text.h), the same shape as the save with one shared implementation. */
static int child_atomic_main(const char *path)
{
    bool ok = file_text_write_atomic(path, k_replacement, strlen(k_replacement), 0644);
    printf("DONE ok=%d version=0 hits=%ld\n", ok, crash_point_hits());
    fflush(stdout);
    return ok ? 0 : 2;
}

static int child_save_main(const char *path)
{
    char err[512] = "";
    bool ok = save_text(path, k_replacement, err, sizeof(err));
    if (!ok) fprintf(stderr, "child: %s\n", err);
    printf("DONE ok=%d version=0 hits=%ld\n", ok, crash_point_hits());
    fflush(stdout);
    return ok ? 0 : 2;
}

static size_t read_whole(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return (size_t)-1;
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

/* Temp files the save leaves beside the target (doc_io names them <target>.scribodo-XXXXXX). */
static int count_temps(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (strstr(ent->d_name, ".scribodo-")) n++;
    closedir(d);
    return n;
}

static const char *content_name(const char *buf, size_t n)
{
    if (n == strlen(k_original) && !memcmp(buf, k_original, n)) return "original";
    if (n == strlen(k_replacement) && !memcmp(buf, k_replacement, n)) return "new";
    if (n == strlen(k_third) && !memcmp(buf, k_third, n)) return "third";
    return "OTHER";
}

static int count_pattern(const char *dir, const char *pattern)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (strstr(ent->d_name, pattern)) n++;
    closedir(d);
    return n;
}

static void run_save_point_with(const char *point, const char *mode, const char *temp_pattern)
{
    const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    for (long k = 1; k < 100; k++) {
        char dir[1024], path[1100], spec[160], err[512] = "", buf[1024];
        snprintf(dir, sizeof(dir), "%s/scribodo-crash-save-XXXXXX", base);
        CHECK(mkdtemp(dir) != NULL);
        snprintf(path, sizeof(path), "%s/doc.md", dir);
        CHECK(save_text(path, k_original, err, sizeof(err)));
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        ChildResult r;
        CHECK(run_child(g_self, spec, mode, path, &r));
        if (r.exited) {
            CHECK(r.code == 0 && r.hits == k - 1);
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t-\t-\t%ld hits in all\n", point, k, r.hits);
            printf("%s: %ld points\n", point, k - 1);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        size_t n = read_whole(path, buf, sizeof(buf));
        const char *found = n == (size_t)-1 ? "MISSING" : content_name(buf, n);
        /* The target is whole: the original or the new text, never a mix, a truncation or nothing. */
        CHECK(!strcmp(found, "original") || !strcmp(found, "new"));
        int temps_at_death = count_pattern(dir, temp_pattern);
        /* The next write (the next process) succeeds and the target holds it whole. */
        bool next = !strcmp(mode, "--child-save") ? save_text(path, k_third, err, sizeof(err))
                                                  : file_text_write_atomic(path, k_third, strlen(k_third), 0644);
        if (!next) fprintf(stderr, "    %s#%ld: %s\n", point, k, err);
        CHECK(next);
        n = read_whole(path, buf, sizeof(buf));
        CHECK(n != (size_t)-1 && !strcmp(content_name(buf, n), "third"));
        int temps_after = count_pattern(dir, temp_pattern);
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\t%s at death\tnext write ok\t%d temp at death, %d after\t%s\n", point, k, found,
                temps_at_death, temps_after, !strcmp(found, "new") || !strcmp(found, "original") ? "target whole" : "TARGET DAMAGED");
        g_rows++;
        /* Leave nothing behind: the temp files are the test's, named by mkstemp under dir. */
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        CHECK(system(cmd) == 0);
    }
}

static void run_save_point(const char *point) { run_save_point_with(point, "--child-save", ".scribodo-"); }

static void test_save_points(void)
{
    run_save_point("save.temp_written");
    run_save_point("save.renamed");
}

static void test_atomic_points(void)
{
    run_save_point_with("atomic.temp_written", "--child-atomic", ".tmp-");
    run_save_point_with("atomic.renamed", "--child-atomic", ".tmp-");
}

/* ---- the disk row: full and read-only volumes ------------------------------ */

/* First line of a command's output, trimmed; "" when it produced none. */
static bool first_line(const char *cmd, char *out, size_t outlen)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool got = fgets(out, (int)outlen, f) != NULL;
    pclose(f);
    if (got) out[strcspn(out, " \t\r\n")] = '\0';
    return got && out[0];
}

/* Fill the volume to the last byte with ever smaller writes; true when a one-byte write fails with ENOSPC. */
static bool fill_volume(const char *dir)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/fill", dir);
    static const size_t sizes[] = { 65536, 4096, 512, 1 };
    static char zeros[65536];
    bool enospc = false;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && !enospc; i++) {
        FILE *f = fopen(path, "ab");
        if (!f) return false;
        for (;;) {
            if (fwrite(zeros, 1, sizes[i], f) != sizes[i] || fflush(f) != 0) { enospc = errno == ENOSPC && sizes[i] == 1; break; }
        }
        fclose(f);
    }
    return enospc;
}

static void test_disk_row(void)
{
    char dev[64], mount[128] = "/Volumes/scribodo-crash-disk", cmd[1200], why[256] = "", err[512] = "", buf[1024];
    snprintf(cmd, sizeof(cmd), "hdiutil detach %s >/dev/null 2>&1; hdiutil attach -nomount ram://4096 2>/dev/null", mount);
    if (!first_line(cmd, dev, sizeof(dev)) || strncmp(dev, "/dev/disk", 9)) {
        printf("disk row: no RAM disk could be attached; skipped\n");
        fprintf(g_matrix, "disk.full\t-\tskipped\tno RAM disk\t-\t-\t-\n");
        return;
    }
    snprintf(cmd, sizeof(cmd), "diskutil eraseVolume HFS+ scribodo-crash-disk %s >/dev/null 2>&1", dev);
    bool mounted = system(cmd) == 0 && exists(mount);
    CHECK(mounted);
    if (!mounted) { snprintf(cmd, sizeof(cmd), "hdiutil detach %s >/dev/null 2>&1", dev); system(cmd); return; }
    char name[64], dir[256], doc[300], chat_id[40];
    snprintf(name, sizeof(name), "scribodo_crash_%d_disk", (int)getpid());
    snprintf(dir, sizeof(dir), "%s/project", mount);
    snprintf(doc, sizeof(doc), "%s/ch1.md", dir);
    CHECK(mkdir(dir, 0755) == 0);
    CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", name) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", name));
    StoreManager *manager = store_manager_create();
    CHECK(manager && store_start(manager, name, why, sizeof(why)));
    CHECK(make_legacy_project(manager, dir, chat_id, sizeof(chat_id)));
    CHECK(save_text(doc, k_original, err, sizeof(err)));
    /* Full. */
    bool full = fill_volume(dir);
    CHECK(full);
    DocStatus st = full ? doc_save(doc, (uint32_t[]){ 'x' }, 1, false, false, NULL, NULL, err, sizeof(err)) : DOC_OK;
    size_t n = read_whole(doc, buf, sizeof(buf));
    bool save_refused = st == DOC_ERR_IO && n != (size_t)-1 && !strcmp(content_name(buf, n), "original") && count_temps(dir) == 0;
    CHECK(save_refused);
    fprintf(g_matrix, "disk.full\tsave\tENOSPC\toriginal whole\trefused: %s\tno temp left\t%s\n", err, save_refused ? "original intact" : "DAMAGED");
    bool import_refused = full && !store_import(manager, dir, why, sizeof(why));
    StoreMode mode_full = store_mode(manager, dir, NULL, 0, NULL);
    ChatMessage *msgs = NULL; size_t nm = 0;
    bool files_intact = chat_store_load(manager, dir, chat_id, &msgs, &nm) && nm == 2;
    if (msgs) chat_store_free_messages(msgs, nm);
    CHECK(import_refused && mode_full == STORE_FILES && files_intact);
    fprintf(g_matrix, "disk.full\timport\tENOSPC\tmode %s, 2 messages in files\trefused: %s\t-\t%s\n", mode_full == STORE_FILES ? "files" : mode_full == STORE_DB ? "db" : "PAUSED", why,
            import_refused && mode_full == STORE_FILES && files_intact ? "files stay authoritative" : "INVARIANT BROKEN");
    /* Space back: the same import succeeds, the data once. */
    snprintf(cmd, sizeof(cmd), "%s/fill", dir);
    CHECK(unlink(cmd) == 0);
    bool again = store_import(manager, dir, why, sizeof(why));
    if (!again) fprintf(stderr, "    import after space freed: %s\n", why);
    msgs = NULL; nm = 0;
    bool once = chat_store_load(manager, dir, chat_id, &msgs, &nm) && nm == 2;
    if (msgs) chat_store_free_messages(msgs, nm);
    char pending[400]; snprintf(pending, sizeof(pending), "%s/.scribodo/import-pending", dir);
    CHECK(again && store_mode(manager, dir, NULL, 0, NULL) == STORE_DB && once && !exists(pending));
    fprintf(g_matrix, "disk.full\timport again\tspace freed\t-\tok, mode db\t2 messages, witness gone\t%s\n", again && once ? "data once" : "INVARIANT BROKEN");
    /* Read-only target: refused with the right status, original intact. */
    CHECK(chmod(doc, 0444) == 0);
    st = doc_save(doc, (uint32_t[]){ 'x' }, 1, false, false, NULL, NULL, err, sizeof(err));
    n = read_whole(doc, buf, sizeof(buf));
    bool ro = st == DOC_ERR_READ_ONLY && n != (size_t)-1 && !strcmp(content_name(buf, n), "original") && count_temps(dir) == 0;
    CHECK(ro);
    chmod(doc, 0644);
    fprintf(g_matrix, "disk.read_only\tsave\tEACCES\toriginal whole\trefused: %s\tno temp left\t%s\n", err, ro ? "original intact" : "DAMAGED");
    g_rows += 4;
    store_manager_destroy(manager);
    admin_name("DROP DATABASE %s", name);
    snprintf(cmd, sizeof(cmd), "hdiutil detach %s >/dev/null 2>&1", dev);
    CHECK(system(cmd) == 0);
}

static void test_migration_points(void)
{
    char ref[64], err[512] = "";
    snprintf(ref, sizeof(ref), "scribodo_crash_%d_ref", (int)getpid());
    CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", ref) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", ref));
    Db *db = open_named(ref, err, sizeof(err));
    CHECK(db && db_migrate(db, err, sizeof(err)) && db_schema_version(db) == LATEST_SCHEMA);
    if (db) {
        g_reference = schema_fingerprint(db, ref);
        db_close(db);
    }
    CHECK(g_reference && strlen(g_reference) > 1000);
    admin_name("DROP DATABASE %s", ref);
    if (!g_reference) return;
    for (size_t i = 0; i < sizeof(k_points) / sizeof(k_points[0]); i++) run_point(k_points[i]);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--child")) return child_main(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--child-save")) return child_save_main(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--child-atomic")) return child_atomic_main(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--child-append")) return child_append_main(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--child-import")) return child_import_main(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "--child-fork")) return child_fork_main(argv[2], argv[3]);
    g_self = argv[0];
    mkdir("build", 0755); mkdir("build/crash", 0755);
    g_matrix = fopen("build/crash/matrix.tsv", "w");
    if (!g_matrix) { perror("build/crash/matrix.tsv"); return 1; }
    fprintf(g_matrix, "point\thit\tdeath\tstate_at_death\trecovery\tdetail\tinvariant\n");
    time_t t0 = time(NULL);
    TestCase tests[] = { { "migration_points", test_migration_points }, { "save_points", test_save_points },
                         { "store_points", test_store_points }, { "import_points", test_import_points },
                         { "fork_points", test_fork_points }, { "disk_row", test_disk_row },
                         { "atomic_points", test_atomic_points } };
    int rc = run_tests(tests, 7);
    fclose(g_matrix);
    printf("%d matrix rows in build/crash/matrix.tsv, %ld s\n", g_rows, (long)(time(NULL) - t0));
    free(g_reference);
    return rc;
}
