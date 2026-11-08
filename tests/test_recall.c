/* Deterministic recall (MEMORY-015 S1) against real SQL in a disposable
 * database: exactness, status, coverage, scope, provenance and bounds. No
 * model, embedder or vector server is involved anywhere in this binary's
 * recall path; the fake embedder is not even created. */
#define _DARWIN_C_SOURCE
#include "app/memory_documents.h"
#include "app/memory_store.h"
#include "app/recall.h"
#include "app/recall_job.h"
#include "app/store_backends.h"
#include "llm/memory_tokens.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static StoreManager *store;
static Db *db;
static char project[64], folder[1024], other_project[64], other_folder[1024], file_folder[1024];
static const char *on = "recall-on", *off = "recall-off", *pending = "recall-pending";
static const char *fact = "Elin keeps 73 bronze keys in the amber cabinet.";
static const char *canon_amber = "The Asterholt observatory key is amber.";
static const char *canon_cobalt = "The ferry code is cobalt heron 17.";
static const char *canon_lens = "Dvo\xC5\x99\xC3\xA1k polishes the Fresnel lens.";
static const char *canon_only_amber = "Amber is the lighthouse colour.";
static const char *revision_text = "Chapter notes. The amber key hangs by the observatory door; Elin counts her keys nightly.";
static MemoryTokenizer tokenizer;
static char fingerprint[65];

static const RecallRow *find_row(const RecallResult *r, RecallRowKind kind, const char *needle)
{
    for (size_t i = 0; i < r->count; i++)
        if (r->rows[i].kind == kind && r->rows[i].text && strstr(r->rows[i].text, needle)) return &r->rows[i];
    return NULL;
}

static void ingest_chat(const char *conversation)
{
    MemoryScope scope;
    CHECK(memory_scope_set(db, project, conversation, true, &scope));
    for (unsigned i = 0; i < 200; i++) {
        bool progress = false;
        char why[256];
        CHECK(memory_ingest_step(db, scope.id, "recall-test", memory_source_tokens, &tokenizer, fingerprint,
                                 &progress, why, sizeof(why)));
        if (!progress) break;
    }
}

static void ingest_document(const char *conversation, const char *document_id, const char *revision_id)
{
    MemoryScope scope;
    CHECK(memory_scope_get(db, project, conversation, &scope) && scope.enabled);
    MemoryDocumentSet set = { 0 };
    DbRows r;
    CHECK(db_query(db, "SELECT DATABASE(), r.source_hash, OCTET_LENGTH(r.content), d.name FROM revisions r"
                       " JOIN documents d ON d.id=r.document_id WHERE r.id=?",
                   (DbArg[]){ DB_S(revision_id) }, 1, &r) && r.nrows == 1);
    MemoryDocumentRef *ref = &set.refs[0];
    set.count = 1;
    snprintf(ref->database, sizeof(ref->database), "%s", db_cell(&r, 0, 0));
    snprintf(ref->project_id, sizeof(ref->project_id), "%s", project);
    snprintf(ref->document_id, sizeof(ref->document_id), "%s", document_id);
    snprintf(ref->revision_id, sizeof(ref->revision_id), "%s", revision_id);
    snprintf(ref->source_hash, sizeof(ref->source_hash), "%s", db_cell(&r, 0, 1));
    snprintf(ref->name, sizeof(ref->name), "%s", db_cell(&r, 0, 3));
    CHECK(db_cell_u64(&r, 0, 2, &ref->bytes));
    db_rows_free(&r);
    CHECK(memory_documents_seed(db, &scope, &set));
    for (unsigned i = 0; i < 200; i++) {
        bool progress = false;
        char why[256];
        CHECK(memory_document_ingest_step(db, &scope, &set, "recall-test", memory_source_tokens, &tokenizer,
                                          fingerprint, &progress, why, sizeof(why)));
        if (!progress) break;
    }
    bool complete = false;
    CHECK(memory_documents_complete(db, &scope, &set, &complete) && complete);
}

