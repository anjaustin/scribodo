/* Startup phases and app_destroy: one destroy path for full and partial initialisation. Moved from src/main.c (AUDIT-004 F3). */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"

#include <mach-o/dyld.h>

#include "ui/mac_window.h"
#include "ui/motion.h"
#include "ui/widgets.h"

static void clay_error(Clay_ErrorData err);
static bool load_fonts(App *app, float dpi);

static void clay_error(Clay_ErrorData err)
{
    fprintf(stderr, "clay: %.*s\n", err.errorText.length, err.errorText.chars);
}

/* One role's font: the author's choice (SCRIBODO_PROSE_FONT first for documents and chat), else the bundled default,
 * then the fallbacks. A choice that does not load falls back to the default and says so in `why`. */
static bool load_role(App *app, int role, SFont *out, bool *choice_failed)
{
    const AppearanceRoleInfo *in = appearance_role_info(role);
    float size = app->appearance.role[role].size;
    SFontSource src[SFONT_MAX_FACES + 1];
    char paths[SFONT_MAX_FACES][PATH_MAX];
    const char *prose = (role == FONT_PROSE || role == FONT_CHAT) ? getenv("SCRIBODO_PROSE_FONT") : NULL;
    int chosen = prose && prose[0] ? 1 : 0; /* where the saved choice sits in src */
    bool has_choice = appearance_has_choice(&app->appearance, role);
    *choice_failed = false;
    for (int pass = 0; pass < 2; pass++) {
        bool with_choice = pass == 0;
        int n = 0;
        if (prose && prose[0]) src[n++] = (SFontSource){ prose, 0, false };
        SFontSource rest[SFONT_MAX_FACES];
        int m = appearance_sources(&app->appearance, role, with_choice, app->assets_dir, rest, paths);
        for (int i = 0; i < m && n < SFONT_MAX_FACES; i++) src[n++] = rest[i];
        bool ok = sfont_load(out, src, n, size, in->line_height, app->dpi);
        if (ok && (!with_choice || !has_choice || out->primary_source <= chosen)) return true;
        if (ok) sfont_unload(out);
        if (with_choice && has_choice) *choice_failed = true;
        if (!has_choice) break; /* the second pass would load the same faces */
    }
    return false;
}

/* The fonts of `roles` from app->appearance. Each loads into a scratch SFont and replaces the old one only when it
 * loaded, so a failure keeps what is on screen; the SFont addresses never change (views and Clay hold them). The batch
 * of text already queued this frame is drawn first, since it names the textures about to be freed. Afterwards the widgets'
 * default font and Clay's text-measurement cache follow; views reflow because each load has a new version. */
unsigned app_fonts_apply(App *app, unsigned roles, char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    unsigned failed = 0;
    if (app->window_ready) rlDrawRenderBatchActive();
    for (int r = 0; r < FONT_COUNT; r++) {
        if (!(roles & (1u << r))) continue;
        SFont next;
        bool choice_failed = false;
        bool ok = load_role(app, r, &next, &choice_failed);
        if (choice_failed) {
            failed |= 1u << r;
            if (why && whylen && !why[0])
                snprintf(why, whylen, "The %s font could not be loaded (%s); using the default",
                         appearance_role_info(r)->label, app->appearance.role[r].path);
        }
        if (!ok) {
            if (why && whylen && !why[0]) snprintf(why, whylen, "No %s font could be loaded", appearance_role_info(r)->label);
            continue;
        }
        if (app->fonts[r].nfaces) sfont_unload(&app->fonts[r]);
        app->fonts[r] = next;
    }
    for (int i = 0; i < FONT_COUNT; i++) app->font_ptrs[i] = &app->fonts[i];
    if (app->fonts[FONT_UI].nfaces) widget_set_font(&app->fonts[FONT_UI]);
    if (app->clay_buf) Clay_ResetMeasureTextCache();
    return failed;
}

