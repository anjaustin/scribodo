/* Disposable MariaDB integration tests. No embedding or Qdrant dependency. */
#define _DARWIN_C_SOURCE
#include "app/memory_gc.h"
#include "llm/memory_vectors.h"
#include "test_util.h"
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Db *db;
static DbConfig config;
static MemoryScope scope;
static const char *fp="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void run(const char *sql)
{
    bool ok=db_run(db,sql,NULL,0,NULL);
    if (!ok) fprintf(stderr,"SQL: %s\n",db_error(db));
    CHECK(ok);
}
static uint64_t scalar(const char *sql)
{
    DbRows r={0}; uint64_t n=UINT64_MAX;
    CHECK(db_query(db,sql,NULL,0,&r));
    CHECK(r.nrows==1 && db_cell_u64(&r,0,0,&n));
    db_rows_free(&r); return n;
}
static void bounded_cleanup(void)
{
    run("INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES('p','p','/p','p',1,1),('other','o','/o','o',1,1)");
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('c','p','c',1,1),('sibling','p','s',1,1)");
    run("INSERT INTO memory_scopes(id,project_id,conversation_id,enabled) VALUES('s','p','c',1),('ss','p','sibling',0)");
    run("UPDATE memory_scopes SET tokenizer='test' WHERE id='s'");
    CHECK(memory_scope_get(db,"p","c",&scope));
    run("INSERT INTO messages(conversation_id,seq,role,content,created_at,memory_sources)"
        " SELECT 'c',seq,'user','original source',1,'historical snapshot' FROM seq_1_to_40");
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('d','p','doc',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at) VALUES('r','d','h','original manuscript','save',1)");
    run("UPDATE documents SET current_revision_id='r',saved_revision_id='r' WHERE id='d'");
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2(CONCAT('u',id),256),'s','p','c',id,seq,1,1,1,0,source_hash,SHA2('derived',256),'derived',1,'test','user'"
        " FROM messages WHERE conversation_id='c'");
    run("INSERT INTO memory_outbox(scope_id,unit_id,generation) SELECT scope_id,id,generation FROM memory_units");
    run("INSERT INTO memory_receipts(scope_id,project_id,conversation_id,message_id,generation,sequence,source_hash,byte_offset,complete,eligible)"
        " SELECT 's','p','c',id,1,seq,source_hash,15,1,1 FROM messages WHERE conversation_id='c'");
    run("INSERT INTO memory_document_receipts(scope_id,project_id,conversation_id,generation,document_id,revision_id,source_hash,source_bytes)"
        " SELECT 's','p','c',1,'d','r',source_hash,OCTET_LENGTH(content) FROM revisions WHERE id='r'");
    MemoryIndexLease old;
    bool claimed=false;
    CHECK(memory_outbox_claim(db,scope.id,"old-delivery",&old,&claimed) && claimed);
    CHECK(db_begin(db));
    CHECK(memory_gc_namespace_enqueue(db,&scope,fp,1));
    db_rollback(db);
    CHECK(scalar("SELECT COUNT(*) FROM memory_retired_namespaces")==0);
    CHECK(db_begin(db));
    CHECK(memory_gc_namespace_enqueue(db,&scope,fp,1));
    run("UPDATE memory_scopes SET generation=5,enabled=0 WHERE id='s'");
    CHECK(db_commit(db));
    CHECK(memory_gc_namespace_enqueue(db,&scope,fp,1));
    CHECK(scalar("SELECT COUNT(*) FROM memory_retired_namespaces")==1);
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2(CONCAT('gap',id),256),'s','p','c',id,seq,1,1,3,0,source_hash,SHA2('gap',256),'gap',1,'test','user'"
        " FROM messages WHERE conversation_id='c' ORDER BY id LIMIT 1");
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2(CONCAT('active',id),256),'s','p','c',id,seq,1,1,5,0,source_hash,SHA2('active',256),'active',1,'test','user'"
        " FROM messages WHERE conversation_id='c' ORDER BY id LIMIT 1");
    MemoryGcStats st;
    MemoryScope wrong=scope;
    snprintf(wrong.database,sizeof(wrong.database),"wrong");
    CHECK(!memory_gc_step(db,&wrong,64,&st) && !st.total);
    wrong=scope; snprintf(wrong.project_id,sizeof(wrong.project_id),"other");
    CHECK(!memory_gc_step(db,&wrong,64,&st));
    CHECK(!memory_gc_step(db,&scope,65,&st));
    CHECK(!memory_gc_step(db,&scope,0,&st));
    /* The stale generation=1 snapshot is not the retirement cutoff. */
    CHECK(memory_gc_step(db,&scope,64,&st) && st.current_generation==5 && st.total==64 && st.outbox==40 && st.receipts==24);
    CHECK(!memory_outbox_ack(db,&old,true));
    memory_index_lease_free(&old);
    CHECK(memory_gc_step(db,&scope,64,&st) && st.total==58 && st.receipts==16 && st.document_receipts==1 && st.units==41);
    CHECK(memory_gc_step(db,&scope,64,&st) && st.total==0);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE generation=5 AND text='active'")==1);
    CHECK(scalar("SELECT COUNT(*) FROM messages WHERE content='original source' AND memory_sources='historical snapshot'")==40);
    CHECK(scalar("SELECT COUNT(*) FROM revisions WHERE content='original manuscript'")==1);
    CHECK(scalar("SELECT COUNT(*) FROM documents WHERE saved_revision_id='r' AND current_revision_id='r'")==1);
}
static void namespace_jobs(void)
{
    MemoryNamespaceLease first,second,wrong;
    bool claimed=true;
    run("UPDATE memory_scopes SET enabled=1 WHERE id='ss'");
    /* An enabled sibling prevents adoption; blocked jobs consume no attempts. */
    CHECK(memory_gc_namespace_claim(db,&scope,"worker",&first,&claimed) && !claimed);
    CHECK(scalar("SELECT SUM(attempts) FROM memory_retired_namespaces")==0);
    /* Low-generation disabled scopes do not keep an inactive project pending
     * forever: floor adoption will fence their next explicit enable. */
    run("UPDATE memory_scopes SET enabled=0 WHERE id='ss'");
    CHECK(memory_gc_namespace_claim(db,&scope,"worker",&first,&claimed) && claimed);
    wrong=first; snprintf(wrong.scope.database,sizeof(wrong.scope.database),"wrong");
    CHECK(!memory_gc_namespace_prepare(db,&wrong));
    CHECK(!memory_gc_namespace_ack(db,&wrong,true));
    wrong=first; snprintf(wrong.scope.project_id,sizeof(wrong.scope.project_id),"other");
    CHECK(!memory_gc_namespace_prepare(db,&wrong));
    CHECK(!memory_gc_namespace_ack(db,&wrong,true));
    run("UPDATE memory_retired_namespaces SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND)");
    CHECK(!memory_gc_namespace_ack(db,&first,true));
    /* A process/connection restart resumes durable work, never a stale lease. */
    db_close(db); char err[512]=""; db=db_open(&config,err,sizeof(err)); CHECK(db!=NULL);
    CHECK(memory_gc_namespace_claim(db,&scope,"worker2",&second,&claimed) && claimed && second.fence>first.fence);
    CHECK(!memory_gc_namespace_ack(db,&first,true));
    CHECK(!memory_gc_namespace_prepare(db,&first));
    /* Recheck immediately before network I/O, not just at claim. */
    run("UPDATE memory_scopes SET generation=1,enabled=1 WHERE id='ss'");
    CHECK(!memory_gc_namespace_prepare(db,&second));
    CHECK(!memory_gc_namespace_ack(db,&second,true));
    run("UPDATE memory_scopes SET enabled=0 WHERE id='ss'");
    /* An unrelated conversation may be busy with an author transaction.
     * Retirement uses the project control lock, not every conversation row. */
    Db *busy = db_open(&config,err,sizeof(err));
    CHECK(busy && db_begin(busy));
    DbRows locked = {0};
    CHECK(db_query(busy,"SELECT id FROM conversations WHERE id='sibling' FOR UPDATE",NULL,0,&locked));
    db_rows_free(&locked);
    run("SET SESSION innodb_lock_wait_timeout=1");
    CHECK(memory_gc_namespace_prepare(db,&second));
    db_rollback(busy);
    db_close(busy);
    CHECK(!db_in_transaction(db));
    CHECK(scalar("SELECT generation_floor FROM memory_namespace_floor WHERE project_id='p'")==1);
    CHECK(memory_gc_namespace_ack(db,&second,true));
    CHECK(!memory_gc_namespace_ack(db,&second,true));
    CHECK(memory_gc_namespace_enqueue(db,&scope,fp,1));
    CHECK(memory_gc_namespace_claim(db,&scope,"worker",&first,&claimed) && !claimed);
    MemoryScope retired=scope; retired.generation=3;
    CHECK(memory_gc_namespace_enqueue(db,&retired,fp,2));
    for (int i=0;i<3;i++) {
        CHECK(memory_gc_namespace_claim(db,&scope,"retry",&first,&claimed) && claimed);
        CHECK(memory_gc_namespace_prepare(db,&first) && !db_in_transaction(db));
        CHECK(memory_gc_namespace_ack(db,&first,false));
        CHECK(memory_gc_namespace_claim(db,&scope,"retry",&second,&claimed) && !claimed);
        run("UPDATE memory_retired_namespaces SET retry_after=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE state='pending'");
    }
    CHECK(scalar("SELECT COUNT(*) FROM memory_retired_namespaces WHERE state='failed' AND attempts=3")==1);
    CHECK(memory_gc_namespace_claim(db,&scope,"retry",&first,&claimed) && !claimed);
    /* Simulate a crashed late creator after successful deletion: its exact
     * tombstone must remain auditable with a new lease fence, including after
     * an audit batch exhausted its bounded retries. */
    run("UPDATE memory_retired_namespaces SET retry_after=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE state='done'");
    CHECK(memory_gc_namespace_claim(db,&scope,"audit",&first,&claimed) && claimed);
    CHECK(first.namespace_version==1 && first.fence>second.fence);
    CHECK(memory_gc_namespace_prepare(db,&first) && !db_in_transaction(db));
    CHECK(memory_gc_namespace_ack(db,&first,true));
    CHECK(!memory_gc_namespace_ack(db,&second,true));
    run("UPDATE memory_retired_namespaces SET retry_after=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE state='failed'");
    CHECK(memory_gc_namespace_claim(db,&scope,"audit-retry",&first,&claimed) && claimed);
    CHECK(first.namespace_version==2);
    CHECK(memory_gc_namespace_prepare(db,&first));
    CHECK(memory_gc_namespace_ack(db,&first,true));
    run("UPDATE memory_scopes SET enabled=1 WHERE id='s'");
    retired.generation=5;
    CHECK(memory_gc_namespace_enqueue(db,&retired,fp,2));
    CHECK(memory_gc_namespace_claim(db,&scope,"retry",&first,&claimed) && !claimed);
    CHECK(scalar("SELECT COUNT(*) FROM memory_retired_namespaces WHERE generation=5 AND attempts=0")==1);
    retired.generation=6;
    CHECK(!memory_gc_namespace_enqueue(db,&retired,fp,2));
    CHECK(!memory_gc_namespace_enqueue(db,&scope,"not-a-fingerprint",2));
    CHECK(!memory_gc_namespace_enqueue(db,&scope,fp,0));
    retired=scope; snprintf(retired.project_id,sizeof(retired.project_id),"unsafe__alias");
    CHECK(!memory_gc_namespace_enqueue(db,&retired,fp,2));
}
static void floor_admission(void)
{
    CHECK(scalar("SELECT generation_floor FROM memory_namespace_floor WHERE project_id='p'")==3);
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('low','p','low',1,1),('new','p','new',1,1)");
    run("INSERT INTO memory_scopes(id,project_id,conversation_id) VALUES('low-scope','p','low')");
    MemoryScope low, fresh;
    CHECK(memory_scope_set(db,"p","low",true,&low) && low.enabled && low.generation==4);
    CHECK(memory_scope_set(db,"p","new",false,&fresh) && !fresh.enabled && fresh.generation==4);
    CHECK(memory_scope_set(db,"p","new",true,&fresh) && fresh.enabled && fresh.generation==5);
    bool allowed=false, retired=false;
    CHECK(memory_gc_namespace_admit(db,&fresh,&allowed,&retired) && allowed && !retired);
    fresh.generation=3;
    CHECK(memory_gc_namespace_admit(db,&fresh,&allowed,&retired) && !allowed && retired);
    snprintf(fresh.database,sizeof(fresh.database),"wrong");
    CHECK(!memory_gc_namespace_admit(db,&fresh,&allowed,&retired) && !allowed && !retired);
    CHECK(memory_scope_rebuild(db,"p","low",&low) && low.generation==5);
}

