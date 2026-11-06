#define _DARWIN_C_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/backup_job.h"
#include "app/entity_backfill_job.h"
#include "app/explorer_snapshot.h"
#include "app/model_settings.h"
#include "app/name_detection_job.h"
#include "app/project_session.h"
#include "app/records.h"
#include "app/service_supervisor.h"
#include "llm/llm_text.h"
#include "llm/memfit.h"
#include "llm/names.h"
#include "llm/observer.h"
#include "llm/semantic.h"
#include "test_state_fixture.h"
#include "test_util.h"

static char g_dir[512];
static StoreManager *g_store;

static void test_markdown_display(void)
{
    struct { const char *in, *out; } cases[] = {
        { "I'd love to call you **Scribe** - short.", "I'd love to call you Scribe - short." },
        { "- Warm and inviting.\n- Ready.", "\xE2\x80\xA2 Warm and inviting.\n\xE2\x80\xA2 Ready." },
        { "### The Great Surrender\nText", "The Great Surrender\nText" },
        { "And *AEON* yes, `code` too", "And AEON yes, code too" },
        { "1. **Paste** it", "1. Paste it" },
        { "5 * 3 = 15 and a*b", "5 * 3 = 15 and a*b" },               /* a lone asterisk stays */
        { "See [the map](https://x.y) now", "See the map (https://x.y) now" },
        { "```\n**not bold** in code\n```\nafter", "**not bold** in code\nafter" },
        { "---", "\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94" },
        { "Zoë — **Ångström**", "Zoë — Ångström" },
        { "", "" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *o = llm_markdown_display(cases[i].in);
        CHECK(o && !strcmp(o, cases[i].out));
        if (o && strcmp(o, cases[i].out)) fprintf(stderr, "    [%s] -> [%s]\n", cases[i].in, o);
        free(o);
    }
    /* Worst case growth fits. */
    char big[3000];
    for (int i = 0; i < 750; i++) memcpy(big + i * 4, "---\n", 4);
    big[2999] = '\0';
    char *o = llm_markdown_display(big);
    CHECK(o != NULL);
    free(o);
}

/* The first user-testing session's chat findings (all about the
 * conversation) are dropped; story findings from a chat are kept. */
static void test_observer_chat_filter(void)
{
    CHECK(obs_about_the_conversation("Tripp", "Tripp \xE2\x80\x94 The author who introduces themselves at the start."));
    CHECK(obs_about_the_conversation("Maz", "The name 'Maz' is described as very special and crisp."));
    CHECK(obs_about_the_conversation("Scribe", "Scribe \xE2\x80\x94 The suggested name for the creative writing partner."));
    CHECK(obs_about_the_conversation("asking to be called something", "Tripp asks the assistant what they would like to be called."));
    CHECK(obs_about_the_conversation("Greetings, my friend!", "A greeting addressed to the author."));
    CHECK(obs_about_the_conversation("first scene", "The Assistant asks what the user wants to do next."));
    CHECK(!obs_about_the_conversation("The Gauge", "The Gauge is a cosmic entity of pure information and love."));
    CHECK(!obs_about_the_conversation("Abyss", "The Abyss was born from the collapse of the Great Dark Star."));
    CHECK(!obs_about_the_conversation("Zo\xC3\xAB", "Zo\xC3\xAB keeps her grandmother's letter."));
}

/* Names noticed in chat: verified against the author's words. */
static void test_names_detect(void)
{
    NameGuess g;
    CHECK(names_detect(NULL, NULL, "Before we begin, my name is Tripp. What would you like to be called?", &g) &&
          !strcmp(g.author, "Tripp") && !g.assistant[0]);
    CHECK(names_detect(NULL, NULL, "I'll call you `Maz`, it's special to me.", &g) && !strcmp(g.assistant, "Maz"));
    CHECK(names_detect(NULL, NULL, "I'll call you Nova from now on.", &g) && !strcmp(g.assistant, "Nova"));
    CHECK(names_detect(NULL, NULL, "Please call me Mary Ann, thanks.", &g) && !strcmp(g.author, "Mary Ann"));
    CHECK(!names_detect(NULL, NULL, "I'll call you later about the name.", &g)); /* no name given */
    CHECK(!names_detect(NULL, NULL, "The lighthouse keeper rows out at dawn.", &g)); /* nothing to check */
    CHECK(names_in_text("Maz", "How about `Maz`? It's special.") && !names_in_text("Maz", "Mazurka") &&
          names_in_text("Zo\xC3\xAB", "I'm Zo\xC3\xAB.") && !names_in_text("", "x"));
    CHECK(names_worth_checking("How about `Maz`?") && !names_worth_checking("Ingrid rows across the fjord."));
}

static void test_name_detection_job(void)
{
    NameDetectionJob job;
    CHECK(name_detection_job_init(&job, NULL));
    CHECK(name_detection_job_start(&job, "How about Maz?", "I'll call you Maz from now on."));
    CHECK(name_detection_job_busy(&job));
    CHECK(!name_detection_job_start(&job, NULL, "My name is Tripp."));
    NameGuess result = { 0 };
    bool succeeded = false, delivered = false;
    for (int i = 0; !delivered && i < 200; i++) {
        delivered = name_detection_job_poll(&job, &result, &succeeded);
        if (!delivered) usleep(1000);
    }
    CHECK(delivered && succeeded && !strcmp(result.assistant, "Maz") && !name_detection_job_busy(&job));
    CHECK(!name_detection_job_poll(&job, &result, &succeeded));
    CHECK(name_detection_job_start(&job, NULL, "Please call me Mary Ann."));
    name_detection_job_destroy(&job);
    CHECK(!name_detection_job_busy(&job));
}

static void test_project_session_and_explorer_snapshot(void)
{
    ProjectSession session;
    project_session_init(&session);
    CHECK(project_session_set_project(&session, "/tmp/book") && session.generation == 1);
    CHECK(!project_session_set_project(&session, "/tmp/book") && session.generation == 1);
    project_session_bind_chat(&session, session.project_dir, "chat-1", "Opening");
    CHECK(!strcmp(session.chat_project, "/tmp/book") && !strcmp(session.chat_id, "chat-1"));
    project_session_clear_chat(&session);
    CHECK(!session.chat_project[0] && !session.chat_id[0] && !session.chat_title[0]);
    project_session_destroy(&session);

    char project[600], path[700];
    snprintf(project, sizeof(project), "%s/explorer-snapshot", g_dir);
    mkdir(project, 0755);
    snprintf(path, sizeof(path), "%s/B.md", project);
    FILE *file = fopen(path, "wb");
    if (file) fclose(file);
    snprintf(path, sizeof(path), "%s/a.txt", project);
    file = fopen(path, "wb");
    if (file) fclose(file);
    ChatMessage message = { .role = CHAT_USER, .content = "hello" };
    CHECK(chat_store_append(g_store, project, "20261105-120000-abcd", "A chat", &message));
    ExplorerSnapshot first;
    CHECK(explorer_snapshot_build(&first, g_store, project, 7, 1));
    CHECK(first.ndocs == 2 && first.nchats == 1 && !strcasecmp(first.docs[0], "a.txt"));
    CHECK(explorer_snapshot_doc(&first, 7, 1, 1) != NULL && explorer_snapshot_doc(&first, 8, 1, 1) == NULL);
    CHECK(explorer_snapshot_doc(&first, 7, 2, 1) == NULL); /* old rows cannot act in a new project */
    CHECK(explorer_snapshot_chat(&first, 7, 1, 0) != NULL && explorer_snapshot_chat(&first, 8, 1, 0) == NULL);
    snprintf(path, sizeof(path), "%s/0.md", project);
    file = fopen(path, "wb");
    if (file) fclose(file);
    ExplorerSnapshot second;
    CHECK(explorer_snapshot_build(&second, g_store, project, 8, 1));
    CHECK(first.ndocs == 2 && second.ndocs == 3); /* refresh never mutates the drawn snapshot */
    explorer_snapshot_destroy(&first);
    explorer_snapshot_destroy(&second);
    ExplorerSnapshot failed;
    CHECK(!explorer_snapshot_build(&failed, g_store, "/a/project/that/does/not/exist", 9, 2));
    explorer_snapshot_destroy(&failed);
}

/* The chat rules with names, and repairing what older rules recorded. */
static void test_observer_rules_and_refilter(void)
{
    FindingInfo f = { 0 };
    snprintf(f.type, sizeof(f.type), "event");
    snprintf(f.subject, sizeof(f.subject), "Scribe");
    CHECK(!obs_chat_finding_allowed(&f, "Tripp proposes 'Scribe' as a preferred title.", "Tripp", "Maz"));
    CHECK(obs_chat_finding_allowed(&f, "Tripp proposes 'Scribe' as a preferred title.", "", "")); /* no names: no name rule */
    snprintf(f.subject, sizeof(f.subject), "drafts, notes, and chats"); /* about the work, not the story */
    CHECK(!obs_chat_finding_allowed(&f, "The work is currently in drafts, notes, and chats.", "", ""));
    FindingInfo q = { 0 };
    snprintf(q.type, sizeof(q.type), "question");
    CHECK(obs_chat_finding_allowed(&q, "Should Ingrid know about the Letter?", "", ""));   /* names someone */
    CHECK(!obs_chat_finding_allowed(&q, "What would you like to do next?", "", ""));       /* the session */
    snprintf(f.subject, sizeof(f.subject), "I'm ready for your story");
    CHECK(!obs_chat_finding_allowed(&f, "Ready to receive the story.", "", ""));
    snprintf(f.subject, sizeof(f.subject), "Maz");
    CHECK(!obs_chat_finding_allowed(&f, "Maz is crisp.", "Tripp", "Maz"));
    snprintf(f.subject, sizeof(f.subject), "The Gauge");
    CHECK(obs_chat_finding_allowed(&f, "The Gauge embraced the Abyss.", "Tripp", "Maz"));
    snprintf(f.subject, sizeof(f.subject), "Trippetta"); /* a word containing the name isn't the name */
    CHECK(obs_chat_finding_allowed(&f, "Trippetta sails north.", "Tripp", "Maz"));

    /* A project with findings recorded under older rules. */
    char proj[600];
    snprintf(proj, sizeof(proj), "%s/refilter", g_dir);
    mkdir(proj, 0755);
    SourceRef chat = { .kind = SRC_CHAT, .message = 2, .start = 0, .len = 5, .quote = "Tripp" };
    snprintf(chat.chat, sizeof(chat.chat), "20261104-170000-abcd");
    SourceRef doc = { .kind = SRC_DOC, .start = 0, .len = 5, .quote = "Tripp" };
    snprintf(doc.doc, sizeof(doc.doc), "ch1.md");
    struct { const char *subject, *desc, *status; SourceRef *src; const char *expect; } rows[] = {
        { "Tripp", "Tripp is mentioned.", "new", &chat, "filtered" },                      /* by name only */
        { "Greetings, my friend!", "A greeting addressed to the author.", "new", &chat, "filtered" },
        { "The Gauge", "The Gauge is made of information.", "new", &chat, "new" },        /* story: kept */
        { "Tripp", "Tripp sails north.", "new", &doc, "new" },                             /* documents: never */
        { "Maz", "The name 'Maz' is special.", "saved", &chat, "saved" },                  /* the author's decision */
    };
    char ids[5][40];
    for (int i = 0; i < 5; i++) {
        FindingInfo fi = { 0 };
        snprintf(fi.type, sizeof(fi.type), "entity");
        snprintf(fi.subject, sizeof(fi.subject), "%s", rows[i].subject);
        records_new_id(REC_FINDING, ids[i], sizeof(ids[i]));
        ids[i][strlen(ids[i]) - 1] = (char)('0' + i); /* distinct within one second */
        CHECK(records_append_ex(g_store, proj, REC_FINDING, ids[i], rows[i].status, rows[i].desc, rows[i].src, 1, &fi, NULL));
    }
    CHECK(obs_refilter(g_store, proj, "Tripp", "Maz") == 2);
    Record *r = NULL;
    size_t n = records_list(g_store, proj, REC_FINDING, &r);
    for (int i = 0; i < 5; i++)
        for (size_t k = 0; k < n; k++)
            if (!strcmp(r[k].id, ids[i])) CHECK(!strcmp(record_current(&r[k])->status, rows[i].expect));
    records_free(r, n);
    CHECK(obs_refilter(g_store, proj, "Tripp", "Maz") == 0); /* idempotent */
    /* Names cleared: the greeting stays filtered (conversation), Tripp comes back. */
    CHECK(obs_refilter(g_store, proj, "", "") == 1);
}

/* The author's model settings (user testing): ranges, steps, and the
 * request they produce. */
static void test_model_settings(void)
{
    ModelSettings m;
    model_settings_defaults(&m);
    CHECK(m.max_reply == 2048 && m.context == 32768 && m.temperature == 0.7);
    model_setting_set(&m, 1, 0.7 + 0.05 * 3); /* steps land on the grid */
    CHECK(m.temperature == 0.85);
    model_setting_set(&m, 1, 9.0);
    CHECK(m.temperature == 1.5);                  /* clamped */
    model_setting_set(&m, 0, 100);
    CHECK(m.max_reply == 256);
    model_setting_set(&m, 7, 10000);              /* context: 8K steps */
    CHECK(m.context == 8192);
    m.max_reply = 8192;
    model_settings_clamp(&m);
    CHECK(m.max_reply == 4096);                   /* at most half the context */
    model_setting_set(&m, 1, NAN);
    CHECK(m.temperature == 0.0);

    ChatSampling sp = { 3000, 0.9, 0.8, 20, 0.1, 1.2, 0.3 };
    char *r = chat_apply_sampling("{\"messages\":[],\"max_tokens\":1024,\"temperature\":0.7,\"top_p\":0.95}", &sp, false);
    CHECK(r && strstr(r, "\"max_tokens\":3000") && strstr(r, "\"temperature\":0.9") && strstr(r, "\"top_p\":0.8") &&
          strstr(r, "\"top_k\":20") && strstr(r, "\"min_p\":0.1") && strstr(r, "\"repeat_penalty\":1.2"));
    free(r);
    /* An edit uses the edit temperature and keeps a longer passage-sized length. */
    r = chat_apply_sampling("{\"messages\":[],\"max_tokens\":5000}", &sp, true);
    CHECK(r && strstr(r, "\"max_tokens\":5000") && strstr(r, "\"temperature\":0.3"));
    free(r);
}

/* The memory check before the model starts (user request): the
 * estimate, and the context chosen when memory is short. */
static void test_memfit(void)
{
    const long long MiB = 1024LL * 1024;
    char path[600];
    snprintf(path, sizeof(path), "%s/fake-model.gguf", g_dir);
    FILE *f = fopen(path, "wb");
    CHECK(f && ftruncate(fileno(f), 3000 * MiB) == 0); /* sparse: no disk used */
    if (f) fclose(f);
    LlmServerConfig cfg;
    llm_server_default_config(&cfg, "");
    snprintf(cfg.model_path, sizeof(cfg.model_path), "%s", path);
    /* file + fixed + per token x ctx_total (two slots of 32K) */
    long long at32 = llm_server_mem_need(&cfg, 65536);
    CHECK(at32 == 3000 * MiB + cfg.mem_fixed + (long long)cfg.mem_per_token * 65536);
    /* The measured LFM3 numbers (KV + compute at 64K total: 779 + 557 + 85
     * MiB) are covered by the per-token cost. */
    CHECK((long long)cfg.mem_per_token * 65536 >= (779 + 557 + 85 - 60) * MiB);
    long long other = 2000 * MiB;
    /* Plenty: the author's context. */
    MemFit m = memfit_context(16000 * MiB, other, &cfg, 32768, 8192, 8192);
    CHECK(m.slot_ctx == 32768 && !m.reduced && !m.short_even_min && m.need == at32);
    CHECK(m.room == 16000 * MiB - other - MEMFIT_HEADROOM);
    /* Exactly enough (the headroom included): still 32K; a byte less: 24K. */
    m = memfit_context(at32 + other + MEMFIT_HEADROOM, other, &cfg, 32768, 8192, 8192);
    CHECK(m.slot_ctx == 32768 && !m.reduced);
    m = memfit_context(at32 + other + MEMFIT_HEADROOM - 1, other, &cfg, 32768, 8192, 8192);
    CHECK(m.slot_ctx == 24576 && m.reduced && !m.short_even_min && m.need_wanted == at32);
    /* Only the smallest fits. */
    long long at8 = llm_server_mem_need(&cfg, 16384);
    m = memfit_context(at8 + other + MEMFIT_HEADROOM, other, &cfg, 32768, 8192, 8192);
    CHECK(m.slot_ctx == 8192 && m.reduced && !m.short_even_min);
    /* Not even that: the smallest, flagged. */
    m = memfit_context(at8 + other, other, &cfg, 32768, 8192, 8192);
    CHECK(m.slot_ctx == 8192 && m.short_even_min);
    m = memfit_context(0, 0, &cfg, 16384, 8192, 8192);
    CHECK(m.slot_ctx == 8192 && m.short_even_min && m.reduced);
    /* A minimum off the step grid is still tried. */
    m = memfit_context(llm_server_mem_need(&cfg, 2 * 5000) + MEMFIT_HEADROOM, 0, &cfg, 32768, 5000, 8192);
    CHECK(m.slot_ctx == 5000 && !m.short_even_min);
    /* The embedding server: a fixed context, so a fixed cost. */
    LlmServerConfig e;
    llm_server_embed_config(&e, "");
    snprintf(e.model_path, sizeof(e.model_path), "%s", path);
    CHECK(llm_server_mem_need(&e, e.ctx_total) == 3000 * MiB + e.mem_fixed);
    /* A missing model file: the fixed costs only. */
    snprintf(cfg.model_path, sizeof(cfg.model_path), "%s/none.gguf", g_dir);
    CHECK(llm_server_mem_need(&cfg, 0) == cfg.mem_fixed);
    unlink(path);
    /* The machine's own numbers, and the override for tests. */
    long long total = sysmem_total(), avail = sysmem_available();
    CHECK(total > 0 && avail > 0 && avail <= total);
    setenv("SCRIBODO_MEM_AVAILABLE_MB", "1234", 1);
    CHECK(sysmem_available() == 1234 * MiB);
    unsetenv("SCRIBODO_MEM_AVAILABLE_MB");
    char gb[32];
    memfit_gb(5400000000LL, gb, sizeof(gb));
    CHECK(!strcmp(gb, "5.4 GB"));
}

static void test_owned_store_dependencies(void)
{
    BackupJob backup;
    EntityBackfillJob backfill;
    ServiceSupervisor services;
    CHECK(!backup_job_init(&backup, NULL));
    CHECK(!entity_backfill_job_init(&backfill, NULL));
    CHECK(!service_supervisor_init(&services, NULL));
    CHECK(semantic_client_create(NULL, NULL) == NULL);
    CHECK(observer_create(NULL, NULL, NULL) == NULL);
}


int main(void)
{
    if (!test_state_fixture_init(g_dir, sizeof(g_dir), &g_store)) return 1;
    const TestCase tests[] = {
        { "markdown_display", test_markdown_display },
        { "observer_chat_filter", test_observer_chat_filter },
        { "names_detect", test_names_detect },
        { "name_detection_job", test_name_detection_job },
        { "project_session_and_explorer_snapshot", test_project_session_and_explorer_snapshot },
        { "observer_rules_refilter", test_observer_rules_and_refilter },
        { "model_settings", test_model_settings },
        { "memfit", test_memfit },
        { "owned_store_dependencies", test_owned_store_dependencies },
    };
    int result = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    test_state_fixture_destroy(g_dir, g_store);
    return result;
}
