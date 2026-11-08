/* Native ChatSession inference/admission with a frozen selection-ceiling seam.
 * Only candidate discovery is replaced: real SQL hydration, eligibility,
 * deadline, token fitting, final admission, streaming and receipt paths run.
 * NEVER link this fixture into the application. No Qdrant lookup is claimed. */
#define memory_plan_build_with_documents native_frozen_plan
#include "../src/llm/chat.c"
#undef memory_plan_build_with_documents
#include "app/store_backends.h"
#include "app/records.h"
#include <sys/stat.h>

typedef struct { Db *db; MemoryTokenCount count; void *counter; } NativeRecall;
static int64_t native_episode_end;

static bool native_scope(void *ctx, const char *p, const char *c, MemoryScope *out)
{ return memory_scope_get(((NativeRecall *)ctx)->db, p, c, out); }
static bool native_candidates(void *ctx, const MemoryScope *scope, const char *query,
                               size_t limit, MemoryCandidate *out, size_t *count)
{
    (void)query; *count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT id,text_hash FROM memory_units WHERE scope_id=? AND generation=?"
             " AND source_kind='chat' AND sequence<? ORDER BY sequence,byte_start LIMIT %zu", limit);
    DbRows rows = {0};
    bool ok = db_query(((NativeRecall *)ctx)->db, sql,
                       (DbArg[]){DB_S(scope->id), DB_I(scope->generation), DB_I(native_episode_end)}, 3, &rows);
    for (size_t i = 0; ok && i < rows.nrows; i++) {
        MemoryCandidate *candidate = &out[i];
        *candidate = (MemoryCandidate){.generation=scope->generation, .history_epoch=scope->history_epoch, .score=1};
        snprintf(candidate->unit_id, sizeof(candidate->unit_id), "%s", db_cell(&rows,i,0));
        snprintf(candidate->text_hash, sizeof(candidate->text_hash), "%s", db_cell(&rows,i,1));
        (*count)++;
    }
    db_rows_free(&rows); return ok;
}
static bool native_no_semantic(void *ctx, const MemoryScope *scope, const char *query,
                                size_t limit, MemoryCandidate *out, size_t *count)
{ (void)ctx; (void)scope; (void)query; (void)limit; (void)out; *count=0; return true; }
static bool native_hydrate(void *ctx, const MemoryScope *s, const MemoryCandidate *c, MemoryUnit *out, bool *found)
{ return memory_candidate_hydrate(((NativeRecall *)ctx)->db,s,c,out,found); }
static long native_count(const char *text, void *ctx)
{ NativeRecall *r=ctx; return r->count(text,r->counter); }

bool native_frozen_plan(Db *db, SemanticClient *vectors, const char *project, const char *conversation,
                        const char *query, int64_t before, bool remote, MemoryTokenCount count,
                        void *counter, const char *tokenizer, const atomic_bool *cancel, MemoryPlan *out,
                        const MemoryDocumentSet *documents)
{
    (void)vectors;
    if (documents && documents->count) return false;
    NativeRecall context = {db,count,counter};
    MemoryRecallOps ops = {.context=&context, .scope=native_scope, .lexical=native_candidates,
        .semantic=native_no_semantic, .hydrate=native_hydrate, .count=native_count, .tokenizer=tokenizer};
    return memory_plan_recall(&ops,project,conversation,query,before,remote,cancel,out);
}

