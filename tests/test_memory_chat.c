/* Native ChatSession + real SQL admission, deterministic fake generation.
 * This tests memory mechanics, not model answer quality. Disposable DB only. */
#define _DARWIN_C_SOURCE
#include "app/memory_brief_store.h"
#include "app/store_backends.h"
#include "app/chat_store.h"
#include "llm/chat.h"
#include "llm/context_planner.h"
#include "llm/generative_gate.h"
#include "llm/memory_brief.h"
#include "llm/memory_tokens.h"
#include "llm/memory_vectors.h"
#include "llm/semantic.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static StoreManager *store;
static SemanticClient *vectors;
static Db *db;
static char project[64], folder[1024];
static const char *conversation = "native-memory";
static const char *original = "The Tarnwick archivist Elin keeps 73 bronze keys.";

static bool yes(void *unused) { (void)unused; return true; }

static void wait_chat(ChatSession *chat)
{
    for (unsigned i = 0; i < 5000 && chat_busy(chat); i++) { chat_poll(chat); usleep(2000); }
    chat_poll(chat);
    CHECK(!chat_busy(chat));
}

static ChatSession *session(void)
{
    ChatMessage *messages = NULL; size_t count = 0;
    CHECK(sdb_chat_load(db, project, conversation, &messages, &count));
    ChatSession *chat = chat_create(NULL);
    CHECK(chat && chat_load_messages(chat, messages, count));
    chat_store_free_messages(messages, count);
    CHECK(chat_memory_configure(chat, store, vectors, folder, conversation, true));
    CHECK(chat_memory_set_briefs_enabled(chat, true));
    return chat;
}

static void expect_sources(bool present)
{
    for (unsigned presentation = 0; presentation < 3; presentation++) {
        ChatSession *chat = session();
        CHECK(chat_memory_set_presentation(chat, (ChatMemoryPresentation)presentation));
        char why[256];
        CHECK(chat_send(chat, "How many bronze keys does the Tarnwick archivist keep?", NULL, why, sizeof(why)));
        wait_chat(chat);
        char *sources = chat_memory_sources(chat);
        CHECK(present ? sources && strstr(sources, original) : !sources || !sources[0]);
        if (present) {
            const char *notice = chat_take_notice(chat);
            CHECK(notice && strstr(notice, "brief") && strstr(notice, "unavailable"));
        }
        free(sources); chat_destroy(chat);
    }
}

static void invalid_brief_does_not_disable_originals(void)
{
    MemoryScope scope;
    MemoryTokenizer tokenizer = {0}; char fingerprint[65], model[65], why[256];
    CHECK(memory_scope_set(db, project, conversation, true, &scope));
    CHECK(memory_tokenizer_fingerprint(&tokenizer, fingerprint));
    for (unsigned i = 0; i < 100; i++) {
        bool progress = false;
        CHECK(memory_ingest_step(db, scope.id, "native-test", memory_source_tokens, &tokenizer,
                                 fingerprint, &progress, why, sizeof(why)));
        if (!progress) break;
    }
    CHECK(memory_scope_get(db, project, conversation, &scope));
    CHECK(memory_brief_model_fingerprint(&tokenizer, model));
    MemoryBriefLease lease = {0}; bool claimed = false, published = false;
    CHECK(memory_brief_claim(db, &scope, "native-test", &lease, &claimed) && claimed);
    const char *summary = "{\"facts\":[{\"text\":\"Elin keeps 73 bronze keys.\",\"sources\":[1]}]}";
    CHECK(memory_brief_publish(db, &lease, summary, (size_t)memory_source_tokens(summary, &tokenizer), model,
                               MEMORY_BRIEF_PROTOCOL, yes, NULL, &published) && published);
    memory_brief_lease_free(&lease);
    /* Syntactically valid JSON, semantically invalid brief schema. Original
     * sources, scope, tokenizer and model fingerprints remain unchanged. */
    CHECK(db_run(db, "UPDATE memory_briefs SET summary_json='{\"facts\":\"invalid\"}' WHERE scope_id=?",
                 (DbArg[]){DB_S(scope.id)}, 1, NULL));
    expect_sources(true);

    /* Brief fallback must not bypass fresh source eligibility or consent. */
    MemoryPlan captured = {0};
    CHECK(memory_plan_build(db, vectors, project, conversation, "Tarnwick bronze keys", 6, false,
                            memory_reference_tokens, &tokenizer, fingerprint, NULL, &captured));
    CHECK(captured.count > 0 && memory_plan_admit(db, &captured, false));
    CHECK(db_run(db, "UPDATE messages SET via='remote' WHERE conversation_id=? AND seq=0",
                 (DbArg[]){DB_S(conversation)}, 1, NULL));
    CHECK(!memory_plan_admit(db, &captured, false));
    expect_sources(false);
    CHECK(db_run(db, "UPDATE messages SET via=NULL WHERE conversation_id=? AND seq=0",
                 (DbArg[]){DB_S(conversation)}, 1, NULL));
    CHECK(memory_plan_admit(db, &captured, false));
    CHECK(db_run(db, "UPDATE messages SET role='assistant' WHERE conversation_id=? AND seq=0",
                 (DbArg[]){DB_S(conversation)}, 1, NULL));
    CHECK(!memory_plan_admit(db, &captured, false));
    CHECK(db_run(db, "UPDATE messages SET role='user' WHERE conversation_id=? AND seq=0",
                 (DbArg[]){DB_S(conversation)}, 1, NULL));
    CHECK(memory_plan_admit(db, &captured, false));
    CHECK(memory_scope_set(db, project, conversation, false, &scope));
    CHECK(!memory_plan_admit(db, &captured, false));
    memory_plan_free(&captured);
    expect_sources(false);
}