/* Frames at the display's own rate: 120 on a ProMotion screen, 60 on this MacBook Air's, so motion is as smooth as
 * the panel allows. UI test runs stay at 60: their waits count frames. Checked again every few seconds by the frame
 * loop, since the window can move to another display. */
void app_match_refresh_rate(App *app)
{
    int hz = app->scripted ? 60 : GetMonitorRefreshRate(GetCurrentMonitor());
    if (hz < 30 || hz > 240) hz = 60;
    if (hz != app->target_fps) {
        SetTargetFPS(hz);
        app->target_fps = hz;
    }
}

void app_apply_look(App *app)
{
    theme_select(app->appearance.theme);
    if (app->editor) editor_set_writing_aids(app->editor, app->appearance.typewriter, app->appearance.dim_paragraphs);
}

void app_set_focus_mode(App *app, bool on)
{
    if (on && app->edit_mode) { /* a chat edit needs the chat: it ends, its passage marker with it */
        leave_edit_mode(app);
        editor_anchor_clear(app->editor);
    }
    app->focus_mode = on;
    if (on) app->focus = FOCUS_DOC;
    set_status(app, on ? "Focus mode: the page alone (\xE2\x8C\x98\\ to come back)" : "Focus mode off");
}

static bool load_fonts(App *app, float dpi)
{
    app->dpi = dpi;
    char exe[PATH_MAX];
    uint32_t exe_len = sizeof(exe);
    if (_NSGetExecutablePath(exe, &exe_len) == 0) sfont_set_probe_executable(exe); /* untrusted faces: ui/font.h */
    appearance_load(&app->appearance);
    if (!assets_dir_resolve(GetApplicationDirectory(), app->assets_dir, sizeof(app->assets_dir)))
        fprintf(stderr, "scribodo: the bundled fonts were not found beside the app; using the system fallbacks\n");
    char why[PATH_MAX + 128];
    unsigned failed = app_fonts_apply(app, APP_FONTS_ALL, why, sizeof(why));
    /* A saved face that no longer loads: the role shows the default, and the panel starts from it (saved again only
     * when the author presses Done). */
    for (int r = 0; r < FONT_COUNT; r++)
        if (failed & (1u << r)) app->appearance.role[r].path[0] = '\0', app->appearance.role[r].index = 0;
    if (!app->assets_dir[0]) snprintf(app->font_notice, sizeof(app->font_notice), "%s",
                                      "The bundled fonts were not found beside Scribodo; showing Arial instead");
    else if (why[0]) snprintf(app->font_notice, sizeof(app->font_notice), "%s", why);
    if (app->font_notice[0]) fprintf(stderr, "scribodo: %s\n", app->font_notice);
    for (int r = 0; r < FONT_COUNT; r++)
        if (!app->fonts[r].nfaces) return false;
    return true;
}