static uint64_t project_derived_count(void)
{
    return scalar("SELECT (SELECT COUNT(*) FROM memory_brief_inputs WHERE scope_id='gcq-retired')"
                  "+(SELECT COUNT(*) FROM memory_briefs WHERE scope_id='gcq-retired')"
                  "+(SELECT COUNT(*) FROM memory_brief_state WHERE scope_id='gcq-retired')"
                  "+(SELECT COUNT(*) FROM memory_outbox WHERE scope_id='gcq-retired')"
                  "+(SELECT COUNT(*) FROM memory_receipts WHERE scope_id='gcq-retired')"
                  "+(SELECT COUNT(*) FROM memory_document_receipts WHERE scope_id='gcq-retired')"
                  "+(SELECT COUNT(*) FROM memory_units WHERE scope_id='gcq-retired')");
}

static void project_queue(void)
{
    /* Simulate adopting pre-012 scopes. Receipt replay is metadata-only and
     * preserves namespace floors and the original/provenance sentinels. */
    run("DELETE FROM schema_migrations WHERE version>=12");
    char error[512] = "";
    CHECK(db_migrate(db, error, sizeof(error)));
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes") == scalar("SELECT COUNT(*) FROM memory_scopes"));
    CHECK(scalar("SELECT generation_floor FROM memory_namespace_floor WHERE project_id='p'") == 3);

    run("INSERT INTO projects(id,name,path,path_hash,created_at,updated_at)"
        " VALUES('gcq','gcq','/gcq','gcq',1,1),('gcq-foreign','foreign','/foreign','foreign',1,1)");
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES"
        " ('gcq-old-c','gcq','old',1,1),('gcq-active-c','gcq','active',1,1),"
        " ('gcq-selected-off','gcq','selected default off',1,1),('gcq-foreign-c','gcq-foreign','foreign',1,1)");
    run("INSERT INTO memory_scopes(id,project_id,conversation_id,enabled,generation) VALUES"
        " ('gcq-retired','gcq','gcq-old-c',1,1),('gcq-active','gcq','gcq-active-c',1,10),"
        " ('gcq-foreign-s','gcq-foreign','gcq-foreign-c',0,2)");
    run("INSERT INTO messages(conversation_id,seq,role,content,created_at,memory_sources)"
        " SELECT 'gcq-old-c',seq,'user','project original',1,'historical provenance' FROM seq_1_to_40");
    run("INSERT INTO messages(conversation_id,seq,role,content,created_at)"
        " VALUES('gcq-foreign-c',1,'user','foreign original',1)");
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2(CONCAT('gcq-unit-',seq),256),'gcq-retired','gcq','gcq-old-c',id,seq,1,1,1,0,"
        " source_hash,SHA2('derived',256),'derived',1,'test','user' FROM messages WHERE conversation_id='gcq-old-c'");
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2('gcq-foreign-unit',256),'gcq-foreign-s','gcq-foreign','gcq-foreign-c',id,seq,1,1,1,0,"
        " source_hash,SHA2('foreign',256),'foreign',1,'test','user' FROM messages WHERE conversation_id='gcq-foreign-c'");
    run("INSERT INTO memory_gc_scopes(scope_id,project_id,conversation_id)"
        " VALUES('gcq-foreign-s','gcq-foreign','gcq-foreign-c')");
    run("INSERT INTO memory_outbox(scope_id,unit_id,generation)"
        " SELECT scope_id,id,generation FROM memory_units WHERE scope_id='gcq-retired'");
    run("INSERT INTO memory_receipts(scope_id,project_id,conversation_id,message_id,generation,sequence,source_hash,byte_offset,complete,eligible)"
        " SELECT 'gcq-retired','gcq','gcq-old-c',id,1,seq,source_hash,16,1,1 FROM messages WHERE conversation_id='gcq-old-c'");
    run("INSERT INTO documents(id,project_id,name,created_at) VALUES('gcq-d','gcq','original.md',1)");
    run("INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
        " VALUES('gcq-r','gcq-d','h','original project manuscript','save',1)");
    run("UPDATE documents SET current_revision_id='gcq-r',saved_revision_id='gcq-r' WHERE id='gcq-d'");
    run("INSERT INTO memory_document_receipts(scope_id,project_id,conversation_id,generation,document_id,revision_id,source_hash,source_bytes)"
        " SELECT 'gcq-retired','gcq','gcq-old-c',1,'gcq-d','gcq-r',source_hash,OCTET_LENGTH(content) FROM revisions WHERE id='gcq-r'");
    run("INSERT INTO memory_briefs(id,scope_id,project_id,conversation_id,history_epoch,policy_epoch,generation,"
        "begin_sequence,begin_byte,end_sequence,end_byte,previous_sequence,previous_byte_end,input_digest,tokenizer,"
        "model_fingerprint,prompt_schema,summary_json,summary_tokens,input_count)"
        " SELECT SHA2(CONCAT('gcq-brief-',seq),256),'gcq-retired','gcq','gcq-old-c',1,1,1,seq,0,seq,7,-1,0,"
        " SHA2('input',256),'test','test-model','test-v1','{}',1,1 FROM seq_1_to_20");
    run("INSERT INTO memory_brief_inputs(brief_id,ordinal,unit_id,scope_id,generation,source_hash,text_hash)"
        " SELECT SHA2(CONCAT('gcq-brief-',m.seq),256),1,u.id,u.scope_id,u.generation,u.source_hash,u.text_hash"
        " FROM messages m JOIN memory_units u ON u.message_id=m.id WHERE u.scope_id='gcq-retired' AND m.seq<=20");
    run("INSERT INTO memory_brief_state(scope_id,generation) VALUES('gcq-retired',1)");

    MemoryScope old, disabled, maintained;
    CHECK(memory_scope_get(db, "gcq", "gcq-old-c", &old));
    CHECK(memory_gc_namespace_enqueue(db, &old, fp, 2));
    CHECK(memory_scope_set(db, "gcq", "gcq-old-c", false, &disabled) && !disabled.enabled && disabled.generation == 2);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-retired'") == 1);
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2('gcq-sentinel-unit',256),'gcq-retired','gcq','gcq-old-c',id,seq,2,2,2,0,"
        " source_hash,SHA2('sentinel',256),'sentinel',1,'test','user' FROM messages WHERE conversation_id='gcq-old-c' ORDER BY id LIMIT 1");
    run("INSERT INTO memory_briefs(id,scope_id,project_id,conversation_id,history_epoch,policy_epoch,generation,"
        "begin_sequence,begin_byte,end_sequence,end_byte,previous_sequence,previous_byte_end,input_digest,tokenizer,"
        "model_fingerprint,prompt_schema,summary_json,summary_tokens,input_count)"
        " VALUES(SHA2('gcq-sentinel-brief',256),'gcq-retired','gcq','gcq-old-c',2,2,2,1,0,1,8,-1,0,"
        " SHA2('input',256),'test','test-model','test-v1','{}',1,1)");
    run("INSERT INTO memory_brief_inputs(brief_id,ordinal,unit_id,scope_id,generation,source_hash,text_hash)"
        " SELECT SHA2('gcq-sentinel-brief',256),1,id,scope_id,generation,source_hash,text_hash FROM memory_units WHERE id=SHA2('gcq-sentinel-unit',256)");
    run("INSERT INTO memory_brief_state(scope_id,generation) VALUES('gcq-retired',2)");

    MemoryGcStats stats;
    CHECK(!memory_gc_project_step(db, "wrong_database", "gcq", 64, &stats, &maintained) && !stats.total && !maintained.id[0]);
    CHECK(!memory_gc_project_step(db, config.database, "missing", 64, &stats, &maintained));
    CHECK(!memory_gc_project_step(db, config.database, "gcq", 65, &stats, &maintained));
    CHECK(!memory_gc_project_step(db, config.database, "gcq", 0, &stats, &maintained));
    CHECK(memory_gc_project_step(db, config.database, "other", 64, &stats, &maintained) && !stats.total && !maintained.id[0]);
    uint64_t before = project_derived_count();
    CHECK(memory_gc_project_step(db, config.database, "gcq", 64, &stats, &maintained));
    CHECK(stats.total == 64 && stats.brief_inputs == 20 && stats.briefs == 20 && stats.brief_states == 1 && stats.outbox == 23);
    CHECK(project_derived_count() == before - 64); /* No hidden FK cascade rows. */
    CHECK(!strcmp(maintained.id, disabled.id) && !maintained.enabled && maintained.generation == 2);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-retired'") == 1);
    size_t total = stats.total;
    for (int i = 0; i < 4 && scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-retired'"); i++) {
        before = project_derived_count();
        CHECK(memory_gc_project_step(db, config.database, "gcq", 64, &stats, &maintained));
        CHECK(stats.total <= 64 && project_derived_count() == before - stats.total);
        total += stats.total;
    }
    CHECK(total == 162 && project_derived_count() == 4);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-retired'") == 0);
    CHECK(scalar("SELECT enabled=1 AND generation=10 FROM memory_scopes WHERE id='gcq-active'") == 1);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE scope_id='gcq-foreign-s'") == 1);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-foreign-s'") == 1);
    CHECK(scalar("SELECT COUNT(*) FROM messages WHERE conversation_id='gcq-old-c' AND content='project original'"
                 " AND memory_sources='historical provenance'") == 40);
    CHECK(scalar("SELECT COUNT(*) FROM revisions WHERE id='gcq-r' AND content='original project manuscript'") == 1);
    CHECK(scalar("SELECT COUNT(*) FROM documents WHERE id='gcq-d' AND saved_revision_id='gcq-r' AND current_revision_id='gcq-r'") == 1);
    CHECK(memory_gc_project_step(db, config.database, "gcq", 64, &stats, &maintained) && !stats.total && maintained.id[0]);
    MemoryNamespaceLease lease;
    bool claimed = false;
    CHECK(memory_gc_namespace_claim(db, &maintained, "project-maintenance", &lease, &claimed) && claimed);
    CHECK(lease.scope.generation == 1 && !strcmp(lease.scope.project_id, "gcq"));
    CHECK(memory_gc_namespace_ack(db, &lease, false));
    CHECK(!db_in_transaction(db));
}

