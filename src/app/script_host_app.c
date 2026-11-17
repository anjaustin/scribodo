/* The scripted host (app/script.h) over App: the adapters UI test scripts drive and read. Moved from src/main.c (AUDIT-004 F3). */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"

static char *host_recall_text(void *ctx);
static bool host_recall_busy(void *ctx);
static Rectangle host_element(void *ctx, const char *name);
static const char *host_status(void *ctx);
static size_t host_history_cache_bytes(void *ctx);
static void host_store_offline(void *ctx, bool offline);
static bool host_history_busy(void *ctx);
static bool host_history_worker_busy(void *ctx);
static bool host_memory_settled(void *ctx, long sequence);
static bool host_memory_enabled(void *ctx);
static char *host_memory_sources(void *ctx);
static bool host_memory_documents_settled(void *ctx, long count);
static bool host_memory_briefs_ready(void *ctx, long count);
static void host_store_restart(void *ctx);
static void host_history_hold(void *ctx, bool hold);
static void host_open(void *ctx, const char *path);
static char *host_record_text(void *ctx);
static void host_observer_enable(void *ctx, bool on);
static bool host_observer_idle(void *ctx);
static char *host_findings_text(void *ctx);
static char *host_attached_text(void *ctx);
static char *host_observer_status(void *ctx);
static char *host_explorer_text(void *ctx);
static bool host_model_ready(void *ctx);
static char *host_model_status(void *ctx);
static void host_drop(void *ctx, const char *path);

static char *host_recall_text(void *ctx)
{
    App *app = ctx;
    return recall_view_text(app->recall);
}

static bool host_recall_busy(void *ctx)
{
    App *app = ctx;
    return recall_job_busy(&app->recall_job) || recall_view_busy(app->recall);
}

