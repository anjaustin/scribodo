/* The MariaDB layer against the local service (make dbtest; needs
 * tools/services.sh up). Uses a throwaway database, dropped at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mysql.h>

#include "../src/app/db.h"
#include "../src/app/store.h"
#include "../src/app/store_backends.h"
#include <sys/stat.h>
#include "../src/app/renames.h"
#include "test_util.h"

static char g_name[64];

/* Create the throwaway database as root and grant it to the app user. */
static bool admin(const char *sql_fmt)
{
    const char *home = getenv("HOME");
    char path[1024], pw[200] = "";
    snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/services/db_root_password", home);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    pw[fread(pw, 1, sizeof(pw) - 1, f)] = '\0';
    fclose(f);
    pw[strcspn(pw, "\r\n")] = '\0';
    MYSQL *m = mysql_init(NULL);
    unsigned t = 5;
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
    bool ok = mysql_real_connect(m, "127.0.0.1", "root", pw, NULL, 33061, NULL, 0) != NULL;
    char sql[512];
    snprintf(sql, sizeof(sql), sql_fmt, g_name, g_name);
    ok = ok && mysql_query(m, sql) == 0;
    if (!ok) fprintf(stderr, "admin: %s\n", mysql_error(m));
    mysql_close(m);
    return ok;
}

static void test_db(void)
{
    DbConfig c;
    char err[512] = "";
    CHECK(db_config_default(&c, err, sizeof(err)));
    snprintf(c.database, sizeof(c.database), "%s", g_name);
    Db *db = db_open(&c, err, sizeof(err));
    CHECK(db != NULL);
    if (!db) {
        fprintf(stderr, "    %s\n", err);
        return;
    }
    CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 1);
    CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 1); /* idempotent */

    /* Author text with quotes, backslashes, SQL and Unicode goes through
     * prepared statements untouched. */
    const char *nasty = "Zoë's \"quote\" \\ '); DROP TABLE projects; -- 🐋 Dvořák";
    DbArg p[5] = { DB_S("p-1"), DB_S("Book"), DB_S("/Users/x/Book"), DB_S("hash1"), DB_D(1.5) };
    CHECK(db_run(db, "INSERT INTO projects (id, name, path, path_hash, created_at, updated_at) VALUES (?,?,?,?,?,?)",
                 (DbArg[6]){ p[0], p[1], p[2], p[3], p[4], p[4] }, 6, NULL));
    DbArg d[4] = { DB_S("d-1"), DB_S("p-1"), DB_S("ch1.md"), DB_D(2.0) };
    CHECK(db_run(db, "INSERT INTO documents (id, project_id, name, created_at) VALUES (?,?,?,?)", d, 4, NULL));
    DbArg r[6] = { DB_S("r-1"), DB_S("d-1"), DB_S("abcdef0123456789"), DB_S(nasty), DB_S("save"), DB_D(3.0) };
    CHECK(db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES (?,?,?,?,?,?)", r, 6,
                 NULL));
    DbRows rows;
    CHECK(db_query(db, "SELECT content, note, created_at FROM revisions WHERE id = ?", (DbArg[1]){ DB_S("r-1") }, 1, &rows));
    CHECK(rows.nrows == 1 && rows.ncols == 3 && !strcmp(db_cell(&rows, 0, 0), nasty) && !strcmp(db_cell(&rows, 0, 1), "") &&
          atof(db_cell(&rows, 0, 2)) == 3.0);
    db_rows_free(&rows);
    CHECK(db_query(db, "SELECT COUNT(*) FROM projects", NULL, 0, &rows) && rows.nrows == 1 && !strcmp(db_cell(&rows, 0, 0), "1"));
    db_rows_free(&rows);
    /* A big value (a long manuscript) round-trips. */
    size_t big = 3u << 20;
    char *text = malloc(big + 1);
    for (size_t i = 0; i < big; i++) text[i] = "abcdefghijklmnopqrstuvwxyz \n"[i % 28];
    text[big] = '\0';
    DbArg r2[6] = { DB_S("r-2"), DB_S("d-1"), DB_S("0000000000000000"), DB_S(text), DB_S("save"), DB_D(4.0) };
    CHECK(db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES (?,?,?,?,?,?)", r2, 6,
                 NULL));
    CHECK(db_query(db, "SELECT content FROM revisions WHERE id = ?", (DbArg[1]){ DB_S("r-2") }, 1, &rows) && rows.nrows == 1 &&
          strlen(db_cell(&rows, 0, 0)) == big && !memcmp(db_cell(&rows, 0, 0), text, big));
    db_rows_free(&rows);
    free(text);
    /* NULLs come back as NULL. */
    CHECK(db_query(db, "SELECT current_revision_id FROM documents WHERE id = ?", (DbArg[1]){ DB_S("d-1") }, 1, &rows) &&
          rows.nrows == 1 && db_cell(&rows, 0, 0) == NULL);
    db_rows_free(&rows);
    /* Foreign keys hold: no revision for a document that doesn't exist. */
    DbArg bad[6] = { DB_S("r-3"), DB_S("d-missing"), DB_S("x"), DB_S("x"), DB_S("save"), DB_D(5.0) };
    CHECK(!db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES (?,?,?,?,?,?)", bad, 6,
                  NULL));
    /* Rollback undoes everything in the transaction. */
    CHECK(db_begin(db));
    DbArg d2[4] = { DB_S("d-2"), DB_S("p-1"), DB_S("ch2.md"), DB_D(6.0) };
    CHECK(db_run(db, "INSERT INTO documents (id, project_id, name, created_at) VALUES (?,?,?,?)", d2, 4, NULL));
    db_rollback(db);
    CHECK(db_query(db, "SELECT COUNT(*) FROM documents", NULL, 0, &rows) && !strcmp(db_cell(&rows, 0, 0), "1"));
    db_rows_free(&rows);
    /* A rename keeps the id (the point of documents.id). */
    uint64_t n = 0;
    CHECK(db_run(db, "UPDATE documents SET name = ?, renamed_at = ? WHERE id = ?",
                 (DbArg[3]){ DB_S("Chapter One.md"), DB_D(7.0), DB_S("d-1") }, 3, &n) && n == 1);
    CHECK(db_query(db, "SELECT COUNT(*) FROM revisions WHERE document_id = ?", (DbArg[1]){ DB_S("d-1") }, 1, &rows) &&
          !strcmp(db_cell(&rows, 0, 0), "2"));
    db_rows_free(&rows);
    /* Deleting a project cascades. */
    CHECK(db_run(db, "DELETE FROM projects WHERE id = ?", (DbArg[1]){ DB_S("p-1") }, 1, NULL));
    CHECK(db_query(db, "SELECT COUNT(*) FROM revisions", NULL, 0, &rows) && !strcmp(db_cell(&rows, 0, 0), "0"));
    db_rows_free(&rows);
    /* Wrong parameter count is refused, not executed. */
    CHECK(!db_run(db, "SELECT ?", NULL, 0, NULL));
    db_close(db);

    /* A wrong password fails cleanly, with a reason. */
    DbConfig wrong = c;
    snprintf(wrong.password, sizeof(wrong.password), "not-the-password");
    err[0] = '\0';
    CHECK(db_open(&wrong, err, sizeof(err)) == NULL && strstr(err, "Access denied"));
}