typedef struct {
    atomic_bool started;
    bool ok;
    MemoryGcStats stats;
    MemoryScope scope;
} ProjectGcRace;

static void *project_gc_peer(void *arg)
{
    ProjectGcRace *race = arg;
    char error[512] = "";
    Db *peer = db_open(&config, error, sizeof(error));
    atomic_store(&race->started, true);
    race->ok = peer && memory_gc_project_step(peer, config.database, "gcq-race", 64, &race->stats, &race->scope);
    db_close(peer);
    return NULL;
}

static void project_generation_race(void)
{
    run("INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES('gcq-race','race','/gcq-race','gcq-race',1,1)");
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('gcq-race-c','gcq-race','race',1,1)");
    run("INSERT INTO memory_scopes(id,project_id,conversation_id,enabled) VALUES('gcq-race-s','gcq-race','gcq-race-c',1)");
    run("INSERT INTO messages(conversation_id,seq,role,content,created_at,memory_sources)"
        " VALUES('gcq-race-c',1,'user','race original',1,'race provenance')");
    run("INSERT INTO memory_units(id,scope_id,project_id,conversation_id,message_id,sequence,history_epoch,policy_epoch,"
        "generation,byte_start,source_hash,text_hash,text,tokens,tokenizer,role)"
        " SELECT SHA2(CONCAT('race-unit-',q.seq),256),'gcq-race-s','gcq-race','gcq-race-c',m.id,m.seq,1,1,q.seq,0,"
        " m.source_hash,SHA2('derived',256),'derived',1,'test','user' FROM messages m CROSS JOIN seq_1_to_3 q"
        " WHERE m.conversation_id='gcq-race-c'");
    MemoryScope changed;
    CHECK(memory_scope_set(db, "gcq-race", "gcq-race-c", false, &changed) && changed.generation == 2);
    CHECK(db_begin(db));
    run("UPDATE memory_scopes SET generation=generation WHERE id='gcq-race-s'");
    ProjectGcRace race = {0};
    atomic_init(&race.started, false);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, project_gc_peer, &race) == 0);
    while (!atomic_load(&race.started)) usleep(1000);
    usleep(30000); /* Give admission time to observe generation 2 before its locking read. */
    /* The same atomic generation+intent writes as scope_change, while the peer
     * cannot acquire its authoritative scope lock. Its cutoff MUST be 3. */
    run("UPDATE memory_scopes SET generation=3,history_epoch=history_epoch+1,policy_epoch=policy_epoch+1 WHERE id='gcq-race-s'");
    run("INSERT INTO memory_gc_scopes(scope_id,project_id,conversation_id) VALUES('gcq-race-s','gcq-race','gcq-race-c')"
        " ON DUPLICATE KEY UPDATE scope_id=scope_id");
    CHECK(db_commit(db));
    pthread_join(worker, NULL);
    CHECK(race.ok && race.stats.current_generation == 3 && race.stats.total == 2 && race.scope.generation == 3);
    CHECK(scalar("SELECT COUNT(*) FROM memory_units WHERE scope_id='gcq-race-s' AND generation=3") == 1);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-race-s'") == 0);
    CHECK(memory_scope_rebuild(db, "gcq-race", "gcq-race-c", &changed) && !changed.enabled && changed.generation == 3);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-race-s'") == 0);
    /* A bump after the previous cleanup commit must requeue, not lose intent. */
    CHECK(memory_scope_set(db, "gcq-race", "gcq-race-c", true, &changed) && changed.generation == 4);
    CHECK(scalar("SELECT COUNT(*) FROM memory_gc_scopes WHERE scope_id='gcq-race-s'") == 1);
    MemoryGcStats stats;
    CHECK(memory_gc_project_step(db, config.database, "gcq-race", 64, &stats, &changed) && stats.total == 1);
    CHECK(scalar("SELECT COUNT(*) FROM messages WHERE conversation_id='gcq-race-c' AND content='race original'"
                 " AND memory_sources='race provenance'") == 1);
}

