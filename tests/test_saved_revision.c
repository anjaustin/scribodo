/* Real disposable MariaDB; no model, editor, or memory ingestion. */
#define _DARWIN_C_SOURCE
#include "app/store_backends.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Db *db;

static int64_t scalar(const char *sql)
{
    DbRows rows;
    int64_t value = -1;
    CHECK(db_query(db, sql, NULL, 0, &rows));
    CHECK(rows.nrows == 1 && db_cell_i64(&rows, 0, 0, &value));
    db_rows_free(&rows);
    return value;
}

static void absent(Db *connection, const char *project, const char *name)
{
    SavedRevisionIdentity identity, zero = {0};
    memset(&identity, 0x7f, sizeof(identity));
    CHECK(!sdb_saved_revision_identity(connection, project, name, &identity));
    CHECK(!memcmp(&identity, &zero, sizeof(identity)));
}

static void invalid_arguments(void)
{
    absent(NULL, "p", "a.md");
    CHECK(!sdb_saved_revision_identity(NULL, NULL, NULL, NULL));
}

static void record(const char *name, const char *text, const char *origin, bool any_match, bool expected, char id[40])
{
    bool recorded = !expected;
    bool ok = sdb_revision_record(db, "p", name, text, origin, "", any_match, &recorded, id, 40);
    if (!ok) fprintf(stderr, "record: %s\n", db_error(db));
    CHECK(ok && recorded == expected && id[0]);
}

static void saved(const char *name, const char *id, uint64_t bytes)
{
    SavedRevisionIdentity identity;
    CHECK(sdb_saved_revision_identity(db, "p", name, &identity));
    CHECK(!strcmp(identity.project_id, "p") && !strcmp(identity.revision_id, id));
    CHECK(identity.bytes == bytes && strlen(identity.source_hash) == 64);
    DbRows rows;
    CHECK(db_query(db, "SELECT DATABASE(),source_hash FROM revisions WHERE id=?",
                   (DbArg[]){ DB_S(id) }, 1, &rows));
    CHECK(rows.nrows == 1 && !strcmp(identity.database, db_cell(&rows, 0, 0)) &&
          !strcmp(identity.source_hash, db_cell(&rows, 0, 1)));
    db_rows_free(&rows);
}

static void receipts(void)
{
    CHECK(db_run(db, "INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES"
                     " ('p','Book','/saved-test','saved-test',1,1),('other','Other','/other','other',1,1)", NULL, 0, NULL));
    absent(db, "p", "missing.md");
    CHECK(scalar("SELECT COUNT(*) FROM documents") == 0);
    char doc[40], first[40], ai[40], newer[40], reused[40];
    CHECK(store_document_id_db(db, "p", "empty.md", true, doc, sizeof(doc)));
    absent(db, "p", "empty.md");
    CHECK(scalar("SELECT COUNT(*) FROM documents WHERE saved_revision_id IS NOT NULL") == 0);
    record("first.md", "first", "save", false, true, first);
    saved("first.md", first, 5);
    record("source-save.md", "source", "source", true, true, first);
    absent(db, "p", "source-save.md");
    record("source-save.md", "source", "save", false, false, reused);
    CHECK(!strcmp(first, reused));
    saved("source-save.md", first, 6);
    CHECK(scalar("SELECT COUNT(*) FROM revisions WHERE content='source' AND origin='source'") == 1);
    record("a.md", "snapshot", "source", true, true, first);
    absent(db, "p", "a.md");
    record("a.md", "café 🐋", "ai-edit", false, true, ai);
    absent(db, "p", "a.md");
    record("a.md", "café 🐋", "save", false, false, reused);
    CHECK(!strcmp(ai, reused));
    saved("a.md", ai, strlen("café 🐋"));
    CHECK(scalar("SELECT COUNT(*) FROM revisions WHERE origin='ai-edit'") == 1);
    record("a.md", "unsaved", "restore", false, true, newer);
    saved("a.md", ai, strlen("café 🐋"));
    /* A failed filesystem save makes no DB call, hence cannot move authority. */
    saved("a.md", ai, strlen("café 🐋"));
    record("a.md", "unsaved", "save", false, false, reused);
    CHECK(!strcmp(newer, reused));
    saved("a.md", newer, 7);
    record("a.md", "snapshot", "save", true, false, reused);
    CHECK(!strcmp(first, reused)); /* even a save-labelled snapshot is not a receipt */
    saved("a.md", newer, 7);
    CHECK(scalar("SELECT COUNT(*) FROM documents WHERE name='a.md' AND current_revision_id=saved_revision_id") == 1);
    absent(db, "other", "a.md");
    CHECK(store_document_id_db(db, "other", "a.md", true, doc, sizeof(doc)));
    CHECK(!db_run(db, "UPDATE documents SET saved_revision_id=? WHERE project_id='other'",
                  (DbArg[]){ DB_S(newer) }, 1, NULL));
    absent(db, "other", "a.md");
    CHECK(!db_run(db, "UPDATE documents SET saved_revision_id=? WHERE name='first.md'",
                  (DbArg[]){ DB_S(newer) }, 1, NULL));
    CHECK(!db_run(db, "UPDATE documents SET saved_revision_id='missing' WHERE name='a.md'", NULL, 0, NULL));
    CHECK(!db_run(db, "DELETE FROM revisions WHERE id=?", (DbArg[]){ DB_S(newer) }, 1, NULL));

    absent(db, NULL, "a.md");
    absent(db, "", "a.md");
    absent(db, "p\n", "a.md");
    absent(db, "p", NULL);
    const char *bad[] = { "", ".", "..", "../a.md", "/a.md", "a/b.md", "a\\b.md", "a..md", "a\n.md", "a\177.md" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) absent(db, "p", bad[i]);
    char oversized[300]; memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
    absent(db, oversized, "a.md");
    absent(db, "p", oversized);
    CHECK(scalar("SELECT COUNT(*) FROM documents") == 5);

    /* Publication failure rolls both inserted source and saved receipt back. */
    int64_t count = scalar("SELECT COUNT(*) FROM revisions");
    CHECK(db_run(db, "CREATE TRIGGER saved_test_failure BEFORE UPDATE ON documents FOR EACH ROW"
                     " BEGIN IF NOT (NEW.saved_revision_id <=> OLD.saved_revision_id) THEN"
                     " SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected saved receipt failure'; END IF; END", NULL, 0, NULL));
    bool recorded = true;
    char failed[40] = "not published";
    CHECK(!sdb_revision_record(db, "p", "a.md", "must rollback", "save", "", false,
                               &recorded, failed, sizeof(failed)) && !recorded && !failed[0]);
    CHECK(scalar("SELECT COUNT(*) FROM revisions") == count);
    saved("a.md", newer, 7);
    CHECK(scalar("SELECT COUNT(*) FROM documents WHERE name='a.md' AND current_revision_id=saved_revision_id") == 1);
    record("a.md", "pending", "ai-edit", false, true, ai);
    CHECK(!sdb_revision_record(db, "p", "a.md", "pending", "save", "", false,
                               &recorded, failed, sizeof(failed)) && !recorded && !failed[0]);
    saved("a.md", newer, 7);
    CHECK(db_run(db, "DROP TRIGGER saved_test_failure", NULL, 0, NULL));
    record("a.md", "pending", "save", false, false, reused);
    saved("a.md", ai, 7);
    /* Renaming changes lookup, not the stable identity. */
    CHECK(db_run(db, "UPDATE documents SET name='renamed.md' WHERE name='a.md'", NULL, 0, NULL));
    absent(db, "p", "a.md");
    saved("renamed.md", ai, 7);
    /* Explicitly release receipts before deleting their revision owners. */
    CHECK(!db_run(db, "DELETE FROM projects WHERE id='p'", NULL, 0, NULL));
    CHECK(db_run(db, "UPDATE documents SET saved_revision_id=NULL WHERE project_id='p'", NULL, 0, NULL));
    CHECK(db_run(db, "DELETE FROM projects WHERE id='p'", NULL, 0, NULL));
    CHECK(scalar("SELECT COUNT(*) FROM revisions") == 0);
}