static void seed(void)
{
    char id[40];
    int ver = 0;
    records_new_id(REC_CANON, id, sizeof(id));
    CHECK(records_append(store, folder, REC_CANON, id, "committed", canon_amber, NULL, 0, &ver) && ver == 1);
    usleep(1100000); /* distinct id stamps and timestamps */
    records_new_id(REC_CANON, id, sizeof(id));
    CHECK(records_append(store, folder, REC_CANON, id, "committed", canon_cobalt, NULL, 0, &ver));
    CHECK(records_append(store, folder, REC_CANON, id, "retracted", canon_cobalt, NULL, 0, &ver) && ver == 2);
    usleep(1100000);
    records_new_id(REC_CANON, id, sizeof(id));
    CHECK(records_append(store, folder, REC_CANON, id, "committed", canon_lens, NULL, 0, &ver));
    usleep(1100000);
    records_new_id(REC_CANON, id, sizeof(id));
    CHECK(records_append(store, folder, REC_CANON, id, "committed", canon_only_amber, NULL, 0, &ver));
    /* Another project with a matching entry: never visible from this one. */
    records_new_id(REC_CANON, id, sizeof(id));
    CHECK(records_append(store, other_folder, REC_CANON, id, "committed", "Amber key in another book.", NULL, 0, &ver));

    for (unsigned i = 0; i < 70; i++) {
        ChatMessage message = { .role = i % 2 ? CHAT_ASSISTANT : CHAT_USER,
                                .content = (char *)(i ? "Ordinary weather note about fog." : fact) };
        CHECK(sdb_chat_append(db, project, on, "Memory on", &message));
    }
    ChatMessage loud = { .role = CHAT_USER, .content = (char *)"Amber keys everywhere, but memory is off here." };
    CHECK(sdb_chat_append(db, project, off, "Memory off", &loud));
    ChatMessage later = { .role = CHAT_USER, .content = (char *)"Amber key mentioned; indexing has not started." };
    CHECK(sdb_chat_append(db, project, pending, "Memory pending", &later));
    MemoryScope scope;
    CHECK(memory_scope_set(db, project, pending, true, &scope) && !scope.tokenizer[0]);

    CHECK(db_run(db, "INSERT INTO documents(id,project_id,name,created_at) VALUES('d-recall',?,'notes.md',1)",
                 (DbArg[]){ DB_S(project) }, 1, NULL));
    CHECK(db_run(db, "INSERT INTO revisions(id,document_id,hash,content,origin,created_at)"
                     " VALUES('r-recall','d-recall','h1',?,'save',1),('r-later','d-recall','h2','Later unsaved text.','ai',2)",
                 (DbArg[]){ DB_S(revision_text) }, 1, NULL));
    CHECK(db_run(db, "UPDATE documents SET saved_revision_id='r-recall',current_revision_id='r-later' WHERE id='d-recall'",
                 NULL, 0, NULL));
    ingest_chat(on);
    ingest_document(on, "d-recall", "r-recall");
}