typedef struct {
    int listener;
    MemoryScope old;
    char expected_path[400];
    int requests, deletes, request_limit, delete_response;
    bool prepared, sql_unlocked;
    MemoryNamespaceLease retirement;
} CreateRace;

static void *race_server(void *arg)
{
    CreateRace *race = arg;
    for (int i = 0; i < race->request_limit; i++) {
        struct pollfd wait = { .fd = race->listener, .events = POLLIN };
        if (poll(&wait, 1, 5000) != 1) break;
        int fd = accept(race->listener, NULL, NULL);
        if (fd < 0) break;
        struct timeval timeout = { .tv_sec = 3 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char request[8192] = "";
        size_t used = 0;
        while (used < sizeof(request) - 1) {
            ssize_t got = recv(fd, request + used, sizeof(request) - 1 - used, 0);
            if (got <= 0) break;
            used += (size_t)got;
            request[used] = '\0';
            char *end = strstr(request, "\r\n\r\n");
            char *length = strcasestr(request, "Content-Length:");
            size_t body = length ? (size_t)strtoul(length + 15, NULL, 10) : 0;
            if (end && used >= (size_t)(end + 4 - request) + body) break;
        }
        char method[16] = "", path[400] = "";
        sscanf(request, "%15s %399s", method, path);
        CHECK(!strcmp(path, race->expected_path));
        ++race->requests;
        const char *body = "{\"status\":\"ok\",\"result\":true}";
        char missing[600];
        int status = 200;
        if (!strcmp(method, "GET")) {
            status = 404;
            body = "{}";
        } else if (!strcmp(method, "PUT")) {
            char error[512] = "";
            Db *peer = db_open(&config, error, sizeof(error));
            CHECK(peer != NULL);
            if (peer) {
                CHECK(db_run(peer, "SET innodb_lock_wait_timeout=1", NULL, 0, NULL));
                MemoryScope disabled;
                race->sql_unlocked = memory_scope_set(peer, "race", "race-c", false, &disabled);
                MemoryNamespaceLease lease;
                bool claimed = false;
                race->prepared = race->sql_unlocked &&
                    memory_gc_namespace_claim(peer, &disabled, "race-worker", &lease, &claimed) && claimed &&
                    memory_gc_namespace_prepare(peer, &lease) && !db_in_transaction(peer);
                CHECK(race->prepared);
                if (race->prepared) race->retirement = lease;
                db_close(peer);
            }
        } else if (!strcmp(method, "DELETE")) {
            ++race->deletes;
            /* This must succeed while the initiating thread waits for HTTP:
             * no transaction/project lock may survive namespace prepare. */
            char error[512] = "";
            Db *peer = db_open(&config, error, sizeof(error));
            CHECK(peer != NULL);
            race->sql_unlocked = peer && db_run(peer, "SET innodb_lock_wait_timeout=1", NULL, 0, NULL) &&
                db_run(peer, "UPDATE projects SET updated_at=updated_at+1 WHERE id='race'", NULL, 0, NULL);
            CHECK(race->sql_unlocked);
            db_close(peer);
            if (race->delete_response == 1) body = "{\"status\":\"ok\",\"result\":false}";
            if (race->delete_response == 2) {
                status = 404;
                body = "{}";
            }
            if (race->delete_response == 3) {
                status = 404;
                snprintf(missing, sizeof(missing),
                    "{\"status\":{\"error\":\"Not found: Collection `%s` doesn't exist!\"}}",
                    race->expected_path + strlen("/collections/"));
                body = missing;
            }
        } else {
            CHECK(false);
        }
        char response[1024];
        int size = snprintf(response, sizeof(response),
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            status, status == 200 ? "OK" : "Not Found", strlen(body), body);
        size_t sent = 0;
        while (sent < (size_t)size) {
            ssize_t count = send(fd, response + sent, (size_t)size - sent, 0);
            if (count <= 0) break;
            sent += (size_t)count;
        }
        close(fd);
    }
    return NULL;
}

static void late_creator(void)
{
    run("INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES('race','race','/race','race',1,1)");
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('race-c','race','race',1,1)");
    CreateRace race = { .listener = -1, .request_limit = 3 };
    CHECK(memory_scope_set(db, "race", "race-c", true, &race.old));
    race.listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(race.listener >= 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(race.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(race.listener, 4) == 0);
    socklen_t length = sizeof(address);
    CHECK(getsockname(race.listener, (struct sockaddr *)&address, &length) == 0);
    const char *previous_port = getenv("SCRIBODO_QDRANT_PORT");
    char *saved_port = previous_port ? strdup(previous_port) : NULL;
    char port[16];
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    setenv("SCRIBODO_QDRANT_PORT", port, 1);
    StoreManager *store = store_manager_create();
    char error[512] = "";
    CHECK(store_start(store, config.database, error, sizeof(error)));
    SemanticClient *client = semantic_client_create(store, NULL);
    CHECK(client != NULL);
    char fingerprint[65];
    CHECK(memory_vectors_fingerprint(client, fingerprint));
    CHECK(memory_gc_namespace_enqueue(db, &race.old, fingerprint, 2));
    snprintf(race.expected_path, sizeof(race.expected_path), "/collections/%s__race__memory_v2__%s__%llu",
             config.database, fingerprint, (unsigned long long)race.old.generation);
    pthread_t server;
    CHECK(pthread_create(&server, NULL, race_server, &race) == 0);
    CHECK(!memory_vectors_create(client, &race.old, error, sizeof(error)));
    pthread_join(server, NULL);
    CHECK(race.requests == 3 && race.deletes == 1 && race.prepared && race.sql_unlocked);
    CHECK(!db_in_transaction(store_db(store)));
    MemoryNamespaceLease wrong = race.retirement;
    snprintf(wrong.scope.database, sizeof(wrong.scope.database), "wrong");
    CHECK(!memory_vectors_retire_namespace(client, &wrong, error, sizeof(error)));
    /* Acknowledged false and generic proxy 404 are not deletion receipts.
     * Exact Qdrant missing and result=true are safe idempotent successes. */
    race.request_limit = 1;
    for (int mode = 1; mode <= 4; mode++) {
        race.delete_response = mode;
        CHECK(pthread_create(&server, NULL, race_server, &race) == 0);
        bool deleted = memory_vectors_retire_namespace(client, &race.retirement, error, sizeof(error));
        pthread_join(server, NULL);
        CHECK(deleted == (mode >= 3));
        CHECK(!db_in_transaction(store_db(store)) && race.sql_unlocked);
    }
    CHECK(memory_gc_namespace_ack(db, &race.retirement, true));
    semantic_client_destroy(client);
    store_manager_destroy(store);
    close(race.listener);
    if (saved_port) setenv("SCRIBODO_QDRANT_PORT", saved_port, 1);
    else unsetenv("SCRIBODO_QDRANT_PORT");
    free(saved_port);
}
static void orphaned_namespace_jobs(void)
{
    run("INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES('orphan','o','/orphan','orphan',1,1)");
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('orphan-c','orphan','o',1,1)");
    MemoryScope original;
    CHECK(memory_scope_set(db,"orphan","orphan-c",true,&original));
    CHECK(memory_gc_namespace_enqueue(db,&original,fp,2));
    run("DELETE FROM conversations WHERE id='orphan-c'");
    MemoryGcStats stats;
    MemoryScope maintained;
    CHECK(memory_gc_project_step(db,config.database,"orphan",64,&stats,&maintained));
    CHECK(!maintained.id[0] && !stats.total);
    MemoryNamespaceLease lease;
    bool claimed = true;
    CHECK(!memory_gc_namespace_claim_project(db,"wrong","orphan","orphan-worker",&lease,&claimed) && !claimed);
    CHECK(memory_gc_namespace_claim_project(db,config.database,"other","orphan-worker",&lease,&claimed) && !claimed);
    CHECK(memory_gc_namespace_claim_project(db,config.database,"orphan","orphan-worker",&lease,&claimed) && claimed);
    CHECK(!strcmp(lease.scope.id,original.id) && !strcmp(lease.scope.conversation_id,"orphan-c"));
    CHECK(lease.scope.generation==original.generation);
    CHECK(memory_gc_namespace_prepare(db,&lease) && !db_in_transaction(db));
    CHECK(memory_gc_namespace_ack(db,&lease,true));
    run("INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES('orphan-new','orphan','new',1,1)");
    MemoryScope fresh;
    CHECK(memory_scope_set(db,"orphan","orphan-new",true,&fresh) && fresh.generation>original.generation);
}

int main(void)
{
    MemoryGcStats st;
    CHECK(!memory_gc_step(NULL,NULL,64,&st) && !st.total);
    char name[64],cmd[256],err[512]="";
    snprintf(name,sizeof(name),"scribodo_test_gc_%ld",(long)getpid());
    snprintf(cmd,sizeof(cmd),"tools/services.sh testdb create %s",name);
    if (system(cmd)!=0) return 1;
    bool configured=db_config_default(&config,err,sizeof(err));
    snprintf(config.database,sizeof(config.database),"%s",name);
    db=configured ? db_open(&config,err,sizeof(err)) : NULL;
    CHECK(db!=NULL);
    bool migrated=db && db_migrate(db,err,sizeof(err)); CHECK(migrated);
    int result=1;
    if (migrated) {
        const TestCase tests[]={{"bounded derived cleanup",bounded_cleanup},{"durable namespace retirement",namespace_jobs},
                                {"floor-safe scope admission",floor_admission},
                                {"project-wide disabled-scope maintenance",project_queue},
                                 {"project maintenance generation race",project_generation_race},
                                 {"namespace cleanup without surviving scopes",orphaned_namespace_jobs}};
        result=run_tests(tests,6);
        const TestCase race_tests[] = {{"late creator postcheck", late_creator}};
        if (run_tests(race_tests, 1)) result = 1;
    } else fprintf(stderr,"%s\n",err);
    db_close(db);
    snprintf(cmd,sizeof(cmd),"tools/services.sh testdb drop %s",name);
    if (system(cmd)!=0) result=1;
    return result;
}
