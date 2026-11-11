/* Core MariaDB behavior and migration tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/db.h"
#include "test_db_internal.h"
#include "test_util.h"

void test_db(void)
{
    CHECK(db_socket(NULL) == -1);
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
    CHECK(db_socket(db) >= 0);
    bool migrated = db_migrate(db, err, sizeof(err));
    if (!migrated) fprintf(stderr, "    %s\n", err);
    CHECK(migrated && db_schema_version(db) == 15);
    CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 15); /* idempotent */

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
void test_migration(void)
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
        CHECK(db_run(db, "INSERT INTO conversations (id,project_id,title,created_at,updated_at) VALUES ('c-1','p-1','old',1,1)", NULL, 0, NULL));
        CHECK(db_run(db, "INSERT INTO messages (conversation_id,seq,role,content,created_at) VALUES ('c-1',0,'assistant','old partial',1)", NULL, 0, NULL));
        /* Inserted out of time order: seq must follow time for old rows. */
        CHECK(db_run(db, "INSERT INTO revisions (id, document_id, hash, content, origin, created_at) VALUES"
                         " ('r-b','d-1','h2','second','save',20), ('r-a','d-1','h1','first','save',10)",
                     NULL, 0, NULL));
        CHECK(db_migrate_upto(db, 8, err, sizeof(err)) && db_schema_version(db) == 8);
        bool migrated = db_migrate_upto(db, 9, err, sizeof(err));
        if (!migrated) fprintf(stderr, "    %s\n", err);
        CHECK(migrated && db_schema_version(db) == 9);
        DbRows r;
        CHECK(db_query(db, "SELECT source_hash=SHA2(content,256) AND memory_sources IS NULL FROM messages WHERE conversation_id='c-1'",
                       NULL, 0, &r) && r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "1"));
        db_rows_free(&r);
        CHECK(db_run(db, "INSERT INTO memory_scopes(id,project_id,conversation_id) VALUES ('default-scope','p-1','c-1')",
                     NULL, 0, NULL));
        CHECK(db_query(db, "SELECT enabled=0 AND history_epoch=1 AND policy_epoch=1 AND generation=1"
                          " AND cursor_seq=-1 AND byte_offset=0 AND pending_seq=-1 AND tokenizer IS NULL"
                          " AND fence=0 AND attempts=0 AND lease_until IS NULL AND retry_after IS NULL"
                          " FROM memory_scopes WHERE id='default-scope'", NULL, 0, &r) &&
              r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "1"));
        db_rows_free(&r);
        CHECK(db_run(db, "UPDATE messages SET content='changed after migration' WHERE conversation_id='c-1'",
                     NULL, 0, NULL));
        CHECK(db_query(db, "SELECT source_hash=SHA2('changed after migration',256) FROM messages WHERE conversation_id='c-1'",
                       NULL, 0, &r) && r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "1"));
        db_rows_free(&r);
        CHECK(db_query(db, "SELECT stopped FROM messages WHERE conversation_id='c-1'", NULL, 0, &r) &&
              r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "0"));
        db_rows_free(&r);
        CHECK(db_run(db, "UPDATE messages SET stopped=1 WHERE conversation_id='c-1'", NULL, 0, NULL));
        /* Simulate a crash after migration 009's idempotent DDL but before its
         * migration receipt. Reapplying must preserve existing rows/defaults. */
        CHECK(db_run(db, "DELETE FROM schema_migrations WHERE version=9", NULL, 0, NULL));
        CHECK(db_migrate_upto(db, 9, err, sizeof(err)) && db_schema_version(db) == 9);
        CHECK(db_run(db, "UPDATE documents SET current_revision_id='r-b' WHERE id='d-1'", NULL, 0, NULL));
        CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 15);
        CHECK(db_query(db, "SELECT saved_revision_id IS NULL AND current_revision_id='r-b' FROM documents WHERE id='d-1'",
                       NULL, 0, &r) && r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "1"));
        db_rows_free(&r);
        CHECK(db_query(db, "SELECT COUNT(*) FROM revisions WHERE source_hash=SHA2(content,256)", NULL, 0, &r) &&
              r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "2"));
        db_rows_free(&r);
        CHECK(db_run(db, "DELETE FROM schema_migrations WHERE version>=10", NULL, 0, NULL));
        CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 15);
        CHECK(db_run(db, "DELETE FROM schema_migrations WHERE version>=11", NULL, 0, NULL));
        CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 15);
        CHECK(db_run(db, "DELETE FROM schema_migrations WHERE version=12", NULL, 0, NULL));
        CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 15);
        CHECK(db_query(db, "SELECT COUNT(*) FROM memory_retired_namespaces", NULL, 0, &r) &&
              r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "0"));
        db_rows_free(&r);
        CHECK(db_query(db, "SELECT stopped FROM messages WHERE conversation_id='c-1'", NULL, 0, &r) &&
              r.nrows == 1 && !strcmp(db_cell(&r, 0, 0), "1"));
        db_rows_free(&r);
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

int run_db_core_tests(void)
{
    const TestCase tests[] = { { "db", test_db }, { "migration", test_migration } };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
