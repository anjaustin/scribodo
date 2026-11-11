#define _DARWIN_C_SOURCE
#include "app/memory_documents.h"
#include "core/experiments.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Db *db;
static MemoryScope scope;
static MemoryDocumentSet attached;

static int64_t scalar(const char *sql)
{
    DbRows r;
    int64_t value = -1;
    CHECK(db_query(db, sql, NULL, 0, &r));
    CHECK(r.nrows == 1 && db_cell_i64(&r, 0, 0, &value));
    db_rows_free(&r);
    return value;
}

static void run(const char *sql)
{
    bool ok = db_run(db, sql, NULL, 0, NULL);
    if (!ok) fprintf(stderr, "SQL: %s\n", db_error(db));
    CHECK(ok);
}

static long tokens(const char *text, void *ctx)
{
    CHECK(!db_in_transaction(db));
    return ctx ? -1 : (long)strlen(text);
}

static void refresh(void)
{
    CHECK(memory_scope_get(db, "p", "c", &scope));
}

static bool step(MemoryTokenCount count, void *context, bool expected)
{
    bool progress = false;
    char why[256];
    refresh();
    bool ok = memory_document_ingest_step(db, &scope, &attached, "worker", count, context,
                                          "exact-v1", &progress, why, sizeof(why));
    if (ok != expected) fprintf(stderr, "document ingest: %s / %s\n", db_error(db), why);
    CHECK(ok == expected);
    return progress;
}

static void capture(const char *document, const char *revision)
{
    DbRows r;
    CHECK(db_query(db, "SELECT DATABASE(),source_hash,OCTET_LENGTH(content) FROM revisions WHERE document_id=? AND id=?",
                   (DbArg[]){ DB_S(document), DB_S(revision) }, 2, &r));
    memset(&attached, 0, sizeof(attached));
    attached.count = 1;
    MemoryDocumentRef *ref = &attached.refs[0];
    snprintf(ref->database, sizeof(ref->database), "%s", db_cell(&r, 0, 0));
    snprintf(ref->project_id, sizeof(ref->project_id), "p");
    snprintf(ref->document_id, sizeof(ref->document_id), "%s", document);
    snprintf(ref->revision_id, sizeof(ref->revision_id), "%s", revision);
    snprintf(ref->source_hash, sizeof(ref->source_hash), "%s", db_cell(&r, 0, 1));
    snprintf(ref->name, sizeof(ref->name), "notes.md");
    CHECK(db_cell_u64(&r, 0, 2, &ref->bytes));
    db_rows_free(&r);
}

static MemoryCandidate locator(void)
{
    MemoryCandidate c = {0};
    DbRows r;
    CHECK(db_query(db, "SELECT id,text_hash FROM memory_units WHERE source_kind='document' AND generation=?"
                      " ORDER BY byte_start LIMIT 1",
                   (DbArg[]){ DB_I(scope.generation) }, 1, &r));
    CHECK(r.nrows == 1);
    if (r.nrows) {
        snprintf(c.unit_id, sizeof(c.unit_id), "%s", db_cell(&r, 0, 0));
        snprintf(c.text_hash, sizeof(c.text_hash), "%s", db_cell(&r, 0, 1));
    }
    c.generation = scope.generation;
    c.history_epoch = scope.history_epoch;
    db_rows_free(&r);
    return c;
}