/* Stable ids for projects and documents; names compare byte for byte. */
static void test_store_ids(void)
{
    char why[256];
    CHECK(store_start(g_name, why, sizeof(why)) && store_online());
    char p1[40], p2[40], d1[40], d2[40], d3[40], d4[40], name[300];
    CHECK(store_project_id("/tmp", true, p1, sizeof(p1)) && p1[0] == 'p');
    CHECK(store_project_id("/private/tmp", false, p2, sizeof(p2)) && !strcmp(p1, p2)); /* same real path */
    CHECK(!store_project_id("/nonexistent-scribodo-dir", false, p2, sizeof(p2)));
    CHECK(store_document_id(p1, "a.md", true, d1, sizeof(d1)) && d1[0] == 'd');
    CHECK(store_document_id(p1, "a.md", true, d2, sizeof(d2)) && !strcmp(d1, d2));
    /* Case and accents are different names (case-sensitive volumes). */
    CHECK(store_document_id(p1, "A.md", true, d3, sizeof(d3)) && strcmp(d1, d3));
    CHECK(store_document_id(p1, "\xC3\xA9.md", true, d4, sizeof(d4)) && store_document_id(p1, "e.md", true, d2, sizeof(d2)) &&
          strcmp(d4, d2));
    CHECK(!store_document_id(p1, "../x.md", true, d2, sizeof(d2)));
    /* A rename keeps the id; a stale row with the new name is moved aside. */
    CHECK(store_document_id(p1, "b.md", true, d2, sizeof(d2)));
    CHECK(store_rename_document(p1, "a.md", "b.md"));
    CHECK(store_document_name(d1, name, sizeof(name)) && !strcmp(name, "b.md"));
    CHECK(store_document_name(d2, name, sizeof(name)) && strncmp(name, "b.md (earlier", 13) == 0);
    CHECK(store_document_id(p1, "b.md", false, d3, sizeof(d3)) && !strcmp(d3, d1));
    store_stop();
    store_set_offline(true);
    CHECK(!store_start(g_name, why, sizeof(why)) && !store_online() && store_db() == NULL);
    store_set_offline(false);
}

