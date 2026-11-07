/* Disposable real MariaDB; no model inference. Migration 013 is applied here
 * when the parent has not yet registered its standalone DDL header. */
#define _DARWIN_C_SOURCE
#include "app/memory_brief_store.h"
#include "app/memory_brief_schema.h"
#include "app/store_backends.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Db *db;
static unsigned serial;
static const char model[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char summary[] = "{\"facts\":[{\"text\":\"Original excerpt; verify sources\",\"sources\":[1]}]}";

static int64_t scalar(const char *sql)
{
    DbRows r = {0};
    int64_t n = -999;
    CHECK(db_query(db, sql, NULL, 0, &r));
    CHECK(r.nrows == 1 && db_cell_i64(&r, 0, 0, &n));
    db_rows_free(&r);
    return n;
}

static long count_tokens(const char *s, void *ctx)
{
    (void)ctx;
    CHECK(!db_in_transaction(db));
    return (long)strlen(s);
}

static void append(const char *conversation, const char *text, const char *via, bool error)
{
    ChatMessage m = {.role=error ? CHAT_ASSISTANT : CHAT_USER, .content=(char *)text,
                     .via=(char *)via, .error=error};
    CHECK(sdb_chat_append(db, "p", conversation, "Brief test", &m));
}

static MemoryScope fixture(bool long_message)
{
    char conversation[40];
    snprintf(conversation, sizeof(conversation), "brief-%u", ++serial);
    if (long_message) {
        char large[1501];
        memset(large, 'x', sizeof(large)-1); large[sizeof(large)-1] = 0;
        append(conversation, large, NULL, false);
    } else append(conversation, "First local original", NULL, false);
    append(conversation, "Remote is excluded", "remote", false);
    append(conversation, "Unsuccessful assistant is excluded", NULL, true);
    for (int i = 0; i < 7; ++i) append(conversation, "Another local original", NULL, false);
    MemoryScope scope = {0};
    CHECK(memory_scope_set(db, "p", conversation, true, &scope));
    for (int i = 0; i < 50; ++i) {
        bool progress = false;
        char why[160];
        bool ok = memory_ingest_step(db, scope.id, "ingest", count_tokens, NULL, "brief-test-tokenizer", &progress, why, sizeof(why));
        if (!ok) fprintf(stderr, "ingestion: %s / %s\n", why, db_error(db));
        CHECK(ok);
        if (!ok || !progress) break;
    }
    CHECK(memory_scope_get(db, "p", conversation, &scope));
    CHECK(scope.ingested_sequence == 9);
    return scope;
}

static MemoryBriefLease claim(const MemoryScope *s, const char *owner)
{
    MemoryBriefLease l = {0};
    bool claimed = false;
    bool ok = memory_brief_claim(db, s, owner, &l, &claimed);
    if (!ok) fprintf(stderr, "claim: %s\n", db_error(db));
    CHECK(ok && claimed);
    CHECK(!db_in_transaction(db));
    return l;
}

static bool yes(void *ctx) { (void)ctx; return true; }
static bool reject_later(void *ctx) { return ++*(unsigned *)ctx < 3; }

static bool publish(const MemoryBriefLease *l)
{
    bool published = false;
    bool ok = memory_brief_publish(db, l, summary, 75, model, MEMORY_BRIEF_PROTOCOL, yes, NULL, &published);
    if (!ok) fprintf(stderr, "publish: %s\n", db_error(db));
    CHECK(ok);
    return published;
}

static void expire(const MemoryScope *s)
{
    CHECK(db_run(db, "UPDATE memory_brief_state SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND),retry_after=NULL"
                     " WHERE scope_id=? AND generation=?", (DbArg[]){DB_S(s->id), DB_I(s->generation)}, 2, NULL));
}

static void retry_now(const MemoryScope *s)
{
    CHECK(db_run(db, "UPDATE memory_brief_state SET retry_after=NULL WHERE scope_id=? AND generation=?",
                 (DbArg[]){DB_S(s->id), DB_I(s->generation)}, 2, NULL));
}

static void ordered_publication(void)
{
    MemoryScope s = fixture(false);
    MemoryBriefLease l = claim(&s, "first");
    CHECK(l.count == 4 && l.inputs[0].sequence == 0 && l.inputs[1].sequence == 3 && l.inputs[3].sequence == 5);
    CHECK(publish(&l));
    CHECK(!publish(&l)); /* duplicate cannot advance twice */
    MemoryBrief recalled[4]; size_t n = 99;
    CHECK(memory_brief_recall(db, &s, 10, model, recalled, &n) && n == 1);
    CHECK(n == 1 && recalled[0].count == 4 && !strcmp(recalled[0].inputs[0].text, "First local original"));
    for (size_t i = 0; i < n; ++i) memory_brief_free(&recalled[i]);
    CHECK(memory_brief_recall(db, &s, 5, model, recalled, &n) && n == 0);
    CHECK(memory_brief_recall(db, &s, 10, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", recalled, &n) && n == 0);
    MemoryBriefLease next = claim(&s, "next");
    CHECK(next.previous_sequence == 5 && next.inputs[0].sequence == 6);
    CHECK(memory_brief_fail(db, &next, true, true));
    memory_brief_lease_free(&next);
    /* Inputs restrict original deletion; derived rows must be GC'd first. */
    CHECK(!db_run(db, "DELETE FROM memory_units WHERE id=?", (DbArg[]){DB_S(l.inputs[0].id)}, 1, NULL));
    memory_brief_lease_free(&l);
}

static void expiry_and_attempts(void)
{
    MemoryScope s = fixture(false);
    MemoryBriefLease old = claim(&s, "old");
    MemoryBriefLease empty = {0}; bool claimed = true;
    CHECK(memory_brief_claim(db, &s, "blocked", &empty, &claimed) && !claimed && !empty.count);
    expire(&s);
    CHECK(!publish(&old)); /* same lease refused even before a replacement */
    MemoryBriefLease replacement = claim(&s, "replacement");
    CHECK(replacement.fence > old.fence);
    CHECK(!publish(&old));
    CHECK(memory_brief_fail(db, &old, true, true)); /* cannot refund new owner */
    CHECK(publish(&replacement));
    memory_brief_lease_free(&old);
    memory_brief_lease_free(&replacement);

    s = fixture(false);
    for (int i = 0; i < 5; ++i) {
        MemoryBriefLease canceled = claim(&s, "preempted");
        CHECK(memory_brief_fail(db, &canceled, true, true));
        memory_brief_lease_free(&canceled);
        retry_now(&s);
    }
    for (int i = 0; i < 3; ++i) {
        MemoryBriefLease failed = claim(&s, "failed");
        CHECK(memory_brief_fail(db, &failed, true, false));
        memory_brief_lease_free(&failed);
        CHECK(memory_brief_claim(db, &s, "backoff", &empty, &claimed) && !claimed);
        retry_now(&s);
    }
    CHECK(memory_brief_claim(db, &s, "exhausted", &empty, &claimed) && !claimed);
}

static void rollback_and_consent(void)
{
    MemoryScope s = fixture(false);
    MemoryBriefLease l = claim(&s, "rollback");
    int64_t before = scalar("SELECT COUNT(*) FROM memory_briefs");
    unsigned calls = 0; bool published = true;
    CHECK(!memory_brief_publish(db, &l, summary, 75, model, MEMORY_BRIEF_PROTOCOL, reject_later, &calls, &published));
    CHECK(!published && scalar("SELECT COUNT(*) FROM memory_briefs") == before);
    CHECK(db_run(db, "CREATE TRIGGER brief_input_failure BEFORE INSERT ON memory_brief_inputs FOR EACH ROW"
                     " SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='brief test rollback'", NULL, 0, NULL));
    CHECK(!memory_brief_publish(db, &l, summary, 75, model, MEMORY_BRIEF_PROTOCOL, yes, NULL, &published) && !published);
    CHECK(scalar("SELECT COUNT(*) FROM memory_briefs") == before);
    CHECK(db_run(db, "DROP TRIGGER brief_input_failure", NULL, 0, NULL));
    CHECK(publish(&l)); /* rollback did not advance or invalidate live lease */
    memory_brief_lease_free(&l);

    s = fixture(false); l = claim(&s, "disable");
    MemoryScope disabled;
    CHECK(memory_scope_set(db, "p", s.conversation_id, false, &disabled));
    CHECK(!publish(&l));
    memory_brief_lease_free(&l);
    s = fixture(false); l = claim(&s, "rebuild");
    MemoryScope rebuilt;
    CHECK(memory_scope_rebuild(db, "p", s.conversation_id, &rebuilt));
    CHECK(rebuilt.generation > s.generation && !publish(&l));
    memory_brief_lease_free(&l);
    /* A generation has no tokenizer until ingestion pins it. Once pinned,
     * it has no frontier/attempt inheritance from the retired generation. */
    MemoryBriefLease empty; bool claimed;
    CHECK(!memory_brief_claim(db, &rebuilt, "new", &empty, &claimed) && !claimed);
    bool progress;
    char why[160];
    CHECK(memory_ingest_step(db, rebuilt.id, "ingest", count_tokens, NULL, "brief-test-tokenizer", &progress, why, sizeof(why)) && progress);
    CHECK(memory_scope_get(db, "p", s.conversation_id, &rebuilt));
    empty = claim(&rebuilt, "new");
    CHECK(empty.previous_sequence == -1 && empty.previous_byte_end == 0 && empty.inputs[0].sequence == 0);
    CHECK(memory_brief_fail(db, &empty, true, true));
    memory_brief_lease_free(&empty);
}

static void mutations_and_holes(void)
{
    const char *mutations[] = {
        "UPDATE messages SET via='remote' WHERE id=?",
        "UPDATE messages SET content='changed original' WHERE id=?",
        "UPDATE messages SET role='assistant',error=1 WHERE id=?",
    };
    for (size_t i = 0; i < sizeof(mutations)/sizeof(*mutations); ++i) {
        MemoryScope s = fixture(false);
        MemoryBriefLease l = claim(&s, "mutated");
        CHECK(db_run(db, mutations[i], (DbArg[]){DB_I(l.inputs[0].message_id)}, 1, NULL));
        CHECK(!publish(&l));
        CHECK(memory_brief_fail(db, &l, true, true));
        memory_brief_lease_free(&l);
    }
    MemoryScope s = fixture(false);
    MemoryBriefLease l = claim(&s, "tokenizer");
    CHECK(db_run(db, "UPDATE memory_scopes SET tokenizer='changed' WHERE id=?", (DbArg[]){DB_S(s.id)}, 1, NULL));
    CHECK(!publish(&l));
    memory_brief_lease_free(&l);

    s = fixture(false); l = claim(&s, "origin");
    strcpy(l.scope.database, "different_database");
    CHECK(!publish(&l));
    memory_brief_lease_free(&l);

    const char *unit_mutations[] = {
        "UPDATE memory_units SET tokenizer='different' WHERE id=?",
        "UPDATE memory_units SET text='changed chunk' WHERE id=?",
        "UPDATE memory_units SET tokens=tokens+1 WHERE id=?",
        "UPDATE memory_units SET byte_start=1 WHERE id=?",
    };
    for (size_t i = 0; i < sizeof(unit_mutations)/sizeof(*unit_mutations); ++i) {
        s = fixture(false); l = claim(&s, "unit-change");
        CHECK(db_run(db, unit_mutations[i], (DbArg[]){DB_S(l.inputs[0].id)}, 1, NULL));
        CHECK(!publish(&l));
        memory_brief_lease_free(&l);
    }

    s = fixture(true);
    s.ingested_sequence = 0; /* caller's complete prefix, not MAX receipt seq */
    l = claim(&s, "chunks");
    CHECK(l.count == 4 && l.inputs[0].sequence == 0 && l.inputs[0].byte_start == 0);
    CHECK(l.inputs[1].sequence == 0 && l.inputs[1].byte_start == l.inputs[0].text_bytes);
    CHECK(l.inputs[2].sequence == 0 && l.inputs[3].sequence == 0);
    CHECK(l.inputs[3].byte_start+l.inputs[3].text_bytes == 1500);
    CHECK(publish(&l));
    MemoryBriefLease beyond; bool beyond_claimed;
    CHECK(memory_brief_claim(db, &s, "beyond-prefix", &beyond, &beyond_claimed) && !beyond_claimed);
    char conversation[40];
    snprintf(conversation, sizeof(conversation), "%s", s.conversation_id);
    CHECK(memory_scope_get(db, "p", conversation, &s));
    MemoryBrief recalled[4]; size_t n;
    CHECK(memory_brief_recall(db, &s, 10, model, recalled, &n) && n == 1);
    for (size_t i = 0; i < n; ++i) memory_brief_free(&recalled[i]);
    /* Invalid current source invalidates the entire brief, never partial cites. */
    CHECK(db_run(db, "UPDATE messages SET stopped=1,role='assistant' WHERE id=?", (DbArg[]){DB_I(l.inputs[0].message_id)}, 1, NULL));
    CHECK(memory_brief_recall(db, &s, 10, model, recalled, &n) && n == 0);
    memory_brief_lease_free(&l);

    s = fixture(false);
    CHECK(db_run(db, "UPDATE memory_receipts SET complete=0,byte_offset=0 WHERE scope_id=? AND sequence=0",
                 (DbArg[]){DB_S(s.id)}, 1, NULL));
    bool claimed;
    CHECK(memory_brief_claim(db, &s, "no-max-holes", &l, &claimed) && !claimed);
    CHECK(db_run(db, "UPDATE memory_receipts r JOIN messages m ON m.id=r.message_id SET r.complete=1,"
                     " r.byte_offset=OCTET_LENGTH(m.content) WHERE r.scope_id=? AND r.sequence=0",
                 (DbArg[]){DB_S(s.id)}, 1, NULL));
    l = claim(&s, "complete");
    CHECK(publish(&l));
    CHECK(db_run(db, "DELETE FROM memory_brief_inputs WHERE brief_id=? AND ordinal=1",
                 (DbArg[]){DB_S(l.input_digest)}, 1, NULL));
    CHECK(!memory_brief_recall(db, &s, 10, model, recalled, &n) && n == 0);
    for (size_t i = 0; i < 4; ++i) CHECK(!recalled[i].count && !recalled[i].inputs[0].text);
    memory_brief_lease_free(&l);

    s = fixture(false);
    CHECK(db_run(db, "DELETE FROM memory_receipts WHERE scope_id=? AND sequence=0",
                 (DbArg[]){DB_S(s.id)}, 1, NULL));
    CHECK(memory_brief_claim(db, &s, "missing-receipt", &l, &claimed) && !claimed);
    s = fixture(false);
    CHECK(db_run(db, "DELETE FROM memory_units WHERE scope_id=? AND source_kind='chat' AND sequence=0",
                 (DbArg[]){DB_S(s.id)}, 1, NULL));
    CHECK(memory_brief_claim(db, &s, "missing-original", &l, &claimed) && !claimed);
}

int main(void)
{
    char name[64], command[256], error[512] = "";
    snprintf(name, sizeof(name), "scribodo_test_briefs_%ld", (long)getpid());
    snprintf(command, sizeof(command), "tools/services.sh testdb create %s", name);
    if (system(command)) return 1;
    DbConfig config;
    bool ok = db_config_default(&config, error, sizeof(error));
    snprintf(config.database, sizeof(config.database), "%s", name);
    db = ok ? db_open(&config, error, sizeof(error)) : NULL;
    ok = db && db_migrate(db, error, sizeof(error));
    if (ok && db_schema_version(db) < 13) {
        for (size_t i = 0; ok && k_migration_013[i]; ++i) ok = db_run(db, k_migration_013[i], NULL, 0, NULL);
    }
    int result = 1;
    if (ok) {
        CHECK(db_run(db, "INSERT INTO projects(id,name,path,path_hash,created_at,updated_at)"
                         " VALUES ('p','Book','/brief-test','brief-test',1,1)", NULL, 0, NULL));
        const TestCase tests[] = {{"ordered chat originals", ordered_publication}, {"leases and bounded retries", expiry_and_attempts},
                                  {"atomic rollback and consent", rollback_and_consent}, {"mutations and contiguous frontier", mutations_and_holes}};
        result = run_tests(tests, sizeof(tests)/sizeof(*tests));
    } else fprintf(stderr, "brief schema: %s / %s\n", error, db ? db_error(db) : "no connection");
    db_close(db);
    snprintf(command, sizeof(command), "tools/services.sh testdb drop %s", name);
    if (system(command)) result = 1;
    return result;
}