static void coverage_and_exactness(void)
{
    RecallResult r;
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(r.canon_ok && r.excerpts_available && r.terms == 2);
    CHECK(r.canon_committed == 3 && r.canon_retracted == 1);
    CHECK(r.conversations == 3 && r.memory_on == 1 && r.memory_off == 1 && r.memory_indexing == 1);
    CHECK(r.candidates >= 2 && r.rejected == 0);
    const RecallRow *amber = find_row(&r, RECALL_CANON, "observatory key");
    const RecallRow *only = find_row(&r, RECALL_CANON, "lighthouse colour");
    CHECK(amber && !strcmp(amber->text, canon_amber) && amber->ver == 1 && amber->matched == 2);
    CHECK(only && only->matched == 1);
    CHECK(amber && only && amber < only); /* two matched terms rank above one */
    CHECK(!find_row(&r, RECALL_CANON, "cobalt"));     /* retracted */
    CHECK(!find_row(&r, RECALL_CANON, "Fresnel"));    /* no term matched */
    CHECK(!find_row(&r, RECALL_CANON, "another book")); /* other project */
    const RecallRow *chat = find_row(&r, RECALL_CHAT_EXCERPT, "bronze keys");
    CHECK(chat && !strcmp(chat->text, fact) && chat->sequence == 0 && chat->codepoint_start == 0 &&
          chat->role == CHAT_USER && !strcmp(chat->conversation_id, on) && !strcmp(chat->conversation_title, "Memory on") &&
          chat->codepoints == strlen(fact) && chat->matched == 2);
    CHECK(!find_row(&r, RECALL_CHAT_EXCERPT, "memory is off"));
    CHECK(!find_row(&r, RECALL_CHAT_EXCERPT, "indexing has not started"));
    const RecallRow *doc = find_row(&r, RECALL_DOCUMENT_EXCERPT, "amber key hangs");
    CHECK(doc && !strcmp(doc->document_name, "notes.md") && !strcmp(doc->revision_id, "r-recall") && doc->current_saved);
    CHECK(doc && strstr(revision_text, doc->text) && doc->codepoint_start == 0 && doc->matched == 2);
    /* Canon rows come first; excerpts follow, best match first. */
    size_t first_excerpt = r.count;
    for (size_t i = 0; i < r.count; i++) if (r.rows[i].kind != RECALL_CANON) { first_excerpt = i; break; }
    for (size_t i = first_excerpt; i < r.count; i++) CHECK(r.rows[i].kind != RECALL_CANON);
    for (size_t i = 0; i < first_excerpt; i++) CHECK(r.rows[i].kind == RECALL_CANON);
    char *text = recall_result_text(&r);
    CHECK(text && strstr(text, "coverage|terms=2|canon_committed=3|canon_retracted=1|conversations=3|memory_on=1|memory_off=1|memory_indexing=1"));
    CHECK(text && strstr(text, "canon|") && strstr(text, canon_amber) && strstr(text, "chat|Memory on|seq=0|author|start=0|matched=2|"));
    CHECK(text && strstr(text, "document|notes.md|revision=r-recall|saved|"));
    free(text);
    recall_result_free(&r);
}

static void provenance_rejection(void)
{
    /* A changed original is no longer the excerpt's source: nothing from it is shown. */
    CHECK(db_run(db, "UPDATE messages SET content=REPLACE(content,'amber','umber') WHERE conversation_id=? AND seq=0",
                 (DbArg[]){ DB_S(on) }, 1, NULL));
    RecallResult r;
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(r.excerpts_available && !find_row(&r, RECALL_CHAT_EXCERPT, "bronze"));
    CHECK(r.rejected >= 1);
    recall_result_free(&r);
    CHECK(db_run(db, "UPDATE messages SET content=REPLACE(content,'umber','amber') WHERE conversation_id=? AND seq=0",
                 (DbArg[]){ DB_S(on) }, 1, NULL));
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(find_row(&r, RECALL_CHAT_EXCERPT, "bronze keys") && r.rejected == 0);
    recall_result_free(&r);
    /* A changed saved revision drops its excerpts outright. */
    CHECK(db_run(db, "UPDATE revisions SET content=CONCAT(content,' edited') WHERE id='r-recall'", NULL, 0, NULL));
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(!find_row(&r, RECALL_DOCUMENT_EXCERPT, "amber key hangs"));
    recall_result_free(&r);
    CHECK(db_run(db, "UPDATE revisions SET content=? WHERE id='r-recall'", (DbArg[]){ DB_S(revision_text) }, 1, NULL));
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(find_row(&r, RECALL_DOCUMENT_EXCERPT, "amber key hangs"));
    recall_result_free(&r);
}

static void consent_and_scope(void)
{
    MemoryScope scope;
    CHECK(memory_scope_set(db, project, on, false, &scope));
    RecallResult r;
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(r.memory_on == 0 && r.memory_off == 2 && !find_row(&r, RECALL_CHAT_EXCERPT, "bronze") &&
          !find_row(&r, RECALL_DOCUMENT_EXCERPT, "amber key hangs"));
    CHECK(find_row(&r, RECALL_CANON, "observatory key")); /* canon is project-wide and unaffected */
    recall_result_free(&r);
    ingest_chat(on);
    ingest_document(on, "d-recall", "r-recall");
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(r.memory_on == 1 && find_row(&r, RECALL_CHAT_EXCERPT, "bronze keys"));
    recall_result_free(&r);
    /* The other project sees only its own canon and no conversations. */
    CHECK(recall_run(store, other_folder, "amber key", &r));
    CHECK(r.count == 1 && r.rows[0].kind == RECALL_CANON && strstr(r.rows[0].text, "another book") &&
          r.conversations == 0 && r.canon_committed == 1);
    recall_result_free(&r);
}