/* The store backends: revisions, records with sources, and chats, all by id. */
static void test_backends(void)
{
    char why[256];
    CHECK(store_start(g_name, why, sizeof(why)));
    Db *db = store_db();
    char pid[40];
    CHECK(db && store_project_id("/private/var", true, pid, sizeof(pid)));
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
    CHECK(store_rename_document(pid, "ch1.md", "Chapter One.md"));
    Record *recs = NULL;
    size_t nr = sdb_record_list(db, pid, REC_CANON, &recs);
    CHECK(nr == 1 && recs[0].nversions == 2 && !strcmp(record_current(&recs[0])->status, "retracted"));
    CHECK(nr == 1 && record_current(&recs[0])->nsources == 1 &&
          !strcmp(record_current(&recs[0])->sources[0].doc, "Chapter One.md") && /* follows the rename */
          !strcmp(record_current(&recs[0])->sources[0].revision, r1) && !strcmp(record_current(&recs[0])->sources[0].quote, "first"));
    records_free(recs, nr);
    nr = sdb_record_list(db, pid, REC_FINDING, &recs);
    CHECK(nr == 1 && !strcmp(record_current(&recs[0])->finding.type, "conflict") &&
          !strcmp(record_current(&recs[0])->finding.related, cid) && record_current(&recs[0])->finding.related_text &&
          !strcmp(record_current(&recs[0])->finding.related_text, "Zoë's eyes are green."));
    records_free(recs, nr);
    /* Revisions are found under the new name too (same document id). */
    n = sdb_revision_list(db, pid, "/x/Chapter One.md", &list);
    CHECK(n == 2);
    free(list);

    /* Chats: messages in order, referenced documents by id. */
    ChatMessage u = { .role = CHAT_USER, .content = "compare", .docs = "Chapter One.md\nnotes.md" };
    ChatMessage a = { .role = CHAT_ASSISTANT, .content = "They differ." };
    CHECK(sdb_chat_append(db, pid, "20261104-150000-abcd", "Compare", &u) && sdb_chat_append(db, pid, "20261104-150000-abcd", "x", &a));
    CHECK(store_rename_document(pid, "notes.md", "Notes.md"));
    ChatMessage *msgs = NULL;
    size_t nm = 0;
    CHECK(sdb_chat_load(db, pid, "20261104-150000-abcd", &msgs, &nm) && nm == 2 && msgs[0].role == CHAT_USER &&
          !strcmp(msgs[1].content, "They differ.") && msgs[0].docs && !strcmp(msgs[0].docs, "Chapter One.md\nNotes.md"));
    chat_store_free_messages(msgs, nm);
    CHECK(sdb_chat_rename(db, pid, "20261104-150000-abcd", "Comparing chapters"));
    ChatSummary *cs = NULL;
    size_t nc = sdb_chat_list(db, pid, &cs);
    CHECK(nc == 1 && !strcmp(cs[0].title, "Comparing chapters"));
    free(cs);
    /* Another project can't touch this one's chat. */
    char pid2[40];
    CHECK(store_project_id("/private/etc", true, pid2, sizeof(pid2)));
    CHECK(!sdb_chat_append(db, pid2, "20261104-150000-abcd", "x", &a) && !sdb_chat_rename(db, pid2, "20261104-150000-abcd", "x"));
    CHECK(sdb_chat_load(db, pid2, "20261104-150000-abcd", &msgs, &nm) && nm == 0);
    chat_store_free_messages(msgs, nm);
    store_stop();
}

/* A project's files moved into the store keep everything: revisions,
 * chats, notes, canon, findings with sources, renames, observer state. */