static const char *native_string(const cJSON *j, const char *key)
{
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
static void native_emit(FILE *file, cJSON *event)
{
    char *text=cJSON_PrintUnformatted(event);
    if (!text) { fprintf(stderr,"native diagnostic allocation failure\n"); exit(2); }
    fprintf(file,"%s\n",text); fflush(file); free(text); cJSON_Delete(event);
}
#define NATIVE_NEED(expr) do { if (!(expr)) { fprintf(stderr,"native evaluation line %d: %s\n",__LINE__,#expr); return false; } } while (0)

static bool native_workload(const cJSON *input, const char *root, const char *database, FILE *events)
{
    const char *protocol=native_string(input,"protocol");
    bool operations=!strcmp(protocol,"native-operation-dev-v1");
    bool canon=!strcmp(protocol,"native-canon-dev-v1");
    NATIVE_NEED(!protocol[0] || operations || canon);
    const char *production_persona=k_persona;
    LlmServerConfig config; llm_server_default_config(&config,NULL);
    snprintf(config.run_dir,sizeof(config.run_dir),"%s",root);
    snprintf(config.log_path,sizeof(config.log_path),"%s/server.log",root);
    snprintf(config.cache_path,sizeof(config.cache_path),"%s/model-verified.txt",root);
    config.ctx_total=16384; config.slots=2;
    LlmServer *server=llm_server_create(&config);
    NATIVE_NEED(server); llm_server_start(server);
    double until=memory_budget_now_ms()+120000;
    while (llm_server_state(server,NULL,0)!=LLM_READY && memory_budget_now_ms()<until) usleep(20000);
    bool ok=llm_server_state(server,NULL,0)==LLM_READY;
    StoreManager *manager=store_manager_create(); SemanticClient *vectors=NULL; char why[512];
    ok=ok && manager && store_start(manager,database,why,sizeof(why));
    char folder[1024],project[64]; snprintf(folder,sizeof(folder),"%s/project",root);
    ok=ok && !mkdir(folder,0700) && store_project_id(manager,folder,true,project,sizeof(project)) && store_mark_imported(folder,project);
    Db *db=ok ? store_db(manager) : NULL;
    if (ok) vectors=semantic_client_create(manager,NULL);
    ok=ok && db && vectors;
    MemoryTokenizer tokenizer={.server=server,.timeout_ms=10000}; char fingerprint[65];
    ok=ok && memory_tokenizer_fingerprint(&tokenizer,fingerprint);
    cJSON *metadata=cJSON_CreateObject();
    cJSON_AddStringToObject(metadata,"event","runtime");
    cJSON_AddStringToObject(metadata,"protocol",operations || canon ? protocol : "native-continuity-dev-v1");
    cJSON_AddStringToObject(metadata,"model_sha256",config.sha256);
    cJSON_AddNumberToObject(metadata,"slot_context",llm_server_slot_ctx(server));
    cJSON_AddStringToObject(metadata,"selection","frozen eligible SQL episode units; no Qdrant selection claim");
    native_emit(events,metadata);
    const cJSON *scenario; unsigned serial=0;
    cJSON_ArrayForEach(scenario,cJSON_GetObjectItemCaseSensitive(input,"cases")) {
        const char *operation=native_string(scenario,"operation");
        const cJSON *personas=cJSON_GetObjectItemCaseSensitive(input,"personas");
        const char *specialized=native_string(personas,operation);
        if (operations && ((strcmp(operation,"recall") && strcmp(operation,"draft")) || !specialized[0])) { ok=false; break; }
        /* Canon protocol: exactly this case's canon is committed for its answers. */
        char canon_ids[8][40]; size_t canon_count=0;
        if (canon) {
            const cJSON *entry;
            cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(scenario,"canon")) {
                if (!cJSON_IsString(entry) || canon_count>=8) { ok=false; break; }
                int ver=0;
                records_new_id(REC_CANON,canon_ids[canon_count],sizeof(canon_ids[canon_count]));
                ok=ok && records_append(manager,folder,REC_CANON,canon_ids[canon_count],"committed",entry->valuestring,NULL,0,&ver);
                canon_count++;
                usleep(1100000); /* distinct ids and timestamps */
            }
        }
        for (unsigned layout=0; ok && layout<2; layout++) {
            char conversation[40]; snprintf(conversation,sizeof(conversation),"native-%u",++serial);
            const cJSON *history=cJSON_GetObjectItemCaseSensitive(scenario,"history");
            native_episode_end=cJSON_GetArraySize(history);
            const cJSON *turn;
            cJSON_ArrayForEach(turn,history) {
                ChatMessage message={.role=!strcmp(native_string(turn,"role"),"assistant") ? CHAT_ASSISTANT : CHAT_USER,
                    .content=(char *)native_string(turn,"content")};
                ok=ok && sdb_chat_append(db,project,conversation,"Native continuity",&message);
            }
            for (unsigned i=0; ok && i<64; i++) {
                ChatMessage message={.role=layout || !(i%2) ? CHAT_USER : CHAT_ASSISTANT,
                    .content=layout || !(i%2) ? "Recent manuscript note: ordinary weather at the quay." : "Noted."};
                ok=sdb_chat_append(db,project,conversation,"Native continuity",&message);
            }
            MemoryScope scope;
            ok=ok && memory_scope_set(db,project,conversation,true,&scope);
            for (unsigned i=0; ok && i<100; i++) {
                bool progress=false;
                ok=memory_ingest_step(db,scope.id,"native-gate",memory_source_tokens,&tokenizer,fingerprint,&progress,why,sizeof(why));
                if (!progress) break;
            }
            for (unsigned mode=0; ok && mode<(operations || canon ? 2u : 3u); mode++) {
                /* Fixture-only explicit operation. Never an automatic router or
                 * production API: all previous workers finish before changing
                 * this diagnostic translation unit's persona pointer. */
                unsigned arm=operations && serial%2 ? 1-mode : mode;
                k_persona=operations && arm ? specialized : production_persona;
                ChatMessage *messages=NULL; size_t count=0;
                ok=sdb_chat_load(db,project,conversation,&messages,&count);
                ChatSession *chat=ok ? chat_create(server) : NULL;
                ok=ok && chat && chat_load_messages(chat,messages,count);
                chat_store_free_messages(messages,count);
                ok=ok && chat_memory_configure(chat,manager,vectors,folder,conversation,true) &&
                    chat_memory_set_presentation(chat,operations ? CHAT_MEMORY_EPISODE : canon ? CHAT_MEMORY_LEGACY : (ChatMemoryPresentation)mode) &&
                    (!canon || chat_set_canon_reference(chat,mode==1));
                ChatSampling sampling={.max_reply=1024,.temperature=0,.edit_temperature=0,.top_p=0.95,.top_k=50,.min_p=0.05,.repeat_penalty=1.05};
                if (chat) chat_set_sampling(chat,&sampling);
                double start=memory_budget_now_ms(),first=-1;
                ok=ok && chat_send(chat,native_string(scenario,"question"),NULL,why,sizeof(why));
                while (ok && chat_busy(chat) && memory_budget_now_ms()-start<120000) {
                    chat_poll(chat); chat_lock(chat);
                    const ChatMessage *reply=chat_message(chat,chat_count(chat)-1);
                    if (first<0 && reply && reply->streaming && reply->content && reply->content[0]) first=memory_budget_now_ms()-start;
                    chat_unlock(chat); usleep(2000);
                }
                if (chat && chat_busy(chat)) { chat_cancel(chat); ok=false; }
                if (chat) { while(chat_busy(chat)) usleep(2000); chat_poll(chat); }
                cJSON *event=cJSON_CreateObject();
                cJSON_AddStringToObject(event,"event","answer");
                cJSON_AddStringToObject(event,"task",native_string(scenario,"name"));
                cJSON_AddStringToObject(event,"layout",layout ? "consecutive_user" : "alternating");
                cJSON_AddStringToObject(event,"presentation",operations ? "episode" : canon ? "legacy" : mode==0 ? "legacy" : mode==1 ? "compact" : "episode");
                if (canon) {
                    cJSON_AddStringToObject(event,"arm",mode==1 ? "canon_on" : "canon_off");
                    cJSON_AddNumberToObject(event,"canon_entries",(double)canon_count);
                }
                if (operations) {
                    cJSON_AddStringToObject(event,"operation",operation);
                    cJSON_AddStringToObject(event,"arm",arm ? "specialized" : "generalist");
                }
                cJSON_AddNumberToObject(event,"wall_ms",memory_budget_now_ms()-start);
                cJSON_AddNumberToObject(event,"ttft_ms",first);
                cJSON_AddBoolToObject(event,"driver_ok",ok);
                cJSON_AddNumberToObject(event,"max_reply",1024);
                if (chat) {
                    const char *notice=chat_take_notice(chat);
                    cJSON_AddStringToObject(event,"notice",notice ? notice : "");
                    chat_lock(chat);
                    const ChatMessage *reply=chat_message(chat,chat_count(chat)-1);
                    cJSON_AddStringToObject(event,"answer",reply && reply->content ? reply->content : "");
                    cJSON_AddBoolToObject(event,"error",!reply || reply->error);
                    cJSON_AddBoolToObject(event,"truncated",reply && reply->truncated);
                    cJSON_AddStringToObject(event,"sources",reply && reply->memory_sources ? reply->memory_sources : "");
                    cJSON_AddStringToObject(event,"request",chat->request ? chat->request : "");
                    cJSON_AddNumberToObject(event,"prompt_tokens",chat->request ? memory_request_tokens(chat->request,&tokenizer) : -1);
                    cJSON_AddStringToObject(event,"completion_usage","unmeasured: visible native output may exclude reasoning");
                    chat_unlock(chat);
                    native_emit(events,event); chat_destroy(chat);
                } else native_emit(events,event);
            }
        }
        if (canon) {
            for (size_t i=0; i<canon_count; i++) {
                int ver=0;
                const char *text=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(scenario,"canon"),(int)i)->valuestring;
                ok=records_append(manager,folder,REC_CANON,canon_ids[i],"retracted",text,NULL,0,&ver) && ok;
            }
        }
        if (!ok) break;
    }
    if (!ok) fprintf(stderr,"native workload failed: %s\n",db ? db_error(db) : "startup");
    k_persona=production_persona;
    semantic_client_destroy(vectors); store_manager_destroy(manager); llm_server_destroy(server);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc!=5) { fprintf(stderr,"usage: native-eval CASES RUN_ROOT DATABASE EVENTS\n"); return 2; }
    FILE *file=fopen(argv[1],"rb"); if (!file) return 2;
    fseek(file,0,SEEK_END); long length=ftell(file); rewind(file);
    if (length<1 || length>1048576) { fclose(file); return 2; }
    char *data=malloc((size_t)length+1);
    bool read_ok=data && fread(data,1,(size_t)length,file)==(size_t)length;
    fclose(file); if (!read_ok) { free(data); return 2; } data[length]=0;
    cJSON *input=json_parse_complete(data,(size_t)length); free(data);
    FILE *events=fopen(argv[4],"w");
    bool ok=input && events && native_workload(input,argv[2],argv[3],events);
    if (events) fclose(events); cJSON_Delete(input);
    return ok ? 0 : 1;
}