void app_destroy(App *app)
{
    if (!app) return;
    draft_controller_destroy(&app->draft); /* a draft write in flight finishes; the draft stays on disk */
    frame_view_model_destroy(&app->frame);
    recent_store_destroy(app->recent_store);
    app->recent_store = NULL;
    script_free(app->script);
    app->script = NULL;
    generative_gate_close(app->generative_gate);
    if (app->chat) chat_cancel(app->chat);
    memory_job_destroy(app->memory_job);
    app->memory_job = NULL;
    atomic_store(&app->history_hold, false);
    chat_history_job_destroy(app->history_job);
    app->history_job = NULL;
    reviewed_edit_controller_destroy(&app->reviewed_edit);
    record_view_destroy(app->record);
    prompt_destroy(app->prompt);
    records_free(app->notes, app->nnotes);
    records_free(app->canon, app->ncanon);
    free(app->revs);
    if (app->backup_initialized) (void)backup_job_destroy(&app->backup);
    if (app->import_job_initialized) import_job_destroy(&app->import_job);
    app->import_job_initialized = false;
    if (app->graph_job_initialized) entity_backfill_job_destroy(&app->graph_job);
    if (app->names_job_initialized) name_detection_job_destroy(&app->names_job);
    observer_destroy(app->observer); /* cancels its request before the server stops */
    chat_controller_destroy(&app->chat_controller); /* joins memory reads before dependencies below */
    memory_document_cache_destroy(app->memory_document_cache);
    if (!generative_gate_destroy(app->generative_gate))
        fprintf(stderr, "scribodo: generation admission still has live borrowers during shutdown\n");
    editor_destroy(app->memory_sources_editor);
    semantic_client_destroy(app->semantic);
    app->semantic = NULL;
    if (app->recall_job_initialized) recall_job_destroy(&app->recall_job);
    app->recall_job_initialized = false;
    if (app->services_initialized) (void)service_supervisor_destroy(&app->services);
    store_manager_destroy(app->store);
    app->store = NULL;
    findings_view_destroy(app->findings);
    recall_view_destroy(app->recall);
    records_free(app->flist, app->nflist);
    entity_list_free(app->ents, app->nents);
    entity_detail_free(&app->ent);
    llm_server_destroy(app->llm);
    llm_server_destroy(app->embed);
    rerank_client_destroy(app->rerank_client);
    llm_server_destroy(app->rerank);
    editor_destroy(app->editor);
    explorer_snapshot_destroy(&app->explorer);
    project_session_destroy(&app->session);
    fonts_panel_destroy(&app->fonts_panel);
    for (int i = 0; i < FONT_COUNT; i++) sfont_unload(&app->fonts[i]);
    free(app->clay_buf);
    app->clay_buf = NULL;
    if (app->window_ready) {
        CloseWindow();
        app->window_ready = false;
    }
    memset(app, 0, sizeof(*app));
}

/* Controllers and the store-backed workers that need no window. 0, or the exit code. */
int app_init_jobs(App *app, const char *script_path)
{
    frame_view_model_init(&app->frame);
    project_session_init(&app->session);
    explorer_snapshot_init(&app->explorer);
    chat_controller_init(&app->chat_controller);
    reviewed_edit_controller_init(&app->reviewed_edit);
    draft_controller_init(&app->draft);
    if (!entity_backfill_job_init(&app->graph_job, app->store)) {
        return 1;
    }
    app->graph_job_initialized = true;
    app->import_job_initialized = import_job_init(&app->import_job, app->store);
    if (!name_detection_job_init(&app->names_job, NULL)) {
        return 1;
    }
    app->names_job_initialized = true;
    app->history_job = script_path ? chat_history_job_create_with_reader(app->store, scripted_history_read, app)
                                  : chat_history_job_create(app->store);
    if (!app->history_job) { return 1; }
    return 0;
}