static void file_project_and_empty_queries(void)
{
    char id[40];
    int ver = 0;
    records_new_id(REC_CANON, id, sizeof(id));
    CHECK(records_append(store, file_folder, REC_CANON, id, "committed", "An amber file-backed entry.", NULL, 0, &ver));
    RecallResult r;
    CHECK(recall_run(store, file_folder, "amber", &r));
    CHECK(r.canon_ok && !r.excerpts_available && r.count == 1 && strstr(r.rows[0].text, "file-backed") &&
          strstr(r.limits, "project store"));
    recall_result_free(&r);
    CHECK(recall_run(store, folder, "a ! .", &r));
    CHECK(r.terms == 0 && r.count == 0 && r.canon_ok && r.canon_committed == 0 && strstr(r.limits, "no searchable terms"));
    recall_result_free(&r);
    char huge[RECALL_QUERY_BYTES + 8];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(!recall_run(store, folder, huge, &r) && r.count == 0 && r.limits[0]);
    recall_result_free(&r);
    CHECK(!recall_run(store, "", "amber", &r) && !recall_run(NULL, folder, "amber", &r) && !recall_run(store, folder, NULL, &r));
    CHECK(!recall_run(store, folder, "amber", NULL));
}

static void bounds(void)
{
    for (unsigned i = 0; i < 70; i++) {
        char id[40], content[80];
        int ver = 0;
        snprintf(id, sizeof(id), "c-20260101-000000-bound%04u", i);
        snprintf(content, sizeof(content), "Bulk amber fact %u.", i);
        CHECK(records_append(store, folder, REC_CANON, id, "committed", content, NULL, 0, &ver));
    }
    RecallResult r;
    CHECK(recall_run(store, folder, "amber", &r));
    size_t canon = 0;
    for (size_t i = 0; i < r.count; i++) canon += r.rows[i].kind == RECALL_CANON;
    CHECK(canon == RECALL_CANON_ROWS_MAX && r.count <= RECALL_ROWS_MAX && r.canon_committed == 73);
    CHECK(strstr(r.limits, "32 best-matching canon"));
    recall_result_free(&r);
}

static void background_job(void)
{
    RecallJob job;
    CHECK(recall_job_init(&job, store));
    CHECK(!recall_job_start(&job, "", "amber") && !recall_job_busy(&job));
    CHECK(recall_job_start(&job, folder, "amber key"));
    CHECK(!recall_job_start(&job, folder, "second")); /* one run at a time */
    RecallResult r;
    bool ok = false;
    unsigned waited = 0;
    while (!recall_job_poll(&job, &r, &ok) && waited++ < 5000) usleep(2000);
    CHECK(waited < 5000 && ok && !recall_job_busy(&job));
    CHECK(find_row(&r, RECALL_CANON, "observatory key") && find_row(&r, RECALL_CHAT_EXCERPT, "bronze keys"));
    recall_result_free(&r);
    CHECK(!recall_job_poll(&job, &r, &ok));
    CHECK(recall_job_start(&job, folder, "amber"));
    recall_job_destroy(&job); /* joins and frees an unpolled result */
}


/* Lexical candidates: InnoDB's raw MATCH score grows with how often a long
 * paragraph repeats the query's words. The production query normalizes by
 * length so the short turn that states the fact once ranks first. */
