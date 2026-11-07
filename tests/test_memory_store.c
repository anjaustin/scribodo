/* Real disposable MariaDB tests, using the same testdb service harness as
 * test_db. No model/tokenizer/network service is called. */
#define _DARWIN_C_SOURCE
#include "app/memory_store.h"
#include "app/store_backends.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

static Db *db;

static Db *open_peer(void)
{
    DbConfig config;
    char error[512];
    DbRows rows;
    CHECK(db_config_default(&config, error, sizeof(error)));
    CHECK(db_query(db, "SELECT DATABASE()", NULL, 0, &rows));
    snprintf(config.database, sizeof(config.database), "%s", db_cell(&rows, 0, 0));
    db_rows_free(&rows);
    Db *peer = db_open(&config, error, sizeof(error));
    CHECK(peer != NULL);
    return peer;
}

/* These checks require no database or service: invalid public arguments must
 * return before dereferencing the connection or writing through NULL outputs. */
static void invalid_arguments(void)
{
    MemoryScope scope;
    MemoryUnit unit;
    MemoryIndexLease lease;
    MemoryCandidate candidates[MEMORY_CANDIDATES];
    bool flag = true;
    size_t count = 99;
    memset(&scope, 0x7f, sizeof(scope));
    CHECK(!memory_scope_get(NULL, "p", "c", &scope) && !scope.enabled && !scope.id[0]);
    CHECK(!memory_scope_get(NULL, NULL, NULL, NULL));
    CHECK(!memory_scope_set(NULL, "p", "c", true, &scope) && !scope.id[0]);
    CHECK(!memory_scope_set(NULL, NULL, NULL, false, NULL));
    CHECK(!memory_scope_rebuild(NULL, "p", "c", &scope) && !scope.id[0]);
    CHECK(!memory_scope_rebuild(NULL, NULL, NULL, NULL));
    CHECK(!memory_message_intent(NULL, NULL, NULL, -1));
    CHECK(!memory_ingest_step(NULL, NULL, NULL, NULL, NULL, NULL, &flag, NULL, 1) && !flag);
    CHECK(!memory_ingest_step(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0));
    CHECK(!memory_outbox_claim(NULL, NULL, NULL, &lease, &flag) && !flag && !lease.id && !lease.unit.text);
    CHECK(!memory_outbox_claim(NULL, NULL, NULL, NULL, NULL));
    CHECK(!memory_outbox_ack(NULL, NULL, true));
    CHECK(!memory_candidate_hydrate(NULL, NULL, NULL, &unit, &flag) && !flag && !unit.text && !unit.id[0]);
    CHECK(!memory_candidate_hydrate(NULL, NULL, NULL, NULL, NULL));
    CHECK(!memory_lexical_candidates(NULL, NULL, NULL, 1, candidates, &count) && !count && !candidates[0].unit_id[0]);
    CHECK(!memory_lexical_candidates(NULL, NULL, NULL, 0, NULL, NULL));
    memory_unit_free(NULL);
    memory_index_lease_free(NULL);
}
static long tokens(const char *text,void *context)
{
    CHECK(!db_in_transaction(db));
    if(context) return -1;
    return (long)strlen(text); /* deliberately forces additional splitting */
}
static int64_t scalar(const char *sql)
{
    DbRows r; int64_t n=-999;
    CHECK(db_query(db,sql,NULL,0,&r));
    CHECK(r.nrows==1 && db_cell_i64(&r,0,0,&n)); db_rows_free(&r); return n;
}
static bool step(const MemoryScope *s)
{
    bool progress=false; char why[256];
    bool ok=memory_ingest_step(db,s->id,"worker",tokens,NULL,"test-exact-v1",&progress,why,sizeof(why));
    if(!ok) fprintf(stderr,"ingest: %s / %s\n",db_error(db),why);
    CHECK(ok); return progress;
}
static long reclaim_tokens(const char *text,void *context)
{
    MemoryScope *s=context;
    CHECK(!db_in_transaction(db));
    CHECK(db_run(db,"UPDATE memory_scopes SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)",NULL,0,NULL));
    bool progress=false; char why[128];
    CHECK(memory_ingest_step(db,s->id,"replacement",tokens,NULL,"test-exact-v1",&progress,why,sizeof(why))&&progress);
    return (long)strlen(text)/4; /* valid bounded count; the expired worker loses */
}