/* The window, the services and backup workers, fonts and the Clay arena. 0, or the exit code. */
int app_open_window(App *app, const char *script_path)
{
    /* Motion (ui/motion.h): off in UI test runs, whose checks read the end state, and under macOS's Reduce motion. */
    motion_set_enabled(!script_path && !motion_system_reduced());
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT;
    if (script_path) {
        flags |= FLAG_WINDOW_HIDDEN;
        input_set_scripted(true);
        SetTraceLogLevel(LOG_ERROR);
    }
    SetConfigFlags(flags);
    InitWindow(1440, 860, "Scribodo");
    if (!IsWindowReady()) {
        fprintf(stderr, "scribodo: could not create a window (is a display available?)\n");
        return 3;
    }
    app->window_ready = true;
    /* The toolbar runs to the top edge, the traffic lights inside it (ui/mac_window.h). */
    app->unified_titlebar = mac_window_unify(GetWindowHandle(), TOOLBAR_H);
    SetWindowMinSize(900, 480);
    SetExitKey(KEY_NULL); /* Esc must never quit a writing app */
    app->scripted = script_path != NULL;
    app_match_refresh_rate(app);

    if (!backup_job_init(&app->backup, app->store)) {
        fprintf(stderr, "scribodo: couldn't initialise the backup worker\n");
        return 1;
    }
    app->backup_initialized = true;
    if (!service_supervisor_init(&app->services, app->store)) {
        fprintf(stderr, "scribodo: couldn't initialise the service supervisor\n");
        return 1;
    }
    app->services_initialized = true;
    app->explorer_w = 210;
    app->observer_w = 260;
    app->chat_w = 420;
    app->focus = FOCUS_DOC;

    float dpi = GetWindowScaleDPI().x;
    if (!load_fonts(app, dpi)) {
        fprintf(stderr, "scribodo: no usable font found (the bundled fonts in assets/fonts, or SCRIBODO_PROSE_FONT)\n");
        return 1;
    }

    uint32_t clay_mem = Clay_MinMemorySize();
    void *clay_buf = malloc(clay_mem);
    if (!clay_buf) {
        fprintf(stderr, "scribodo: out of memory\n");
        return 1;
    }
    app->clay_buf = clay_buf;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem, clay_buf);
    Clay_Initialize(arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_error, 0 });
    Clay_SetMeasureTextFunction(clay_measure_text, app->font_ptrs);
    return 0;
}

/* The model settings, the local model servers (UI scripts use a fake model unless SCRIBODO_LLM=real), the semantic
 * client, the reranker and the memory experiment. 0, or the exit code. */
int app_start_models(App *app, const char *script_path)
{
    /* The local model. UI scripts use a fake model unless SCRIBODO_LLM=real. */
    /* The author's model settings (UI scripts keep their own: startup_env_prepare). */
    model_settings_load(&app->model);
    /* The verified-answer pipeline (default on since 2026-11-14) runs for conversations with memory on; it needs the
     * memory experiment and, with a real model, the pinned reranker file. */
    const char *llm_mode = getenv("SCRIBODO_LLM");
    bool fake = (llm_mode && !strcmp(llm_mode, "fake")) || (script_path && !(llm_mode && !strcmp(llm_mode, "real")));
    /* With the fake backend nothing can extract a span, so the pipeline runs only when a test asks for its fixture. */
    app->answer_pipeline = experiments()->answer_pipeline && experiments()->memory_experiment &&
                          (!fake || experiments()->answer_pipeline_requested);
    if (!fake) {
        LlmServerConfig cfg, ecfg, rcfg;
        llm_server_default_config(&cfg, GetApplicationDirectory());
        llm_server_embed_config(&ecfg, GetApplicationDirectory());
        llm_server_rerank_config(&rcfg, GetApplicationDirectory());
        if (app->answer_pipeline && !local_models_reranker_present(&rcfg)) app->answer_pipeline = false;
        /* The author's context per conversation, as far as memory allows
         * (the other servers are about to start). */
        const ModelSettingInfo *ci = model_setting_info(7);
        long long others = llm_server_mem_need(&ecfg, ecfg.ctx_total) +
                           (app->answer_pipeline ? llm_server_mem_need(&rcfg, rcfg.ctx_total) : 0);
        (void)local_models_reap_orphans(&cfg, &ecfg, app->answer_pipeline ? &rcfg : NULL);
        MemFit fit = memfit_context(sysmem_available(), others, &cfg, app->model.context, (int)ci->min, (int)ci->step);
        memfit_message(&fit, app->model.context, app->mem_msg, sizeof(app->mem_msg));
        if (app->mem_msg[0]) fprintf(stderr, "scribodo: %s\n", app->mem_msg);
        cfg.ctx_total = fit.slot_ctx * (cfg.slots > 0 ? cfg.slots : 1);
        app->llm = llm_server_create(&cfg);
        if (app->llm) llm_server_start(app->llm);
        /* Embeddings for the semantic index (a second, small server). */
        app->embed = llm_server_create(&ecfg);
        if (app->embed) llm_server_start(app->embed);
        if (app->answer_pipeline) {
            app->rerank = llm_server_create(&rcfg);
            if (app->rerank) llm_server_start(app->rerank);
        }
        if (!app->llm || !app->embed || (app->answer_pipeline && !app->rerank)) {
            fprintf(stderr, "scribodo: could not allocate the pinned local model supervisors (no fake fallback)\n");
            return 1;
        }
    }
    app->semantic = semantic_client_create(app->store, app->embed);
    if (app->answer_pipeline) {
        /* With the fake backend there is no reranker server: a deterministic fixture scores instead. */
        app->rerank_client = rerank_client_create(app->rerank);
        if (!app->rerank_client) { return 1; }
    }
    app->memory_experiment = experiments()->memory_experiment;
    app->memory_briefs_experiment = app->memory_experiment && experiments()->memory_briefs;
    if (app->memory_experiment) {
        app->memory_document_cache = memory_document_cache_create();
        app->generative_gate = generative_gate_create();
        app->memory_job = memory_job_create(app->store, app->semantic, app->llm);
        if (!app->memory_job || !app->memory_document_cache || !app->generative_gate ||
            !memory_job_set_document_cache(app->memory_job, app->memory_document_cache) ||
            !memory_job_set_generative_gate(app->memory_job, app->generative_gate) ||
            !memory_job_set_briefs_enabled(app->memory_job, app->memory_briefs_experiment)) { return 1; }
    }
    return 0;
}

