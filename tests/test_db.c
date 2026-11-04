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
#include "../src/app/store_entities.h"
#include "../src/llm/semantic.h"
#include <fcntl.h>
#include <sys/file.h>
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
    bool migrated = db_migrate(db, err, sizeof(err));
    if (!migrated) fprintf(stderr, "    %s\n", err);
    CHECK(migrated && db_schema_version(db) == 4);
    CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 4); /* idempotent */

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
/* Migration 003 on a database that already has data (red-team 008):
 * History gets an insertion order that holds when times tie, and names that
 * differ only by trailing spaces are different names. */
static void test_migration(void)
{
    char saved[64], err[512] = "";
    snprintf(saved, sizeof(saved), "%s", g_name);
    snprintf(g_name, sizeof(g_name), "%s_m", saved);
    CHECK(admin("CREATE DATABASE %s CHARACTER SET utf8mb4") && admin("GRANT ALL ON %s.* TO 'scribodo'@'%%'"));
    DbConfig c;
    CHECK(db_config_default(&c, err, sizeof(err)));
    snprintf(c.database, sizeof(c.database), "%s", g_name);
    Db *db = db_open(&c, err, sizeof(err));
    CHECK(db != NULL);
    if (db) {
        CHECK(db_migrate_upto(db, 2, err, sizeof(err)) && db_schema_version(db) == 2);
        CHECK(db_run(db, "INSERT INTO projects (id, name, path, path_hash, created_at, updated_at) VALUES ('p-1','b','/b','h',1,1)",
                     NULL, 0, NULL));
        CHECK(db_run(db, "INSERT INTO documents (id, project_id, name, created_at) VALUES ('d-1','p-1','ch.md',1)", NULL, 0, NULL));
        /* Inserted out of time order: seq must follow time for old rows. */
        CHECK(db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES"
                         " ('r-b','d-1','h2','second','save',20), ('r-a','d-1','h1','first','save',10)",
                     NULL, 0, NULL));
        bool migrated = db_migrate(db, err, sizeof(err));
        if (!migrated) fprintf(stderr, "    %s\n", err);
        CHECK(migrated && db_schema_version(db) == 4);
        DbRows r;
        CHECK(db_query(db, "SELECT id FROM revisions ORDER BY seq", NULL, 0, &r) && r.nrows == 2 &&
              !strcmp(db_cell(&r, 0, 0), "r-a") && !strcmp(db_cell(&r, 1, 0), "r-b"));
        db_rows_free(&r);
        /* New rows come after, even with an older (or equal) time. */
        CHECK(db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES"
                         " ('r-c','d-1','h3','third','save',5)",
                     NULL, 0, NULL));
        CHECK(db_query(db, "SELECT id FROM revisions ORDER BY seq DESC LIMIT 1", NULL, 0, &r) && r.nrows == 1 &&
              !strcmp(db_cell(&r, 0, 0), "r-c"));
        db_rows_free(&r);
        /* Trailing spaces are significant now. */
        CHECK(db_run(db, "INSERT INTO documents (id, project_id, name, created_at) VALUES ('d-2','p-1','ch.md ',1)", NULL, 0,
                     NULL));
        CHECK(db_query(db, "SELECT id FROM documents WHERE name = ?", (DbArg[1]){ DB_S("ch.md") }, 1, &r) && r.nrows == 1 &&
              !strcmp(db_cell(&r, 0, 0), "d-1"));
        db_rows_free(&r);
        /* Foreign keys still hold after the change. */
        CHECK(!db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES"
                          " ('r-x','d-missing','h','x','save',1)",
                      NULL, 0, NULL));
        db_close(db);
    }
    admin("DROP DATABASE %s");
    snprintf(g_name, sizeof(g_name), "%s", saved);
}

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
    /* A connection the server drops (restart, timeout) is noticed and
     * reopened on next use (red-team 008). */
    Db *db = store_db();
    DbRows r;
    CHECK(db && db_query(db, "SELECT CONNECTION_ID()", NULL, 0, &r) && r.nrows == 1);
    char kill_sql[64];
    snprintf(kill_sql, sizeof(kill_sql), "KILL CONNECTION %s", r.nrows ? db_cell(&r, 0, 0) : "0");
    db_rows_free(&r);
    CHECK(admin(kill_sql));
    CHECK(!db_query(db, "SELECT 1", NULL, 0, &r) && db_broken(db));
    Db *again = store_db();
    CHECK(again && store_online() && db_query(again, "SELECT 1", NULL, 0, &r) && r.nrows == 1);
    if (again) db_rows_free(&r);
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
    /* History order is the order recorded, whatever the clock said (two
     * saves in one millisecond, a clock stepped back). */
    CHECK(db_run(db, "UPDATE revisions SET created_at = 100 WHERE id = ?", (DbArg[1]){ DB_S(r2) }, 1, NULL));
    CHECK(db_run(db, "UPDATE revisions SET created_at = 200 WHERE id = ?", (DbArg[1]){ DB_S(r1) }, 1, NULL));
    n = sdb_revision_list(db, pid, "/x/ch1.md", &list);
    CHECK(n == 2 && !strcmp(list[0].origin, "ai-edit")); /* r2, recorded last, is still the newest */
    free(list);
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

    /* Another project's ids are never linked (red-team 008): its revision,
     * its chat and its records come through as plain quotes. */
    char other[40];
    CHECK(store_project_id("/private/var/empty", true, other, sizeof(other)));
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
    ChatMessage a = { .role = CHAT_ASSISTANT, .content = "They differ." };
    CHECK(sdb_chat_append(db, pid, "20261104-150000-abcd", "Compare", &u) && sdb_chat_append(db, pid, "20261104-150000-abcd", "x", &a));
    CHECK(store_rename_document(pid, "notes.md", "Notes.md"));
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
    size_t n = records_list(proj, REC_NOTE, &r);
    records_free(r, n);
    return n;
}