/* "Chat0" -> element "Chat" index 0; "ChatInput" -> "ChatInput". */
static Rectangle host_element(void *ctx, const char *name)
{
    App *app = ctx;
    if (!strcmp(name, "ReviewPanel")) return review_active(app->review) ? app->review_rect : (Rectangle){ 0 };
    if (!strcmp(name, "AcceptEdit")) return review_active(app->review) ? review_accept_rect(app->review_rect) : (Rectangle){ 0 };
    if (!strcmp(name, "ApplyLast")) return chat_view_last_apply_rect(app->chat_view);
    if (!strcmp(name, "ChatNoteToggle")) return chat_view_note_toggle_rect(app->chat_view, app->chat_input_rect);
    if (!strcmp(name, "NoteLast")) return chat_view_last_note_rect(app->chat_view, false);
    if (!strcmp(name, "CanonLast")) return chat_view_last_note_rect(app->chat_view, true);
    if (!strcmp(name, "SourcesLast")) return chat_view_last_sources_rect(app->chat_view);
    if (!strcmp(name, "MemorySourcesClose")) return app->memory_sources_open ? app->memory_sources_close : (Rectangle){0};
    if (!strcmp(name, "ChatOlder")) return chat_view_page_rect(app->chat_view, CHAT_PAGE_OLDER);
    if (!strcmp(name, "ChatNewer")) return chat_view_page_rect(app->chat_view, CHAT_PAGE_NEWER);
    if (!strcmp(name, "ChatLatest")) return chat_view_page_rect(app->chat_view, CHAT_PAGE_LATEST);
    if (!strncmp(name, "Record", 6)) return record_view_element(app->record, name);
    if (!strncmp(name, "Prompt", 6)) return prompt_element(app->prompt, name);
    if (!strcmp(name, "AttachAdd")) return app->attach_add;
    if (!strncmp(name, "Fonts", 5)) return fonts_panel_element(&app->fonts_panel, name);
    if (!strncmp(name, "Set", 3) && app->settings_open) { /* the model settings panel */
        if (!strcmp(name, "SetApply")) return app->set_apply;
        if (!strcmp(name, "SetCancel")) return app->set_cancel;
        if (!strcmp(name, "SetDefaults")) return app->set_defaults;
        bool plus = !strncmp(name, "SetPlus", 7), minus = !strncmp(name, "SetMinus", 8);
        int i = atoi(name + (plus ? 7 : 8));
        if ((plus || minus) && i >= 0 && i < MODEL_SETTING_COUNT) return plus ? app->set_plus[i] : app->set_minus[i];
    }
    if (!strncmp(name, "AttachChip", 10) || !strncmp(name, "AttachRemove", 12)) {
        bool rm = name[6] == 'R';
        int i = atoi(name + (rm ? 12 : 10));
        return i >= 0 && i < MAX_ATTACH ? (rm ? app->attach_x[i] : app->attach_chip[i]) : (Rectangle){ 0 };
    }
    if (!strncmp(name, "MenuItem", 8)) return menu_element(&app->menu, name);
    static const char *const welcome[] = { "WelcomeOpen", "WelcomeNew", "WelcomeSample" };
    for (int i = 0; i < 3; i++)
        if (!strcmp(name, welcome[i])) return editor_path(app->editor) ? (Rectangle){ 0 } : app->welcome_btn[i];
    if (!strcmp(name, "EmptyNewDoc")) return editor_path(app->editor) ? (Rectangle){ 0 } : app->empty_btn[0];
    if (!strcmp(name, "EmptyImport")) return editor_path(app->editor) ? (Rectangle){ 0 } : app->empty_btn[1];
    if (!strncmp(name, "Recent", 6) && name[6] >= '0' && name[6] <= '9') {
        int i = atoi(name + 6);
        return i >= 0 && i < RECENT_MAX && !editor_path(app->editor) ? app->recent_rect[i] : (Rectangle){ 0 };
    }
    if (!strncmp(name, "Finding", 7) && strcmp(name, "Findings") != 0)
        return app->observer_open ? findings_view_element(app->findings, name) : (Rectangle){ 0 };
    if (!strcmp(name, "RejectEdit")) return review_active(app->review) ? review_reject_rect(app->review_rect) : (Rectangle){ 0 };
    if (!strncmp(name, "Recall", 6) && strcmp(name, "RecallToggle") && strcmp(name, "RecallBody") && strcmp(name, "Recall"))
        return app->recall_open ? recall_view_element(app->recall, name) : (Rectangle){ 0 };
    size_t n = strlen(name), k = n;
    while (k > 0 && name[k - 1] >= '0' && name[k - 1] <= '9') k--;
    Clay_String s = { .isStaticallyAllocated = false, .length = (int32_t)k, .chars = name };
    bool indexed = k < n;
    uint32_t index = indexed ? (uint32_t)atoi(name + k) : 0;
    Rectangle r = element_rect_str(s, index, indexed);
    /* An explorer item laid out below or above the panel's visible area (a
     * project with sixty chapters puts the chat list off screen) is scrolled
     * into view, as a user would with the wheel, and its rectangle is
     * reported where it will be once the next layout applies the offset. The
     * click itself lands on a later frame, so the two agree (MEMORY-021).
     * Only the explorer's own descendants: a toolbar button shares the
     * panel's columns but does not move with it (red-team 016). */
    Rectangle explorer = ELEMENT_RECT("Explorer");
    Clay_ElementId id = indexed ? Clay_GetElementIdWithIndex(s, index) : Clay_GetElementId(s);
    if (r.width > 0 && explorer.width > 0 && clay_element_within(CLAY_ID("Explorer"), id) &&
        (r.y < explorer.y || r.y + r.height > explorer.y + explorer.height)) {
        Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("Explorer"));
        if (sc.found && sc.scrollPosition) {
            float want = r.y < explorer.y ? explorer.y + 12.0f - r.y : (explorer.y + explorer.height - 12.0f) - (r.y + r.height);
            float lowest = -(sc.contentDimensions.height - sc.scrollContainerDimensions.height);
            if (lowest > 0) lowest = 0;
            float target = sc.scrollPosition->y + want;
            if (target < lowest) target = lowest;
            if (target > 0) target = 0;
            float applied = target - sc.scrollPosition->y;
            sc.scrollPosition->y = target;
            r.y += applied;
        }
    }
    return r;
}

static char *host_font_name(void *ctx, int role)
{
    App *app = ctx;
    return strdup(role >= 0 && role < FONT_COUNT ? sfont_primary_name(&app->fonts[role]) : "");
}

static const char *host_status(void *ctx)
{
    App *app = ctx;
    return GetTime() - app->status_time < 6.0 ? app->status : "";
}

static size_t host_history_cache_bytes(void *ctx) { return chat_view_cache_bytes(((App *)ctx)->chat_view); }