/* The chat session, the remote model, the editor and every view; the profile and model settings applied.
 * 0, or the exit code. */
int app_create_views(App *app, const char *script_path)
{
    ChatSession *owned_chat = chat_create(app->llm);
    if (owned_chat) {
        chat_memory_set_document_cache(owned_chat, app->memory_document_cache);
        chat_set_generative_gate(owned_chat, app->generative_gate);
        chat_memory_set_briefs_enabled(owned_chat, app->memory_briefs_experiment);
        chat_answer_pipeline_configure(owned_chat, app->rerank_client, app->answer_pipeline);
        if (app->memory_experiment && experiments()->memory_presentation_compact)
            chat_memory_set_presentation(owned_chat, CHAT_MEMORY_COMPACT);
        else if (app->memory_experiment && experiments()->memory_presentation_episode)
            chat_memory_set_presentation(owned_chat, CHAT_MEMORY_EPISODE);
    }
    name_detection_job_set_server(&app->names_job, app->llm);
    /* A remote model only if the author configured one; never selected here. TLS secrets are never written out for
     * its traffic, and UI scripts never read the author's remote configuration (its provider name and host would
     * reach a run's artefacts); they keep their own, normally absent, in the fixture folder (startup_env_prepare). */
    app->remote_ok = remote_load(&app->remote, app->remote_why, sizeof(app->remote_why));
    if (!app->remote_ok && app->remote_why[0]) {
        char msg[300];
        snprintf(msg, sizeof(msg), "The remote model isn't available: %s", app->remote_why);
        set_status(app, msg);
    }
    app->editor = editor_create(&app->fonts[FONT_PROSE]);
    editor_on_will_change(app->editor, memory_editor_will_change, app);
    ChatView *chat_view = owned_chat ? chat_view_create(&app->fonts[FONT_CHAT], &app->fonts[FONT_UI], owned_chat) : NULL;
    ReviewView *review = review_create(&app->fonts[FONT_PROSE], &app->fonts[FONT_UI]);
    if (!chat_controller_adopt(&app->chat_controller, owned_chat, chat_view)) {
        chat_view_destroy(chat_view);
        chat_destroy(owned_chat);
    }
    app->memory_sources_editor = editor_create(&app->fonts[FONT_CHAT]);
    if (app->memory_sources_editor) editor_set_layout(app->memory_sources_editor, 32.0f, 28.0f, 720.0f); /* the width it had before the measure (8d5d6a4) */
    if (!app->memory_sources_editor) { return 1; }
    editor_set_modal_owner(app->memory_sources_editor, true);
    if (!reviewed_edit_controller_adopt(&app->reviewed_edit, app->editor, review)) review_destroy(review);
    app->record = record_view_create(&app->fonts[FONT_CHAT], &app->fonts[FONT_UI]);
    app->prompt = prompt_create(&app->fonts[FONT_CHAT], &app->fonts[FONT_UI]);
    app->observer = observer_create(app->store, app->llm, app->semantic);
    if (app->observer) observer_set_generative_gate(app->observer, app->generative_gate);
    app->findings = findings_view_create(&app->fonts[FONT_CHAT], &app->fonts[FONT_UI]);
    app->recall = recall_view_create(&app->fonts[FONT_CHAT], &app->fonts[FONT_UI]);
    app->recall_job_initialized = recall_job_init(&app->recall_job, app->store);
    app->findings_version = UINT64_MAX;
    /* UI scripts start with the observer off (deterministic files); a
     * script turns it on with "observer on". */
    if (app->observer && script_path) observer_set_enabled(app->observer, false);
    if (!app->chat || !app->editor || !app->chat_view || !app->review || !app->record || !app->observer || !app->semantic ||
        !app->findings || !app->prompt || !app->recall || !app->recall_job_initialized) {
        fprintf(stderr, "scribodo: out of memory\n");
        return 1;
    }
    chat_on_message_done(app->chat, on_message_done, app);
    /* Who is who. UI scripts keep their own profile, never the author's (startup_env_prepare). */
    profile_load(&app->profile);
    apply_profile(app);
    apply_model_settings(app, false);
    app_apply_look(app); /* the saved palette and writing aids, now that the editor exists */
    if (app->mem_msg[0]) set_status(app, app->mem_msg);
    if (!app->profile.asked && !script_path) begin_names(app); /* asked once, on first launch */
    return 0;
}