static void saved_authority(void)
{
    run("INSERT INTO projects(id,name,path,path_hash,created_at,updated_at)"
        " VALUES('p','Book','/docs','docs',1,1),('other','Other','/other','other',1,1)");
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('c','p','Chat',1,1)");
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('d','p','notes.md',1),('foreign','other','other.md',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('saved','d','h','rarewhalefact saved authors text','save',1),"
        "('ai','d','h2','unsaved AI text','ai',2),('foreign-r','foreign','h3','other project','save',1)");
    run("UPDATE documents SET saved_revision_id='saved',current_revision_id='ai' WHERE id='d'");
    CHECK(memory_scope_set(db, "p", "c", true, &scope));
    capture("d", "saved");
    CHECK(memory_documents_seed(db, &scope, &attached));
    CHECK(memory_documents_seed(db, &scope, &attached));
    bool complete = true;
    CHECK(memory_documents_complete(db, &scope, &attached, &complete) && !complete);
    CHECK(memory_documents_complete(db, &scope, NULL, &complete) && complete);
    MemoryScope before_pin = scope;
    bool pin_progress = true;
    char pin_why[128];
    CHECK(!before_pin.tokenizer[0]);
    CHECK(memory_ingest_step(db, scope.id, "chat", tokens, NULL, "exact-v1", &pin_progress,
                             pin_why, sizeof(pin_why)) && !pin_progress);
    CHECK(memory_documents_seed(db, &before_pin, &attached));
    CHECK(memory_documents_complete(db, &before_pin, &attached, &complete) && !complete);
    CHECK(!memory_document_ingest_step(db, &before_pin, &attached, "worker", tokens, NULL, "wrong-v2",
                                       &pin_progress, pin_why, sizeof(pin_why)) && !pin_progress);
    CHECK(scalar("SELECT COUNT(*) FROM memory_document_receipts") == 1);
    MemoryDocumentSet bad = attached;
    snprintf(bad.refs[0].revision_id, sizeof(bad.refs[0].revision_id), "ai");
    CHECK(!memory_documents_seed(db, &scope, &bad));
    bad = attached;
    snprintf(bad.refs[0].document_id, sizeof(bad.refs[0].document_id), "foreign");
    snprintf(bad.refs[0].revision_id, sizeof(bad.refs[0].revision_id), "foreign-r");
    CHECK(!memory_documents_seed(db, &scope, &bad));
    bad = attached;
    ++bad.refs[0].bytes;
    CHECK(!memory_documents_seed(db, &scope, &bad));
    bad = attached;
    bad.count = 9;
    CHECK(!memory_documents_seed(db, &scope, &bad));

    run("CREATE TRIGGER doc_outbox_fail BEFORE INSERT ON memory_outbox FOR EACH ROW"
        " SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected'");
    CHECK(!step(tokens, NULL, false));
    CHECK(scalar("SELECT COUNT(*) FROM memory_units") == 0);
    CHECK(scalar("SELECT byte_offset FROM memory_document_receipts") == 0);
    run("DROP TRIGGER doc_outbox_fail");
    run("UPDATE memory_document_receipts SET retry_after=NULL");
    CHECK(memory_document_ingest_step(db, &before_pin, &attached, "worker", tokens, NULL, "exact-v1",
                                      &pin_progress, pin_why, sizeof(pin_why)) && pin_progress);
    CHECK(memory_documents_complete(db, &before_pin, &attached, &complete) && complete);
    refresh();
    CHECK(!step(tokens, NULL, true));
    CHECK(scalar("SELECT COUNT(*) FROM memory_units") == 1 && scalar("SELECT COUNT(*) FROM memory_outbox") == 1);
    CHECK(scalar("SELECT complete FROM memory_document_receipts") == 1);
    CHECK(memory_documents_complete(db, &scope, &attached, &complete) && complete);

    MemoryCandidate c = locator(), cs[32];
    MemoryUnit u;
    bool found = false;
    size_t n = 0;
    CHECK(memory_candidate_hydrate(db, &scope, &c, &u, &found) && !found);
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, &attached, &c, &u, &found) && found);
    if (found) {
        CHECK(u.kind == MEMORY_SOURCE_DOCUMENT && !strcmp(u.document_id, "d") && !strcmp(u.revision_id, "saved"));
        CHECK(u.role == CHAT_USER && u.message_id == 0 && u.sequence == 0);
        CHECK(!strcmp(u.text, "rarewhalefact saved authors text"));
    }
    memory_unit_free(&u);
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, NULL, &c, &u, &found) && !found);
    CHECK(memory_lexical_candidates(db, &scope, "rarewhalefact", 16, cs, &n) && !n);
    CHECK(memory_lexical_candidates_with_documents(db, &scope, &attached, "rarewhalefact", 16, cs, &n) && n == 1);
    CHECK(memory_lexical_candidates_with_documents(db, &scope, NULL, "rarewhalefact", 16, cs, &n) && !n);
    /* SQL shape and ownership reject invalid units independently of API. */
    CHECK(!db_run(db, "UPDATE memory_units SET source_kind='chat'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET document_id='foreign'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET revision_id='foreign-r'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET sequence=0 WHERE source_kind='document'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET message_id=1 WHERE source_kind='document'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET role='author' WHERE source_kind='document'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET role=NULL WHERE source_kind='document'", NULL, 0, NULL));
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE source_kind='document' AND role='user'"
                 " AND message_id IS NULL AND sequence IS NULL") == 1);
    MemoryIndexLease lease;
    bool claimed = false;
    CHECK(memory_outbox_claim(db, scope.id, "index", &lease, &claimed) && claimed);
    CHECK(lease.unit.kind == MEMORY_SOURCE_DOCUMENT);
    run("UPDATE documents SET current_revision_id='saved' WHERE id='d'");
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, &attached, &c, &u, &found) && found);
    memory_unit_free(&u);
    run("UPDATE documents SET saved_revision_id='ai' WHERE id='d'");
    CHECK(!memory_documents_complete(db, &scope, &attached, &complete) && !complete);
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, &attached, &c, &u, &found) && !found);
    CHECK(!memory_outbox_ack(db, &lease, true));
    memory_index_lease_free(&lease);
    run("UPDATE memory_outbox SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)");
    CHECK(memory_outbox_claim(db, scope.id, "index", &lease, &claimed) && !claimed);
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox WHERE state='done'") == 1);
    run("UPDATE documents SET saved_revision_id='saved' WHERE id='d'");
    uint64_t gen = scope.generation;
    CHECK(memory_scope_rebuild(db, "p", "c", &scope) && scope.generation > gen);
    CHECK(memory_documents_seed(db, &scope, &attached));
    CHECK(memory_documents_seed(db, &scope, &attached));
    CHECK(scalar("SELECT COUNT(*) FROM memory_document_receipts") == 2);
    CHECK(step(tokens, NULL, true));
    refresh();
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, &attached, &c, &u, &found) && !found);
    /* Detached derived data may index locally, but cannot hydrate for a model. */
    c = locator();
    CHECK(memory_outbox_claim(db, scope.id, "index", &lease, &claimed) && claimed);
    CHECK(memory_outbox_ack(db, &lease, true));
    memory_index_lease_free(&lease);
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, NULL, &c, &u, &found) && !found);
}