static void host_store_offline(void *ctx, bool offline)
{
    App *app = ctx;
    store_set_offline(app->store, offline);
    if (offline) cancel_history(app);
    app->lists_dirty = true;
}

static bool host_history_busy(void *ctx) { return ((App *)ctx)->history_pending; }

static bool host_history_worker_busy(void *ctx) { return history_read_busy(ctx); }

static bool host_memory_settled(void *ctx, long sequence)
{
    App *app = ctx; MemoryJobStatus status;
    if (!app->memory_job || !memory_job_status(app->memory_job, &status) || !status.active ||
        strcmp(status.scope.conversation_id, app->chat_id)) return false;
    return sequence < 0 ? !status.enabled : status.enabled && status.scope.ingested_sequence >= sequence &&
        status.scope.pending_sequence <= status.scope.ingested_sequence && !status.scope.index_pending;
}

static bool host_memory_enabled(void *ctx)
{
    App *app = ctx; MemoryJobStatus status;
    return app->memory_job && memory_job_status(app->memory_job, &status) && status.enabled &&
        !strcmp(status.scope.conversation_id, app->chat_id);
}

static char *host_memory_sources(void *ctx)
{
    App *app = ctx;
    return app->memory_sources_open ? editor_text_utf8(app->memory_sources_editor) : chat_memory_sources(app->chat);
}

static bool host_memory_documents_settled(void *ctx, long count)
{
    App *app = ctx; MemoryJobStatus status;
    return count >= 0 && app->memory_job && memory_job_status(app->memory_job, &status) &&
        status.active && status.enabled && !status.controls_pending && !status.documents_pending &&
        status.document_sources == (size_t)count && !status.scope.index_pending &&
        !strcmp(status.scope.conversation_id, app->chat_id);
}

static bool host_memory_briefs_ready(void *ctx, long count)
{
    App *app = ctx; MemoryJobStatus status;
    return count > 0 && app->memory_job && memory_job_status(app->memory_job, &status) && status.active && status.enabled &&
        !status.controls_pending && status.briefs_published >= (size_t)count && !strcmp(status.scope.conversation_id, app->chat_id);
}

static void host_store_restart(void *ctx)
{
    App *app = ctx;
    char database[64], why[256];
    store_database(app->store, database, sizeof(database));
    /* Deliberately no intent cancellation: exercise publication's epoch fence. */
    if (!store_start(app->store, database, why, sizeof(why))) set_status(app, why);
}

static void host_history_hold(void *ctx, bool hold) { atomic_store(&((App *)ctx)->history_hold, hold); }