static void test_import_safety(void)
{
    char why[256];
    CHECK(store_start(g_name, why, sizeof(why)));
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
    CHECK(records_append(a, REC_NOTE, nid, "active", "original note", NULL, 0, NULL));
    CHECK(revisions_record(a, doc, "One.\n", "save", NULL, NULL));

    /* Copied before either was moved into the store: both import the same
     * snapshot files, and each must get its own History. */
    char d0[512];
    snprintf(d0, sizeof(d0), "%s/scribodo-safe-d-%d", tmp, (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && cp -R '%s' '%s'", d0, a, d0);
    CHECK(system(cmd) == 0);

    /* Another importer holds the lock: refused, and writers see it paused. */
    char lockp[700];
    snprintf(lockp, sizeof(lockp), "%s/.scribodo/import.lock", a);
    int fd = open(lockp, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    CHECK(store_mode(a, NULL, 0, NULL) == STORE_PAUSED);
    CHECK(!records_append(a, REC_NOTE, nid, "active", "lost?", NULL, 0, NULL));
    CHECK(!store_import(a, why, sizeof(why)) && strstr(why, "another window"));
    flock(fd, LOCK_UN);
    close(fd);
    CHECK(store_mode(a, NULL, 0, NULL) == STORE_FILES);

    /* A stale row for this path (an earlier project here, marker lost) is
     * superseded, not reused. */
    char stale[40], pa[40];
    CHECK(store_project_id(a, true, stale, sizeof(stale)));
    CHECK(store_import(a, why, sizeof(why)));
    CHECK(store_mode(a, pa, sizeof(pa), NULL) == STORE_DB && strcmp(pa, stale) != 0);
    CHECK(note_count(a) == 1);
    CHECK(store_import(d0, why, sizeof(why)));
    char pd[40], docd[700];
    CHECK(store_mode(d0, pd, sizeof(pd), NULL) == STORE_DB && strcmp(pd, pa) != 0);
    snprintf(docd, sizeof(docd), "%s/ch1.md", d0);
    Revision *drevs = NULL;
    CHECK(revisions_list(d0, docd, &drevs) == 1);
    free(drevs);
    drevs = NULL;
    CHECK(revisions_list(a, doc, &drevs) == 1);
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
    store_forget_verification();
    char orig[1024];
    CHECK(store_mode(b, NULL, 0, NULL) == STORE_PAUSED);
    CHECK(store_problem(b, orig, sizeof(orig)) == STORE_PROBLEM_COPY);
    CHECK(store_fork(b, why, sizeof(why)));
    char pb[40];
    CHECK(store_mode(b, pb, sizeof(pb), NULL) == STORE_DB && strcmp(pa, pb) != 0);
    CHECK(note_count(b) == 1);
    Record *r = NULL;
    size_t n = records_list(b, REC_NOTE, &r);
    CHECK(n == 1 && strcmp(r[0].id, nid) != 0 && !strcmp(record_current(&r[0])->content, "original note"));
    char bnid[40];
    snprintf(bnid, sizeof(bnid), "%s", n ? r[0].id : "");
    records_free(r, n);
    CHECK(records_append(b, REC_NOTE, bnid, "active", "copy's edit", NULL, 0, NULL));
    n = records_list(a, REC_NOTE, &r);
    CHECK(n == 1 && !strcmp(record_current(&r[0])->content, "original note"));
    records_free(r, n);
    char docb[700];
    snprintf(docb, sizeof(docb), "%s/ch1.md", b);
    Revision *revs = NULL;
    n = revisions_list(b, docb, &revs);
    CHECK(n == 1);
    free(revs);
    store_forget_verification();
    CHECK(store_mode(a, NULL, 0, NULL) == STORE_DB); /* the original is still itself */

    /* Moved to another volume (or restored from a backup): a new inode, but
     * the original is gone, so it is the project, not a copy. */
    char c[512];
    snprintf(c, sizeof(c), "%s/scribodo-safe-c-%d", tmp, (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && cp -R '%s' '%s' && rm -rf '%s'", c, b, c, b);
    CHECK(system(cmd) == 0);
    store_forget_verification();
    char pc[40];
    CHECK(store_mode(c, pc, sizeof(pc), NULL) == STORE_DB && !strcmp(pc, pb));
    CHECK(store_problem(c, NULL, 0) == 0);
    n = records_list(c, REC_NOTE, &r);
    CHECK(n == 1 && !strcmp(record_current(&r[0])->content, "copy's edit"));
    records_free(r, n);
    snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", c, b); /* and a copy of it again is a copy */
    CHECK(system(cmd) == 0);
    store_forget_verification();
    CHECK(store_problem(b, NULL, 0) == STORE_PROBLEM_COPY);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", c);
    system(cmd);

    /* The project's rows disappear (store reset): paused as missing, then
     * restored from the set-aside files. */
    Db *db = store_db();
    CHECK(db_run(db, "DELETE FROM projects WHERE id = ?", (DbArg[1]){ DB_S(pa) }, 1, NULL));
    store_forget_verification();
    CHECK(store_mode(a, NULL, 0, NULL) == STORE_PAUSED && store_problem(a, NULL, 0) == STORE_PROBLEM_MISSING);
    CHECK(store_restore(a, why, sizeof(why)));
    char pa2[40];
    CHECK(store_mode(a, pa2, sizeof(pa2), NULL) == STORE_DB && strcmp(pa2, pa) != 0);
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

    CHECK(store_start(g_name, why, sizeof(why)));
    Db *db = store_db();
    char pid[40], other[40];
    CHECK(store_project_id("/private/var/db", true, pid, sizeof(pid)));
    CHECK(store_project_id("/private/var/log", true, other, sizeof(other)));
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
    CHECK(store_project_id("/private/var/run", true, p3, sizeof(p3)));
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
    store_stop();
}

/* Semantic retrieval against the local Qdrant (fake embedder): committed
 * canon only, kinds kept apart, and a wiped index rebuilt from MariaDB with
 * exact provenance (SOW acceptance 10). */
static void test_semantic(void)
{
    char why[256];
    CHECK(store_start(g_name, why, sizeof(why)));
    semantic_set_embedder(NULL);
    CHECK(semantic_ready());
    char pid[40];
    CHECK(store_project_id("/private/var/tmp", true, pid, sizeof(pid)));
    semantic_drop(pid);
    Db *db = store_db();
    char c1[40], c2[40], c3[40], n1[40];
    store_new_id('c', c1, sizeof(c1));
    store_new_id('c', c2, sizeof(c2));
    store_new_id('c', c3, sizeof(c3));
    store_new_id('n', n1, sizeof(n1));
    CHECK(sdb_record_append(db, pid, REC_CANON, c1, "committed", "Zoë's eyes are green.", NULL, 0, NULL, true, NULL));
    CHECK(sdb_record_append(db, pid, REC_CANON, c2, "committed", "The lighthouse stands on Ytre Sula.", NULL, 0, NULL, true, NULL));
    CHECK(sdb_record_append(db, pid, REC_CANON, c3, "retracted", "Dvořák is Zoë's uncle.", NULL, 0, NULL, true, NULL));
    CHECK(sdb_record_append(db, pid, REC_NOTE, n1, "active", "Zoë's eyes might be grey in chapter two.", NULL, 0, NULL, true, NULL));
    CHECK(semantic_sync_records(pid, why, sizeof(why)));
    if (why[0]) fprintf(stderr, "    sync: %s\n", why);
    SemHit *h = NULL;
    size_t n = semantic_canon_for(pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].record_id, c1) && !strcmp(h[0].kind, "canon"));
    bool leaked = false;
    for (size_t i = 0; i < n; i++) leaked = leaked || !strcmp(h[i].record_id, c3) || !strcmp(h[i].record_id, n1);
    CHECK(!leaked); /* retracted canon and notes are never canon */
    semantic_hits_free(h, n);
    /* Retracting moves an entry out of canon retrieval. */
    CHECK(sdb_record_append(db, pid, REC_CANON, c1, "retracted", "Zoë's eyes are green.", NULL, 0, NULL, false, NULL));
    CHECK(semantic_sync_records(pid, why, sizeof(why)));
    n = semantic_canon_for(pid, "Zoë's brown eyes narrowed.", 3, &h);
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
    long count = semantic_rebuild(pid, why, sizeof(why));
    CHECK(count >= 6); /* 4 records + 2 paragraphs */
    n = semantic_search(pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].kind, "draft") && !strcmp(h[0].revision_id, rv) && h[0].start == 18 && h[0].len == 34 &&
          !strcmp(h[0].text, "Zoë climbed the lighthouse stairs."));
    semantic_hits_free(h, n);
    /* Wipe the index; retrieval is empty; a rebuild restores it exactly. */
    CHECK(semantic_drop(pid));
    n = semantic_canon_for(pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n == 0);
    semantic_hits_free(h, n);
    CHECK(semantic_rebuild(pid, why, sizeof(why)) == count);
    n = semantic_canon_for(pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].record_id, c1) && !strcmp(h[0].status, "committed"));
    semantic_hits_free(h, n);
    n = semantic_search(pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].revision_id, rv) && h[0].start == 18);
    semantic_hits_free(h, n);
    /* Syncing again does no work (everything is marked). */
    DbRows r;
    CHECK(db_query(db, "SELECT COUNT(*) FROM semantic_marks WHERE project_id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r) &&
          atol(db_cell(&r, 0, 0)) == count);
    db_rows_free(&r);
    CHECK(semantic_sync_records(pid, why, sizeof(why)));
    /* The index lost but its marks kept (restored under another database,
     * Qdrant wiped): the next sync rebuilds everything, documents too. */
    CHECK(semantic_forget_collection(pid));
    CHECK(semantic_sync_records(pid, why, sizeof(why)));
    n = semantic_search(pid, "lighthouse stairs", "draft", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].revision_id, rv) && h[0].start == 18);
    semantic_hits_free(h, n);
    n = semantic_canon_for(pid, "Zoë's brown eyes narrowed.", 3, &h);
    CHECK(n >= 1 && !strcmp(h[0].record_id, c1));
    semantic_hits_free(h, n);
    semantic_drop(pid);
    store_stop();
}

int main(void)
{
    snprintf(g_name, sizeof(g_name), "scribodo_test_%d", (int)getpid());
    if (!admin("CREATE DATABASE %s CHARACTER SET utf8mb4") || !admin("GRANT ALL ON %s.* TO 'scribodo'@'%%'")) {
        fprintf(stderr, "dbtest: can't set up a test database (is tools/services.sh up?)\n");
        return 1;
    }
    TestCase tests[] = { { "db", test_db }, { "migration", test_migration }, { "store_ids", test_store_ids }, { "backends", test_backends }, { "import", test_import }, { "import_safety", test_import_safety }, { "entities", test_entities }, { "semantic", test_semantic } };
    int rc = run_tests(tests, 8);
    admin("DROP DATABASE %s");
    return rc;
}