/* Exercise every public entry point with the same malformed owner metadata.
 * The live disposable connection makes early argument validation observable:
 * these calls must neither publish nor leave a transaction open. */
static void reject_metadata(Db *connection, const MemoryScope *s, const MemoryDocumentSet *set)
{
    MemoryCandidate candidate = {0}, candidates[2], empty_candidates[2] = {0};
    MemoryUnit unit, empty_unit = {0};
    memset(candidate.unit_id, 'a', 64);
    memset(candidate.text_hash, 'b', 64);
    candidate.generation = 1;
    candidate.history_epoch = 1;

    CHECK(!memory_documents_seed(connection, s, set));
    bool complete = true;
    CHECK(!memory_documents_complete(connection, s, set, &complete) && !complete);
    bool progress = true;
    char why[32];
    memset(why, 'x', sizeof(why));
    CHECK(!memory_document_ingest_step(connection, s, set, "worker", tokens, NULL, "exact-v1",
                                       &progress, why, sizeof(why)) && !progress && !why[0]);
    size_t count = 99;
    memset(candidates, 0x7f, sizeof(candidates));
    CHECK(!memory_lexical_candidates_with_documents(connection, s, set, "query", 2, candidates, &count));
    CHECK(!count && !memcmp(candidates, empty_candidates, sizeof(candidates)));
    bool found = true;
    memset(&unit, 0x7f, sizeof(unit));
    CHECK(!memory_candidate_hydrate_with_documents(connection, s, set, &candidate, &unit, &found));
    CHECK(!found && !memcmp(&unit, &empty_unit, sizeof(unit)));
    CHECK(!db_in_transaction(db));
}

static void invalid_public_arguments(void)
{
    int64_t units_before = scalar("SELECT COUNT(*) FROM memory_units");
    int64_t receipts_before = scalar("SELECT COUNT(*) FROM memory_document_receipts");
    reject_metadata(NULL, &scope, &attached);
    reject_metadata(db, NULL, &attached);

    /* Each embedded string must terminate within its own public field. */
#define REJECT_SCOPE_FIELD(field) do { \
    MemoryScope bad = scope; \
    memset(bad.field, 'x', sizeof(bad.field)); \
    reject_metadata(db, &bad, &attached); \
} while (0)
    REJECT_SCOPE_FIELD(id);
    REJECT_SCOPE_FIELD(project_id);
    REJECT_SCOPE_FIELD(conversation_id);
    REJECT_SCOPE_FIELD(database);
    REJECT_SCOPE_FIELD(tokenizer);
#undef REJECT_SCOPE_FIELD

#define REJECT_REF_FIELD(field) do { \
    MemoryDocumentSet bad = attached; \
    memset(bad.refs[0].field, 'x', sizeof(bad.refs[0].field)); \
    reject_metadata(db, &scope, &bad); \
} while (0)
    REJECT_REF_FIELD(database);
    REJECT_REF_FIELD(project_id);
    REJECT_REF_FIELD(document_id);
    REJECT_REF_FIELD(revision_id);
    REJECT_REF_FIELD(source_hash);
    REJECT_REF_FIELD(name);