static void canceled_preparation_has_no_sources(void)
{
    MemoryScope scope;
    CHECK(memory_scope_set(db, project, conversation, true, &scope));
    GenerativeGate *gate = generative_gate_create();
    GenerativeTicket background = {0}; atomic_bool cancel; atomic_init(&cancel, false);
    CHECK(generative_gate_try_background(gate, &background, &cancel));
    ChatSession *chat = session(); CHECK(chat_set_generative_gate(chat, gate));
    char why[256];
    CHECK(chat_send(chat, "Recall Tarnwick", NULL, why, sizeof(why)));
    chat_cancel(chat); wait_chat(chat);
    char *sources = chat_memory_sources(chat); CHECK(!sources || !sources[0]); free(sources);
    generative_gate_finish(&background); chat_destroy(chat);
    CHECK(generative_gate_destroy(gate));
}

static void edit_bypasses_recall(void)
{
    MemoryScope scope;
    CHECK(memory_scope_set(db, project, conversation, true, &scope));
    for (unsigned presentation = 0; presentation < 3; presentation++) {
        ChatSession *chat = session();
        CHECK(chat_memory_set_presentation(chat, (ChatMemoryPresentation)presentation));
        char why[256];
        CHECK(chat_send_edit(chat, "Make this uppercase", "a quiet quay", NULL, why, sizeof(why)));
        wait_chat(chat);
        char *sources = chat_memory_sources(chat);
        CHECK(!sources || !sources[0]);
        const char *notice = chat_take_notice(chat);
        CHECK(!notice || !strstr(notice, "Local memory"));
        free(sources); chat_destroy(chat);
    }
}

static void context_trimmed_history_remains_retrievable(void)
{
    const char *id = "history-gap";
    const char *fact = "The Brannock ledger records 61 silver seals.";
    char text[1001];
    for (unsigned i = 0; i < 70; i++) {
        memset(text, 'x', sizeof(text)-1); text[sizeof(text)-1] = 0;
        if (i == 20) memcpy(text, fact, strlen(fact));
        ChatMessage message = {.role=CHAT_USER, .content=text};
        CHECK(sdb_chat_append(db, project, id, "Trimmed history", &message));
    }
    MemoryScope scope;
    MemoryTokenizer tokenizer = {0}; char fingerprint[65], why[256];
    CHECK(memory_scope_set(db, project, id, true, &scope));
    CHECK(memory_tokenizer_fingerprint(&tokenizer, fingerprint));
    bool progress = true;
    unsigned steps = 0;
    while (progress && steps++ < 1024)
        CHECK(memory_ingest_step(db, scope.id, "gap-test", memory_source_tokens, &tokenizer,
                                 fingerprint, &progress, why, sizeof(why)));
    CHECK(!progress && steps < 1024);
    ChatMessage *messages = NULL; size_t count = 0;
    CHECK(sdb_chat_load(db, project, id, &messages, &count) && count == 70);
    ChatSession *chat = chat_create(NULL);
    CHECK(chat && chat_load_messages(chat, messages, count));
    chat_store_free_messages(messages, count);
    CHECK(chat_memory_configure(chat, store, vectors, folder, id, true));
    CHECK(chat_send(chat, "How many silver seals does the Brannock ledger record?", NULL, why, sizeof(why)));
    wait_chat(chat);
    chat_lock(chat);
    CHECK(!chat_message(chat, chat_count(chat)-1)->error);
    chat_unlock(chat);
    char *sources = chat_memory_sources(chat);
    CHECK(sources && strstr(sources, fact));
    free(sources); chat_destroy(chat);
}