static long disable_tokens(const char *text, void *context)
{
    Db *peer = context;
    MemoryScope disabled;
    CHECK(!db_in_transaction(db));
    CHECK(memory_scope_set(peer, "p", "mutating", false, &disabled) && !disabled.enabled);
    return (long)strlen(text) / 4;
}
static void append(const char *content,ChatRole role,bool error,bool stopped,bool truncated,const char *via)
{
    ChatMessage m={0}; m.content=(char *)content; m.role=role; m.error=error; m.stopped=stopped; m.truncated=truncated; m.via=(char *)via;
    CHECK(sdb_chat_append(db,"p","c","Chat",&m));
}
static void memory_tests(void)
{
    MemoryScope s={0};
    CHECK(db_run(db,"INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES ('p','Book','/memory-test','memory-test',1,1),('other','Other','/other','other',1,1)",NULL,0,NULL));
    const char *sources = "{\"as_of\":\"test-snapshot\",\"units\":[\"not-model-history\"]}";
    ChatMessage with_sources = {0};
    with_sources.role = CHAT_USER;
    with_sources.content = "rarewhalefact first request";
    with_sources.memory_sources = (char *)sources;
    CHECK(sdb_chat_append(db, "p", "c", "Chat", &with_sources));
    ChatMessage *loaded = NULL;
    size_t loaded_count = 0;
    CHECK(sdb_chat_load(db, "p", "c", &loaded, &loaded_count) && loaded_count == 1);
    CHECK(loaded_count == 1 && loaded[0].memory_sources && !strcmp(loaded[0].memory_sources, sources));
    chat_store_free_messages(loaded, loaded_count);
    char oversized_sources[CHAT_MEMORY_SOURCES_BYTES + 2];
    memset(oversized_sources, 'x', sizeof(oversized_sources) - 1);
    oversized_sources[sizeof(oversized_sources) - 1] = 0;
    with_sources.memory_sources = oversized_sources;
    CHECK(!sdb_chat_append(db, "p", "c", "Chat", &with_sources));
    CHECK(scalar("SELECT COUNT(*) FROM messages") == 1);
    CHECK(!db_run(db, "INSERT INTO messages(conversation_id,seq,role,content,created_at,memory_sources)"
                      " VALUES ('c',1,'user','must fail',1,REPEAT('x',16385))", NULL, 0, NULL));
    CHECK(scalar("SELECT source_hash=SHA2(content,256) FROM messages WHERE conversation_id='c'") == 1);
    CHECK(!memory_scope_get(db,"p","c",&s) && !s.enabled);
    CHECK(scalar("SELECT COUNT(*) FROM memory_scopes")==0);
    CHECK(!memory_scope_set(db,"other","c",true,&s));
    CHECK(memory_scope_rebuild(db, "p", "c", &s) && !s.enabled);
    uint64_t disabled_generation = s.generation;
    CHECK(memory_scope_rebuild(db, "p", "c", &s) && !s.enabled && s.generation == disabled_generation);
    bool enabled=memory_scope_set(db,"p","c",true,&s);
    if(!enabled) fprintf(stderr,"enable: %s\n",db_error(db));
    CHECK(enabled&&s.enabled);
    if(!enabled) return;
    /* A queued Disable -> Rebuild -> Enable must not stick at a permanently
     * rejected FIFO head or let Rebuild grant consent by itself. */
    CHECK(memory_scope_set(db, "p", "c", false, &s) && !s.enabled);
    disabled_generation = s.generation;
    CHECK(memory_scope_rebuild(db, "p", "c", &s) && !s.enabled && s.generation == disabled_generation);
    CHECK(memory_scope_set(db, "p", "c", true, &s) && s.enabled);
    CHECK(s.pending_sequence == 0 && s.ingested_sequence == -1 && !s.index_pending);
    CHECK(!s.tokenizer[0]);
    char oversized[2002]; memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
    bool invalid_progress = true;
    MemoryScope invalid_scope;
    MemoryUnit invalid_unit;
    MemoryIndexLease invalid_lease;
    MemoryCandidate invalid_candidates[MEMORY_CANDIDATES];
    size_t invalid_count = 7;
    CHECK(!memory_scope_get(db, NULL, "c", &invalid_scope) && !invalid_scope.id[0]);
    CHECK(!memory_scope_set(db, "", "c", true, &invalid_scope));
    CHECK(!memory_scope_rebuild(db, "p", oversized, &invalid_scope));
    CHECK(!memory_message_intent(db, "p", "c", (int64_t)INT_MAX + 1));
    CHECK(!memory_ingest_step(db, s.id, "worker", tokens, NULL, "test-exact-v1", &invalid_progress, NULL, 1) && !invalid_progress);
    CHECK(!memory_ingest_step(db, s.id, oversized, tokens, NULL, "test-exact-v1", &invalid_progress, NULL, 0));
    CHECK(!memory_ingest_step(db, s.id, "worker", tokens, NULL, oversized, &invalid_progress, NULL, 0));
    CHECK(!memory_ingest_step(db, "", "worker", tokens, NULL, "test-exact-v1", &invalid_progress, NULL, 0));
    CHECK(!memory_outbox_claim(db, s.id, oversized, &invalid_lease, &invalid_progress) && !invalid_progress && !invalid_lease.id);
    CHECK(!memory_outbox_ack(db, NULL, false));
    CHECK(!memory_candidate_hydrate(db, NULL, NULL, &invalid_unit, &invalid_progress) && !invalid_progress && !invalid_unit.text);
    CHECK(!memory_lexical_candidates(db, &s, oversized, 1, invalid_candidates, &invalid_count) && invalid_count == 0);
    CHECK(!memory_lexical_candidates(db, &s, "query", MEMORY_CANDIDATES + 1, invalid_candidates, &invalid_count));
    CHECK(!memory_lexical_candidates(db, &s, "", 1, invalid_candidates, &invalid_count));
    CHECK(!db_in_transaction(db));
    DbRows origin;
    CHECK(db_query(db,"SELECT DATABASE()",NULL,0,&origin));
    CHECK(origin.nrows==1&&!strcmp(s.database,db_cell(&origin,0,0)));
    db_rows_free(&origin);
    CHECK(!memory_message_intent(db,"p","c",0)); /* unsaved intent refused */
    CHECK(!db_run(db,"INSERT INTO memory_scopes(id,project_id,conversation_id) VALUES ('wrong','other','c')",NULL,0,NULL));
    /* Source and append intent disappear together on rollback. */
    CHECK(db_begin(db));
    CHECK(db_run(db,"INSERT INTO messages(conversation_id,seq,role,content,created_at) VALUES ('c',1,'user','rollback',1)",NULL,0,NULL));
    CHECK(memory_message_intent(db,"p","c",1));
    db_rollback(db);
    CHECK(scalar("SELECT pending_seq FROM memory_scopes")==0);
    CHECK(scalar("SELECT COUNT(*) FROM messages")==1);
    CHECK(db_run(db,"CREATE TRIGGER memory_test_intent_failure BEFORE UPDATE ON memory_scopes FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected intent failure'",NULL,0,NULL));
    ChatMessage rollback_message = {0}; rollback_message.role = CHAT_USER; rollback_message.content = "must roll back";
    CHECK(!sdb_chat_append(db, "p", "c", "Chat", &rollback_message));
    CHECK(scalar("SELECT COUNT(*) FROM messages") == 1);
    CHECK(scalar("SELECT pending_seq FROM memory_scopes") == 0);
    CHECK(db_run(db,"DROP TRIGGER memory_test_intent_failure",NULL,0,NULL));
    /* Fail the outbox insert: immutable unit and receipt/cursor publication
     * must roll back too. The author's already committed source remains. */
    CHECK(db_run(db,"CREATE TRIGGER memory_test_outbox_failure BEFORE INSERT ON memory_outbox FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected outbox failure'",NULL,0,NULL));
    bool atomic_progress=false; char atomic_why[128];
    CHECK(!memory_ingest_step(db,s.id,"worker",tokens,NULL,"test-exact-v1",&atomic_progress,atomic_why,sizeof(atomic_why)));
    CHECK(!atomic_progress);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units")==0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_receipts")==0);
    CHECK(scalar("SELECT cursor_seq FROM memory_scopes")==-1);
    CHECK(scalar("SELECT COUNT(*) FROM messages")==1);
    CHECK(db_run(db,"DROP TRIGGER memory_test_outbox_failure",NULL,0,NULL));
    CHECK(db_run(db,"UPDATE memory_scopes SET retry_after=NULL",NULL,0,NULL));
    CHECK(step(&s));
    CHECK(memory_scope_get(db,"p","c",&s)&&s.ingested_sequence==0);
    CHECK(s.index_pending && s.pending_sequence == 0);
    CHECK(!strcmp(s.tokenizer, "test-exact-v1"));
    bool mismatch_progress = true; char mismatch_why[128];
    CHECK(!memory_ingest_step(db, s.id, "worker", tokens, NULL, "different-tokenizer", &mismatch_progress,
                             mismatch_why, sizeof(mismatch_why)) && !mismatch_progress && mismatch_why[0]);
    CHECK(scalar("SELECT cursor_seq FROM memory_scopes") == 0);
    CHECK(memory_scope_get(db, "p", "c", &s) && !strcmp(s.tokenizer, "test-exact-v1"));
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox")==1);
    CHECK(scalar("SELECT COUNT(*) FROM memory_receipts WHERE complete=1")==1);
    MemoryCandidate cs[32]; size_t n=0;
    CHECK(!memory_lexical_candidates(db,&s,"rarewhalefact",0,cs,&n));
    CHECK(memory_lexical_candidates(db,&s,"rarewhalefact",1,cs,&n)&&n==1);
    MemoryCandidate first=n?cs[0]:(MemoryCandidate){0}; MemoryUnit u; bool found=false;
    CHECK(memory_candidate_hydrate(db,&s,&first,&u,&found)&&found);
    if(found) CHECK(u.kind==MEMORY_SOURCE_CHAT && !u.document_id[0] && !u.revision_id[0] && !strcmp(u.text,"rarewhalefact first request"));
    memory_unit_free(&u);
    MemoryScope wrong=s; snprintf(wrong.project_id,sizeof(wrong.project_id),"other");
    CHECK(memory_candidate_hydrate(db,&wrong,&first,&u,&found)&&!found);
    wrong = s;
    snprintf(wrong.tokenizer, sizeof(wrong.tokenizer), "different-runtime");
    CHECK(memory_candidate_hydrate(db, &wrong, &first, &u, &found) && !found);
    CHECK(memory_lexical_candidates(db, &wrong, "rarewhalefact", 1, cs, &n) && n == 0);
    wrong=s; snprintf(wrong.database,sizeof(wrong.database),"another_database");
    CHECK(memory_candidate_hydrate(db,&wrong,&first,&u,&found)&&!found);
    CHECK(memory_lexical_candidates(db,&wrong,"rarewhalefact",1,cs,&n)&&n==0);
    MemoryCandidate bad=first; bad.text_hash[0]=bad.text_hash[0]=='a'?'b':'a';
    CHECK(memory_candidate_hydrate(db,&s,&bad,&u,&found)&&!found);
    MemoryIndexLease a,b; bool claimed=false;
    Db *peer = open_peer();
    if (!peer) return;
    CHECK(memory_outbox_claim(db,s.id,"index-a",&a,&claimed)&&claimed);
    CHECK(memory_outbox_claim(peer,s.id,"index-b",&b,&claimed)&&!claimed && !b.id && !b.scope.id[0]);
    CHECK(db_run(peer,"UPDATE memory_outbox SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)",NULL,0,NULL));
    CHECK(!memory_outbox_ack(db,&a,true));
    CHECK(memory_outbox_claim(peer,s.id,"index-b",&b,&claimed)&&claimed&&b.fence>a.fence);
    CHECK(!memory_outbox_ack(db,&a,true));
    CHECK(memory_outbox_ack(peer,&b,true)); CHECK(!memory_outbox_ack(db,&b,true));
    CHECK(memory_scope_get(db,"p","c",&s) && !s.index_pending);
    memory_index_lease_free(&a); memory_index_lease_free(&b);
    /* UTF-8 source far above any client window; restart after a partial chunk. */
    size_t bytes=24000; char *huge=malloc(bytes+1); CHECK(huge!=NULL);
    if(!huge) return;
    for(size_t i=0;i<bytes;i+=4) memcpy(huge+i,"🐋",4);
    huge[bytes]=0; append(huge,CHAT_USER,true,false,false,NULL); free(huge);
    CHECK(step(&s));
    CHECK(memory_scope_get(db,"p","c",&s)&&s.ingested_sequence==0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_receipts WHERE complete=0")==1);
    /* Drop the connection entirely, preserving only the owned scope snapshot. */
    DbRows database_rows; char database_name[64];
    CHECK(db_query(db,"SELECT DATABASE()",NULL,0,&database_rows));
    snprintf(database_name,sizeof(database_name),"%s",db_cell(&database_rows,0,0)); db_rows_free(&database_rows);
    db_close(db); DbConfig restart; char restart_error[512];
    CHECK(db_config_default(&restart,restart_error,sizeof(restart_error)));
    snprintf(restart.database,sizeof(restart.database),"%s",database_name);
    db=db_open(&restart,restart_error,sizeof(restart_error)); CHECK(db!=NULL);
    if(!db) return;
    int steps=0; while(step(&s)&&++steps<100) { }
    CHECK(steps<100);
    CHECK(memory_scope_get(db,"p","c",&s)&&s.ingested_sequence==1);
    CHECK(scalar("SELECT SUM(OCTET_LENGTH(text)) FROM memory_units WHERE sequence=1")==24000);
    CHECK(scalar("SELECT MAX(OCTET_LENGTH(text)) FROM memory_units")<=512);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units") == scalar("SELECT COUNT(*) FROM memory_outbox"));
    append("error",CHAT_ASSISTANT,true,false,false,NULL);
    append("stopped",CHAT_ASSISTANT,false,true,false,NULL);
    append("truncated",CHAT_ASSISTANT,false,false,true,NULL);
    append("remote",CHAT_ASSISTANT,false,false,false,"provider");
    append("failed user request",CHAT_USER,true,false,false,NULL);
    for(int i=0;i<5;i++) CHECK(step(&s));
    CHECK(memory_scope_get(db,"p","c",&s)&&s.ingested_sequence==6);
    CHECK(scalar("SELECT COUNT(*) FROM memory_receipts WHERE eligible=0 AND complete=1")==4);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE sequence BETWEEN 2 AND 5")==0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE sequence=6")==1);
    append("remote user", CHAT_USER, false, false, false, "provider");
    append("stopped user's request", CHAT_USER, true, true, true, NULL);
    CHECK(step(&s)); CHECK(step(&s));
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE sequence=7") == 0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE sequence=8") == 1);
    /* A source edit makes old locators unusable without trusting timestamps. */
    CHECK(db_run(db,"UPDATE messages SET content='changed' WHERE conversation_id='c' AND seq=0",NULL,0,NULL));
    CHECK(scalar("SELECT source_hash=SHA2('changed',256) FROM messages WHERE conversation_id='c' AND seq=0") == 1);
    CHECK(memory_candidate_hydrate(db,&s,&first,&u,&found)&&!found);
    CHECK(memory_outbox_claim(db,s.id,"index",&a,&claimed)&&claimed);
    uint64_t epoch=s.policy_epoch;
    CHECK(memory_scope_set(peer,"p","c",false,&s)&&!s.enabled&&s.policy_epoch>epoch);
    CHECK(!memory_outbox_ack(db,&a,true)); memory_index_lease_free(&a);
    db_close(peer);
    CHECK(memory_candidate_hydrate(db,&s,&first,&u,&found)&&!found);
    CHECK(memory_scope_set(db,"p","c",true,&s)&&s.enabled&&s.ingested_sequence==-1);
    CHECK(memory_candidate_hydrate(db,&s,&first,&u,&found)&&!found);
    CHECK(step(&s));
    uint64_t gen=s.generation;
    CHECK(memory_scope_rebuild(db,"p","c",&s)&&s.generation>gen&&s.ingested_sequence==-1);
    CHECK(step(&s));
    bool late_progress=false; char late_why[128];
    int64_t units_before=scalar("SELECT COUNT(*) FROM memory_units");
    CHECK(!memory_ingest_step(db,s.id,"late-worker",reclaim_tokens,&s,"test-exact-v1",&late_progress,late_why,sizeof(late_why))&&!late_progress);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units")==units_before+1);
    /* Unknown tokenizer count: no receipt completion or cursor movement. */
    append("unknown tokens",CHAT_USER,false,false,false,NULL);
    bool progress=false; char why[256];
    CHECK(!memory_ingest_step(db,s.id,"worker",tokens,(void *)1,"test-exact-v1",&progress,why,sizeof(why))&&!progress);
    CHECK(memory_scope_get(db,"p","c",&s)&&s.ingested_sequence==0);
    /* DB-clock lease expiry fences ingestion as well as index deliveries.
     * Three failed tokenizer attempts stop automatic retries, without moving
     * the source cursor or claiming completion. */
    for(int attempt=0;attempt<2;attempt++) {
        CHECK(db_run(db,"UPDATE memory_scopes SET retry_after=NULL",NULL,0,NULL));
        CHECK(!memory_ingest_step(db,s.id,"worker",tokens,(void *)1,"test-exact-v1",&progress,why,sizeof(why))&&!progress);
    }
    CHECK(scalar("SELECT attempts FROM memory_scopes")==3);
    CHECK(db_run(db,"UPDATE memory_scopes SET retry_after=NULL",NULL,0,NULL));
    CHECK(memory_ingest_step(db,s.id,"worker",tokens,NULL,"test-exact-v1",&progress,why,sizeof(why))&&!progress);
    CHECK(scalar("SELECT cursor_seq FROM memory_scopes")==0);
    CHECK(db_begin(db));
    CHECK(!memory_ingest_step(db,s.id,"worker",tokens,NULL,"test-exact-v1",&progress,why,sizeof(why)));
    CHECK(!memory_scope_set(db, "p", "c", false, &invalid_scope));
    CHECK(!memory_scope_rebuild(db, "p", "c", &invalid_scope));
    CHECK(!memory_outbox_claim(db, s.id, "index", &invalid_lease, &progress) && !progress && !invalid_lease.id);
    CHECK(db_in_transaction(db)); db_rollback(db);
}