#undef REJECT_REF_FIELD

    MemoryDocumentSet bad = attached;
    bad.count = MEMORY_DOCUMENTS_MAX + 1;
    reject_metadata(db, &scope, &bad);
    bad = attached;
    bad.refs[0].bytes = 16777217;
    reject_metadata(db, &scope, &bad);
    bad = attached;
    bad.refs[0].source_hash[0] = 'g';
    reject_metadata(db, &scope, &bad);
    bad = attached;
    bad.refs[0].document_id[0] = ':';
    reject_metadata(db, &scope, &bad);
    MemoryScope bad_scope = scope;
    bad_scope.generation = 0;
    reject_metadata(db, &bad_scope, &attached);

    bool complete = true, progress = true, found = true;
    char why[32] = "not cleared";
    CHECK(!memory_documents_complete(db, &scope, &attached, NULL));
    CHECK(!memory_document_ingest_step(db, &scope, &attached, "worker", tokens, NULL, "exact-v1",
                                       NULL, why, sizeof(why)) && !why[0]);
    CHECK(!memory_document_ingest_step(db, &scope, &attached, "worker", tokens, NULL, "exact-v1",
                                       &progress, NULL, 1) && !progress);
    progress = true;
    CHECK(!memory_document_ingest_step(db, &scope, &attached, NULL, tokens, NULL, "exact-v1",
                                       &progress, NULL, 0) && !progress);
    progress = true;
    CHECK(!memory_document_ingest_step(db, &scope, &attached, "worker", tokens, NULL, NULL,
                                       &progress, NULL, 0) && !progress);
    progress = true;
    CHECK(!memory_document_ingest_step(db, &scope, &attached, "worker", NULL, NULL, "exact-v1",
                                       &progress, NULL, 0) && !progress);
    char owner[64], tokenizer[192], query[2001];
    memset(owner, 'x', sizeof(owner));
    memset(tokenizer, 'x', sizeof(tokenizer));
    memset(query, 'x', sizeof(query));
    progress = true;
    CHECK(!memory_document_ingest_step(db, &scope, &attached, owner, tokens, NULL, "exact-v1",
                                       &progress, NULL, 0) && !progress);
    progress = true;
    CHECK(!memory_document_ingest_step(db, &scope, &attached, "worker", tokens, NULL, tokenizer,
                                       &progress, NULL, 0) && !progress);

    MemoryCandidate candidates[2], empty_candidates[2] = {0};
    size_t count = 99;
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, "query", 2, NULL, &count) && !count);
    memset(candidates, 0x7f, sizeof(candidates));
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, "query", 2, candidates, NULL));
    CHECK(!memcmp(candidates, empty_candidates, sizeof(candidates)));
    count = 99;
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, NULL, 2, candidates, &count) && !count);
    count = 99;
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, query, 2, candidates, &count) && !count);
    count = 99;
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, "query", 0, candidates, &count) && !count);
    count = 99;
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, "query", MEMORY_CANDIDATES + 1,
                                                   candidates, &count) && !count);

    MemoryUnit unit, empty_unit = {0};
    MemoryCandidate candidate = locator();
    memset(&unit, 0x7f, sizeof(unit));
    CHECK(!memory_candidate_hydrate_with_documents(db, &scope, &attached, &candidate, &unit, NULL));
    CHECK(!memcmp(&unit, &empty_unit, sizeof(unit)));
    CHECK(!memory_candidate_hydrate_with_documents(db, &scope, &attached, &candidate, NULL, &found) && !found);
    found = true;
    CHECK(!memory_candidate_hydrate_with_documents(db, &scope, &attached, NULL, &unit, &found) && !found);
    memset(candidate.unit_id, 'a', sizeof(candidate.unit_id));
    found = true;
    CHECK(!memory_candidate_hydrate_with_documents(db, &scope, &attached, &candidate, &unit, &found) && !found);
    CHECK(!memcmp(&unit, &empty_unit, sizeof(unit)));
    candidate = locator();
    memset(candidate.text_hash, 'b', sizeof(candidate.text_hash));
    found = true;
    CHECK(!memory_candidate_hydrate_with_documents(db, &scope, &attached, &candidate, &unit, &found) && !found);
    CHECK(!memcmp(&unit, &empty_unit, sizeof(unit)));

    CHECK(!db_in_transaction(db));
    CHECK(scalar("SELECT COUNT(*) FROM memory_units") == units_before);
    CHECK(scalar("SELECT COUNT(*) FROM memory_document_receipts") == receipts_before);
    /* The agreed empty-tokenizer snapshot remains valid for metadata checks. */
    MemoryScope unpinned = scope;
    unpinned.tokenizer[0] = 0;
    CHECK(memory_documents_seed(db, &unpinned, &attached));
    CHECK(memory_documents_complete(db, &unpinned, &attached, &complete) && complete);
}

static long reclaim(const char *text, void *ctx)
{
    CHECK(!db_in_transaction(db));
    run("UPDATE memory_document_receipts SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)");
    CHECK(step(tokens, NULL, true));
    return (long)strlen(text) / 4;
}