static void retained_tail_must_not_starve_archive_candidates(void)
{
    const char *id = "candidate-starvation";
    const char *query = "Zorwick gate passphrase";
    const char *fact = "The Zorwick gate passphrase is amber.";
    char old[401];
    memset(old, 'x', sizeof(old)-1); old[sizeof(old)-1] = 0;
    memcpy(old, fact, strlen(fact));
    ChatMessage message = {.role=CHAT_USER, .content=old};
    CHECK(sdb_chat_append(db, project, id, "Candidate starvation", &message));
    for (unsigned i = 0; i < 32; i++) {
        message.content = (char *)query;
        CHECK(sdb_chat_append(db, project, id, "Candidate starvation", &message));
    }
    MemoryScope scope;
    MemoryTokenizer tokenizer = {0}; char fingerprint[65], why[256];
    CHECK(memory_scope_set(db, project, id, true, &scope));
    CHECK(memory_tokenizer_fingerprint(&tokenizer, fingerprint));
    bool progress = true;
    unsigned steps = 0;
    while (progress && steps++ < 1024)
        CHECK(memory_ingest_step(db, scope.id, "starve-test", memory_source_tokens, &tokenizer,
                                 fingerprint, &progress, why, sizeof(why)));
    CHECK(!progress && steps < 1024);
    CHECK(memory_scope_get(db, project, id, &scope));
    CHECK(memory_vectors_create(vectors, &scope, why, sizeof(why)));
    unsigned indexed = 0;
    for (;;) {
        MemoryIndexLease lease = {0}; bool claimed = false;
        bool ok = memory_outbox_claim(db, scope.id, "starve-index", &lease, &claimed);
        CHECK(ok);
        if (!ok || !claimed) { memory_index_lease_free(&lease); break; }
        bool delivered = memory_vectors_upsert(vectors, &lease, why, sizeof(why));
        CHECK(delivered);
        CHECK(memory_outbox_ack(db, &lease, delivered));
        memory_index_lease_free(&lease);
        if (!delivered || ++indexed > 1024) break;
    }
    CHECK(indexed == 33);
    CHECK(memory_scope_get(db, project, id, &scope) && !scope.index_pending);
    MemoryCandidate lexical[MEMORY_CANDIDATES], semantic[MEMORY_CANDIDATES];
    size_t nl = 0, ns = 0;
    CHECK(memory_lexical_candidates_before(db, &scope, query, 1, 16, lexical, &nl));
    CHECK(memory_vectors_search_with_documents_before(vectors, &scope, NULL, query, 1, 16,
                                                       semantic, &ns, why, sizeof(why)));
    CHECK(nl == 1 && ns == 1);
    CHECK(nl && ns && !strcmp(lexical[0].unit_id, semantic[0].unit_id));
    printf("    pre-top-k cutoff: lexical=%zu semantic=%zu same_original=%d\n", nl, ns,
           nl && ns && !strcmp(lexical[0].unit_id, semantic[0].unit_id));
    MemoryPlan plan = {0};
    /* The 32 recent units are already in the actual model context. Only
     * sequence 0 needs retrieval. They must be excluded BEFORE either top-k. */
    CHECK(memory_plan_build(db, vectors, project, id, query, 1, false,
                            memory_reference_tokens, &tokenizer, fingerprint, NULL, &plan));
    printf("    archive starvation: indexed=%u lexical_ok=%d semantic_ok=%d candidates=%zu admitted=%zu\n",
           indexed, plan.lexical_ok, plan.semantic_ok, plan.candidates, plan.count);
    CHECK(plan.lexical_ok && plan.semantic_ok);
    CHECK(plan.reference && strstr(plan.reference, fact));
    memory_plan_free(&plan);
    /* Positive control: the old unit is valid and reachable when queried by
     * its distinguishing term. This is not missing ingestion or a stale index. */
    CHECK(memory_plan_build(db, vectors, project, id, "amber", 1, false,
                            memory_reference_tokens, &tokenizer, fingerprint, NULL, &plan));
    CHECK(plan.reference && strstr(plan.reference, fact));
    memory_plan_free(&plan);
    CHECK(memory_vectors_drop(vectors, &scope, why, sizeof(why)));
}

int main(void)
{
    char name[64], command[256], why[512];
    snprintf(name, sizeof(name), "scribodo_test_memory_chat_%ld", (long)getpid());
    snprintf(command, sizeof(command), "tools/services.sh testdb create %s", name);
    if (system(command)) return 1;
    store = store_manager_create();
    bool ready = store && store_start(store, name, why, sizeof(why)); CHECK(ready);
    if (ready) {
        const char *tmp = getenv("TMPDIR");
        snprintf(folder, sizeof(folder), "%s/native-memory-XXXXXX", tmp ? tmp : "build");
        CHECK(mkdtemp(folder));
        CHECK(store_project_id(store, folder, true, project, sizeof(project)));
        CHECK(store_mark_imported(folder, project));
        db = store_db(store); CHECK(db);
        for (unsigned i = 0; i < 70; i++) {
            ChatMessage message = {.role = CHAT_USER, .content = (char *)(i ? "Ordinary weather note." : original)};
            CHECK(sdb_chat_append(db, project, conversation, "Native memory", &message));
        }
        vectors = semantic_client_create(store, NULL); CHECK(vectors);
        const TestCase tests[] = {{"brief failure preserves only eligible originals", invalid_brief_does_not_disable_originals},
                                  {"canceled native preparation", canceled_preparation_has_no_sources},
                                   {"edit bypasses recall", edit_bypasses_recall},
                                    {"context-trimmed history remains retrievable", context_trimmed_history_remains_retrievable},
                                    {"retained tail cannot starve archive candidates", retained_tail_must_not_starve_archive_candidates}};
        run_tests(tests, sizeof(tests) / sizeof(tests[0]));
        semantic_client_destroy(vectors);
    }
    store_manager_destroy(store);
    snprintf(command, sizeof(command), "tools/services.sh testdb drop %s", name);
    CHECK(system(command) == 0);
    return g_failures ? 1 : 0;
}