int app_load_recents(App *app, MainLoop *loop, const StartupEnv *env, const char *script_path)
{
    /* Default to the bundled sample, located relative to the executable
     * (build/scribodo) rather than the working directory, which is "/" when
     * launched from Finder. */
    snprintf(loop->sample, sizeof(loop->sample), "%s../assets/sample/sample.md", GetApplicationDirectory());
    /* Recent projects live in the app's support folder; a script keeps its
     * own list in its fixture folder. */
    {
        char state[PATH_MAX];
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') { /* Finder launches have HOME; be safe anyway */
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        if (script_path) snprintf(state, sizeof(state), "%s/.scribodo-state", env->script_dir);
        else snprintf(state, sizeof(state), "%s/Library/Application Support/Scribodo", home ? home : ".");
        app->recent_store = recent_store_create(state);
        if (!app->recent_store) {
            fprintf(stderr, "scribodo: out of memory\n");
            return 1;
        }
        app->nrecents = recent_store_load(app->recent_store, app->recents, RECENT_MAX);
    }
    return 0;
}

void app_open_initial(App *app, const char *path, const char *script_path, const char *sample)
{
    /* The project store: on for the app (SCRIBODO_STORE=off disables it);
     * UI scripts use files unless SCRIBODO_STORE=db. */
    {
        const char *sm = getenv("SCRIBODO_STORE");
        app->store_wanted = script_path ? (sm && !strcmp(sm, "db")) : !(sm && !strcmp(sm, "off"));
        if (app->store_wanted) store_begin(app);
    }
    if (!script_path) {
        struct stat pst;
        if (path && stat(path, &pst) == 0 && S_ISDIR(pst.st_mode)) {
            open_project(app, path);
        } else if (path) {
            open_path(app, path);
        } else if (app->nrecents) { /* where the author left off */
            empty_action(app, 10, sample);
        }
        /* otherwise: the welcome screen */
    }
}