static void unicode_and_fences(void)
{
    size_t bytes = 24000;
    char *text = malloc(bytes + 1);
    CHECK(text != NULL);
    if (!text) return;
    for (size_t i = 0; i < bytes; i += 4) {
        memcpy(text + i, "🐋", 4);
    }
    text[bytes] = 0;
    CHECK(db_run(db, "INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
                     " VALUES('huge','d','h',?,'save',3)",
                 (DbArg[]){ DB_S(text) }, 1, NULL));
    free(text);
    run("UPDATE documents SET saved_revision_id='huge' WHERE id='d'");
    capture("d", "huge");
    CHECK(memory_scope_rebuild(db, "p", "c", &scope));
    CHECK(!step(reclaim, NULL, false));
    CHECK(scalar("SELECT SUM(OCTET_LENGTH(text)) FROM memory_units WHERE revision_id='huge'") <= 512);
    MemoryDocumentSet saved = attached;
    attached.count = 0;
    CHECK(!step(tokens, NULL, true));
    attached = saved;
    /* Resume from only durable SQL receipt state after closing the worker's
     * connection; no source window or cursor is retained by the client. */
    DbConfig restart;
    char error[512];
    CHECK(db_config_default(&restart, error, sizeof(error)));
    snprintf(restart.database, sizeof(restart.database), "%s", scope.database);
    db_close(db);
    db = db_open(&restart, error, sizeof(error));
    CHECK(db != NULL);
    if (!db) return;
    int steps = 0;
    while (step(tokens, NULL, true) && ++steps < 400) { }
    CHECK(steps < 400);
    CHECK(scalar("SELECT SUM(OCTET_LENGTH(text)) FROM memory_units WHERE revision_id='huge'") == 24000);
    CHECK(scalar("SELECT MAX(tokens) FROM memory_units WHERE revision_id='huge'") <= MEMORY_DOCUMENT_SMALL_UNIT_TOKENS);
    CHECK(scalar("SELECT MAX(OCTET_LENGTH(text)) FROM memory_units WHERE revision_id='huge'") <= MEMORY_DOCUMENT_SMALL_UNIT_TOKENS);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE revision_id='huge' AND MOD(byte_start,4)<>0") == 0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units") == scalar("SELECT COUNT(*) FROM memory_outbox"));
    CHECK(memory_scope_rebuild(db, "p", "c", &scope));
    for (int i = 0; i < 3; i++) {
        run("UPDATE memory_document_receipts SET retry_after=NULL");
        CHECK(!step(tokens, (void *)1, false));
    }
    CHECK(!step(tokens, NULL, true));
    CHECK(scalar("SELECT MAX(attempts) FROM memory_document_receipts") == 3);
    CHECK(memory_scope_set(db, "p", "c", false, &scope));
    CHECK(!step(tokens, NULL, true));
}

static void lexical_allowset_before_limit(void)
{
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('lex-a','p','a.md',1),('lex-z','p','z.md',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('lex-ar','lex-a','h','unionterm distinct author fact','save',1),"
        "('lex-zr','lex-z','h',REPEAT('unionterm ',2000),'save',1)");
    run("UPDATE documents SET saved_revision_id='lex-ar' WHERE id='lex-a'");
    run("UPDATE documents SET saved_revision_id='lex-zr' WHERE id='lex-z'");
    CHECK(memory_scope_set(db, "p", "c", true, &scope));
    capture("lex-z", "lex-zr");
    int steps = 0;
    while (step(tokens, NULL, true) && ++steps < 1024) { }
    CHECK(steps > 16 && steps < 1024);
    bool complete = false;
    CHECK(memory_documents_complete(db, &scope, &attached, &complete) && complete);
    CHECK(scalar("SELECT SUM(OCTET_LENGTH(text)) FROM memory_units WHERE revision_id='lex-zr'") == 20000);
    CHECK(scalar("SELECT MAX(tokens) FROM memory_units WHERE revision_id='lex-zr'") <= MEMORY_DOCUMENT_SMALL_UNIT_TOKENS);
    printf("    lexical fixture: %d measured ingestion steps, 20000 source bytes preserved\n", steps);
    capture("lex-a", "lex-ar");
    CHECK(step(tokens, NULL, true));
    refresh();
    MemoryCandidate candidates[32];
    size_t n = 0;
    CHECK(memory_lexical_candidates_with_documents(db, &scope, &attached, "unionterm", 16, candidates, &n) && n == 1);
    MemoryUnit u;
    bool found = false;
    CHECK(memory_candidate_hydrate_with_documents(db, &scope, &attached, &candidates[0], &u, &found) && found);
    if (found) CHECK(!strcmp(u.document_id, "lex-a"));
    memory_unit_free(&u);
    CHECK(memory_lexical_candidates_with_documents(db, &scope, NULL, "unionterm", 16, candidates, &n) && !n);
    run("INSERT INTO messages(conversation_id,seq,role,content,created_at) VALUES('c',0,'user','unionterm chat fact',1)");
    bool progress = false;
    char why[128];
    CHECK(memory_ingest_step(db, scope.id, "chat", tokens, NULL, "exact-v1", &progress, why, sizeof(why)) && progress);
    refresh();
    CHECK(!db_run(db, "UPDATE memory_units SET sequence=NULL WHERE source_kind='chat'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET message_id=NULL WHERE source_kind='chat'", NULL, 0, NULL));
    CHECK(!db_run(db, "UPDATE memory_units SET role=NULL WHERE source_kind='chat'", NULL, 0, NULL));
    CHECK(memory_lexical_candidates_with_documents(db, &scope, &attached, "unionterm", 32, candidates, &n) && n == 2);
    CHECK(memory_lexical_candidates_with_documents(db, &scope, &attached, "unionterm", 1, candidates, &n) && n == 1);
    attached.count = 2;
    attached.refs[1] = attached.refs[0];
    CHECK(!memory_lexical_candidates_with_documents(db, &scope, &attached, "unionterm", 16, candidates, &n) && !n);
}