bool scripted_history_read(StoreManager *store, const char *project, const char *conversation,
                                  const ChatWindowRequest *request, ChatWindowPage *page, void *context)
{
    App *app = context;
    bool ok = chat_store_window(store, project, conversation, request, page);
    while (atomic_load(&app->history_hold)) {
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    return ok;
}

static void host_open(void *ctx, const char *path) { open_path(ctx, path); }

static char *host_record_text(void *ctx)
{
    App *app = ctx;
    if (!record_view_active(app->record)) return strdup("");
    char *t = record_view_text(app->record);
    const char *hd = record_view_header(app->record);
    size_t n = strlen(hd) + (t ? strlen(t) : 0) + 2;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s\n%s", hd, t ? t : "");
    free(t);
    return out;
}

static void host_observer_enable(void *ctx, bool on) { observer_set_enabled(((App *)ctx)->observer, on); }

static bool host_observer_idle(void *ctx) { return observer_idle(((App *)ctx)->observer); }

static char *host_findings_text(void *ctx) { return findings_view_text(((App *)ctx)->findings); }

static char *host_attached_text(void *ctx)
{
    App *app = ctx;
    size_t cap = 2;
    for (size_t i = 0; i < app->nattached; i++) cap += strlen(app->attached[i]) + 1;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (size_t i = 0; i < app->nattached; i++) {
        if (i) strcat(s, ",");
        strcat(s, app->attached[i]);
    }
    return s;
}

static char *host_observer_status(void *ctx)
{
    char st[200];
    observer_status(((App *)ctx)->observer, st, sizeof(st));
    return strdup(st);
}

static char *host_explorer_text(void *ctx)
{
    App *app = ctx;
    size_t cap = 2;
    for (size_t i = 0; i < app->ndocs; i++) cap += strlen(app->docs[i]) + 1;
    for (size_t i = 0; i < app->nchats; i++) cap += sizeof(app->chat_labels[0]) + 1;
    cap += 48; /* "Archived (n)" */
    cap += (sizeof(app->ent_labels[0]) + 1) * ENT_SHOW + (sizeof(app->ent_row_labels[0]) + 1) * ENT_ROWS;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (size_t i = 0; i < app->ndocs; i++) strcat(strcat(s, app->docs[i]), "\n");
    /* Chats as the Explorer draws them: archived ones under their heading, listed only while it is open. */
    size_t narchived = 0;
    for (size_t i = 0; i < app->nchats; i++) {
        if (app->chats[i].archived) { narchived++; continue; }
        strcat(strcat(s, app->chat_labels ? app->chat_labels[i] : app->chats[i].title), "\n");
    }
    if (narchived) {
        char head[48];
        snprintf(head, sizeof(head), "Archived (%zu)\n", narchived);
        strcat(s, head);
    }
    for (size_t i = 0; app->chats_archived_open && i < app->nchats; i++)
        if (app->chats[i].archived) strcat(strcat(s, app->chat_labels ? app->chat_labels[i] : app->chats[i].title), "\n");
    /* Entities, and the open one's rows (without their indent). */
    for (size_t i = 0; i < app->nents && i < ENT_SHOW; i++) strcat(strcat(s, app->ent_labels[i]), "\n");
    for (size_t i = 0; app->ent_open[0] && i < app->ent.nrows && i < ENT_ROWS; i++)
        strcat(strcat(s, app->ent_row_labels[i] + strspn(app->ent_row_labels[i], " ")), "\n");
    return s;
}

static bool host_model_ready(void *ctx)
{
    App *app = ctx;
    return !app->llm || llm_server_state(app->llm, NULL, 0) == LLM_READY;
}

static char *host_model_status(void *ctx)
{
    App *app = ctx;
    char msg[512] = "";
    if (app->llm) llm_server_state(app->llm, msg, sizeof(msg));
    return strdup(msg);
}

static void host_drop(void *ctx, const char *path)
{
    App *app = ctx; /* the same rule as a real drop: ignored behind a prompt or menu */
    if (!prompt_active(app->prompt) && !app->menu.active) drop_path(app, path);
}

/* The scripted host every UI test drives (app/script.h), and the script itself. 0, or the exit code. */
int script_host_setup(App *app, MainLoop *loop, const StartupEnv *env, const char *script_path)
{
    dialogs_script_mode(script_path != NULL);
    loop->host = (ScriptHost){
        .editor = app->editor,
        .explorer_w = &app->explorer_w,
        .observer_w = &app->observer_w,
        .editor_rect = &app->editor_rect,
        .status = host_status,
        .font_name = host_font_name,
        .open = host_open,
        .drop = host_drop,
        .ctx = app,
        .chat = app->chat,
        .element = host_element,
        .model_ready = host_model_ready,
        .record_text = host_record_text,
        .observer_enable = host_observer_enable,
        .observer_idle = host_observer_idle,
        .findings_text = host_findings_text,
        .attached_text = host_attached_text,
        .explorer_text = host_explorer_text,
        .observer_status = host_observer_status,
        .history_cache_bytes = host_history_cache_bytes,
        .store_offline = host_store_offline,
        .history_busy = host_history_busy,
        .history_hold = host_history_hold,
        .history_worker_busy = host_history_worker_busy,
        .store_restart = host_store_restart,
        .memory_settled = host_memory_settled,
        .memory_enabled = host_memory_enabled,
        .memory_sources = host_memory_sources,
        .memory_documents_settled = host_memory_documents_settled,
        .memory_briefs_ready = host_memory_briefs_ready,
        .recall_text = host_recall_text,
        .recall_busy = host_recall_busy,
        .model_status = host_model_status,
    };
    if (script_path) {
        const char *missing = script_host_missing(&loop->host);
        if (missing) {
            fprintf(stderr, "scribodo: the script host has no %s\n", missing);
            return 2;
        }
        char err[256];
        app->script = script_load(script_path, env->has_script_dir ? env->script_dir : NULL,
                                 env->has_shot_dir ? env->shot_dir : NULL, err, sizeof(err));
        if (!app->script) {
            fprintf(stderr, "scribodo: %s\n", err);
            return 2;
        }
        printf("script %s\n", script_path);
    }
    return 0;
}