static void hash_collision(void)
{
    char original[40], candidate[40], reused[40];
    /* Use the remaining project with a fresh project id alias for record(). */
    CHECK(db_run(db, "INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES"
                     " ('p','Book','/collision','collision',1,1)", NULL, 0, NULL));
    record("collision.md", "wrong original", "source", true, true, original);
    record("hash.md", "candidate", "source", true, true, candidate);
    CHECK(db_run(db, "UPDATE revisions old JOIN revisions candidate ON candidate.id=? SET old.hash=candidate.hash WHERE old.id=?",
                 (DbArg[]){ DB_S(candidate), DB_S(original) }, 2, NULL));
    record("collision.md", "candidate", "save", false, true, reused);
    CHECK(strcmp(original, reused));
    saved("collision.md", reused, 9);
    /* The any-match path must also reject the colliding historical row. */
    record("snapshot.md", "wrong original", "source", true, true, original);
    CHECK(db_run(db, "UPDATE revisions old JOIN revisions candidate ON candidate.id=? SET old.hash=candidate.hash WHERE old.id=?",
                 (DbArg[]){ DB_S(candidate), DB_S(original) }, 2, NULL));
    record("snapshot.md", "candidate", "source", true, true, reused);
    CHECK(strcmp(original, reused));
    absent(db, "p", "snapshot.md");
    /* SQL permits 40-byte imported ids, but the bounded identity DTO must not
     * silently truncate one into an apparently valid authority. */
    CHECK(db_run(db, "INSERT INTO documents(id,project_id,name,created_at) VALUES ('malformed','p','malformed.md',1)", NULL, 0, NULL));
    CHECK(db_run(db, "INSERT INTO revisions(id,document_id,hash,content,origin,created_at) VALUES"
                     " (REPEAT('r',40),'malformed','old','imported','save',1)", NULL, 0, NULL));
    CHECK(db_run(db, "UPDATE documents SET saved_revision_id=REPEAT('r',40) WHERE id='malformed'", NULL, 0, NULL));
    absent(db, "p", "malformed.md");
}

int main(void)
{
    invalid_arguments();
    char name[64], command[256], error[512] = "";
    snprintf(name, sizeof(name), "scribodo_test_saved_%ld", (long)getpid());
    snprintf(command, sizeof(command), "tools/services.sh testdb create %s", name);
    if (system(command) != 0) return 1;
    DbConfig config;
    bool configured = db_config_default(&config, error, sizeof(error));
    snprintf(config.database, sizeof(config.database), "%s", name);
    db = configured ? db_open(&config, error, sizeof(error)) : NULL;
    CHECK(db != NULL);
    bool migrated = db && db_migrate(db, error, sizeof(error));
    CHECK(migrated);
    int result = 1;
    if (migrated) {
        const TestCase tests[] = { { "saved write authority", receipts }, { "strong hash dedup", hash_collision } };
        result = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    } else fprintf(stderr, "%s\n", error);
    db_close(db);
    snprintf(command, sizeof(command), "tools/services.sh testdb drop %s", name);
    if (system(command) != 0) result = 1;
    return result;
}