static void stale_outbox_batches(void)
{
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('queue','p','Queue',1,1)");
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('queue-doc','p','queue.md',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('queue-old','queue-doc','h',REPEAT('q',129),'save',1),"
        "('queue-new','queue-doc','h','queueword current saved document','save',2)");
    run("UPDATE documents SET saved_revision_id='queue-old' WHERE id='queue-doc'");
    MemoryScope queue;
    CHECK(memory_scope_set(db, "p", "queue", true, &queue));
    /* Real typed, source-exact one-byte chunks, published before the saved
     * pointer changes. Their queue IDs all precede both ready jobs below. */
    CHECK(db_run(db, "INSERT INTO memory_units(id,scope_id,project_id,conversation_id,history_epoch,policy_epoch,"
                     "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role,source_kind,document_id,revision_id)"
                     " SELECT SHA2(CONCAT(s.id,':queue-old:',q.seq),256),s.id,s.project_id,s.conversation_id,"
                     "s.history_epoch,s.policy_epoch,s.generation,q.seq-1,r.source_hash,SHA2('q',256),'q',1,"
                     "'exact-v1','user','document',r.document_id,r.id FROM memory_scopes s"
                     " JOIN revisions r ON r.id='queue-old' CROSS JOIN seq_1_to_129 q WHERE s.id=?",
                 (DbArg[]){ DB_S(queue.id) }, 1, NULL));
    CHECK(db_run(db, "INSERT INTO memory_outbox(scope_id,unit_id,generation)"
                     " SELECT scope_id,id,generation FROM memory_units WHERE scope_id=? ORDER BY byte_start",
                 (DbArg[]){ DB_S(queue.id) }, 1, NULL));
    run("UPDATE documents SET saved_revision_id='queue-new' WHERE id='queue-doc'");
    run("INSERT INTO messages(conversation_id,seq,role,content,created_at) VALUES('queue',0,'user','queueword ready chat',1)");
    bool progress = false;
    char why[128];
    CHECK(memory_ingest_step(db, queue.id, "queue-worker", tokens, NULL, "exact-v1",
                             &progress, why, sizeof(why)) && progress);
    CHECK(memory_scope_get(db, "p", "queue", &queue));
    capture("queue-doc", "queue-new");
    MemoryDocumentSet documents = attached;
    CHECK(memory_document_ingest_step(db, &queue, &documents, "queue-worker", tokens, NULL, "exact-v1",
                                      &progress, why, sizeof(why)) && progress);

    int calls = 0, ready = 0;
    bool saw_chat = false, saw_document = false;
    while (ready < 2 && calls < 4) {
        MemoryIndexLease lease;
        bool claimed = false;
        int64_t before = scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                                " WHERE s.conversation_id='queue' AND o.state='done'");
        CHECK(memory_outbox_claim(db, queue.id, "queue-index", &lease, &claimed));
        calls++;
        int64_t after = scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                               " WHERE s.conversation_id='queue' AND o.state='done'");
        CHECK(after - before <= 64);
        CHECK(lease.maintenance == (after > before));
        CHECK(!db_in_transaction(db));
        if (claimed) {
            if (lease.unit.kind == MEMORY_SOURCE_CHAT) saw_chat = true;
            if (lease.unit.kind == MEMORY_SOURCE_DOCUMENT) {
                CHECK(!strcmp(lease.unit.revision_id, "queue-new"));
                saw_document = true;
            }
            CHECK(memory_outbox_ack(db, &lease, true));
            ready++;
        } else {
            CHECK(lease.maintenance);
            CHECK(!lease.id && !lease.fence && !lease.owner[0] && !lease.scope.id[0] && !lease.unit.text);
        }
        memory_index_lease_free(&lease);
    }
    CHECK(calls <= 4 && ready == 2 && saw_chat && saw_document);
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                 " WHERE s.conversation_id='queue' AND o.state='done'") == 131);
    MemoryIndexLease idle;
    bool claimed = true;
    CHECK(memory_outbox_claim(db, queue.id, "queue-index", &idle, &claimed) && !claimed && !idle.maintenance);
    CHECK(!idle.id && !idle.unit.text && !idle.scope.id[0]);
}