static void test_import(void)
{
    char why[256];
    CHECK(store_start(g_name, why, sizeof(why)));
    char proj[512], doc[600];
    snprintf(proj, sizeof(proj), "%s/scribodo-import-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(proj, 0755);
    snprintf(doc, sizeof(doc), "%s/ch1.md", proj);
    FILE *f = fopen(doc, "wb");
    fputs("Zoë kept the letter.\n", f);
    fclose(f);
    /* Not in the store yet: everything below goes to files. */
    CHECK(store_mode(proj, NULL, 0, NULL) == STORE_FILES);
    bool rec;
    CHECK(revisions_record(proj, doc, "first draft", "save", NULL, &rec) && rec);
    char snap[160];
    CHECK(revisions_snapshot(proj, doc, "Zoë kept the letter.\n", snap, sizeof(snap)));
    SourceRef src = { .kind = SRC_DOC, .start = 0, .len = 3, .quote = "Zoë" };
    snprintf(src.doc, sizeof(src.doc), "ch1.md");
    snprintf(src.revision, sizeof(src.revision), "%s", snap);
    char nid[40], cid[40];
    records_new_id(REC_NOTE, nid, sizeof(nid));
    records_new_id(REC_CANON, cid, sizeof(cid));
    CHECK(records_append(proj, REC_NOTE, nid, "active", "Check the date.", &src, 1, NULL));
    CHECK(records_append(proj, REC_NOTE, nid, "active", "Check the date (1911?).", &src, 1, NULL));
    CHECK(records_append(proj, REC_CANON, cid, "committed", "Zoë's eyes are green.", NULL, 0, NULL));
    ChatMessage u = { .role = CHAT_USER, .content = "hello", .docs = "ch1.md" };
    ChatMessage a = { .role = CHAT_ASSISTANT, .content = "hi" };
    CHECK(chat_store_append(proj, "20261104-160000-abcd", "Greeting", &u) && chat_store_append(proj, "20261104-160000-abcd", "x", &a));
    /* A rename made before the import. */
    char doc2[600];
    snprintf(doc2, sizeof(doc2), "%s/Chapter One.md", proj);
    CHECK(rename(doc, doc2) == 0);
    char rv_old[700], rv_new[700];
    snprintf(rv_old, sizeof(rv_old), "%s/.scribodo/revisions/ch1.md", proj);
    snprintf(rv_new, sizeof(rv_new), "%s/.scribodo/revisions/Chapter One.md", proj);
    CHECK(rename(rv_old, rv_new) == 0);
    CHECK(renames_record(proj, "ch1.md", "Chapter One.md"));

    why[0] = '\0';
    CHECK(store_import(proj, why, sizeof(why)));
    if (why[0]) fprintf(stderr, "    %s\n", why);
    CHECK(store_mode(proj, NULL, 0, NULL) == STORE_DB);
    CHECK(store_import(proj, why, sizeof(why))); /* again: nothing to do */
    /* The same APIs now read the store. */
    Revision *revs = NULL;
    size_t nr = revisions_list(proj, doc2, &revs);
    CHECK(nr == 2);
    char *t = nr ? revisions_load(proj, doc2, &revs[nr - 1]) : NULL;
    CHECK(t && !strcmp(t, "first draft"));
    free(t);
    free(revs);
    Record *notes = NULL;
    size_t nn = records_list(proj, REC_NOTE, &notes);
    CHECK(nn == 1 && notes[0].nversions == 2 && !strcmp(record_current(&notes[0])->content, "Check the date (1911?)."));
    CHECK(nn == 1 && record_current(&notes[0])->nsources == 1 &&
          !strcmp(record_current(&notes[0])->sources[0].doc, "Chapter One.md") && /* the rename, resolved to the id */
          record_current(&notes[0])->sources[0].revision[0] == 'r');
    records_free(notes, nn);
    ChatMessage *msgs = NULL;
    size_t nm = 0;
    CHECK(chat_store_load(proj, "20261104-160000-abcd", &msgs, &nm) && nm == 2 && msgs[0].docs &&
          !strcmp(msgs[0].docs, "Chapter One.md"));
    chat_store_free_messages(msgs, nm);
    /* New writes go to the store; the old files were set aside, not deleted. */
    CHECK(records_append(proj, REC_NOTE, nid, "active", "v3", NULL, 0, NULL));
    nn = records_list(proj, REC_NOTE, &notes);
    CHECK(nn == 1 && notes[0].nversions == 3);
    records_free(notes, nn);
    char legacy_glob[700];
    snprintf(legacy_glob, sizeof(legacy_glob), "ls -d '%s'/.scribodo/legacy-*/notes.jsonl >/dev/null 2>&1", proj);
    CHECK(system(legacy_glob) == 0);
    char notes_path[700];
    snprintf(notes_path, sizeof(notes_path), "%s/.scribodo/notes.jsonl", proj);
    struct stat st;
    CHECK(stat(notes_path, &st) != 0);
    /* With the database down, a moved project pauses instead of using files. */
    store_stop();
    CHECK(store_mode(proj, NULL, 0, NULL) == STORE_PAUSED);
    CHECK(!records_append(proj, REC_NOTE, nid, "active", "while paused", NULL, 0, NULL));
    CHECK(records_list(proj, REC_NOTE, &notes) == 0);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", proj);
    system(cmd);
}

int main(void)
{
    snprintf(g_name, sizeof(g_name), "scribodo_test_%d", (int)getpid());
    if (!admin("CREATE DATABASE %s CHARACTER SET utf8mb4") || !admin("GRANT ALL ON %s.* TO 'scribodo'@'%%'")) {
        fprintf(stderr, "dbtest: can't set up a test database (is tools/services.sh up?)\n");
        return 1;
    }
    TestCase tests[] = { { "db", test_db }, { "store_ids", test_store_ids }, { "backends", test_backends }, { "import", test_import } };
    int rc = run_tests(tests, 4);
    admin("DROP DATABASE %s");
    return rc;
}