static void adversarial_sources(void)
{
    char long_text[2401];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = 0;
    ChatMessage message = {0};
    message.role = CHAT_USER;
    message.content = long_text;
    CHECK(sdb_chat_append(db, "p", "mutating", "Mutating", &message));
    MemoryScope scope;
    CHECK(memory_scope_set(db, "p", "mutating", true, &scope));
    Db *peer = open_peer();
    if (!peer) return;
    bool raced_progress = true;
    char raced_why[128];
    CHECK(!memory_ingest_step(db, scope.id, "raced-worker", disable_tokens, peer, "test-exact-v1",
                             &raced_progress, raced_why, sizeof(raced_why)) && !raced_progress);
    db_close(peer);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && !scope.enabled && scope.ingested_sequence == -1);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE conversation_id='mutating'") == 0);
    CHECK(memory_scope_set(db, "p", "mutating", true, &scope));
    CHECK(step(&scope));
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.ingested_sequence == -1);
    CHECK(db_run(db, "UPDATE messages SET content=REPEAT('b',2400) WHERE conversation_id='mutating'", NULL, 0, NULL));
    bool progress = true;
    char why[256];
    CHECK(!memory_ingest_step(db, scope.id, "worker", tokens, NULL, "test-exact-v1", &progress, why, sizeof(why)) && !progress);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.ingested_sequence == -1);
    CHECK(memory_scope_rebuild(db, "p", "mutating", &scope));
    CHECK(step(&scope));
    CHECK(db_run(db, "UPDATE messages SET content='x' WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(!memory_ingest_step(db, scope.id, "worker", tokens, NULL, "test-exact-v1", &progress, why, sizeof(why)) && !progress);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.ingested_sequence == -1);
    CHECK(db_run(db, "UPDATE messages SET content=REPEAT('c',2400) WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(memory_scope_rebuild(db, "p", "mutating", &scope));
    CHECK(step(&scope));
    CHECK(db_run(db, "DELETE FROM messages WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(!memory_ingest_step(db, scope.id, "worker", tokens, NULL, "test-exact-v1", &progress, why, sizeof(why)) && !progress);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.ingested_sequence == -1);
    CHECK(db_run(db, "UPDATE memory_scopes SET retry_after=NULL WHERE conversation_id='mutating'", NULL, 0, NULL));
    /* A different keyset successor cannot inherit the deleted source's byte offset. */
    CHECK(db_run(db, "INSERT INTO messages(conversation_id,seq,role,content,created_at) VALUES ('mutating',1,'user',REPEAT('d',2400),1)", NULL, 0, NULL));
    CHECK(!memory_ingest_step(db, scope.id, "worker", tokens, NULL, "test-exact-v1", &progress, why, sizeof(why)) && !progress);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.ingested_sequence == -1);
    CHECK(db_run(db, "UPDATE messages SET content='tokenizerboundaryfact' WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(memory_scope_rebuild(db, "p", "mutating", &scope) && scope.pending_sequence == 1);
    CHECK(!scope.tokenizer[0]);
    CHECK(memory_ingest_step(db, scope.id, "worker", tokens, NULL, "alternate-v2", &progress, why, sizeof(why)) && progress);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.ingested_sequence == 1 && scope.index_pending);
    CHECK(!strcmp(scope.tokenizer, "alternate-v2"));
    CHECK(!memory_ingest_step(db, scope.id, "worker", tokens, NULL, "test-exact-v1", &progress, why, sizeof(why)) && !progress);
    MemoryCandidate candidates[MEMORY_CANDIDATES];
    size_t count = 0;
    CHECK(memory_lexical_candidates(db, &scope, "tokenizerboundaryfact", 1, candidates, &count) && count == 1);
    MemoryUnit unit;
    bool found = false;
    CHECK(memory_candidate_hydrate(db, &scope, &candidates[0], &unit, &found) && found);
    memory_unit_free(&unit);
    /* Source role is part of canonical eligibility, independently of its text hash. */
    CHECK(db_run(db, "UPDATE messages SET role='assistant' WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(memory_candidate_hydrate(db, &scope, &candidates[0], &unit, &found) && !found);
    CHECK(db_run(db, "SET SESSION sql_mode='NO_ENGINE_SUBSTITUTION'", NULL, 0, NULL));
    CHECK(db_run(db, "UPDATE messages SET role='' WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(db_run(db, "SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ZERO_DATE,NO_ENGINE_SUBSTITUTION'", NULL, 0, NULL));
    CHECK(memory_candidate_hydrate(db, &scope, &candidates[0], &unit, &found) && !found);
    CHECK(db_run(db, "UPDATE messages SET role='user' WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(db_run(db, "UPDATE memory_units SET tokenizer='incorrect' WHERE conversation_id='mutating'", NULL, 0, NULL));
    CHECK(memory_candidate_hydrate(db, &scope, &candidates[0], &unit, &found) && !found);
    CHECK(db_run(db, "UPDATE memory_units SET tokenizer='alternate-v2' WHERE conversation_id='mutating'", NULL, 0, NULL));

    /* Failed acknowledgments back off and terminate after three deliveries. */
    MemoryIndexLease lease, duplicate;
    bool claimed = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        CHECK(memory_outbox_claim(db, scope.id, "delivery", &lease, &claimed) && claimed);
        CHECK(memory_outbox_ack(db, &lease, false));
        memory_index_lease_free(&lease);
        CHECK(memory_outbox_claim(db, scope.id, "duplicate", &duplicate, &claimed) && !claimed && !duplicate.id);
        CHECK(db_run(db, "UPDATE memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id"
                          " SET o.retry_after=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE s.conversation_id='mutating'", NULL, 0, NULL));
    }
    CHECK(scalar("SELECT COUNT(*) FROM memory_outbox o JOIN memory_scopes s ON s.id=o.scope_id WHERE s.conversation_id='mutating' AND o.state='failed' AND o.attempts=3") == 1);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && scope.index_pending);
    CHECK(memory_scope_rebuild(db, "p", "mutating", &scope));
    CHECK(step(&scope));
    CHECK(memory_outbox_claim(db, scope.id, "delivery", &lease, &claimed) && claimed);
    CHECK(memory_outbox_ack(db, &lease, true));
    memory_index_lease_free(&lease);
    CHECK(memory_scope_get(db, "p", "mutating", &scope) && !scope.index_pending);

    /* Thousands of retired-generation jobs must not dominate active admission.
     * These synthetic rows are never hydrated or delivered. */
    CHECK(db_run(db, "INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
                      " SELECT SHA2(CONCAT(u.id,':retired:',q.seq),256),u.scope_id,u.project_id,u.conversation_id,u.message_id,u.sequence,u.history_epoch,u.policy_epoch,u.generation,100000+q.seq,u.source_hash,u.text_hash,u.text,u.tokens,u.tokenizer,u.role"
                      " FROM memory_units u JOIN memory_scopes s ON s.id=u.scope_id AND s.generation=u.generation CROSS JOIN seq_1_to_4096 q WHERE s.conversation_id='mutating'", NULL, 0, NULL));
    CHECK(db_run(db, "INSERT INTO memory_outbox(scope_id,unit_id,generation) SELECT u.scope_id,u.id,u.generation FROM memory_units u"
                      " JOIN memory_scopes s ON s.id=u.scope_id WHERE s.conversation_id='mutating' AND u.byte_start>=100000", NULL, 0, NULL));
    CHECK(memory_scope_rebuild(db, "p", "mutating", &scope));
    CHECK(step(&scope));
    CHECK(db_run(db, "ANALYZE TABLE memory_outbox", NULL, 0, NULL));
    DbRows explain;
    CHECK(db_query(db, "EXPLAIN SELECT id FROM memory_outbox WHERE scope_id=? AND generation=? AND attempts<3"
                       " AND state IN ('pending','leased') AND (lease_until IS NULL OR lease_until<=NOW(6))"
                       " AND (retry_after IS NULL OR retry_after<=NOW(6)) ORDER BY id LIMIT 1",
                   (DbArg[]){ DB_S(scope.id), DB_I(scope.generation) }, 2, &explain));
    const char *key = NULL;
    uint64_t estimated_rows = UINT64_MAX;
    CHECK(explain.nrows == 1 && db_cell_text(&explain, 0, 5, &key) && !strcmp(key, "admission") &&
          db_cell_u64(&explain, 0, 8, &estimated_rows) && estimated_rows <= 4);
    fprintf(stderr, "    outbox EXPLAIN: key=%s estimated_rows=%llu with 4096 retired-generation jobs\n",
            key ? key : "NULL", (unsigned long long)estimated_rows);
    db_rows_free(&explain);
}
int main(void)
{
    invalid_arguments();
    char name[64],cmd[256],err[512]="";
    snprintf(name,sizeof(name),"scribodo_test_memory_%ld",(long)getpid());
    snprintf(cmd,sizeof(cmd),"tools/services.sh testdb create %s",name);
    if(system(cmd)!=0) return 1;
    DbConfig config; bool configured=db_config_default(&config,err,sizeof(err));
    snprintf(config.database,sizeof(config.database),"%s",name);
    db=configured?db_open(&config,err,sizeof(err)):NULL;
    CHECK(db!=NULL);
    bool migrated=db&&db_migrate(db,err,sizeof(err)); CHECK(migrated);
    int result=1;
    if(migrated) {
        const TestCase tests[]={{"memory store",memory_tests},{"adversarial source lifecycle",adversarial_sources}};
        result=run_tests(tests,2);
    }
    else fprintf(stderr,"%s\n",err);
    db_close(db);
    snprintf(cmd,sizeof(cmd),"tools/services.sh testdb drop %s",name);
    if(system(cmd)!=0) result=1;
    return result;
}