static void mutable_saved_source(void)
{
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('mutation','p','Mutation',1,1)");
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('mutable-doc','p','mutable.md',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('mutable-r','mutable-doc','h','oldword saved text','save',1)");
    run("UPDATE documents SET saved_revision_id='mutable-r' WHERE id='mutable-doc'");
    MemoryScope current;
    CHECK(memory_scope_set(db, "p", "mutation", true, &current));
    capture("mutable-doc", "mutable-r");
    MemoryDocumentSet original = attached;
    bool progress = false;
    char why[128];
    CHECK(memory_document_ingest_step(db, &current, &original, "mutation-worker", tokens, NULL, "exact-v1",
                                      &progress, why, sizeof(why)) && progress);
    CHECK(memory_scope_get(db, "p", "mutation", &current));
    MemoryIndexLease lease;
    bool claimed = false;
    CHECK(memory_outbox_claim(db, current.id, "mutation-index", &lease, &claimed) && claimed);
    MemoryCandidate candidate = {0};
    snprintf(candidate.unit_id, sizeof(candidate.unit_id), "%s", lease.unit.id);
    snprintf(candidate.text_hash, sizeof(candidate.text_hash), "%s", lease.unit.text_hash);
    candidate.generation = current.generation;
    candidate.history_epoch = current.history_epoch;

    run("UPDATE revisions SET content='newword saved text' WHERE id='mutable-r'");
    CHECK(scalar("SELECT OCTET_LENGTH(content)=OCTET_LENGTH('oldword saved text')"
                 " FROM revisions WHERE id='mutable-r'") == 1);
    CHECK(!memory_documents_seed(db, &current, &original));
    bool complete = true;
    CHECK(!memory_documents_complete(db, &current, &original, &complete) && !complete);
    progress = true;
    CHECK(!memory_document_ingest_step(db, &current, &original, "mutation-worker", tokens, NULL, "exact-v1",
                                       &progress, why, sizeof(why)) && !progress);
    MemoryUnit unit;
    bool found = true;
    CHECK(memory_candidate_hydrate_with_documents(db, &current, &original, &candidate, &unit, &found) && !found);
    MemoryCandidate candidates[16];
    size_t count = 99;
    CHECK(memory_lexical_candidates_with_documents(db, &current, &original, "oldword", 16, candidates, &count) && !count);
    CHECK(!memory_outbox_ack(db, &lease, true));
    CHECK(!db_in_transaction(db));
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                 " WHERE s.conversation_id='mutation' AND o.state='leased'") == 1);

    run("UPDATE revisions SET content='oldword saved text' WHERE id='mutable-r'");
    /* A copy must never silently repair a malformed original terminator. */
    lease.unit.text_hash[64] = 'x';
    CHECK(!memory_outbox_ack(db, &lease, true));
    CHECK(!db_in_transaction(db));
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                 " WHERE s.conversation_id='mutation' AND o.state='leased'") == 1);
    lease.unit.text_hash[64] = 0;
    CHECK(memory_outbox_ack(db, &lease, true));
    memory_index_lease_free(&lease);
}

static long expire_receipt(const char *text, void *context)
{
    const MemoryScope *current = context;
    CHECK(!db_in_transaction(db));
    CHECK(db_run(db, "UPDATE memory_document_receipts SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)"
                     " WHERE scope_id=? AND generation=?",
                 (DbArg[]){ DB_S(current->id), DB_I(current->generation) }, 2, NULL));
    return (long)strlen(text) / 4;
}

static void expired_document_publication(void)
{
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('expiry','p','Expiry',1,1)");
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('expiry-doc','p','expiry.md',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('expiry-r','expiry-doc','h','expiryword saved text','save',1)");
    run("UPDATE documents SET saved_revision_id='expiry-r' WHERE id='expiry-doc'");
    MemoryScope current;
    CHECK(memory_scope_set(db, "p", "expiry", true, &current));
    capture("expiry-doc", "expiry-r");
    MemoryDocumentSet documents = attached;
    bool progress = true;
    char why[128];
    CHECK(!memory_document_ingest_step(db, &current, &documents, "expired-worker", expire_receipt, &current,
                                       "exact-v1", &progress, why, sizeof(why)) && !progress);
    CHECK(!db_in_transaction(db));
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE conversation_id='expiry'") == 0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                 " WHERE s.conversation_id='expiry'") == 0);
    CHECK(scalar("SELECT byte_offset+complete FROM memory_document_receipts WHERE conversation_id='expiry'") == 0);
    run("UPDATE memory_document_receipts SET retry_after=NULL WHERE conversation_id='expiry'");
    CHECK(memory_scope_get(db, "p", "expiry", &current));
    CHECK(memory_document_ingest_step(db, &current, &documents, "replacement-worker", tokens, NULL,
                                      "exact-v1", &progress, why, sizeof(why)) && progress);
    MemoryIndexLease old, replacement;
    bool claimed = false;
    CHECK(memory_outbox_claim(db, current.id, "old-index", &old, &claimed) && claimed);
    CHECK(db_run(db, "UPDATE memory_outbox SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE id=?",
                 (DbArg[]){ DB_I(old.id) }, 1, NULL));
    CHECK(!memory_outbox_ack(db, &old, true));
    CHECK(memory_outbox_claim(db, current.id, "replacement-index", &replacement, &claimed) && claimed);
    CHECK(replacement.fence > old.fence);
    CHECK(!memory_outbox_ack(db, &old, true));
    CHECK(memory_outbox_ack(db, &replacement, false));
    memory_index_lease_free(&old);
    memory_index_lease_free(&replacement);
    CHECK(memory_outbox_claim(db, current.id, "backoff-index", &replacement, &claimed) && !claimed);
    CHECK(!replacement.maintenance);
    CHECK(db_run(db, "UPDATE memory_outbox SET retry_after=NULL WHERE scope_id=?",
                 (DbArg[]){ DB_S(current.id) }, 1, NULL));
    CHECK(memory_outbox_claim(db, current.id, "third-index", &replacement, &claimed) && claimed);
    CHECK(memory_outbox_ack(db, &replacement, false));
    memory_index_lease_free(&replacement);
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                 " WHERE s.conversation_id='expiry' AND o.state='failed' AND o.attempts=3") == 1);
    CHECK(memory_outbox_claim(db, current.id, "exhausted-index", &replacement, &claimed) && !claimed);
    CHECK(!replacement.maintenance && !replacement.id && !replacement.unit.text);
    /* The other terminal path is a crashed third delivery, retired without
     * yielding any lease ownership but still waking the maintenance loop. */
    CHECK(db_run(db, "UPDATE memory_outbox SET state='leased',lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)"
                     " WHERE scope_id=? AND attempts=3",
                 (DbArg[]){ DB_S(current.id) }, 1, NULL));
    CHECK(memory_outbox_claim(db, current.id, "terminal-cleanup", &replacement, &claimed) && !claimed);
    CHECK(replacement.maintenance && !replacement.id && !replacement.fence && !replacement.owner[0] &&
          !replacement.scope.id[0] && !replacement.unit.text);
    CHECK(memory_outbox_claim(db, current.id, "terminal-cleanup", &replacement, &claimed) && !claimed);
    CHECK(!replacement.maintenance);
}