static void lexical_length_normalization(void)
{
    /* A realistic paragraph: about 110 words, the query's words a few times
     * each (as the fixture's filler does), not a pathological repetition. The
     * raw MATCH score still prefers it (3 mentions of "amber" to 1); the
     * length-normalized score does not. Repetition is not unbounded: at this
     * exponent a 1,000-character paragraph must repeat a query word about
     * eleven times to outrank a one-sentence statement of it (MEMORY-018). */
    const char *paragraph =
        "Elin lifted the amber lantern by the door while fog came in off the water and the ferry bell rang twice. "
        "Oren had left the ledger open on the table at the quay, its pages damp, and nobody could say who had written "
        "the last line. By the time the tide slackened, Elin and Mira had stopped speaking of the keys. The amber light "
        "faded behind the stalls; Tarn counted the keys again out of habit. Nothing at the lighthouse explained the "
        "amber stain on the compass Dvořák had polished, and the evening settled over the harbour without an answer.";
    ChatMessage filler = { .role = CHAT_USER, .content = (char *)paragraph };
    CHECK(sdb_chat_append(db, project, on, "Memory on", &filler));
    ingest_chat(on);
    MemoryScope scope;
    CHECK(memory_scope_get(db, project, on, &scope) && scope.enabled);
    MemoryCandidate candidates[MEMORY_CANDIDATES];
    size_t n = 0;
    CHECK(memory_lexical_candidates(db, &scope, "amber key", 16, candidates, &n) && n >= 2);
    long fact_rank = -1, paragraph_rank = -1;
    for (size_t i = 0; i < n; i++) {
        MemoryUnit unit = { 0 };
        bool found = false;
        CHECK(memory_candidate_hydrate(db, &scope, &candidates[i], &unit, &found) && found);
        if (unit.text && !strcmp(unit.text, fact)) fact_rank = (long)i;
        if (unit.text && strstr(unit.text, "counted the keys again")) paragraph_rank = (long)i;
        memory_unit_free(&unit);
    }
    CHECK(fact_rank >= 0 && paragraph_rank >= 0);
    CHECK(fact_rank < paragraph_rank); /* the one-sentence fact outranks the repeating paragraph */
    /* Case-insensitive discovery (migration 014): the index matches the fact
     * for an upper-case query and for a lower-case name. */
    CHECK(memory_lexical_candidates(db, &scope, "AMBER KEY", 16, candidates, &n) && n >= 1);
    bool upper_found = false;
    for (size_t i = 0; i < n && !upper_found; i++) {
        MemoryUnit unit = { 0 };
        bool found = false;
        CHECK(memory_candidate_hydrate(db, &scope, &candidates[i], &unit, &found) && found);
        upper_found = unit.text && !strcmp(unit.text, fact);
        memory_unit_free(&unit);
    }
    CHECK(upper_found);
    CHECK(memory_lexical_candidates(db, &scope, "elin", 16, candidates, &n) && n >= 1);
    RecallResult r;
    CHECK(recall_run(store, folder, "amber key", &r));
    CHECK(find_row(&r, RECALL_CHAT_EXCERPT, "bronze keys"));
    recall_result_free(&r);
}

int main(void)
{
    char name[64], command[256], why[512];
    snprintf(name, sizeof(name), "scribodo_test_recall_%ld", (long)getpid());
    snprintf(command, sizeof(command), "tools/services.sh testdb create %s", name);
    if (system(command)) return 1;
    store = store_manager_create();
    bool ready = store && store_start(store, name, why, sizeof(why));
    CHECK(ready);
    if (ready) {
        const char *tmp = getenv("TMPDIR");
        snprintf(folder, sizeof(folder), "%s/recall-XXXXXX", tmp ? tmp : "build");
        snprintf(other_folder, sizeof(other_folder), "%s/recall-other-XXXXXX", tmp ? tmp : "build");
        snprintf(file_folder, sizeof(file_folder), "%s/recall-files-XXXXXX", tmp ? tmp : "build");
        CHECK(mkdtemp(folder) && mkdtemp(other_folder) && mkdtemp(file_folder));
        CHECK(store_project_id(store, folder, true, project, sizeof(project)));
        CHECK(store_mark_imported(folder, project));
        CHECK(store_project_id(store, other_folder, true, other_project, sizeof(other_project)));
        CHECK(store_mark_imported(other_folder, other_project));
        db = store_db(store);
        CHECK(db);
        CHECK(memory_tokenizer_fingerprint(&tokenizer, fingerprint));
        seed();
        const TestCase tests[] = { { "coverage and exactness", coverage_and_exactness },
                                   { "provenance rejection", provenance_rejection },
                                   { "consent and scope", consent_and_scope },
                                   { "file project and empty queries", file_project_and_empty_queries },
                                   { "bounds", bounds },
                                   { "background job", background_job },
                                   { "lexical length normalization", lexical_length_normalization } };
        run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    }
    store_manager_destroy(store);
    snprintf(command, sizeof(command), "tools/services.sh testdb drop %s", name);
    CHECK(system(command) == 0);
    return g_failures ? 1 : 0;
}