static void default_and_experimental_unit_limits(void)
{
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('unit-policy','d','policy',REPEAT('policyword ',100),'save',7)");
    run("UPDATE documents SET saved_revision_id='unit-policy' WHERE id='d'");
    capture("d", "unit-policy");
    for (unsigned small = 0; small < 2; small++) {
        CHECK((small ? setenv("SCRIBODO_MEMORY_SMALL_UNITS", "1", 1) : unsetenv("SCRIBODO_MEMORY_SMALL_UNITS")) == 0); experiments_reload();
        CHECK(memory_scope_set(db, "p", "c", true, &scope));
        CHECK(memory_scope_rebuild(db, "p", "c", &scope));
        unsigned steps = 0;
        while (step(tokens, NULL, true) && ++steps < 1024) { }
        bool complete = false;
        CHECK(steps < 1024 && memory_documents_complete(db, &scope, &attached, &complete) && complete);
        CHECK(scalar("SELECT SUM(OCTET_LENGTH(u.text)) FROM memory_units u JOIN memory_scopes s"
                     " ON s.id=u.scope_id AND s.generation=u.generation WHERE u.revision_id='unit-policy'") == 1100);
        int64_t maximum = scalar("SELECT MAX(u.tokens) FROM memory_units u JOIN memory_scopes s"
                                 " ON s.id=u.scope_id AND s.generation=u.generation WHERE u.revision_id='unit-policy'");
        CHECK(small ? maximum <= MEMORY_DOCUMENT_SMALL_UNIT_TOKENS :
                      maximum > MEMORY_DOCUMENT_SMALL_UNIT_TOKENS && maximum <= MEMORY_UNIT_TOKENS);
        printf("    unit policy small=%u measured_steps=%u measured_max_tokens=%lld\n", small, steps, (long long)maximum);
    }
}

int main(void)
{
    CHECK(setenv("SCRIBODO_MEMORY_SMALL_UNITS", "1", 1) == 0); experiments_reload();
    char name[64], cmd[256], error[512] = "";
    snprintf(name, sizeof(name), "scribodo_test_documents_%ld", (long)getpid());
    snprintf(cmd, sizeof(cmd), "tools/services.sh testdb create %s", name);
    if (system(cmd) != 0) return 1;
    DbConfig config;
    bool configured = db_config_default(&config, error, sizeof(error));
    snprintf(config.database, sizeof(config.database), "%s", name);
    db = configured ? db_open(&config, error, sizeof(error)) : NULL;
    CHECK(db != NULL);
    bool migrated = db && db_migrate(db, error, sizeof(error));
    CHECK(migrated);
    int result = 1;
    if (migrated) {
        const TestCase tests[] = {
            { "saved authority and typed delivery", saved_authority },
            { "bounded public argument validation", invalid_public_arguments },
            { "unicode resumability and fencing", unicode_and_fences },
            { "lexical allowset before limit", lexical_allowset_before_limit },
            { "bounded stale outbox maintenance", stale_outbox_batches },
            { "mutable saved source and malformed acknowledgment", mutable_saved_source },
             { "expired document publication and delivery backoff", expired_document_publication },
            { "default and experimental unit limits", default_and_experimental_unit_limits },
        };
        result = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    } else {
        fprintf(stderr, "%s\n", error);
    }
    db_close(db);
    snprintf(cmd, sizeof(cmd), "tools/services.sh testdb drop %s", name);
    if (system(cmd) != 0) result = 1;
    return result;
}
