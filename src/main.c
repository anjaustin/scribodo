/* Scribodo: native workbench (raylib + Clay layout + raygui controls) with
 * the prose editor, a creative chat backed by the local LFM3 model, and the
 * project explorer.
 *
 *   scribodo [file.md|file.txt]
 *
 * Drop a .md/.txt file on the window to open it. ⌘S saves. ⌘L focuses the
 * chat; Esc returns to the document.
 */
#define _DARWIN_C_SOURCE
#include <signal.h>
#include <dirent.h>
#include <stdatomic.h>
#include <pthread.h>
#include <pwd.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clay.h"
#include "raygui.h"
#include "raylib.h"
#include "rlgl.h"

#include "app/chat_store.h"
#include "core/utf8.h"
#include "app/recent.h"
#include "app/records.h"
#include "app/renames.h"
#include "app/store.h"
#include "app/project_store.h"
#include "app/store_backends.h"
#include "app/store_entities.h"
#include "app/backup.h"
#include "app/backup_job.h"
#include "app/chat_history_job.h"
#include "app/memory_job.h"
#include "app/memory_documents.h"
#include "app/service_supervisor.h"
#include "app/entity_backfill_job.h"
#include "app/name_detection_job.h"
#include "app/project_session.h"
#include "app/explorer_snapshot.h"
#include "app/chat_controller.h"
#include "app/reviewed_edit_controller.h"
#include "app/frame_view_model.h"
#include "app/profile.h"
#include "app/model_settings.h"
#include "llm/memfit.h"
#include "llm/names.h"
#include "app/revisions.h"
#include "llm/llm_text.h"
#include "app/script.h"
#include "llm/chat.h"
#include "llm/observer.h"
#include "llm/semantic.h"
#include "llm/server.h"
#include "ui/chat_view.h"
#include "ui/clay_render.h"
#include "ui/editor.h"
#include "ui/findings_view.h"
#include "ui/font.h"
#include "ui/input.h"
#include "platform/dialogs.h"
#include "ui/menu_view.h"
#include "ui/prompt_view.h"
#include "ui/record_view.h"
#include "ui/review_view.h"

/* raylib re-reads GLFW's close flag every frame, so clearing it cancels a
 * close request. GLFWwindow is opaque; raylib hands us the pointer. */
void glfwSetWindowShouldClose(void *window, int value);

enum { FONT_UI = 0, FONT_PROSE = 1, FONT_CHAT = 2, FONT_COUNT };
enum { FOCUS_NONE, FOCUS_DOC, FOCUS_CHAT, FOCUS_RECORD };
enum { DRAG_NONE, DRAG_EXPLORER, DRAG_OBSERVER, DRAG_CHAT };
enum { PT_NEW_DOC = 1, PT_RENAME_DOC, PT_RENAME_CHAT, PT_ATTACH_NAME, PT_NAME_AUTHOR, PT_NAME_ASSISTANT };  /* what the name prompt is for */
enum { MT_DOC = 1, MT_CHAT, MT_ATTACH, MT_MODEL };  /* what a context menu is for */
#define MAX_ATTACH CHAT_CONTROLLER_MAX_ATTACH

#define RGB(r, g, b) ((Clay_Color){ r, g, b, 255 })
static const Clay_Color COL_BG      = RGB(0x1F, 0x1F, 0x1F);
static const Clay_Color COL_PANEL   = RGB(0x18, 0x18, 0x18);
static const Clay_Color COL_BORDER  = RGB(0x2B, 0x2B, 0x2B);
static const Clay_Color COL_TEXT    = RGB(0xCC, 0xCC, 0xCC);
static const Clay_Color COL_MUTED   = RGB(0x8B, 0x8B, 0x8B);
static const Clay_Color COL_DIM     = RGB(0x5E, 0x5E, 0x5E);
static const Clay_Color COL_ACCENT  = RGB(0x00, 0x78, 0xD4);
static const Clay_Color COL_ACTIVE  = RGB(0x37, 0x37, 0x3D);
static const Clay_Color COL_HOVER   = RGB(0x2A, 0x2D, 0x2E);
static const Clay_Color COL_WARN    = RGB(0xCC, 0xA7, 0x00);
static const Clay_Color COL_OK      = RGB(0x4E, 0xC9, 0x8B);
static const Clay_Color COL_ERR     = RGB(0xE5, 0x73, 0x73);

#define SPLITTER_W 5
#define MAX_DOCS EXPLORER_MAX_DOCS

/* Revisions listed in History (newest first). */
#define REV_SHOW 100
/* Notes / canon entries listed in the Explorer (most recent first). */
#define REC_SHOW 200
#define ENT_SHOW 60   /* entities listed in the Explorer */
#define ENT_ROWS 60   /* relationships and mentions shown for the open one */

typedef struct {
    FrameViewModel frame;
    Editor *editor;
    SFont fonts[FONT_COUNT];
    SFont *font_ptrs[FONT_COUNT];
    float explorer_w, observer_w, chat_w;
    bool observer_open;
    int dragging;
    int focus;
    Rectangle editor_rect, chat_list_rect, chat_input_rect;
    char status[512];
    double status_time;
    bool close_armed;
    double close_armed_at;
    bool chat_discard_armed;
    double chat_discard_at;
    ChatHistoryJob *history_job;
    uint64_t history_ticket, history_session, history_version;
    bool history_pending, history_restore, history_send_ready;
    char history_project[PATH_MAX], history_id[40], history_title[160];
    char history_from_project[PATH_MAX], history_from_id[40];
    ChatWindowRequest history_request;
    uint64_t history_attachment_revision;
    StoreProjectIdentity history_identity;
    atomic_bool history_hold; /* script-only deterministic stalled-reader gate */

    /* Model and chat */
    LlmServer *llm;
    LlmServer *embed;             /* the embedding model (semantic retrieval) */
    SemanticClient *semantic;
    MemoryJob *memory_job;
    bool memory_experiment, memory_sources_open;
    char memory_bound_project[PATH_MAX], memory_bound_chat[40], memory_label[280];
    uint64_t memory_bound_generation;
    MemoryDocumentName memory_names[MEMORY_DOCUMENTS_MAX];
    size_t memory_nnames;
    Editor *memory_sources_editor;
    Rectangle memory_sources_close;
    union {
        ChatController chat_controller;
        struct {
            ChatSession *chat;
            ChatView *chat_view;
            char attached[MAX_ATTACH][256];
            size_t nattached;
            bool attach_touched;
            uint64_t attachment_revision;
        };
    };
    /* A conversation belongs to the project it started in, even if the author
     * opens a document elsewhere while a reply is still streaming. The
     * anonymous view keeps call sites readable while ProjectSession owns it. */
    union {
        ProjectSession session;
        struct {
            char project_dir[PATH_MAX];
            char chat_project[PATH_MAX];
            bool clear_chat_when_idle;
            char chat_id[40];
            char chat_title[160];
            uint64_t session_generation;
        };
    };

    /* Reviewed AI edits */
    union {
        ReviewedEditController reviewed_edit;
        struct {
            ReviewView *review;
            Editor *review_editor;
            bool edit_mode;              /* next chat message is an edit instruction */
            char *edit_text;             /* the selected passage (its position is the editor's anchor) */
            char edit_banner[240];
            PendingReviewedEdit pending;
            ReviewedEditProposal proposal;
        };
    };
    Rectangle review_rect;
    Revision *revs;
    size_t nrevs;
    char rev_labels[REV_SHOW][64];

    /* Notes and canon (SOW §4.4) */
    Record *notes, *canon;
    size_t nnotes, ncanon;
    char note_labels[REC_SHOW][72];
    char canon_labels[REC_SHOW][72];
    /* The entity graph (store projects): the list, and the open entity. */
    EntityRow *ents;
    size_t nents;
    char ent_labels[ENT_SHOW][96];
    char ent_open[40];
    EntityDetail ent;
    char ent_row_labels[ENT_ROWS][160];
    /* What each entity row held when it was laid out: clicks act on these,
     * never on indices into lists that may have been refreshed since. */
    char drawn_ent[ENT_SHOW][40];
    char drawn_row_finding[ENT_ROWS][40], drawn_row_other[ENT_ROWS][40];
    /* Building a project's graph from its findings (once per project, in
     * the background: thousands of findings take seconds). */
    EntityBackfillJob graph_job;
    char graph_tried[PATH_MAX]; /* one attempt per project per session */
    RecordView *record;
    Rectangle record_rect;
    char canon_finding[40];      /* the open canon draft came from this finding */

    /* Observer (SOW §4.3) */
    Observer *observer;
    FindingsView *findings;
    Record *flist;
    size_t nflist;
    uint64_t findings_version;
    Rectangle findings_rect;
    char observer_label[220];
    char observer_button[40];

    /* Projects and documents (SOW §4.1) */
    PromptView *prompt;
    MenuView menu;
    char rename_from[256];        /* document being renamed */
    char prompt_project[PATH_MAX]; /* the project the prompt was opened in */
    char menu_doc[256];           /* the document a context menu is for (by name) */
    char menu_chat[40];           /* the chat a context menu is for */
    char menu_names[MENU_MAX_ITEMS][256]; /* the attach menu's documents, by item */

    /* Documents attached to the current chat (SOW §4.1: by reference, not copied) */
    Rectangle attach_chip[MAX_ATTACH], attach_x[MAX_ATTACH], attach_add;
    RecentStore *recent_store;
    RecentProject recents[RECENT_MAX];
    size_t nrecents;
    Rectangle welcome_btn[3], recent_rect[RECENT_MAX], empty_btn[2];

    /* The project store (MariaDB; docs/STORE-001-design.md) */
    bool store_wanted;            /* use the store (off for UI scripts unless SCRIBODO_STORE=db) */
    char store_why[256];          /* why it's offline */
    StoreManager *store;
    ServiceSupervisor services;
    /* A backup running in the background (Explorer: PROJECT). */
    BackupJob backup;
    char backup_label[160];
    double store_next_probe;      /* while offline: when to probe again */
    bool store_just_moved;        /* the project was just moved into the store (for the status) */
    bool services_started;
    bool record_close_armed;     /* a discard of uncommitted canon text was asked for... */
    double record_close_armed_at; /* ...at this time (a second press within 5 s confirms) */

    /* Immutable document/chat rows currently represented by Clay ids. */
    union {
        ExplorerSnapshot explorer;
        struct {
            char *docs[MAX_DOCS];
            size_t ndocs;
            ChatSummary *chats;
            char (*chat_labels)[200];
            size_t nchats;
            uint64_t explorer_generation;
            uint64_t explorer_project_generation;
        };
    };
    uint64_t drawn_explorer_generation;
    /* The chat model's settings, and the panel that edits them. */
    ModelSettings model, model_edit;
    bool settings_open;
    Rectangle set_minus[MODEL_SETTING_COUNT], set_plus[MODEL_SETTING_COUNT], set_apply, set_cancel, set_defaults;
    int model_menu_settings; /* which item of the model menu opens the panel */
    bool ctx_pending;        /* a context change waiting for the reply to finish */
    long long mem_for_model; /* the panel's readout: memory the model could have (bytes) */
    char mem_msg[400];       /* startup: the context was reduced for memory */
    /* Who is who (stored names) and the two-step "Names..." prompt. */
    Profile profile;
    int names_step;         /* 0 none, 1 author prompt, 2 assistant prompt to open, 3 assistant prompt */
    char names_author[64];  /* the author step's answer */
    /* Names noticed in chat, offered for confirmation (never saved unasked). */
    NameDetectionJob names_job;
    NameGuess names_guess;
    char suggest_author[64], suggest_assistant[64];
    char declined_author[64], declined_assistant[64];
    char suggest_text[300];
    double lists_refreshed;
    bool lists_dirty; /* refreshed between click handling and layout (stable hit-testing) */

    /* Strings referenced by Clay must outlive the frame's render pass. */
    char doc_label[512];
    char status_left[256];
    char model_label[512];
    char store_label[160];
    /* The optional remote model: configured or not; selected only by the
     * author, each session starting local (SOW: no automatic fallback). */
    RemoteConfig remote;
    bool remote_ok;
    char remote_why[200];
    char remote_badge[200];
    int store_problem;          /* STORE_PROBLEM_COPY / _MISSING, or 0 */
    char store_original[PATH_MAX];
    Clay_Color store_color;
    ProjectStoreInspection store_inspection;
    char chat_header[200];
    Clay_Color model_color;
    bool window_ready;
    bool backup_initialized;
    bool services_initialized;
    bool graph_job_initialized;
    bool names_job_initialized;
    void *clay_buf;
    Script *script;
} App;

static void store_adopt_project(App *app);

static void set_status(App *app, const char *msg)
{
    snprintf(app->status, sizeof(app->status), "%s", msg);
    app->status_time = GetTime();
}

/* A storage failure, explained: while the project store is offline the
 * file name in `fallback` would mislead. */
static const char *store_says(App *app, const char *fallback)
{
    ProjectStoreInspection inspection = project_store_inspect(app->store, app->project_dir);
    if (!(inspection.capabilities & PROJECT_STORE_CAP_STRUCTURED_WRITE)) {
        if (inspection.blocked_by == PROJECT_STORE_BLOCK_COPY)
            return "This folder is a copy of another project: this wasn't saved. Make it its own project first (Explorer).";
        if (inspection.blocked_by == PROJECT_STORE_BLOCK_MISSING)
            return "This project is missing from the project store: this wasn't saved. Restore it first (Explorer).";
        if (inspection.blocked_by == PROJECT_STORE_BLOCK_TRANSITION)
            return "The project is being moved into the project store: this wasn't saved. Try again in a moment.";
        return "The project store is offline: this wasn't saved. Your documents are unaffected; try again once it's back.";
    }
    if (inspection.capabilities & PROJECT_STORE_CAP_LINKED_DOCUMENTS)
        return "The project store couldn't save that (see its log)";
    return fallback;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- project lists ------------------------------------------------------------- */

static void refresh_records(App *app);
static void refresh_findings(App *app);
static void refresh_entities(App *app);

static bool refresh_lists(App *app)
{
    app->lists_refreshed = GetTime();
    ExplorerSnapshot next;
    if (!explorer_snapshot_build(&next, app->store, app->project_dir, app->explorer.generation + 1, app->session.generation))
        return false;
    explorer_snapshot_destroy(&app->explorer);
    app->explorer = next;
    free(app->revs);
    app->revs = NULL;
    app->nrevs = 0;
    if (editor_path(app->editor) &&
        !revisions_list_checked(app->store, app->project_dir, editor_path(app->editor), &app->revs, &app->nrevs))
        return false;
    static const struct { const char *origin, *label; } names[] = {
        { "save", "Saved" }, { "ai-edit", "AI edit" }, { "before-ai-edit", "Before AI edit" },
        { "restore", "Restored" }, { "before-restore", "Before restore" },
        { "source", "Quoted" },
    };
    for (size_t i = 0; i < app->nrevs && i < REV_SHOW; i++) {
        const char *lbl = app->revs[i].origin;
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            if (!strcmp(names[k].origin, app->revs[i].origin)) lbl = names[k].label;
        struct tm tmv;
        localtime_r(&app->revs[i].ts, &tmv);
        char when[32];
        strftime(when, sizeof(when), "%b %d %H:%M", &tmv);
        snprintf(app->rev_labels[i], sizeof(app->rev_labels[i]), "%s \xC2\xB7 %s", when, lbl);
    }
    refresh_records(app);
    refresh_findings(app); /* against the current canon; also refreshes the entities */
    return true;
}

/* First line of a record, shortened, as an Explorer label. */
static void record_label(char *out, size_t outlen, const char *prefix, const char *content)
{
    char line[64];
    size_t n = strcspn(content, "\n");
    if (n > 52) {
        n = 52;
        while (n > 0 && ((unsigned char)content[n] & 0xC0) == 0x80) n--; /* UTF-8 boundary */
    }
    snprintf(line, sizeof(line), "%.*s%s", (int)n, content, strlen(content) > n ? "\xE2\x80\xA6" : "");
    snprintf(out, outlen, "%s%s", prefix, line[0] ? line : "(empty)");
}

static void refresh_entities(App *app)
{
    entity_list_free(app->ents, app->nents);
    app->ents = NULL;
    app->nents = 0;
    if (!app->project_dir[0] || !project_store_entities_available(app->store, app->project_dir)) {
        entity_detail_free(&app->ent);
        app->ent_open[0] = '\0';
        return;
    }
    /* Findings from before the graph existed are folded in once per
     * project, off the UI thread (red-team 009). */
    if (!entity_backfill_job_busy(&app->graph_job) && strcmp(app->graph_tried, app->project_dir) != 0 &&
        !project_store_entities_built(app->store, app->project_dir)) {
        snprintf(app->graph_tried, sizeof(app->graph_tried), "%s", app->project_dir);
        (void)entity_backfill_job_start(&app->graph_job, app->project_dir);
    }
    app->nents = project_store_entity_list(app->store, app->project_dir, &app->ents);
    for (size_t i = 0; i < app->nents && i < ENT_SHOW; i++) {
        char name[64];
        utf8_clip(name, sizeof(name), app->ents[i].name, 48);
        snprintf(app->ent_labels[i], sizeof(app->ent_labels[i]), "%s \xC2\xB7 %d", name, app->ents[i].mentions);
    }
    entity_detail_free(&app->ent);
    if (app->ent_open[0] && !project_store_entity_detail(app->store, app->project_dir, app->ent_open, &app->ent)) app->ent_open[0] = '\0';
    for (size_t i = 0; i < app->ent.nrows && i < ENT_ROWS; i++) {
        const EntityDetailRow *r = &app->ent.rows[i];
        char a[100], b[80];
        if (r->is_link) {
            utf8_clip(a, sizeof(a), r->relation, 40);
            utf8_clip(b, sizeof(b), r->other_name, 40);
            if (r->outgoing) snprintf(app->ent_row_labels[i], sizeof(app->ent_row_labels[i]), "  %s \xE2\x86\x92 %s", a, b);
            else snprintf(app->ent_row_labels[i], sizeof(app->ent_row_labels[i]), "  %s %s \xE2\x86\x92 this", b, a);
        } else {
            utf8_clip(a, sizeof(a), r->text, 60);
            snprintf(app->ent_row_labels[i], sizeof(app->ent_row_labels[i]), "  \xC2\xB7 %s%s", a, strlen(r->text) > strlen(a) ? "\xE2\x80\xA6" : "");
        }
    }
}

/* Open (or close, if it's open) an entity in the Explorer. */
static void toggle_entity(App *app, const char *id)
{
    if (!strcmp(app->ent_open, id)) app->ent_open[0] = '\0';
    else snprintf(app->ent_open, sizeof(app->ent_open), "%s", id);
    refresh_entities(app);
}

static void open_source(App *app, const SourceRef *s);

/* The passage a finding came from: a relationship's evidence, or a
 * mention. */
static void open_finding_source(App *app, const char *finding_id)
{
    Record *r = NULL;
    size_t n = 0;
    if (!records_list_checked(app->store, app->project_dir, REC_FINDING, &r, &n)) {
        set_status(app, "Couldn't load findings from the project store");
        return;
    }
    SourceRef *copy = NULL;
    for (size_t i = 0; i < n && !copy; i++)
        if (!strcmp(r[i].id, finding_id)) {
            const RecordVersion *v = record_current(&r[i]);
            if (v->nsources) copy = records_copy_sources(v->sources, 1);
        }
    records_free(r, n);
    if (copy) open_source(app, copy);
    else set_status(app, "That finding has no source passage");
    records_free_sources(copy, copy ? 1 : 0);
}

static void refresh_records(App *app)
{
    records_free(app->notes, app->nnotes);
    records_free(app->canon, app->ncanon);
    app->notes = app->canon = NULL;
    app->nnotes = app->ncanon = 0;
    if (!app->project_dir[0]) return;
    bool notes_ok = records_list_checked(app->store, app->project_dir, REC_NOTE, &app->notes, &app->nnotes);
    bool canon_ok = records_list_checked(app->store, app->project_dir, REC_CANON, &app->canon, &app->ncanon);
    if (!notes_ok || !canon_ok) {
        records_free(app->notes, app->nnotes);
        records_free(app->canon, app->ncanon);
        app->notes = app->canon = NULL;
        app->nnotes = app->ncanon = 0;
        set_status(app, "Couldn't load records from the project store");
    }
    for (size_t i = 0; i < app->nnotes && i < REC_SHOW; i++) {
        const RecordVersion *v = record_current(&app->notes[i]);
        if (!strcmp(v->status, "deleted")) app->note_labels[i][0] = '\0'; /* hidden; still in the log */
        else record_label(app->note_labels[i], sizeof(app->note_labels[i]), "", v->content);
    }
    for (size_t i = 0; i < app->ncanon && i < REC_SHOW; i++) {
        const RecordVersion *v = record_current(&app->canon[i]);
        record_label(app->canon_labels[i], sizeof(app->canon_labels[i]),
                     !strcmp(v->status, "retracted") ? "(retracted) " : "", v->content);
    }
}

static void project_of(const char *doc_path, char *out, size_t outlen);

static void cancel_history(App *app)
{
    if (app->history_job) chat_history_job_cancel(app->history_job);
    app->history_pending = false;
    app->history_ticket++;
}

/* Routing hint only: a stored project always goes through the worker, including
 * paused/malformed markers. Actual authorization stays in ProjectStore there. */
static bool database_history_project(const char *project)
{
    char marker[PATH_MAX + 64];
    struct stat st;
    snprintf(marker, sizeof(marker), "%s/.scribodo/store.json", project);
    return lstat(marker, &st) == 0;
}

static bool history_read_busy(App *app)
{
    return app->history_pending || chat_history_job_busy(app->history_job);
}

static bool memory_controls_pending(App *app)
{
    MemoryJobStatus status;
    return app->memory_job && memory_job_status(app->memory_job, &status) && status.controls_pending;
}

static void memory_editor_will_change(void *context)
{
    App *app = context;
    const char *path = editor_path(app->editor);
    if (path && chat_controller_has_attachment(&app->chat_controller, base_name(path)))
        chat_memory_attachments_changed(app->chat);
}

static void memory_document_names(App *app)
{
    MemoryDocumentName names[MEMORY_DOCUMENTS_MAX] = {0};
    size_t count = app->nattached < MEMORY_DOCUMENTS_MAX ? app->nattached : MEMORY_DOCUMENTS_MAX;
    const char *open = editor_path(app->editor);
    for (size_t i = 0; i < count; i++) {
        snprintf(names[i].name, sizeof(names[i].name), "%s", app->attached[i]);
        names[i].dirty = open && !strcmp(base_name(open), app->attached[i]) && editor_is_dirty(app->editor);
    }
    if (count != app->memory_nnames || memcmp(names, app->memory_names, sizeof(names))) {
        memcpy(app->memory_names, names, sizeof(names));
        app->memory_nnames = count;
        chat_memory_attachments_changed(app->chat);
    }
}

static void memory_tick(App *app)
{
    memory_document_names(app);
    if (!app->memory_job) return;
    StoreProjectIdentity identity;
    bool bound = app->chat_id[0] && !strcmp(app->chat_project, app->project_dir) &&
                 store_project_identity(app->store, app->chat_project, &identity);
    if (!bound) {
        if (app->memory_bound_chat[0]) memory_job_clear(app->memory_job);
        app->memory_bound_chat[0] = app->memory_bound_project[0] = 0;
        snprintf(app->memory_label, sizeof(app->memory_label), "Memory (experimental): disabled/unavailable");
        return;
    }
    if (strcmp(app->memory_bound_project, app->chat_project) || strcmp(app->memory_bound_chat, app->chat_id) ||
        app->memory_bound_generation != identity.generation) {
        if (memory_job_select(app->memory_job, app->chat_project, app->chat_id, MEMORY_JOB_RESUME)) {
            snprintf(app->memory_bound_project, sizeof(app->memory_bound_project), "%s", app->chat_project);
            snprintf(app->memory_bound_chat, sizeof(app->memory_bound_chat), "%s", app->chat_id);
            app->memory_bound_generation = identity.generation;
        }
    }
    memory_job_documents(app->memory_job, app->chat_project, app->chat_id, app->memory_names,
        app->memory_nnames, chat_memory_attachment_version(app->chat));
    memory_job_pause(app->memory_job, chat_busy(app->chat) || chat_remote_selected(app->chat) || history_read_busy(app));
    MemoryJobStatus status;
    if (memory_job_status(app->memory_job, &status))
        snprintf(app->memory_label, sizeof(app->memory_label), "Memory (chat/saved attachments): %.220s", status.why);
}

static void memory_control(App *app, MemoryJobAction action)
{
    if (!app->memory_job || strcmp(app->menu_chat, app->chat_id) || strcmp(app->chat_project, app->project_dir)) {
        set_status(app, "Open that stored conversation before changing local memory"); return;
    }
    if (chat_busy(app->chat) || app->history_pending) {
        set_status(app, "Stop the reply and finish history navigation before changing memory"); return;
    }
    StoreProjectIdentity identity;
    if (!store_project_identity(app->store, app->chat_project, &identity) ||
        !memory_job_select(app->memory_job, app->chat_project, app->chat_id, action)) {
        set_status(app, "Could not change memory; no file fallback was used"); return;
    }
    snprintf(app->memory_bound_project, sizeof(app->memory_bound_project), "%s", app->chat_project);
    snprintf(app->memory_bound_chat, sizeof(app->memory_bound_chat), "%s", app->chat_id);
    app->memory_bound_generation = identity.generation;
    set_status(app, action == MEMORY_JOB_DISABLE ? "Disabling local memory; old replies keep their as-of sources"
        : action == MEMORY_JOB_REBUILD ? "Rebuilding local memory from original chat sources; coverage remains partial"
        : "Enabling experimental local memory for this conversation only; originals stay intact");
}

static void show_memory_sources(App *app, long index)
{
    chat_lock(app->chat);
    const ChatMessage *message = index >= 0 ? chat_message(app->chat, (size_t)index) : NULL;
    char *sources = message && message->memory_sources ? strdup(message->memory_sources) : NULL;
    chat_unlock(app->chat);
    if (!sources) { set_status(app, "No retrieval sources were recorded for that reply"); return; }
    if (editor_set_text_utf8(app->memory_sources_editor, sources)) app->memory_sources_open = true;
    else set_status(app, "Could not open the source snapshot");
    free(sources);
}

static void draw_memory_sources(App *app)
{
    float width = fminf(900, GetScreenWidth() - 60.0f), height = fminf(700, GetScreenHeight() - 60.0f);
    Rectangle box = { (GetScreenWidth() - width) / 2, (GetScreenHeight() - height) / 2, width, height };
    DrawRectangleRec(box, (Color){0x28, 0x28, 0x28, 0xff});
    const char *title = "As-of retrieval sources - read only; not canon or proof of exhaustive recall";
    sfont_draw_utf8(&app->fonts[FONT_UI], title, (int)strlen(title), box.x + 14, box.y + 12, (Color){0xcc,0xcc,0xcc,0xff});
    app->memory_sources_close = (Rectangle){ box.x + box.width - 90, box.y + box.height - 40, 76, 28 };
    Rectangle content = { box.x + 8, box.y + 38, box.width - 16, box.height - 88 };
    editor_update(app->memory_sources_editor, content, true);
    editor_draw(app->memory_sources_editor, content, true);
    if (GuiButton(app->memory_sources_close, "Close") || IsKeyPressed(KEY_ESCAPE)) app->memory_sources_open = false;
}

static void set_project(App *app, const char *real)
{
    if (strcmp(real, app->project_dir) != 0) {
        /* A prompt or menu was for the old project's files. */
        if (app->prompt) prompt_close(app->prompt);
        app->menu.active = false;
        (void)project_session_set_project(&app->session, real);
        cancel_history(app);
        ExplorerSnapshot empty;
        explorer_snapshot_init(&empty);
        empty.generation = app->explorer.generation + 1;
        empty.project_generation = app->session.generation;
        explorer_snapshot_destroy(&app->explorer);
        app->explorer = empty;
        /* Into the store first, so the observer never starts reading (and
         * writing) the files an import is about to set aside. */
        store_adopt_project(app);
        if (app->observer) observer_set_project(app->observer, real);
        app->findings_version = UINT64_MAX; /* re-list */
        /* A different project has different chats: start fresh (after the
         * current reply finishes, so it is saved with its own project). */
        if (app->chat) app->clear_chat_when_idle = true;
    }
    app->lists_dirty = true;
}

static void store_try_later(App *app)
{
    if (service_supervisor_busy(&app->services)) return;
    char tools[PATH_MAX];
    snprintf(tools, sizeof(tools), "%s../tools", GetApplicationDirectory());
    bool start = !app->services_started && !getenv("SCRIBODO_SCRIPT");
    (void)service_supervisor_start(&app->services, tools, start);
}

static void store_begin(App *app)
{
    app->store_why[0] = '\0';
    if (store_start(app->store, getenv("SCRIBODO_DB_NAME"), app->store_why, sizeof(app->store_why))) return;
    store_try_later(app); /* start the services / keep probing in the background */
}

/* A project opened while the store is up is moved into it once. */
static void store_adopt_project(App *app)
{
    if (!app->project_dir[0] || !store_online(app->store) || !store_needs_import(app->store, app->project_dir)) return;
    char why[256] = "";
    if (store_import(app->store, app->project_dir, why, sizeof(why))) {
        app->store_just_moved = true;
        set_status(app, "This project is now kept in the project store (its old files are in .scribodo/legacy-\xE2\x80\xA6)");
        app->lists_dirty = true;
        app->findings_version = UINT64_MAX;
        if (app->observer) observer_poke(app->observer);
    } else {
        char msg[400];
        snprintf(msg, sizeof(msg), "%s", why);
        set_status(app, msg);
    }
}

/* The two ways out of a paused project that isn't offline (red-team 008). */
static void store_resolve_problem(App *app)
{
    cancel_history(app);
    char why[256] = "";
    int problem = store_problem(app->store, app->project_dir, NULL, 0);
    bool ok = problem == STORE_PROBLEM_COPY ? store_fork(app->store, app->project_dir, why, sizeof(why))
            : problem == STORE_PROBLEM_MISSING ? store_restore(app->store, app->project_dir, why, sizeof(why))
                                                : false;
    if (ok)
        set_status(app, why[0] ? why
                        : problem == STORE_PROBLEM_COPY ? "This copy is now its own project (the original is unchanged)"
                                                        : "The project was restored into the project store from its saved files");
    else
        set_status(app, why[0] ? why : "Nothing to do");
    app->lists_dirty = true;
    app->findings_version = UINT64_MAX;
    if (app->observer) {
        observer_set_project(app->observer, app->project_dir);
        observer_poke(app->observer);
    }
}

/* ---- backups (SOW §7) ------------------------------------------------------------ */

static void open_project(App *app, const char *dir);

static void tools_dir(char *out, size_t outlen) { snprintf(out, outlen, "%s../tools", GetApplicationDirectory()); }

static void begin_backup(App *app)
{
    if (!app->project_dir[0]) {
        set_status(app, "Open a project to back it up");
        return;
    }
    if (backup_job_busy(&app->backup)) {
        set_status(app, "A backup is already running");
        return;
    }
    if (editor_is_dirty(app->editor)) {
        set_status(app, "Save the document first: a backup takes what's on disk");
        return;
    }
    char tools[PATH_MAX];
    tools_dir(tools, sizeof(tools));
    if (!backup_job_start(&app->backup, tools, app->project_dir)) {
        set_status(app, "Couldn't start the backup");
        return;
    }
    set_status(app, "Backing up this project\xE2\x80\xA6");
}

static void backup_poll(App *app)
{
    bool ok;
    char dest[PATH_MAX + 64], why[400];
    if (!backup_job_poll(&app->backup, &ok, dest, sizeof(dest), why, sizeof(why))) return;
    char msg[800];
    if (ok) {
        snprintf(msg, sizeof(msg), "Backed up to %s", dest);
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char when[32];
        strftime(when, sizeof(when), "%H:%M", &tmv);
        snprintf(app->backup_label, sizeof(app->backup_label), "Last backup: today %s", when);
    } else {
        snprintf(msg, sizeof(msg), "%s", why[0] ? why : "The backup failed");
    }
    set_status(app, msg);
}

static void begin_restore(App *app)
{
    char chosen[PATH_MAX];
    char def[PATH_MAX];
    backup_default_dir(def, sizeof(def));
    if (!dialogs_choose_folder("Choose a backup to restore (it opens as a separate project; nothing is overwritten)", "Restore",
                               false, chosen, sizeof(chosen)))
        return;
    /* Next to the current project (or in the backups' folder). */
    char parent[PATH_MAX];
    if (app->project_dir[0]) {
        snprintf(parent, sizeof(parent), "%s", app->project_dir);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) *sl = '\0';
    } else {
        snprintf(parent, sizeof(parent), "%s", def);
    }
    char tools[PATH_MAX], restored[PATH_MAX + 400], why[400];
    tools_dir(tools, sizeof(tools));
    set_status(app, "Restoring\xE2\x80\xA6");
    if (!backup_restore_copy(tools, chosen, parent, restored, sizeof(restored), why, sizeof(why))) {
        set_status(app, why);
        return;
    }
    open_project(app, restored);
    char msg[900];
    snprintf(msg, sizeof(msg), "Restored as \xE2\x80\x9C%s\xE2\x80\x9D, a separate project (the original is unchanged)", base_name(restored));
    set_status(app, msg);
}

/* ---- which chats the observer reads ----------------------------------------------- */

static const ChatSummary *find_chat(const App *app, const char *id)
{
    for (size_t i = 0; id && id[0] && i < app->nchats; i++)
        if (!strcmp(app->chats[i].id, id)) return &app->chats[i];
    return NULL;
}

/* The open chat has been saved (the setting belongs to a stored chat). */
static bool open_chat_stored(const App *app) { return app->project_dir[0] && find_chat(app, app->chat_id) != NULL; }

static void set_chat_observer(App *app, const char *id, bool read)
{
    char cid[40];
    snprintf(cid, sizeof(cid), "%s", id); /* the list is refreshed below */
    if (!chat_store_set_observer(app->store, app->project_dir, cid, read)) {
        set_status(app, store_says(app, "Couldn't save that setting for the chat"));
        return;
    }
    set_status(app, read ? "The observer reads this chat again (from where it left off)"
                         : "The observer won't read this chat (findings it already made stay; dismiss them if you like)");
    app->lists_dirty = true; /* refreshed after this frame is drawn (rows point into the lists) */
    if (read && app->observer) observer_poke(app->observer);
}

static void set_project_from(App *app, const char *doc_path)
{
    char real[PATH_MAX];
    project_of(doc_path, real, sizeof(real));
    set_project(app, real);
}

/* ---- reviewed AI edits ------------------------------------------------------------- */

static void clear_proposal(App *app)
{
    reviewed_edit_controller_clear_proposal(&app->reviewed_edit);
}

static void clear_pending(App *app)
{
    reviewed_edit_controller_clear_pending(&app->reviewed_edit);
}

static void leave_edit_mode(App *app)
{
    reviewed_edit_controller_leave_mode(&app->reviewed_edit);
}

/* Show a proposal for review. Takes ownership of nothing; copies. */
static void propose(App *app, size_t start, size_t len, const char *original, const char *replacement, const char *note,
                    bool truncated)
{
    ReviewedEditResult result = reviewed_edit_controller_propose(&app->reviewed_edit, start, len, original, replacement,
                                                                 note, truncated);
    if (result == REVIEWED_EDIT_EMPTY_REPLY)
        set_status(app, "The model returned no text for this edit; nothing was proposed");
    else if (result == REVIEWED_EDIT_OUT_OF_MEMORY)
        set_status(app, "Could not show the proposed edit");
}

/* Returns false (and says so) if the revision could not be written. */
static bool record_revision(App *app, const char *origin, const char *note)
{
    if (!app->project_dir[0] || !editor_path(app->editor)) return true; /* no history to keep */
    char *text = editor_text_utf8(app->editor);
    bool ok = text && revisions_record(app->store, app->project_dir, editor_path(app->editor), text, origin, note, NULL);
    free(text);
    if (!ok) set_status(app, store_says(app, "Could not record a revision in .scribodo/revisions"));
    if (ok && !strcmp(origin, "save")) chat_memory_attachments_changed(app->chat);
    app->lists_dirty = true;
    return ok;
}

static bool record_revision_for_edit(void *context, const char *origin, const char *note)
{
    return record_revision(context, origin, note);
}

static void accept_proposal(App *app)
{
    ReviewedEditResult result = reviewed_edit_controller_accept(&app->reviewed_edit, record_revision_for_edit, app);
    if (result == REVIEWED_EDIT_WRONG_DOCUMENT) set_status(app, "That edit was for another document");
    else if (result == REVIEWED_EDIT_NOT_EDITABLE) set_status(app, "This document is read-only; the edit can't be applied");
    else if (result == REVIEWED_EDIT_CHANGED)
        set_status(app, "The passage changed since this edit was proposed. Select it again and request a new edit.");
    else if (result == REVIEWED_EDIT_HISTORY_BEFORE_FAILED)
        set_status(app, "The edit was not applied: could not save the current text to History (.scribodo/revisions)");
    else if (result == REVIEWED_EDIT_REPLACE_FAILED) set_status(app, "Could not apply the edit");
    else if (result == REVIEWED_EDIT_OK) {
        set_status(app, "Edit applied \xC2\xB7 \xE2\x8C\x98Z undoes it \xC2\xB7 the file isn't saved until you press \xE2\x8C\x98S");
        app->focus = FOCUS_DOC;
    } else if (result == REVIEWED_EDIT_HISTORY_AFTER_FAILED) {
        set_status(app, "Edit applied, but it could not be recorded in History \xC2\xB7 \xE2\x8C\x98Z undoes it");
        app->focus = FOCUS_DOC;
    }
}

static void reject_proposal(App *app)
{
    clear_proposal(app);
    set_status(app, "Edit rejected \xE2\x80\x94 the document is unchanged");
}

/* ⌘K: the next chat message becomes an edit request for the selection. */
static void begin_edit(App *app)
{
    ReviewedEditResult result = reviewed_edit_controller_begin(&app->reviewed_edit);
    if (result == REVIEWED_EDIT_NOT_EDITABLE)
        set_status(app, "Open an editable document to request edits");
    else if (result == REVIEWED_EDIT_NO_SELECTION)
        set_status(app, "Select the passage you want edited, then press \xE2\x8C\x98K");
    else if (result == REVIEWED_EDIT_BUSY)
        set_status(app, "Wait for the current edit to finish (or stop it) before requesting another");
    else if (result == REVIEWED_EDIT_OK) app->focus = FOCUS_CHAT;
}

static void restore_revision(App *app, const Revision *r)
{
    if (!editor_path(app->editor)) return;
    if (editor_is_read_only(app->editor)) {
        set_status(app, "This document is read-only");
        return;
    }
    char *text = revisions_load(app->store, app->project_dir, editor_path(app->editor), r);
    if (!text) {
        set_status(app, "That revision could not be read (missing or damaged)");
        return;
    }
    char *now = editor_text_utf8(app->editor);
    if (now && !strcmp(now, text)) {
        set_status(app, "That revision is already the current text");
    } else {
        if (!record_revision(app, "before-restore", r->file)) {
            set_status(app, "Not restored: could not save the current text to History (.scribodo/revisions)");
        } else if (clear_proposal(app), editor_replace_range_utf8(app->editor, 0, editor_length(app->editor), text)) {
            if (record_revision(app, "restore", r->file))
                set_status(app, "Revision restored \xC2\xB7 \xE2\x8C\x98Z undoes it \xC2\xB7 not saved to the file yet");
            else
                set_status(app, "Revision restored, but it could not be recorded in History \xC2\xB7 \xE2\x8C\x98Z undoes it");
        } else {
            set_status(app, "Could not restore that revision");
        }
    }
    free(now);
    free(text);
}

/* ---- documents ------------------------------------------------------------------ */

static bool close_record(App *app, bool explicit_close);
static void attach_reset(App *app);
static bool listed_doc(App *app, const char *name);
static bool attach(App *app, const char *name);
static void open_project(App *app, const char *dir);

/* The project folder a document belongs to (its directory, resolved). */
static void project_of(const char *doc_path, char *out, size_t outlen)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", doc_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    char real[PATH_MAX];
    snprintf(out, outlen, "%s", realpath(dir, real) ? real : dir);
}

static void open_path(App *app, const char *path)
{
    char msg[512];
    /* An open note or canon entry belongs to its project: settle it before
     * switching to another one. */
    if (record_view_active(app->record)) {
        char proj[PATH_MAX];
        project_of(path, proj, sizeof(proj));
        if (strcmp(proj, app->project_dir) != 0 && !close_record(app, false)) return;
    }
    if (editor_load_file(app->editor, path, msg, sizeof(msg))) {
        /* Edits and proposals belong to the document they were made for. */
        clear_proposal(app);
        clear_pending(app);
        leave_edit_mode(app);
        set_status(app, msg[0] ? msg : "Opened");
        set_project_from(app, path);
        recent_store_touch(app->recent_store, app->project_dir, base_name(path));
        /* A conversation that hasn't started yet follows the open document. */
        chat_lock(app->chat);
        size_t nmsg = chat_count(app->chat);
        chat_unlock(app->chat);
        if (nmsg == 0 && !app->clear_chat_when_idle && !app->attach_touched) attach_reset(app);
    } else {
        set_status(app, msg);
    }
}

/* File-drop handler (also used by the Explorer and UI scripts). */
static void drop_path(App *app, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) { /* a dropped folder opens as a project */
        open_project(app, path);
        return;
    }
    if (editor_is_dirty(app->editor)) set_status(app, "Save the current document before opening another");
    else open_path(app, path);
}

/* ---- projects and documents (SOW §4.1) --------------------------------------- */

/* Leave the current document and project state cleanly; false (with a
 * status) if something unsaved is in the way. */
static bool can_switch(App *app)
{
    if (editor_is_dirty(app->editor)) {
        set_status(app, "Save the current document first");
        return false;
    }
    return close_record(app, false);
}

/* Open a folder as the project, with no document open (or `doc` if given
 * and present). */
static void open_project(App *app, const char *dir)
{
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(dir, real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_status(app, "That folder can't be opened");
        return;
    }
    if (!can_switch(app)) return;
    clear_proposal(app);
    clear_pending(app);
    leave_edit_mode(app);
    editor_close(app->editor);
    set_project(app, real);
    recent_store_touch(app->recent_store, real, NULL);
    app->nrecents = recent_store_load(app->recent_store, app->recents, RECENT_MAX);
    char msg[PATH_MAX + 160];
    snprintf(msg, sizeof(msg), "Opened %s%s", base_name(real),
             app->store_just_moved ? " \xC2\xB7 it is now kept in the project store (old files are in .scribodo/legacy-\xE2\x80\xA6)" : "");
    app->store_just_moved = false;
    set_status(app, msg);
    app->focus = FOCUS_NONE;
}

static void choose_project(App *app, bool create)
{
    if (!can_switch(app)) return;
    char dir[PATH_MAX];
    if (dialogs_choose_folder(create ? "Choose or create a folder for the new project" : "Open a folder of documents",
                              create ? "Create" : "Open", create, dir, sizeof(dir)))
        open_project(app, dir);
}

/* A document name the author typed: trimmed, ".md" added unless it already
 * ends in .md/.markdown/.txt, and safe as a file name in the project. */
static bool doc_name(const char *typed, char *out, size_t outlen, char *err, size_t errlen)
{
    size_t n = strlen(typed);
    if (n == 0) {
        snprintf(err, errlen, "Type a name");
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)typed; *p; p++)
        if (*p < 0x20 || *p == '/' || *p == ':' || *p == '\\') {
            snprintf(err, errlen, "A name can't contain / : \\ or control characters");
            return false;
        }
    if (typed[0] == '.') {
        snprintf(err, errlen, "A name can't start with a dot");
        return false;
    }
    bool ext = (n > 3 && !strcasecmp(typed + n - 3, ".md")) || (n > 4 && !strcasecmp(typed + n - 4, ".txt")) ||
               (n > 9 && !strcasecmp(typed + n - 9, ".markdown"));
    if (n + (ext ? 0 : 3) > 200) {
        snprintf(err, errlen, "That name is too long");
        return false;
    }
    snprintf(out, outlen, "%s%s", typed, ext ? "" : ".md");
    return true;
}

static bool name_taken(App *app, const char *name)
{
    char full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    struct stat st;
    return lstat(full, &st) == 0;
}

static void begin_new_document(App *app)
{
    if (!app->project_dir[0]) {
        set_status(app, "Open a project folder first (\xE2\x8C\x98O)");
        return;
    }
    if (editor_is_dirty(app->editor)) {
        set_status(app, "Save the current document first");
        return;
    }
    char name[64] = "Untitled.md";
    for (int i = 2; name_taken(app, name) && i < 1000; i++) snprintf(name, sizeof(name), "Untitled %d.md", i);
    snprintf(app->prompt_project, sizeof(app->prompt_project), "%s", app->project_dir);
    prompt_open(app->prompt, PT_NEW_DOC, "New document", name, "Create");
}

static void begin_rename(App *app, const char *name)
{
    snprintf(app->rename_from, sizeof(app->rename_from), "%s", name);
    char title[320];
    snprintf(title, sizeof(title), "Rename \xE2\x80\x9C%s\xE2\x80\x9D", name);
    snprintf(app->prompt_project, sizeof(app->prompt_project), "%s", app->project_dir);
    prompt_open(app->prompt, PT_RENAME_DOC, title, name, "Rename");
}

/* Returns true when the prompt can close. */
static bool same_file(const char *a, const char *b)
{
    struct stat x, y;
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

/* History left under a name by an earlier, different document: set it
 * aside so a new document of that name never inherits it (red-team 006). */
static bool set_aside_history(App *app, const char *name, bool linked_documents)
{
    if (linked_documents) {
        /* An earlier document's row with this name keeps its history under
         * another name; the new document gets its own id. */
        char aside[300];
        snprintf(aside, sizeof(aside), "%s (earlier %ld)", name, (long)time(NULL));
        return project_store_document_set_aside(app->store, app->project_dir, name, aside);
    }
    char rev[PATH_MAX + 300];
    snprintf(rev, sizeof(rev), "%s/.scribodo/revisions/%s", app->project_dir, name);
    struct stat st;
    if (stat(rev, &st) != 0) return true;
    char aside[PATH_MAX + 340];
    snprintf(aside, sizeof(aside), "%s.earlier-%ld", rev, (long)time(NULL));
    return rename(rev, aside) == 0;
}

static volatile sig_atomic_t g_quit_signal;
static void on_quit_signal(int sig) { g_quit_signal = sig; }

/* The memory the chat model could have now: what's free, plus what the
 * running model holds (a restart hands it back). */
static long long memory_for_model(App *app)
{
    long long avail = sysmem_available();
    LlmState st = llm_server_state(app->llm, NULL, 0);
    if (st == LLM_READY || st == LLM_STARTING) avail += llm_server_mem_need(llm_server_config(app->llm), llm_server_config(app->llm)->ctx_total);
    return avail;
}

/* What the author is told when memory decides the context (memfit.h). */
static void memfit_message(const MemFit *f, int wanted, char *out, size_t outlen)
{
    char need[32], free_gb[32];
    memfit_gb(f->room, free_gb, sizeof(free_gb)); /* after the embedding server and the headroom */
    if (f->short_even_min) {
        memfit_gb(f->need, need, sizeof(need));
        snprintf(out, outlen,
                 "Low memory: the model needs about %s and %s is free for it. It's starting with a %dK context anyway; "
                 "replies may be slow until other apps are closed",
                 need, free_gb, f->slot_ctx / 1024);
    } else if (f->reduced) {
        memfit_gb(f->need_wanted, need, sizeof(need));
        snprintf(out, outlen,
                 "Low memory: a %dK context needs about %s and %s is free for it, so the model runs with %dK for now. "
                 "Close other apps, then Apply in Model settings to try again",
                 wanted / 1024, need, free_gb, f->slot_ctx / 1024);
    } else {
        out[0] = '\0';
    }
}

/* The author's context per conversation, or the largest that fits in
 * memory now; restarts the model if that differs from what it runs with.
 * Only when nothing is being written. */
static void fit_context(App *app, bool announce)
{
    const ModelSettingInfo *in = model_setting_info(7);
    int running = llm_server_slot_ctx(app->llm);
    MemFit f = memfit_context(memory_for_model(app), 0, llm_server_config(app->llm), app->model.context, (int)in->min,
                              (int)in->step);
    char msg[400];
    memfit_message(&f, app->model.context, msg, sizeof(msg));
    if (msg[0]) fprintf(stderr, "scribodo: %s\n", msg);
    if (f.slot_ctx != running) llm_server_set_slot_ctx(app->llm, f.slot_ctx);
    if (msg[0]) set_status(app, msg);
    else if (announce && f.slot_ctx != running) set_status(app, "Model settings saved; the model is restarting with the new context size");
    else if (announce) set_status(app, "Model settings saved (they apply to the next message)");
}

/* The model settings go into every chat request; a new context size
 * restarts the model (only when nothing is being written, and only as far
 * as memory allows). `context`: false at startup, where the context was
 * already chosen. */
static void apply_model_settings(App *app, bool context)
{
    ChatSampling sp = { app->model.max_reply, app->model.temperature, app->model.top_p, app->model.top_k,
                        app->model.min_p, app->model.repeat_penalty, app->model.edit_temperature };
    if (app->chat) chat_set_sampling(app->chat, &sp);
    if (!context) return;
    if (app->llm && llm_server_slot_ctx(app->llm) != app->model.context) {
        if ((app->chat && chat_busy(app->chat)) || name_detection_job_busy(&app->names_job)) {
            app->ctx_pending = true;
            set_status(app, "Saved. The new context size applies after the reply finishes and the model restarts");
            return;
        }
        fit_context(app, true);
        return;
    }
    set_status(app, "Model settings saved (they apply to the next message)");
}

static void open_model_settings(App *app)
{
    app->model_edit = app->model;
    app->settings_open = true;
    app->mem_for_model = app->llm ? memory_for_model(app) : 0;
}

/* The panel (drawn over everything; raygui draws ASCII only). */
static void draw_model_settings(App *app, Rectangle screen)
{
    DrawRectangleRec(screen, (Color){ 0, 0, 0, 140 });
    float w = 620, rowh = 44, h = 110 + rowh * MODEL_SETTING_COUNT + 56 + (app->llm ? 26 : 0);
    Rectangle p = { (screen.width - w) / 2, (screen.height - h) / 2, w, h };
    DrawRectangleRounded(p, 0.03f, 6, (Color){ 0x26, 0x27, 0x2B, 0xFF });
    DrawRectangleRoundedLinesEx(p, 0.03f, 6, 1.0f, (Color){ 0x45, 0x47, 0x4D, 0xFF });
    GuiLabel((Rectangle){ p.x + 24, p.y + 16, w - 48, 24 }, "Model settings (chat)");
    GuiLabel((Rectangle){ p.x + 24, p.y + 44, w - 48, 20 },
             "The observer always uses temperature 0, so its findings stay reproducible.");
    for (int i = 0; i < MODEL_SETTING_COUNT; i++) {
        const ModelSettingInfo *in = model_setting_info(i);
        float y = p.y + 80 + rowh * (float)i;
        GuiLabel((Rectangle){ p.x + 24, y, 230, 20 }, in->label);
        GuiLabel((Rectangle){ p.x + 24, y + 18, 330, 18 }, in->hint);
        double v = model_setting_get(&app->model_edit, i);
        char val[32];
        if (in->integer) snprintf(val, sizeof(val), "%d", (int)v);
        else snprintf(val, sizeof(val), "%.2f", v);
        app->set_minus[i] = (Rectangle){ p.x + w - 200, y + 4, 36, 28 };
        app->set_plus[i] = (Rectangle){ p.x + w - 60, y + 4, 36, 28 };
        if (GuiButton(app->set_minus[i], "-")) model_setting_set(&app->model_edit, i, v - in->step);
        GuiLabel((Rectangle){ p.x + w - 156, y + 4, 92, 28 }, val);
        if (GuiButton(app->set_plus[i], "+")) model_setting_set(&app->model_edit, i, v + in->step);
    }
    model_settings_clamp(&app->model_edit);
    float by = p.y + h - 48;
    if (app->llm) { /* the memory readout for the context being chosen */
        const LlmServerConfig *cfg = llm_server_config(app->llm);
        int slots = cfg->slots > 0 ? cfg->slots : 1;
        long long need = llm_server_mem_need(cfg, app->model_edit.context * slots);
        long long room = app->mem_for_model - MEMFIT_HEADROOM;
        char a[32], b[32], line[200];
        memfit_gb(need, a, sizeof(a));
        memfit_gb(room > 0 ? room : 0, b, sizeof(b));
        int running = llm_server_slot_ctx(app->llm);
        if (need > room)
            snprintf(line, sizeof(line), "Memory: about %s at this context, %s free: it would start smaller", a, b);
        else if (running != app->model.context)
            snprintf(line, sizeof(line), "Memory: about %s at this context, %s free (running with %dK now)", a, b, running / 1024);
        else
            snprintf(line, sizeof(line), "Memory: about %s at this context, %s free", a, b);
        GuiLabel((Rectangle){ p.x + 24, by - 32, w - 48, 20 }, line);
    }
    app->set_defaults = (Rectangle){ p.x + 24, by, 100, 30 };
    app->set_cancel = (Rectangle){ p.x + w - 228, by, 96, 30 };
    app->set_apply = (Rectangle){ p.x + w - 120, by, 96, 30 };
    if (GuiButton(app->set_defaults, "Defaults")) model_settings_defaults(&app->model_edit);
    if (GuiButton(app->set_cancel, "Cancel") || IsKeyPressed(KEY_ESCAPE)) app->settings_open = false;
    if (GuiButton(app->set_apply, "Apply")) {
        if (model_settings_save(&app->model_edit)) {
            app->model = app->model_edit;
            app->settings_open = false;
            apply_model_settings(app, true);
        } else {
            set_status(app, "Couldn't save the model settings");
        }
    }
}

/* The stored names go everywhere they're used: chat requests, the chat's
 * labels, and the observer's rules. */
static void apply_profile(App *app)
{
    if (app->chat) chat_set_names(app->chat, app->profile.author, app->profile.assistant);
    if (app->chat_view) {
        chat_view_set_assistant_name(app->chat_view, profile_assistant_name(&app->profile));
        chat_view_set_author_name(app->chat_view, app->profile.author);
    }
    if (app->observer) observer_set_names(app->observer, app->profile.author, app->profile.assistant);
}

static void begin_names(App *app)
{
    app->names_step = 1;
    prompt_open(app->prompt, PT_NAME_AUTHOR, "Your name (the assistant calls you this)", app->profile.author, "Next");
}

/* Each frame: the second step opens after the first closes; a cancelled
 * first-launch question isn't asked again. */
static void names_tick(App *app)
{
    if (app->names_step == 2 && !prompt_active(app->prompt)) {
        app->names_step = 3;
        prompt_open(app->prompt, PT_NAME_ASSISTANT, "What should the assistant be called?", app->profile.assistant, "Save");
    } else if ((app->names_step == 1 || app->names_step == 3) && !prompt_active(app->prompt)) {
        app->names_step = 0;
        if (!app->profile.asked) {
            app->profile.asked = true;
            profile_save(&app->profile);
        }
    }
}

static bool finish_prompt(App *app, const char *typed)
{
    if (prompt_tag(app->prompt) == PT_ATTACH_NAME) {
        if (strcmp(app->prompt_project, app->project_dir) != 0) return true;
        if (!listed_doc(app, typed)) {
            prompt_set_error(app->prompt, "No document by that name in this project");
            return false;
        }
        attach(app, typed);
        chat_controller_mark_attachments_changed(&app->chat_controller);
        return true;
    }
    if (prompt_tag(app->prompt) == PT_NAME_AUTHOR) {
        profile_clean_name(typed, app->names_author, sizeof(app->names_author));
        app->names_step = 2; /* the assistant's name next (opened after this prompt closes) */
        return true;
    }
    if (prompt_tag(app->prompt) == PT_NAME_ASSISTANT) {
        Profile p = app->profile;
        snprintf(p.author, sizeof(p.author), "%s", app->names_author);
        profile_clean_name(typed, p.assistant, sizeof(p.assistant));
        p.asked = true;
        if (!profile_save(&p)) {
            prompt_set_error(app->prompt, "Couldn't save the names");
            return false;
        }
        app->profile = p;
        apply_profile(app);
        app->names_step = 0;
        char msg[200];
        snprintf(msg, sizeof(msg), "Names saved: you are %s; the assistant is %s", p.author[0] ? p.author : "(not set)",
                 profile_assistant_name(&p));
        set_status(app, msg);
        return true;
    }
    if (prompt_tag(app->prompt) == PT_RENAME_CHAT) {
        if (!typed[0]) {
            prompt_set_error(app->prompt, "Type a title");
            return false;
        }
        /* Titles are kept to 150 bytes, cut on a character boundary. */
        char title[160];
        utf8_clip(title, sizeof(title), typed, 150);
        typed = title;
        if (strcmp(app->prompt_project, app->project_dir) != 0) return true; /* project changed: nothing done */
        if (!chat_store_rename(app->store, app->project_dir, app->menu_chat, typed)) {
            prompt_set_error(app->prompt, "Couldn't rename that chat");
            return false;
        }
        if (!strcmp(app->chat_id, app->menu_chat)) snprintf(app->chat_title, sizeof(app->chat_title), "%s", typed);
        if (app->history_pending && !strcmp(app->history_id, app->menu_chat))
            snprintf(app->history_title, sizeof(app->history_title), "%s", typed);
        set_status(app, "Chat renamed");
        app->lists_dirty = true;
        return true;
    }
    char name[260], err[160];
    if (!doc_name(typed, name, sizeof(name), err, sizeof(err))) {
        prompt_set_error(app->prompt, err);
        return false;
    }
    int tag = prompt_tag(app->prompt);
    /* The prompt belongs to the project it was opened in. */
    if (strcmp(app->prompt_project, app->project_dir) != 0) {
        set_status(app, "The project changed; nothing was done");
        return true;
    }
    if (tag == PT_RENAME_DOC && !strcmp(name, app->rename_from)) return true; /* unchanged */
    char full[PATH_MAX + 260], old_full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    snprintf(old_full, sizeof(old_full), "%s/%s", app->project_dir, app->rename_from);
    /* "Same file" is decided by inode, never by comparing names: a case- or
     * normalisation-only rename on a case-insensitive volume is the same
     * file, but on a case-sensitive volume "A.md" is another document that
     * must not be replaced (red-team 006). */
    bool self = tag == PT_RENAME_DOC && same_file(old_full, full);
    if (name_taken(app, name) && !self) {
        prompt_set_error(app->prompt, "A file with that name already exists");
        return false;
    }
    if (tag == PT_NEW_DOC) {
        ProjectStoreGuard guard;
        if (!project_store_guard_begin(app->store, app->project_dir, &guard)) {
            prompt_set_error(app->prompt, "New documents wait for the project store (notes, canon, chats and History are paused)");
            return false;
        }
        int fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            project_store_guard_end(&guard);
            prompt_set_error(app->prompt, strerror(errno));
            return false;
        }
        close(fd);
        bool aside = set_aside_history(app, name, (guard.capabilities & PROJECT_STORE_CAP_LINKED_DOCUMENTS) != 0);
        if (!aside) unlink(full);
        project_store_guard_end(&guard);
        if (!aside) {
            prompt_set_error(app->prompt, "Older History under this name couldn't be set aside; nothing was created");
            return false;
        }
        open_path(app, full);
        char msg[320];
        snprintf(msg, sizeof(msg), "Created %s", name);
        set_status(app, msg);
        app->focus = FOCUS_DOC;
        app->lists_dirty = true;
        return true;
    }
    /* Is the renamed file the open document? Decided by inode, before the
     * rename (paths may differ in case or normalisation). */
    bool is_open = editor_path(app->editor) && same_file(editor_path(app->editor), old_full);
    if (is_open && editor_is_dirty(app->editor)) {
        prompt_set_error(app->prompt, "Save the document before renaming it");
        return false;
    }
    ProjectStoreGuard guard;
    if (!project_store_guard_begin(app->store, app->project_dir, &guard)) {
        /* The store keys history by document; a rename it doesn't see would
         * disconnect the document from its History and sources. */
        prompt_set_error(app->prompt, "Renaming waits for the project store (it's offline)");
        return false;
    }
    if (renamex_np(old_full, full, self ? 0 : RENAME_EXCL) != 0) {
        project_store_guard_end(&guard);
        prompt_set_error(app->prompt, strerror(errno));
        return false;
    }
    if (guard.capabilities & PROJECT_STORE_CAP_LINKED_DOCUMENTS) {
        /* One update: the document keeps its id, so its History, sources,
         * attachments and observer state all follow. */
        char problems[160] = "";
        if (!project_store_document_rename(app->store, app->project_dir, app->rename_from, name)) {
            /* Never leave the file renamed without its record: that would cut
             * the document off from its History and sources. */
            renamex_np(full, old_full, self ? 0 : RENAME_EXCL);
            project_store_guard_end(&guard);
            prompt_set_error(app->prompt, "The project store couldn't record the rename; nothing was changed");
            return false;
        }
        (void)chat_controller_rename_attachment(&app->chat_controller, app->rename_from, name);
        recent_store_rename_doc(app->recent_store, app->project_dir, app->rename_from, name);
        if (is_open) editor_set_path(app->editor, full);
        project_store_guard_end(&guard);
        char msg[600];
        if (problems[0]) snprintf(msg, sizeof(msg), "Renamed to %s, but %s", name, problems);
        else snprintf(msg, sizeof(msg), "Renamed to %s", name);
        set_status(app, msg);
        app->lists_dirty = true;
        return true;
    }
    /* Then its History, the record of the rename (so sources made under the
     * old name still resolve), and observer state. Failures are reported. */
    char problems[300] = "";
    char rev_old[PATH_MAX + 300], rev_new[PATH_MAX + 300];
    snprintf(rev_old, sizeof(rev_old), "%s/.scribodo/revisions/%s", app->project_dir, app->rename_from);
    snprintf(rev_new, sizeof(rev_new), "%s/.scribodo/revisions/%s", app->project_dir, name);
    struct stat st;
    if (stat(rev_old, &st) == 0) {
        bool same_dir = same_file(rev_old, rev_new); /* a case-only rename of the folder */
        if (!same_dir && !set_aside_history(app, name, false)) snprintf(problems, sizeof(problems), "History couldn't be moved");
        else if (renamex_np(rev_old, rev_new, same_dir ? 0 : RENAME_EXCL) != 0)
            snprintf(problems, sizeof(problems), "History couldn't be moved");
    }
    if (!renames_record(app->project_dir, app->rename_from, name))
        snprintf(problems + strlen(problems), sizeof(problems) - strlen(problems), "%sthe rename couldn't be recorded",
                 problems[0] ? "; " : "");
    observer_rename_doc(app->observer, app->project_dir, app->rename_from, name);
    (void)chat_controller_rename_attachment(&app->chat_controller, app->rename_from, name);
    recent_store_rename_doc(app->recent_store, app->project_dir, app->rename_from, name);
    if (is_open) editor_set_path(app->editor, full);
    project_store_guard_end(&guard);
    char msg[700];
    if (problems[0]) snprintf(msg, sizeof(msg), "Renamed to %s, but %s", name, problems);
    else snprintf(msg, sizeof(msg), "Renamed to %s", name);
    set_status(app, msg);
    app->lists_dirty = true;
    return true;
}

/* Copy chosen .md/.txt files into the project (never overwriting). */
static void import_documents(App *app)
{
    if (!app->project_dir[0]) {
        set_status(app, "Open a project folder first (\xE2\x8C\x98O)");
        return;
    }
    char **paths = NULL;
    size_t n = dialogs_choose_files("Import documents into this project (they are copied)", &paths);
    if (n == 0) {
        dialogs_free_paths(paths, n);
        return;
    }
    ProjectStoreGuard guard;
    if (!project_store_guard_begin(app->store, app->project_dir, &guard)) {
        dialogs_free_paths(paths, n);
        set_status(app, "Importing waits for the project store (notes, canon, chats and History are paused)");
        return;
    }
    size_t done = 0;
    bool history_kept = false;
    char first[PATH_MAX + 260] = "";
    for (size_t i = 0; i < n; i++) {
        const char *base = base_name(paths[i]);
        char name[260];
        snprintf(name, sizeof(name), "%s", base);
        const char *dot = strrchr(base, '.');
        size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
        for (int k = 2; name_taken(app, name) && k < 1000; k++)
            snprintf(name, sizeof(name), "%.*s %d%s", (int)stem, base, k, dot && dot != base ? dot : "");
        char dst[PATH_MAX + 260];
        snprintf(dst, sizeof(dst), "%s/%s", app->project_dir, name);
        FILE *in = fopen(paths[i], "rb");
        int fd = in ? open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644) : -1;
        bool ok = in && fd >= 0;
        char buf[65536];
        size_t r;
        while (ok && (r = fread(buf, 1, sizeof(buf), in)) > 0) ok = write(fd, buf, r) == (ssize_t)r;
        ok = ok && !ferror(in) && fsync(fd) == 0; /* a read error is not the end of the file */
        if (fd >= 0) ok = close(fd) == 0 && ok;
        if (in) fclose(in);
        if (!ok && fd >= 0) unlink(dst);
        if (ok) {
            /* An earlier document by this name keeps its History; this one
             * starts its own (as with New). */
            if (!set_aside_history(app, name, (guard.capabilities & PROJECT_STORE_CAP_LINKED_DOCUMENTS) != 0)) {
                unlink(dst);
                history_kept = true;
            } else {
                done++;
                if (!first[0]) snprintf(first, sizeof(first), "%s", dst);
            }
        }
    }
    project_store_guard_end(&guard);
    dialogs_free_paths(paths, n);
    app->lists_dirty = true;
    if (first[0] && !editor_is_dirty(app->editor)) open_path(app, first);
    char msg[200];
    snprintf(msg, sizeof(msg), "Imported %zu of %zu document%s%s", done, n, n == 1 ? "" : "s",
             history_kept ? " (older History under a reused name couldn't be set aside)" : "");
    set_status(app, msg);
    if (done) observer_poke(app->observer);
}

static void reveal_document(App *app, const char *name)
{
    char full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    dialogs_reveal(full);
}

static void save(App *app)
{
    char msg[512];
    bool ok = editor_save(app->editor, msg, sizeof(msg));
    set_status(app, msg);
    if (ok) record_revision(app, "save", NULL);
    if (ok && app->observer) observer_poke(app->observer); /* a saved revision: new text to read */
    app->lists_dirty = true;
}

/* ---- chat ---------------------------------------------------------------------- */

/* ---- names noticed in chat ----------------------------------------------------------- */

/* After a reply: did the author's message before it say a name? Checked in
 * the background (the local model, structured output, verified against the
 * author's words). */
static void names_check(App *app, size_t reply_index)
{
    if (name_detection_job_busy(&app->names_job)) return;
    chat_lock(app->chat);
    const ChatMessage *um = chat_message(app->chat, reply_index - 1);
    const ChatMessage *pm = reply_index >= 2 ? chat_message(app->chat, reply_index - 2) : NULL;
    bool go = um && um->role == CHAT_USER && !um->is_edit && names_worth_checking(um->content);
    const char *author_msg = go ? um->content : NULL;
    const char *assistant_prev = go && pm && pm->role == CHAT_ASSISTANT ? pm->content : NULL;
    if (go) (void)name_detection_job_start(&app->names_job, assistant_prev, author_msg);
    chat_unlock(app->chat);
}

static void names_poll(App *app)
{
    bool succeeded = false;
    if (!name_detection_job_poll(&app->names_job, &app->names_guess, &succeeded)) return;
    if (!succeeded) return;
    const NameGuess *g = &app->names_guess;
    /* Offered only if new: not what's stored, not what the author declined. */
    if (g->author[0] && strcasecmp(g->author, app->profile.author) && strcasecmp(g->author, app->declined_author))
        snprintf(app->suggest_author, sizeof(app->suggest_author), "%s", g->author);
    if (g->assistant[0] && strcasecmp(g->assistant, app->profile.assistant) && strcasecmp(g->assistant, app->declined_assistant))
        snprintf(app->suggest_assistant, sizeof(app->suggest_assistant), "%s", g->assistant);
    if (app->suggest_author[0] && app->suggest_assistant[0])
        snprintf(app->suggest_text, sizeof(app->suggest_text),
                 "Save these names? You: \xE2\x80\x9C%s\xE2\x80\x9D, the assistant: \xE2\x80\x9C%s\xE2\x80\x9D",
                 app->suggest_author, app->suggest_assistant);
    else if (app->suggest_author[0])
        snprintf(app->suggest_text, sizeof(app->suggest_text), "Save \xE2\x80\x9C%s\xE2\x80\x9D as your name?", app->suggest_author);
    else if (app->suggest_assistant[0])
        snprintf(app->suggest_text, sizeof(app->suggest_text), "Save \xE2\x80\x9C%s\xE2\x80\x9D as the assistant's name?",
                 app->suggest_assistant);
}

static void names_answer(App *app, bool save)
{
    if (save) {
        Profile p = app->profile;
        if (app->suggest_author[0]) snprintf(p.author, sizeof(p.author), "%s", app->suggest_author);
        if (app->suggest_assistant[0]) snprintf(p.assistant, sizeof(p.assistant), "%s", app->suggest_assistant);
        p.asked = true;
        if (profile_save(&p)) {
            app->profile = p;
            apply_profile(app);
            set_status(app, "Names saved (change them any time: Explorer \xE2\x86\x92 PROJECT \xE2\x86\x92 Names\xE2\x80\xA6)");
        } else {
            set_status(app, "Couldn't save the names");
        }
    } else {
        snprintf(app->declined_author, sizeof(app->declined_author), "%s", app->suggest_author);
        snprintf(app->declined_assistant, sizeof(app->declined_assistant), "%s", app->suggest_assistant);
    }
    app->suggest_author[0] = app->suggest_assistant[0] = app->suggest_text[0] = '\0';
}

static void on_message_done(ChatSession *s, size_t index, void *ctx)
{
    App *app = ctx;
    chat_lock(s);
    const ChatMessage *m = chat_message(s, index);
    bool found = m != NULL;
    ChatMessage copy = { 0 };
    if (m) {
        copy = *m;
        copy.content = strdup(m->content ? m->content : "");
    }
    chat_unlock(s);
    char *pending_original = NULL, *pending_instruction = NULL;
    bool pending_finished = reviewed_edit_controller_take_finished(&app->reviewed_edit, index, &pending_original,
                                                                    &pending_instruction);
    if (!found || !copy.content) {
        if (pending_finished) {
            editor_anchor_clear(app->editor);
            set_status(app, "The edit reply finished, but there wasn't enough memory to review it");
        }
        free(pending_original);
        free(pending_instruction);
        return;
    }
    if (pending_finished) {
        size_t start = 0, len = 0;
        bool intact = editor_anchor_get(app->editor, &start, &len);
        if (copy.error || !copy.content[0]) {
            editor_anchor_clear(app->editor);
        } else if (copy.stopped) {
            /* A reply stopped part-way is not a replacement for the whole
             * passage (red-team 004). */
            editor_anchor_clear(app->editor);
            set_status(app, "Edit stopped \xE2\x80\x94 nothing was proposed");
        } else if (!intact) {
            editor_anchor_clear(app->editor);
            set_status(app, "The passage changed while the edit was being written; nothing was proposed");
        } else {
            char *clean = llm_clean_edit(copy.content, pending_original);
            if (clean) propose(app, start, len, pending_original, clean, pending_instruction, copy.truncated);
            else editor_anchor_clear(app->editor);
            free(clean);
        }
        free(pending_original);
        free(pending_instruction);
    }
    if (copy.role == CHAT_ASSISTANT && !copy.error && !copy.is_edit && index >= 1) names_check(app, index);
    if (!app->chat_project[0]) { free(copy.content); return; }
    int64_t sequence = -1;
    if (!chat_store_append_sequenced(app->store, app->chat_project, app->chat_id, app->chat_title, &copy, &sequence))
        set_status(app, store_says(app, "Could not save the chat to .scribodo/chats"));
    else {
        if (sequence >= 0) chat_mark_saved(s, index, sequence);
        if (app->observer && copy.role == CHAT_ASSISTANT) observer_poke(app->observer);
    }
    free(copy.content);
    app->lists_dirty = true;
}

/* A conversation started before any project was open has nowhere to be
 * saved; when a project appears, adopt it rather than discard it (red-team
 * 003: it used to be cleared, silently losing the author's text). */
static void adopt_chat(App *app)
{
    if (!app->chat_id[0]) chat_store_new_id(app->chat_id, sizeof(app->chat_id));
    if (!app->chat_title[0]) snprintf(app->chat_title, sizeof(app->chat_title), "Chat");
    project_session_bind_chat(&app->session, app->project_dir, app->chat_id, app->chat_title);
    chat_lock(app->chat);
    size_t n = chat_count(app->chat);
    ChatMessage *copy = calloc(n ? n : 1, sizeof(ChatMessage));
    for (size_t i = 0; copy && i < n; i++) {
        copy[i] = *chat_message(app->chat, i);
        copy[i].content = strdup(copy[i].content ? copy[i].content : "");
    }
    chat_unlock(app->chat);
    bool ok = copy != NULL;
    for (size_t i = 0; ok && i < n; i++) {
        int64_t sequence = -1;
        ok = copy[i].content && chat_store_append_sequenced(app->store, app->chat_project, app->chat_id, app->chat_title, &copy[i], &sequence);
        if (ok && sequence >= 0) chat_mark_saved(app->chat, i, sequence);
    }
    for (size_t i = 0; copy && i < n; i++) free(copy[i].content);
    free(copy);
    if (chat_trim_saved(app->chat)) chat_view_history_changed(app->chat_view);
    set_status(app, ok ? "Chat saved to this project" : store_says(app, "Could not save the chat to .scribodo/chats"));
    app->lists_dirty = true;
}

/* ---- attachments ---------------------------------------------------------------- */

static bool is_attached(App *app, const char *name)
{
    return chat_controller_has_attachment(&app->chat_controller, name);
}

/* A document the Explorer lists for this project: never a path, a hidden
 * file or anything outside the folder (red-team 007: a chat log could name
 * "../other/secret.md"). */
static bool listed_doc(App *app, const char *name)
{
    if (!name[0] || name[0] == '.' || strchr(name, '/')) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20) return false; /* would break the record */
    /* The open document (a new one isn't in the list until it refreshes). */
    if (editor_path(app->editor) && !strcmp(base_name(editor_path(app->editor)), name)) {
        char proj[PATH_MAX];
        project_of(editor_path(app->editor), proj, sizeof(proj));
        if (!strcmp(proj, app->project_dir)) return true;
    }
    for (size_t i = 0; i < app->ndocs; i++)
        if (!strcmp(app->docs[i], name)) return true;
    return false;
}

static bool attach(App *app, const char *name)
{
    if (is_attached(app, name)) return true;
    if (!listed_doc(app, name)) return false;
    if (app->nattached == MAX_ATTACH) {
        set_status(app, "A chat can have up to 8 documents attached");
        return false;
    }
    return chat_controller_add_attachment(&app->chat_controller, name);
}

static void detach(App *app, size_t i)
{
    (void)chat_controller_remove_attachment(&app->chat_controller, i);
}

/* A new conversation starts with the open document attached. */
static void attach_reset(App *app)
{
    chat_controller_clear_attachments(&app->chat_controller);
    if (editor_path(app->editor)) attach(app, base_name(editor_path(app->editor)));
}

/* Read an attached document as the editor shows it: the open one from the
 * buffer (unsaved text included), others from disk with BOM and CRLF
 * normalised. NULL if it can't be read. */
static char *attached_text(App *app, const char *name)
{
    if (!listed_doc(app, name)) return NULL;
    if (editor_path(app->editor) && !strcmp(base_name(editor_path(app->editor)), name)) return editor_text_utf8(app->editor);
    char full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (16 << 20)) return NULL;
    FILE *f = fopen(full, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    size_t r = 0, w = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) r = 3;
    for (; r < got; r++) {
        if (buf[r] == '\r') {
            if (r + 1 < got && buf[r + 1] == '\n') r++;
            buf[w++] = '\n';
        } else if (buf[r] != '\0') {
            buf[w++] = buf[r];
        }
    }
    buf[w] = '\0';
    /* Invalid UTF-8 becomes U+FFFD, as in the editor; raw bytes would make
     * the request invalid JSON (red-team 007). */
    uint32_t *cps = malloc((w + 1) * sizeof(uint32_t));
    char *out = NULL;
    if (cps) {
        size_t invalid = 0, n = utf8_decode((const uint8_t *)buf, w, cps, &invalid);
        if (invalid == 0) {
            free(cps);
            return buf;
        }
        size_t bytes = utf8_encoded_len(cps, n);
        out = malloc(bytes + 1);
        if (out) {
            utf8_encode(cps, n, (uint8_t *)out);
            out[bytes] = '\0';
        }
        free(cps);
    }
    free(buf);
    return out;
}

static bool unsaved_chat(App *app)
{
    chat_lock(app->chat);
    bool unsaved = chat_window_info(app->chat).unsaved;
    chat_unlock(app->chat);
    return unsaved;
}

static void new_chat(App *app)
{
    if (chat_busy(app->chat)) {
        set_status(app, "Wait for the reply to finish (or press Stop)");
        return;
    }
    if (unsaved_chat(app) && !(app->chat_discard_armed && GetTime() - app->chat_discard_at < 5.0)) {
        app->chat_discard_armed = true;
        app->chat_discard_at = GetTime();
        set_status(app, "Unsaved chat — copy what you need, then click New chat again to discard");
        return;
    }
    app->chat_discard_armed = false;
    cancel_history(app);
    chat_clear(app->chat);
    chat_view_history_changed(app->chat_view);
    project_session_clear_chat(&app->session);
    attach_reset(app);
    app->focus = FOCUS_CHAT;
}

static bool load_chat_window(App *app, const char *project, const char *id, const char *title,
                             ChatWindowRequest request, bool restore)
{
    if (chat_busy(app->chat)) { set_status(app, "Stop the reply before changing history"); return false; }
    chat_lock(app->chat);
    ChatWindowInfo info = chat_window_info(app->chat);
    chat_unlock(app->chat);
    if (info.unsaved) { set_status(app, "History has unsaved messages; keep them before changing windows"); return false; }
    StoreProjectIdentity identity;
    if (!store_project_identity(app->store, project, &identity)) {
        set_status(app, "Could not identify the stored project; the current conversation was kept");
        return false;
    }
    uint64_t ticket = app->history_ticket + 1;
    if (!chat_history_job_start(app->history_job, project, id, &request, ticket)) {
        set_status(app, "Could not start history loading; the current conversation was kept");
        return false;
    }
    app->history_ticket = ticket;
    app->history_pending = true;
    app->history_restore = restore;
    app->history_send_ready = false;
    app->history_session = app->session_generation;
    app->history_version = chat_version(app->chat);
    app->history_request = request;
    app->history_identity = identity;
    snprintf(app->history_project, sizeof(app->history_project), "%s", project);
    snprintf(app->history_id, sizeof(app->history_id), "%s", id);
    snprintf(app->history_title, sizeof(app->history_title), "%s", title ? title : app->chat_title);
    snprintf(app->history_from_project, sizeof(app->history_from_project), "%s", app->chat_project);
    snprintf(app->history_from_id, sizeof(app->history_from_id), "%s", app->chat_id);
    app->history_attachment_revision = app->attachment_revision;
    set_status(app, "Loading history — keep writing, or Cancel to keep this window");
    return true;
}

static void history_poll(App *app)
{
    ChatHistoryResult result;
    if (!chat_history_job_poll(app->history_job, &result)) return;
    StoreProjectIdentity identity;
    bool same_identity = store_project_identity(app->store, app->history_project, &identity) &&
                         identity.generation == app->history_identity.generation &&
                         identity.device == app->history_identity.device && identity.inode == app->history_identity.inode &&
                         !strcmp(identity.project_id, app->history_identity.project_id);
    /* Publication is a UI-thread operation, conditional on the original intent.
     * A completed old read cannot overwrite new/unsaved prose or cross projects. */
    bool current = app->history_pending && result.ticket == app->history_ticket &&
                   app->history_session == app->session_generation &&
                   app->history_version == chat_version(app->chat) &&
                   !strcmp(app->history_from_project, app->chat_project) &&
                   !strcmp(app->history_from_id, app->chat_id) && !chat_busy(app->chat) && !unsaved_chat(app) &&
                   store_online(app->store) && same_identity;
    if (!current) {
        if (result.ticket == app->history_ticket) app->history_pending = false;
        chat_window_page_free(&result.page);
        return;
    }
    app->history_pending = false;
    if (!result.ok) {
        set_status(app, result.why[0] ? result.why : "Could not load history; the current conversation was kept");
        chat_window_page_free(&result.page);
        return;
    }
    ChatWindowPage *page = &result.page;
    bool ok = chat_load_window(app->chat, page->messages, page->count, page->older, page->newer);
    if (ok) {
        chat_view_history_changed(app->chat_view);
        if (app->history_restore) {
            project_session_bind_chat(&app->session, app->history_project, app->history_id, app->history_title);
        }
        bool unchanged = app->attachment_revision == app->history_attachment_revision;
        if (app->history_restore && unchanged) {
            chat_controller_use_no_attachments(&app->chat_controller);
            for (char *save = NULL, *name = page->latest_user_docs ? strtok_r(page->latest_user_docs, "\n", &save) : NULL;
                 name; name = strtok_r(NULL, "\n", &save)) attach(app, name);
        }
        if (app->history_request.direction == CHAT_WINDOW_FOCUS) {
            chat_lock(app->chat);
            long index = chat_find_sequence(app->chat, app->history_request.sequence);
            chat_unlock(app->chat);
            if (index >= 0) chat_view_reveal(app->chat_view, index);
        }
        set_status(app, app->history_send_ready ? "Latest history ready; review attachments/provider and press Send again"
                        : app->history_restore && !unchanged ? "History opened; your current attachment choices were kept"
                        : app->history_restore && !page->attachments_known
                        ? "History opened; prior attachments could not be restored. Choose attachments explicitly."
                        : "History window loaded (large messages have navigable excerpts)");
    } else set_status(app, "Could not replace history; the current conversation was kept");
    chat_window_page_free(page);
}

static void page_chat(App *app, ChatPageAction action)
{
    chat_lock(app->chat);
    ChatWindowInfo info = chat_window_info(app->chat);
    chat_unlock(app->chat);
    if (!info.active || !app->chat_project[0] || !app->chat_id[0]) return;
    ChatWindowRequest request = { .direction = CHAT_WINDOW_LATEST };
    if (action.kind == CHAT_PAGE_OLDER) request = (ChatWindowRequest){ .direction = CHAT_WINDOW_BEFORE, .sequence = info.first };
    if (action.kind == CHAT_PAGE_NEWER) request = (ChatWindowRequest){ .direction = CHAT_WINDOW_AFTER, .sequence = info.last };
    if (action.kind == CHAT_PAGE_PART) request = (ChatWindowRequest){ CHAT_WINDOW_FOCUS, action.sequence, action.start };
    (void)load_chat_window(app, app->chat_project, app->chat_id, NULL, request, false);
}

static void open_chat(App *app, const ChatSummary *c)
{
    if (chat_busy(app->chat)) {
        set_status(app, "Wait for the reply to finish (or press Stop)");
        return;
    }
    if (unsaved_chat(app)) { set_status(app, "History has unsaved messages; keep them before opening another chat"); return; }
    if (database_history_project(app->project_dir)) {
        (void)load_chat_window(app, app->project_dir, c->id, c->title, (ChatWindowRequest){ .direction = CHAT_WINDOW_LATEST }, true);
        return;
    }
    cancel_history(app);
    ChatMessage *msgs = NULL;
    size_t n = 0;
    if (!chat_store_load(app->store, app->project_dir, c->id, &msgs, &n) || !chat_load_messages(app->chat, msgs, n)) {
        chat_store_free_messages(msgs, n);
        set_status(app, "Could not open that chat");
        return;
    }
    /* Its attachments are those of its newest user message, exactly: if
     * that one was sent with nothing, nothing is attached (red-team 007).
     * Renames are followed only if they came after the message. */
    chat_controller_use_no_attachments(&app->chat_controller);
    for (size_t i = n; i-- > 0;) {
        if (msgs[i].role != CHAT_USER) continue;
        char *copy = msgs[i].docs ? strdup(msgs[i].docs) : NULL;
        for (char *save = NULL, *t = copy ? strtok_r(copy, "\n", &save) : NULL; t; t = strtok_r(NULL, "\n", &save)) {
            char cur[256];
            renames_resolve_since(app->project_dir, t, msgs[i].ts, cur, sizeof(cur));
            attach(app, cur); /* only if still a listed document */
        }
        free(copy);
        break;
    }
    chat_store_free_messages(msgs, n);
    project_session_bind_chat(&app->session, app->project_dir, c->id, c->title);
    chat_view_history_changed(app->chat_view);
    chat_view_scroll_to_end(app->chat_view);
}

/* ---- notes and canon --------------------------------------------------------------- */

static bool record_unsaved_canon(App *app)
{
    if (!record_view_active(app->record) || record_view_kind(app->record) != REC_CANON) return false;
    if (record_view_dirty(app->record)) return true;
    if (record_view_id(app->record)[0]) return false;
    char *t = record_view_text(app->record); /* a draft with text in it */
    bool blank = !t || !t[strspn(t, " \n\t\r")];
    free(t);
    return !blank;
}

static bool is_blank(const char *s) { return !s || !s[strspn(s, " \n\t\r")]; }

/* Store the open note's text as a new version. */
static bool save_note(App *app)
{
    RecordView *v = app->record;
    char *text = record_view_text(v);
    if (!text) return false;
    char id[40];
    snprintf(id, sizeof(id), "%s", record_view_id(v));
    if (!id[0]) {
        if (is_blank(text)) { /* an untouched new note is nothing to keep */
            free(text);
            return true;
        }
        records_new_id(REC_NOTE, id, sizeof(id));
    }
    size_t ns;
    const SourceRef *src = record_view_sources(v, &ns);
    int ver = 0;
    bool ok = app->project_dir[0] && records_append(app->store, app->project_dir, REC_NOTE, id, "active", text, src, ns, &ver);
    if (ok) {
        record_view_stored(v, id, ver, "active", text);
        set_status(app, "Note saved");
    } else {
        set_status(app, store_says(app, "Could not save the note to .scribodo/notes.jsonl"));
    }
    free(text);
    app->lists_dirty = true;
    return ok;
}

/* Close the panel if nothing would be lost: notes are saved first; canon
 * text that was never committed needs a second Close within 5 s. Returns
 * false (panel stays open) otherwise. */
static bool close_record(App *app, bool explicit_close)
{
    RecordView *v = app->record;
    if (!record_view_active(v)) return true;
    if (record_view_kind(v) == REC_NOTE) {
        if (record_view_dirty(v) && !save_note(app)) return false;
    } else if (record_unsaved_canon(app)) {
        bool armed = explicit_close && app->record_close_armed && GetTime() - app->record_close_armed_at < 5.0;
        if (!armed) {
            if (explicit_close) {
                app->record_close_armed = true;
                app->record_close_armed_at = GetTime();
            }
            set_status(app, explicit_close
                                ? "This canon text isn't committed. Commit or Revise to keep it, or press again to discard it."
                                : "Commit or discard the open canon entry first");
            return false;
        }
    }
    app->record_close_armed = false;
    record_view_close(v);
    if (app->focus == FOCUS_RECORD) app->focus = FOCUS_DOC;
    return true;
}

/* Open an entry in the panel. Returns false if the panel is still showing
 * something else (unsaved canon text that must be settled first): callers
 * must then stop, or they would act on the wrong entry (red-team 005). */
static bool show_record(App *app, RecordKind kind, const Record *rec, const char *content, const SourceRef *src, size_t ns)
{
    if (!close_record(app, false)) return false;
    if (!record_view_open(app->record, kind, rec, content, src, ns)) {
        set_status(app, "Could not open that entry");
        return false;
    }
    app->record_close_armed = false;
    app->canon_finding[0] = '\0';
    app->focus = FOCUS_RECORD;
    return true;
}

/* A source for the current document selection: an immutable revision
 * snapshot of the whole text, the range and the quote. */
static bool selection_source(App *app, SourceRef *s)
{
    memset(s, 0, sizeof(*s));
    size_t a, c;
    editor_get_selection(app->editor, &a, &c);
    size_t lo = a < c ? a : c, hi = a < c ? c : a;
    if (!editor_path(app->editor) || !app->project_dir[0] || hi == lo) return false;
    char *full = editor_text_utf8(app->editor);
    bool ok = full && revisions_snapshot(app->store, app->project_dir, editor_path(app->editor), full, s->revision, sizeof(s->revision));
    free(full);
    if (!ok) {
        set_status(app, "Could not record the source text in .scribodo/revisions");
        return false;
    }
    s->kind = SRC_DOC;
    snprintf(s->doc, sizeof(s->doc), "%s", base_name(editor_path(app->editor)));
    s->start = lo;
    s->len = hi - lo;
    s->quote = editor_range_utf8(app->editor, lo, hi - lo);
    app->lists_dirty = true; /* History may have a new snapshot */
    return s->quote != NULL;
}

/* ⌘⇧N: the selection becomes a saved note (or a blank one, without a selection). */
static void note_from_selection(App *app)
{
    if (!app->project_dir[0]) {
        set_status(app, "Open a document first: notes are kept with its project");
        return;
    }
    SourceRef s;
    bool have = selection_source(app, &s);
    bool shown = show_record(app, REC_NOTE, NULL, have ? s.quote : "", have ? &s : NULL, have ? 1 : 0);
    if (have && shown) {
        /* Saved right away: "Save as note" is the author's action. */
        RecordView *v = app->record;
        char id[40];
        records_new_id(REC_NOTE, id, sizeof(id));
        int ver = 0;
        if (records_append(app->store, app->project_dir, REC_NOTE, id, "active", s.quote, &s, 1, &ver)) {
            record_view_stored(v, id, ver, "active", s.quote);
            set_status(app, "Saved as a note \xC2\xB7 edit it below");
        } else {
            set_status(app, store_says(app, "Could not save the note to .scribodo/notes.jsonl"));
        }
        app->lists_dirty = true;
    }
    if (have) free(s.quote);
}

/* ⌘⇧C: a canon draft from the selection. Nothing is canon until Commit. */
static void canon_from_selection(App *app)
{
    SourceRef s;
    if (!selection_source(app, &s)) {
        if (!editor_path(app->editor) || !app->project_dir[0]) set_status(app, "Open a document first");
        else set_status(app, "Select the passage that establishes the fact, then press \xE2\x8C\x98\xE2\x87\xA7" "C");
        return;
    }
    show_record(app, REC_CANON, NULL, s.quote, &s, 1);
    free(s.quote);
}

/* A chat reply as the source of a note or canon draft. */
static void record_from_reply(App *app, long index, bool canon)
{
    if (!app->project_dir[0] || !app->chat_id[0] || strcmp(app->chat_project, app->project_dir) != 0) {
        set_status(app, "Open a document first: notes and canon are kept with its project");
        return;
    }
    chat_lock(app->chat);
    const ChatMessage *m = chat_message(app->chat, (size_t)index);
    ChatWindowInfo window = chat_window_info(app->chat);
    long source_sequence = m && m->sequence_known ? (long)m->sequence : index;
    char *text = m && !m->streaming && !m->excerpt && (!window.active || m->sequence_known) ? strdup(m->content) : NULL;
    chat_unlock(app->chat);
    if (!text) return;
    SourceRef s = { .kind = SRC_CHAT, .message = source_sequence, .start = 0, .quote = text };
    snprintf(s.chat, sizeof(s.chat), "%s", app->chat_id);
    size_t cps = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) cps += (*p & 0xC0) != 0x80;
    s.len = cps;
    if (canon) {
        show_record(app, REC_CANON, NULL, text, &s, 1);
    } else {
        if (show_record(app, REC_NOTE, NULL, text, &s, 1)) {
            char id[40];
            records_new_id(REC_NOTE, id, sizeof(id));
            int ver = 0;
            if (records_append(app->store, app->project_dir, REC_NOTE, id, "active", text, &s, 1, &ver)) {
                record_view_stored(app->record, id, ver, "active", text);
                set_status(app, "Saved the reply as a note");
            } else {
                set_status(app, store_says(app, "Could not save the note to .scribodo/notes.jsonl"));
            }
            app->lists_dirty = true;
        }
    }
    free(text);
}

/* Open a record's source: the document passage (found again if it moved)
 * or the chat it came from. */
static void open_source(App *app, const SourceRef *s)
{
    if (s->kind == SRC_CHAT) {
        if (s->message < 0 || s->message == LONG_MAX) { set_status(app, "Invalid source message"); return; }
        for (size_t i = 0; i < app->nchats; i++)
            if (!strcmp(app->chats[i].id, s->chat)) {
                if (database_history_project(app->project_dir)) {
                    (void)load_chat_window(app, app->project_dir, s->chat, app->chats[i].title,
                                           (ChatWindowRequest){ CHAT_WINDOW_FOCUS, s->message, s->start },
                                           strcmp(app->chat_id, s->chat) != 0 || strcmp(app->chat_project, app->project_dir) != 0);
                    return;
                }
                if (strcmp(app->chat_id, s->chat) != 0) open_chat(app, &app->chats[i]);
                char msg[120];
                snprintf(msg, sizeof(msg), "Source: message %ld of this chat", s->message + 1);
                if (!strcmp(app->chat_id, s->chat)) {
                    chat_lock(app->chat);
                    ChatWindowInfo window = chat_window_info(app->chat);
                    chat_unlock(app->chat);
                    long index = s->message;
                    if (window.active) {
                        if (!load_chat_window(app, app->chat_project, app->chat_id, NULL,
                                              (ChatWindowRequest){ CHAT_WINDOW_FOCUS, s->message, s->start }, false)) return;
                        return;
                    }
                    if (index < 0) { set_status(app, "That source message is unavailable"); return; }
                    chat_view_reveal(app->chat_view, index);
                    set_status(app, msg);
                }
                return;
            }
        set_status(app, "The source chat is no longer in this project");
        return;
    }
    /* The document may have been renamed since the source was made; the
     * source's snapshot name ("YYYYMMDD-HHMMSS-…") says when that was. */
    char doc[256];
    double made = 0;
    struct tm tmv = { 0 };
    const char *rest = strptime(s->revision, "%Y%m%d-%H%M%S", &tmv);
    if (rest) {
        tmv.tm_isdst = -1;
        made = (double)mktime(&tmv);
        if (rest[0] == '-' && rest[1] >= '0' && rest[1] <= '9') made += (double)atoi(rest + 1) / 1000.0; /* "-846" ms */
    }
    renames_resolve_since(app->project_dir, s->doc, made, doc, sizeof(doc));
    char full[PATH_MAX + 300];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, doc);
    if (!editor_path(app->editor) || strcmp(base_name(editor_path(app->editor)), doc) != 0) {
        if (editor_is_dirty(app->editor)) {
            set_status(app, "Save the current document before opening the source");
            return;
        }
        struct stat dst;
        if (stat(full, &dst) != 0) {
            set_status(app, "The source document is no longer in this project");
            return;
        }
        open_path(app, full);
        if (!editor_path(app->editor) || strcmp(base_name(editor_path(app->editor)), doc) != 0) return;
    }
    char *at = editor_range_utf8(app->editor, s->start, s->len);
    bool same = at && !strcmp(at, s->quote);
    free(at);
    size_t pos = s->start;
    if (same || editor_find_near_utf8(app->editor, s->quote, s->start, &pos)) {
        size_t cps = 0;
        for (const unsigned char *p = (const unsigned char *)s->quote; *p; p++) cps += (*p & 0xC0) != 0x80;
        editor_set_selection(app->editor, pos, pos + cps);
        set_status(app, same ? "Source passage selected" : "Source passage selected (it has moved since)");
    } else {
        char msg[400];
        snprintf(msg, sizeof(msg), "The source passage has changed since; the original is in History (%s)", s->revision);
        set_status(app, msg);
    }
    app->focus = FOCUS_DOC;
}

static void store_canon(App *app, const char *status, bool use_editor_text)
{
    RecordView *v = app->record;
    char *text = record_view_text(v);
    if (!text) return;
    if (!use_editor_text && record_view_dirty(v)) {
        set_status(app, "Revise to keep your edits (or undo them) first");
        free(text);
        return;
    }
    if (is_blank(text)) {
        set_status(app, "A canon entry needs an assertion");
        free(text);
        return;
    }
    char id[40];
    snprintf(id, sizeof(id), "%s", record_view_id(v));
    if (!id[0]) records_new_id(REC_CANON, id, sizeof(id));
    size_t ns;
    const SourceRef *src = record_view_sources(v, &ns);
    int ver = 0;
    if (app->project_dir[0] && records_append(app->store, app->project_dir, REC_CANON, id, status, text, src, ns, &ver)) {
        bool first = !record_view_id(v)[0];
        record_view_stored(v, id, ver, status, text);
        if (first && app->canon_finding[0]) { /* the finding it came from is now committed */
            observer_set_finding_status(app->observer, app->project_dir, app->canon_finding, "committed");
            app->canon_finding[0] = '\0';
        }
        set_status(app, !strcmp(status, "retracted") ? "Retracted \xC2\xB7 the entry and its history are kept"
                        : first                      ? "Committed to canon"
                                                     : "Canon revised \xC2\xB7 earlier versions are kept");
    } else {
        set_status(app, store_says(app, "Could not save to .scribodo/canon.jsonl"));
    }
    free(text);
    app->lists_dirty = true;
}

static void record_action(App *app, RecordAction act)
{
    RecordView *v = app->record;
    switch (act) {
    case RA_NONE: break;
    case RA_SAVE: save_note(app); break;
    case RA_TO_CANON: {
        if (record_view_dirty(v) && !save_note(app)) break;
        char *text = record_view_text(v);
        size_t ns;
        SourceRef *src = records_copy_sources(record_view_sources(v, &ns), ns);
        if (text) show_record(app, REC_CANON, NULL, text, src, src ? ns : 0);
        records_free_sources(src, src ? ns : 0);
        free(text);
        break;
    }
    case RA_DELETE: {
        char *text = record_view_text(v);
        size_t ns;
        const SourceRef *src = record_view_sources(v, &ns);
        bool ok = !record_view_id(v)[0] ||
                  records_append(app->store, app->project_dir, REC_NOTE, record_view_id(v), "deleted", text ? text : "", src, ns, NULL);
        free(text);
        if (ok) {
            record_view_close(v);
            if (app->focus == FOCUS_RECORD) app->focus = FOCUS_DOC;
            set_status(app, "Note deleted");
        } else {
            set_status(app, "Could not delete the note");
        }
        app->lists_dirty = true;
        break;
    }
    case RA_CLOSE: close_record(app, true); break;
    case RA_COMMIT:
    case RA_REVISE: store_canon(app, "committed", true); break;
    case RA_RETRACT: store_canon(app, "retracted", false); break;
    case RA_RECOMMIT: store_canon(app, "committed", false); break;
    case RA_SOURCE: {
        size_t ns;
        const SourceRef *src = record_view_sources(v, &ns);
        if (ns) {
            SourceRef *copy = records_copy_sources(src, 1); /* opening may change the panel */
            if (copy) open_source(app, copy);
            records_free_sources(copy, copy ? 1 : 0);
        }
        break;
    }
    }
}

/* ---- observer findings ----------------------------------------------------------- */

static void refresh_findings(App *app)
{
    records_free(app->flist, app->nflist);
    app->flist = NULL;
    app->nflist = 0;
    if (app->project_dir[0] &&
        !records_list_checked(app->store, app->project_dir, REC_FINDING, &app->flist, &app->nflist))
        set_status(app, "Couldn't load findings from the project store");
    findings_view_set(app->findings, app->flist, app->nflist, app->canon, app->ncanon);
    size_t fresh = 0;
    for (size_t i = 0; i < app->nflist; i++) fresh += !strcmp(record_current(&app->flist[i])->status, "new");
    if (fresh) snprintf(app->observer_button, sizeof(app->observer_button), "Observer (%zu)", fresh);
    else snprintf(app->observer_button, sizeof(app->observer_button), "Observer");
    refresh_entities(app); /* the graph follows the findings */
}

static const Record *find_finding(App *app, const char *id)
{
    for (size_t i = 0; i < app->nflist; i++)
        if (!strcmp(app->flist[i].id, id)) return &app->flist[i];
    return NULL;
}

static void finding_action(App *app, FindingAction act, const char *id)
{
    const Record *r = find_finding(app, id);
    if (!r || act == FA_NONE) return;
    const RecordVersion *v = record_current(r);
    switch (act) {
    case FA_NONE: break;
    case FA_DISCUSS: {
        /* The finding goes into the message box, visibly; nothing reaches
         * the assistant until the author sends it (SOW §4.3). */
        if (chat_busy(app->chat)) {
            set_status(app, "Wait for the reply to finish (or press Stop)");
            break;
        }
        char text[2400];
        int k = snprintf(text, sizeof(text), "About this observer finding: %s", v->content);
        if (v->nsources && v->sources[0].quote && v->sources[0].quote[0])
            k += snprintf(text + k, sizeof(text) - (size_t)k, "\nFrom %s: \xE2\x80\x9C%.600s\xE2\x80\x9D",
                          v->sources[0].kind == SRC_DOC ? v->sources[0].doc : "our chat", v->sources[0].quote);
        if (v->finding.related_text && !strcmp(v->finding.type, "conflict"))
            snprintf(text + k, sizeof(text) - (size_t)k, "\nCanon says: %.600s", v->finding.related_text);
        leave_edit_mode(app);
        /* Added to what the author is typing, as an undoable edit (red-team
         * 005: it used to replace an unsent message). */
        chat_view_append_input(app->chat_view, text);
        app->focus = FOCUS_CHAT;
        set_status(app, "The finding is in the message box \xC2\xB7 edit it and press Enter to discuss (\xE2\x8C\x98Z removes it)");
        break;
    }
    case FA_NOTE: {
        if (!close_record(app, false)) break; /* nothing is written unless the panel is free */
        char nid[40];
        records_new_id(REC_NOTE, nid, sizeof(nid));
        int ver = 0;
        if (!records_append(app->store, app->project_dir, REC_NOTE, nid, "active", v->content, v->sources, v->nsources, &ver)) {
            set_status(app, store_says(app, "Could not save the note to .scribodo/notes.jsonl"));
            break;
        }
        observer_set_finding_status(app->observer, app->project_dir, id, "saved");
        RecordVersion nv = { .ver = ver, .ts = time(NULL), .content = v->content, .sources = v->sources, .nsources = v->nsources };
        snprintf(nv.status, sizeof(nv.status), "active");
        Record nr = { .versions = &nv, .nversions = 1 };
        snprintf(nr.id, sizeof(nr.id), "%s", nid);
        show_record(app, REC_NOTE, &nr, NULL, NULL, 0);
        set_status(app, "Saved as a note");
        app->lists_dirty = true;
        break;
    }
    case FA_CANON: {
        char fid[40];
        snprintf(fid, sizeof(fid), "%s", id);
        if (show_record(app, REC_CANON, NULL, v->content, v->sources, v->nsources)) {
            snprintf(app->canon_finding, sizeof(app->canon_finding), "%s", fid);
            set_status(app, "A canon draft from the finding \xC2\xB7 state the fact, then Commit");
        }
        break;
    }
    case FA_DISMISS:
        if (!observer_set_finding_status(app->observer, app->project_dir, id, "dismissed"))
            set_status(app, store_says(app, "Could not update .scribodo/findings.jsonl"));
        break;
    case FA_SOURCE:
        if (v->nsources) {
            SourceRef *copy = records_copy_sources(v->sources, 1);
            if (copy) open_source(app, copy);
            records_free_sources(copy, copy ? 1 : 0);
        }
        break;
    }
}

/* The document area when nothing is open: a welcome screen (no project)
 * or a prompt to pick or create a document. Returns the button pressed:
 * 1 open folder, 2 new project, 3 sample, 10+i recent project i,
 * 20 new document, 21 import; 0 for none. */
static int draw_empty(App *app, Rectangle r)
{
    const SFont *big = &app->fonts[FONT_PROSE], *ui = &app->fonts[FONT_UI];
    Color text = { 0xDD, 0xDD, 0xDD, 0xFF }, muted = { 0x8B, 0x8B, 0x8B, 0xFF };
    float x = r.x + (r.width > 520 ? (r.width - 440) / 2 : 40), y = r.y + r.height * 0.18f;
    int act = 0;
    for (int i = 0; i < 3; i++) app->welcome_btn[i] = (Rectangle){ 0 };
    for (int i = 0; i < 2; i++) app->empty_btn[i] = (Rectangle){ 0 };
    for (size_t i = 0; i < RECENT_MAX; i++) app->recent_rect[i] = (Rectangle){ 0 };
    if (!app->project_dir[0]) {
        const char *t = "Welcome to Scribodo";
        sfont_draw_utf8(big, t, (int)strlen(t), x, y, text);
        const char *sub = "A project is a folder of Markdown or text documents.";
        sfont_draw_utf8(ui, sub, (int)strlen(sub), x, y + big->line_height + 6, muted);
        y += big->line_height + 40;
        const char *labels[3] = { "Open folder...", "New project...", "Open the sample" };
        for (int i = 0; i < 3; i++) {
            app->welcome_btn[i] = (Rectangle){ x + (float)i * 148, y, 140, 30 };
            if (GuiButton(app->welcome_btn[i], labels[i])) act = 1 + i;
        }
        y += 56;
        if (app->nrecents) {
            sfont_draw_utf8(ui, "RECENT", 6, x, y, muted);
            y += ui->line_height + 6;
            for (size_t i = 0; i < app->nrecents && i < RECENT_MAX; i++) {
                const char *name = base_name(app->recents[i].path);
                float row_w = r.x + r.width - 24 - (x - 6);
                Rectangle rr = { x - 6, y - 3, row_w < 440 ? row_w : 440, ui->line_height + 6 };
                app->recent_rect[i] = rr;
                bool hot = CheckCollisionPointRec(GetMousePosition(), rr) && !prompt_active(app->prompt) && !app->menu.active;
                if (hot) DrawRectangleRounded(rr, 0.2f, 4, (Color){ 0x2A, 0x2D, 0x2E, 0xFF });
                sfont_draw_utf8(ui, name, (int)strlen(name), x, y, (Color){ 0x6C, 0xA8, 0xE0, 0xFF });
                float nw = sfont_measure_utf8(ui, name, (int)strlen(name));
                /* The folder it lives in, with ~ for home, cut to fit. */
                char dir[PATH_MAX];
                const char *home = getenv("HOME");
                const char *p = app->recents[i].path;
                size_t hl = home ? strlen(home) : 0;
                if (hl && !strncmp(p, home, hl) && (p[hl] == '/' || p[hl] == '\0')) snprintf(dir, sizeof(dir), "~%s", p + hl);
                else snprintf(dir, sizeof(dir), "%s", p);
                char *slash = strrchr(dir, '/');
                if (slash && slash != dir) *slash = '\0';
                float avail = rr.x + rr.width - (x + nw + 12) - 6;
                int dl = (int)strlen(dir);
                if (sfont_measure_utf8(ui, dir, dl) > avail) {
                    float ell = sfont_measure_utf8(ui, "\xE2\x80\xA6", 3);
                    const char *tail = dir;
                    while (*tail && sfont_measure_utf8(ui, tail, (int)strlen(tail)) + ell > avail) {
                        tail++;
                        while (((unsigned char)*tail & 0xC0) == 0x80) tail++;
                    }
                    char cut[PATH_MAX + 4];
                    snprintf(cut, sizeof(cut), "\xE2\x80\xA6%s", tail);
                    snprintf(dir, sizeof(dir), "%s", cut);
                    dl = (int)strlen(dir);
                }
                if (avail > 20) sfont_draw_utf8(ui, dir, dl, x + nw + 12, y, muted);
                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) act = 10 + (int)i;
                y += ui->line_height + 8;
            }
        }
    } else {
        const char *t = "No document open";
        sfont_draw_utf8(big, t, (int)strlen(t), x, y, text);
        const char *sub = app->ndocs ? "Choose one in the Explorer, or start a new one." : "This folder has no documents yet.";
        sfont_draw_utf8(ui, sub, (int)strlen(sub), x, y + big->line_height + 6, muted);
        y += big->line_height + 40;
        app->empty_btn[0] = (Rectangle){ x, y, 150, 30 };
        app->empty_btn[1] = (Rectangle){ x + 158, y, 150, 30 };
        if (GuiButton(app->empty_btn[0], "New document")) act = 20;
        if (GuiButton(app->empty_btn[1], "Import...")) act = 21;
    }
    return act;
}

static void empty_action(App *app, int act, const char *sample)
{
    if (act == 1) choose_project(app, false);
    else if (act == 2) choose_project(app, true);
    else if (act == 3) open_path(app, sample);
    else if (act >= 10 && act < 10 + RECENT_MAX && (size_t)(act - 10) < app->nrecents) {
        RecentProject rp = app->recents[act - 10];
        open_project(app, rp.path);
        if (rp.doc[0] && !strcmp(app->project_dir, rp.path)) {
            char full[PATH_MAX + 260];
            snprintf(full, sizeof(full), "%s/%s", rp.path, rp.doc);
            struct stat st;
            if (stat(full, &st) == 0) open_path(app, full);
        }
    } else if (act == 20) begin_new_document(app);
    else if (act == 21) import_documents(app);
}

/* `s` shortened with an ellipsis to fit `maxw` (whole UTF-8 characters). */
static void fit_label(const SFont *f, const char *s, float maxw, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s", s);
    int n = (int)strlen(out);
    if (sfont_measure_utf8(f, out, n) <= maxw) return;
    float ell = sfont_measure_utf8(f, "\xE2\x80\xA6", 3);
    while (n > 0 && sfont_measure_utf8(f, out, n) + ell > maxw) {
        n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
    }
    if ((size_t)n + 4 <= outlen) snprintf(out + n, outlen - (size_t)n, "\xE2\x80\xA6");
}

/* The chat footer's attachments: "Attached:" then a chip per document
 * (with ×), "+N more" if they don't all fit, then "+ Attach". Everything
 * stays inside the row (red-team 007: a long name hid the only chip, and
 * "+ Attach" overlapped Send). Returns -2 for Attach or "+N more", i >= 0 to
 * remove chip i, -1 for nothing. */
static int draw_attach_row(App *app, Rectangle r)
{
    const SFont *ui = &app->fonts[FONT_UI];
    Color muted = { 0x8B, 0x8B, 0x8B, 0xFF }, chip_bg = { 0x2C, 0x30, 0x36, 0xFF }, text = { 0xD0, 0xD0, 0xD0, 0xFF };
    Color link = { 0x6C, 0xA8, 0xE0, 0xFF };
    float x = r.x, y = r.y + (r.height - ui->line_height) / 2, right = r.x + r.width;
    Vector2 m = GetMousePosition();
    bool click = !input_mouse_blocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    int act = -1;
    for (size_t i = 0; i < MAX_ATTACH; i++) app->attach_chip[i] = app->attach_x[i] = (Rectangle){ 0 };
    const char *lbl = app->nattached ? "Attached:" : "No documents attached";
    const char *add = "+ Attach";
    float add_w = sfont_measure_utf8(ui, add, (int)strlen(add)) + 8;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    sfont_draw_utf8(ui, lbl, (int)strlen(lbl), x, y, muted);
    x += sfont_measure_utf8(ui, lbl, (int)strlen(lbl)) + 8;
    float more_w = sfont_measure_utf8(ui, "+8 more", 7) + 10;
    size_t shown = 0;
    for (size_t i = 0; i < app->nattached; i++) {
        bool more_after = i + 1 < app->nattached;
        float limit = right - add_w - (more_after ? more_w : 0) - x; /* room for this chip */
        char name[300];
        snprintf(name, sizeof(name), "%s", app->attached[i]);
        float w = sfont_measure_utf8(ui, name, (int)strlen(name)) + 30;
        if (w > limit) {
            if (shown > 0 || limit < 60) break; /* the rest go under "+N more" */
            fit_label(ui, app->attached[i], limit - 30, name, sizeof(name)); /* the first chip always shows */
            w = sfont_measure_utf8(ui, name, (int)strlen(name)) + 30;
        }
        Rectangle chip = { x, r.y + 3, w, r.height - 6 };
        DrawRectangleRounded(chip, 0.5f, 6, chip_bg);
        sfont_draw_utf8(ui, name, (int)strlen(name), x + 8, y, text);
        Rectangle xr = { x + w - 20, r.y + 3, 18, r.height - 6 };
        bool hot = CheckCollisionPointRec(m, xr);
        sfont_draw_utf8(ui, "\xC3\x97", 2, xr.x + 4, y, hot ? (Color){ 0xF2, 0x8B, 0x82, 0xFF } : muted);
        app->attach_chip[i] = chip;
        app->attach_x[i] = xr;
        if (click && hot) act = (int)i;
        x += w + 6;
        shown++;
    }
    if (shown < app->nattached) {
        char more[32];
        snprintf(more, sizeof(more), "+%zu more", app->nattached - shown);
        Rectangle mr = { x, r.y + 3, sfont_measure_utf8(ui, more, (int)strlen(more)) + 4, r.height - 6 };
        sfont_draw_utf8(ui, more, (int)strlen(more), x, y, link);
        if (click && CheckCollisionPointRec(m, mr)) act = -2; /* the menu shows them all */
        x += mr.width + 6;
    }
    if (x + add_w > right) x = right - add_w;
    app->attach_add = (Rectangle){ x, r.y + 3, add_w, r.height - 6 };
    sfont_draw_utf8(ui, add, (int)strlen(add), x + 4, y, link);
    if (click && CheckCollisionPointRec(m, app->attach_add)) act = -2;
    EndScissorMode();
    return act;
}

static void send_chat(App *app, char *text)
{
    memory_document_names(app);
    if (chat_busy(app->chat)) {
        set_status(app, "A reply is already running; your input is kept. Stop it before sending");
        free(text); return;
    }
    if (app->history_pending) {
        set_status(app, "History is loading; your input is kept. Wait, or Cancel before sending");
        free(text); return;
    }
    if (memory_controls_pending(app)) {
        set_status(app, "Memory settings are not committed yet; your input is kept. Wait before sending");
        free(text); return;
    }
    if (app->project_dir[0] &&
        !(project_store_inspect(app->store, app->project_dir).capabilities & PROJECT_STORE_CAP_STRUCTURED_WRITE)) {
        set_status(app, app->store_problem == STORE_PROBLEM_COPY      ? "Chat is paused: this folder is a copy (make it its own project in the Explorer)"
                        : app->store_problem == STORE_PROBLEM_MISSING ? "Chat is paused: this project is missing from the store (restore it in the Explorer)"
                                                                      : "Chat is paused while the project store is offline (nothing would be saved)");
        free(text);
        return;
    }
    if (!app->chat_id[0]) {
        chat_store_new_id(app->chat_id, sizeof(app->chat_id));
        /* Title: the first line of the first message, shortened. */
        size_t line = strcspn(text, "\n");
        size_t n = utf8_clip(app->chat_title, sizeof(app->chat_title) - 4, text, line < 48 ? line : 48);
        if (strlen(text) > n) strcat(app->chat_title, "\xE2\x80\xA6");
        project_session_bind_chat(&app->session, app->project_dir, app->chat_id, app->chat_title);
    }
    if (project_store_inspect(app->store, app->chat_project).capabilities & PROJECT_STORE_CAP_LINKED_DOCUMENTS) {
        chat_lock(app->chat);
        ChatWindowInfo window = chat_window_info(app->chat);
        chat_unlock(app->chat);
        if (window.newer) {
            if (load_chat_window(app, app->chat_project, app->chat_id, NULL,
                                 (ChatWindowRequest){ .direction = CHAT_WINDOW_LATEST }, false)) {
                app->history_send_ready = true;
                set_status(app, "Loading latest history; input kept. Review and press Send again when ready");
            }
            free(text); return;
        }
        chat_enable_window(app->chat);
    }
    if (!chat_memory_configure(app->chat, app->store, app->semantic, app->chat_project, app->chat_id,
                               app->memory_experiment && database_history_project(app->chat_project))) {
        set_status(app, "Could not freeze local memory routing; your input was kept"); free(text); return;
    }
    /* The attached documents, read now (by reference: nothing is copied
     * into the project). An edit request uses the edited document alone. */
    ChatContext ctx = { 0 };
    char *doc = NULL;
    ChatDoc cdocs[MAX_ATTACH];
    char *texts[MAX_ATTACH] = { 0 };
    size_t ncd = 0;
    char missing[300] = "";
    for (size_t i = 0; i < app->nattached; i++) {
        texts[ncd] = attached_text(app, app->attached[i]);
        if (!texts[ncd]) {
            snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s%s", missing[0] ? ", " : "", app->attached[i]);
            continue;
        }
        cdocs[ncd] = (ChatDoc){ .title = app->attached[i], .text = texts[ncd],
            .unsaved = editor_path(app->editor) && !strcmp(base_name(editor_path(app->editor)), app->attached[i]) &&
                       editor_is_dirty(app->editor) };
        ncd++;
    }
    /* An edit request sends (and records) only the edited document. */
    ctx.docs = app->edit_mode ? NULL : cdocs;
    ctx.ndocs = app->edit_mode ? 0 : ncd;
    if (app->edit_mode && editor_path(app->editor) && is_attached(app, base_name(editor_path(app->editor)))) {
        doc = editor_text_utf8(app->editor);
        ctx.doc_title = base_name(editor_path(app->editor));
        ctx.doc_text = doc;
    }
    if (missing[0]) {
        char msg[400];
        snprintf(msg, sizeof(msg), "Not attached (can't be read): %s", missing);
        set_status(app, msg);
    }
    char err[256] = "";
    bool sent;
    size_t a_start = 0, a_len = 0;
    if (app->edit_mode && app->edit_text && !editor_anchor_get(app->editor, &a_start, &a_len)) {
        /* The passage was changed after ⌘K: the request would describe stale text. */
        leave_edit_mode(app);
        editor_anchor_clear(app->editor);
        sent = false;
        snprintf(err, sizeof(err), "The selected passage changed. Select it again and press \xE2\x8C\x98K.");
    } else if (app->edit_mode && app->edit_text) {
        sent = chat_send_edit(app->chat, text, app->edit_text, &ctx, err, sizeof(err));
        if (sent) {
            chat_lock(app->chat);
            size_t message_index = chat_count(app->chat) - 1;
            chat_unlock(app->chat);
            ReviewedEditResult tracked = reviewed_edit_controller_track_request(
                &app->reviewed_edit, message_index, app->edit_text, text, a_start, a_len);
            if (tracked != REVIEWED_EDIT_OK) {
                reviewed_edit_controller_leave_mode(&app->reviewed_edit);
                editor_anchor_clear(app->editor);
                set_status(app, tracked == REVIEWED_EDIT_OUT_OF_MEMORY
                                    ? "The edit was sent, but there wasn't enough memory to review its reply"
                                    : "The selected passage changed while the edit was being sent");
            }
        }
    } else {
        sent = chat_send(app->chat, text, &ctx, err, sizeof(err));
    }
    if (sent) {
        chat_view_clear_input(app->chat_view);
        if (err[0]) set_status(app, err);
    } else {
        set_status(app, err);
    }
    free(doc);
    for (size_t i = 0; i < ncd; i++) free(texts[i]);
    free(text);
}

/* ---- layout helpers ------------------------------------------------------------- */

static Clay_String cstr(FrameViewModel *frame, const char *s)
{
    const char *copy = frame_view_model_copy(frame, s);
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(copy), .chars = copy };
}

static void label(App *app, const char *s, int font, Clay_Color color)
{
    CLAY_TEXT(cstr(&app->frame, s), CLAY_TEXT_CONFIG({ .fontId = (uint16_t)font, .textColor = color,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
}


static void section_header(App *app, const char *s)
{
    CLAY({ .layout = { .padding = { 0, 0, 10, 4 } } }) { label(app, s, FONT_UI, COL_MUTED); }
}

/* A clickable list row. */
static void list_item(App *app, Clay_ElementId id, const char *s, bool active)
{
    bool hover = Clay_PointerOver(id);
    CLAY({ .id = id,
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) },
                       .padding = { 8, 8, 0, 0 },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = active ? COL_ACTIVE : hover ? COL_HOVER : (Clay_Color){ 0 },
           .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
        /* Clipped by the Explorer itself: a clip on every row made each a
         * Clay scroll container and overflowed Clay's limit (red-team 005). */
        label(app, s, FONT_UI, active ? COL_TEXT : COL_MUTED);
    }
}

/* Inactive placeholder text: deliberately not styled like a row. */
static void coming_soon(App *app, const char *s)
{
    CLAY({ .layout = { .padding = { 8, 8, 2, 2 } } }) { label(app, s, FONT_UI, COL_DIM); }
}

static void splitter(Clay_ElementId id, bool active)
{
    bool hot = active || Clay_PointerOver(id);
    CLAY({ .id = id,
           .layout = { .sizing = { CLAY_SIZING_FIXED(SPLITTER_W), CLAY_SIZING_GROW(0) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER } },
           .backgroundColor = hot ? COL_ACCENT : COL_PANEL }) {
        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_GROW(0) } },
               .backgroundColor = hot ? COL_ACCENT : COL_BORDER }) {}
    }
}

static void slot(Clay_ElementId id, float w, float h)
{
    CLAY({ .id = id, .layout = { .sizing = { CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(h) } } }) {}
}

static void grow(void)
{
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
}

static void update_labels(App *app)
{
    Editor *ed = app->editor;
    const char *path = editor_path(ed);
    const char *proj = app->project_dir[0] ? base_name(app->project_dir) : "";
    if (!path && !proj[0])
        snprintf(app->doc_label, sizeof(app->doc_label), "No project open");
    else if (!path)
        snprintf(app->doc_label, sizeof(app->doc_label), "%s", proj);
    else
        snprintf(app->doc_label, sizeof(app->doc_label), "%s \xE2\x80\xBA %s%s%s", proj, base_name(path),
                 editor_is_dirty(ed) ? "  \xE2\x97\x8F" : "", editor_is_read_only(ed) ? "  (read-only)" : "");

    EditorStats st = editor_stats(ed);
    if (st.selected)
        snprintf(app->status_left, sizeof(app->status_left), "Ln %d, Col %d (%zu selected)   %zu words   UTF-8   %s",
                 st.line, st.column, st.selected, st.words, st.crlf ? "CRLF" : "LF");
    else
        snprintf(app->status_left, sizeof(app->status_left), "Ln %d, Col %d   %zu words   UTF-8   %s",
                 st.line, st.column, st.words, st.crlf ? "CRLF" : "LF");

    char msg[480];
    LlmState ls = app->llm ? llm_server_state(app->llm, msg, sizeof(msg)) : LLM_READY;
    if (!app->llm) snprintf(msg, sizeof(msg), "Fake model (testing)");
    snprintf(app->model_label, sizeof(app->model_label), "\xE2\x97\x8F %s", msg);
    bool remote_on = app->chat && chat_remote_selected(app->chat);
    if (remote_on)
        snprintf(app->model_label, sizeof(app->model_label), "\xE2\x97\x8F Remote: %s \xC2\xB7 chat and attached documents go to %s",
                 app->remote.name, app->remote.host);
    if (remote_on) snprintf(app->remote_badge, sizeof(app->remote_badge), "\xE2\x86\x97 sent to %s", app->remote.host);
    else app->remote_badge[0] = '\0';
    if (!history_read_busy(app)) app->store_inspection = project_store_inspect(app->store, app->project_dir);
    app->store_problem = app->store_inspection.blocked_by == PROJECT_STORE_BLOCK_COPY      ? STORE_PROBLEM_COPY
                         : app->store_inspection.blocked_by == PROJECT_STORE_BLOCK_MISSING ? STORE_PROBLEM_MISSING
                                                                                           : 0;
    if (app->store_problem && !history_read_busy(app)) store_problem(app->store, app->project_dir, app->store_original, sizeof(app->store_original));
    if (!app->store_wanted) {
        app->store_label[0] = '\0';
    } else if (service_supervisor_busy(&app->services)) {
        snprintf(app->store_label, sizeof(app->store_label), "\xE2\x97\x8F Starting the project store\xE2\x80\xA6");
        app->store_color = COL_WARN;
    } else if (store_online(app->store) && app->store_problem) {
        snprintf(app->store_label, sizeof(app->store_label), "\xE2\x97\x8F Project store \xC2\xB7 %s",
                 app->store_problem == STORE_PROBLEM_COPY ? "this folder is a copy" : "project missing");
        app->store_color = COL_ERR;
    } else if (store_online(app->store)) {
        snprintf(app->store_label, sizeof(app->store_label), "\xE2\x97\x8F Project store");
        app->store_color = COL_OK;
    } else {
        snprintf(app->store_label, sizeof(app->store_label), "\xE2\x97\x8F Project store offline%s",
                  app->store_inspection.blocked_by != PROJECT_STORE_BLOCK_NONE
                      ? " \xC2\xB7 notes, canon, chats and History paused" : "");
        app->store_color = app->store_inspection.blocked_by != PROJECT_STORE_BLOCK_NONE ? COL_ERR : COL_WARN;
    }
    app->model_color = ls == LLM_READY ? COL_OK : ls == LLM_FAILED ? COL_ERR : COL_WARN;
    if (remote_on) app->model_color = COL_WARN; /* project content leaves this Mac */

    snprintf(app->chat_header, sizeof(app->chat_header), "%s", app->chat_title[0] ? app->chat_title : "New chat");
}

static Clay_RenderCommandArray build_layout(App *app)
{
    app->drawn_explorer_generation = app->explorer.generation;
    frame_view_model_begin(&app->frame);
    update_labels(app);
    Clay_BeginLayout();
    CLAY({ .id = CLAY_ID("Root"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = COL_BG }) {

        /* Toolbar */
        CLAY({ .id = CLAY_ID("Toolbar"),
               .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40) },
                           .padding = { 14, 14, 0, 0 }, .childGap = 14,
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = COL_PANEL,
               .border = { .color = COL_BORDER, .width = { .bottom = 1 } } }) {
            label(app, "Scribodo", FONT_UI, COL_TEXT);
            slot(CLAY_ID("OpenButton"), 64, 24);
            label(app, app->doc_label, FONT_UI, COL_MUTED);
            slot(CLAY_ID("SaveButton"), 64, 24);
            grow();
            if (app->store_label[0]) label(app, app->store_label, FONT_UI, app->store_color);
            /* The model in use; click to choose (when a remote is configured). */
            CLAY({ .id = CLAY_ID("ModelPicker"),
                   .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(26) }, .padding = { 8, 8, 0, 0 },
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = Clay_PointerOver(CLAY_ID("ModelPicker")) ? COL_HOVER : (Clay_Color){ 0 },
                   .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                label(app, app->model_label, FONT_UI, app->model_color);
            }
            slot(CLAY_ID("ObserverToggle"), 96, 24);
        }

        CLAY({ .id = CLAY_ID("Main"), .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
            /* Explorer */
            CLAY({ .id = CLAY_ID("Explorer"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(app->explorer_w), CLAY_SIZING_GROW(0) },
                               .padding = CLAY_PADDING_ALL(12), .childGap = 2,
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = COL_PANEL, .clip = { .horizontal = true, .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
                if (app->store_problem == STORE_PROBLEM_COPY) {
                    coming_soon(app, "This folder is a copy of a project in the store.");
                    coming_soon(app, "Notes, canon, chats and History are paused.");
                    list_item(app, CLAY_ID("StoreFixItem"), "Make it its own project", false);
                } else if (app->store_problem == STORE_PROBLEM_MISSING) {
                    coming_soon(app, "This project is missing from the store.");
                    coming_soon(app, "Notes, canon, chats and History are paused.");
                    list_item(app, CLAY_ID("StoreFixItem"), "Restore it from its saved files", false);
                } else if (app->store_inspection.blocked_by != PROJECT_STORE_BLOCK_NONE)
                    coming_soon(app, "Project store offline: notes, canon, chats and History are paused");
                section_header(app, "DOCUMENTS");
                const char *cur = editor_path(app->editor) ? base_name(editor_path(app->editor)) : "";
                if (!app->project_dir[0]) coming_soon(app, "Open a folder (\xE2\x8C\x98O)");
                else if (app->ndocs == 0) coming_soon(app, "No .md or .txt files yet");
                for (size_t i = 0; i < app->ndocs; i++)
                    list_item(app, CLAY_IDI("DocItem", (uint32_t)i), app->docs[i], !strcmp(cur, app->docs[i]));
                if (app->project_dir[0]) {
                    list_item(app, CLAY_ID("NewDocItem"), "+ New document", false);
                    list_item(app, CLAY_ID("ImportItem"), "Import\xE2\x80\xA6", false);
                }
                section_header(app, "CHATS");
                list_item(app, CLAY_ID("NewChatItem"), "+ New chat", false);
                for (size_t i = 0; i < app->nchats; i++)
                    list_item(app, CLAY_IDI("ChatItem", (uint32_t)i), app->chat_labels ? app->chat_labels[i] : app->chats[i].title,
                              !strcmp(app->chat_id, app->chats[i].id));
                if (!app->project_dir[0]) coming_soon(app, "Open a project to save chats");
                section_header(app, "HISTORY");
                if (app->nrevs == 0) coming_soon(app, editor_path(app->editor) ? "No revisions yet" : "Open a document");
                for (size_t i = 0; i < app->nrevs && i < REV_SHOW; i++)
                    list_item(app, CLAY_IDI("RevItem", (uint32_t)i), app->rev_labels[i], false);
                if (app->nrevs > REV_SHOW) coming_soon(app, "Older revisions are in .scribodo/revisions");
                section_header(app, "NOTES");
                const char *open_id = record_view_active(app->record) ? record_view_id(app->record) : "";
                if (app->project_dir[0]) list_item(app, CLAY_ID("NewNoteItem"), "+ New note", false);
                else coming_soon(app, "Open a project to keep notes");
                for (size_t i = 0; i < app->nnotes && i < REC_SHOW; i++)
                    if (app->note_labels[i][0])
                        list_item(app, CLAY_IDI("NoteItem", (uint32_t)i), app->note_labels[i], !strcmp(open_id, app->notes[i].id));
                section_header(app, "CANON");
                if (app->project_dir[0] && app->ncanon == 0) coming_soon(app, "Select a passage, then \xE2\x8C\x98\xE2\x87\xA7" "C");
                for (size_t i = 0; i < app->ncanon && i < REC_SHOW; i++)
                    list_item(app, CLAY_IDI("CanonItem", (uint32_t)i), app->canon_labels[i], !strcmp(open_id, app->canon[i].id));
                if (app->project_dir[0]) {
                    section_header(app, "PROJECT");
                    list_item(app, CLAY_ID("BackupItem"), backup_job_busy(&app->backup) ? "Backing up\xE2\x80\xA6" : "Back up now", false);
                    list_item(app, CLAY_ID("RestoreItem"), "Restore from a backup\xE2\x80\xA6", false);
                    list_item(app, CLAY_ID("NamesItem"), "Names\xE2\x80\xA6", false);
                    if (app->backup_label[0]) coming_soon(app, app->backup_label);
                }
                memset(app->drawn_ent, 0, sizeof(app->drawn_ent));
                memset(app->drawn_row_finding, 0, sizeof(app->drawn_row_finding));
                memset(app->drawn_row_other, 0, sizeof(app->drawn_row_other));
                if (app->store_inspection.capabilities & PROJECT_STORE_CAP_ENTITIES) {
                    section_header(app, "ENTITIES");
                    if (app->nents == 0) coming_soon(app, "The observer lists who and what appears here");
                    for (size_t i = 0; i < app->nents && i < ENT_SHOW; i++) {
                        bool open = !strcmp(app->ent_open, app->ents[i].id);
                        snprintf(app->drawn_ent[i], sizeof(app->drawn_ent[i]), "%s", app->ents[i].id);
                        list_item(app, CLAY_IDI("EntityItem", (uint32_t)i), app->ent_labels[i], open);
                        if (!open) continue;
                        /* Its relationships (click: the passage; \xE2\x80\xBA: go to the other one)
                         * and mentions (click: the passage). */
                        for (size_t k = 0; k < app->ent.nrows && k < ENT_ROWS; k++) {
                            snprintf(app->drawn_row_finding[k], sizeof(app->drawn_row_finding[k]), "%s", app->ent.rows[k].finding_id);
                            snprintf(app->drawn_row_other[k], sizeof(app->drawn_row_other[k]), "%s",
                                     app->ent.rows[k].is_link ? app->ent.rows[k].other_id : "");
                            if (!app->ent.rows[k].is_link) {
                                list_item(app, CLAY_IDI("EntRow", (uint32_t)k), app->ent_row_labels[k], false);
                                continue;
                            }
                            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) } } }) {
                                list_item(app, CLAY_IDI("EntRow", (uint32_t)k), app->ent_row_labels[k], false);
                                bool hover = Clay_PointerOver(CLAY_IDI("EntGo", (uint32_t)k));
                                CLAY({ .id = CLAY_IDI("EntGo", (uint32_t)k),
                                       .layout = { .sizing = { CLAY_SIZING_FIXED(24), CLAY_SIZING_FIXED(24) },
                                                   .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                                       .backgroundColor = hover ? COL_HOVER : (Clay_Color){ 0 },
                                       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                    label(app, "\xE2\x80\xBA", FONT_UI, COL_MUTED);
                                }
                            }
                        }
                        if (app->ent.nrows == 0) coming_soon(app, "  No relationships or mentions left");
                    }
                }
            }
            splitter(CLAY_ID("SplitLeft"), app->dragging == DRAG_EXPLORER);

            CLAY({ .id = CLAY_ID("Editor"), .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                   .backgroundColor = COL_BG }) {}

            splitter(CLAY_ID("SplitChat"), app->dragging == DRAG_CHAT);

            /* Chat */
            float input_h = chat_view_input_height(app->chat_view, app->chat_w);
            CLAY({ .id = CLAY_ID("Chat"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(app->chat_w), CLAY_SIZING_GROW(0) },
                               .padding = { 0, 0, 0, 10 }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = COL_PANEL }) {
                CLAY({ .id = CLAY_ID("ChatHeader"),
                       .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(36) },
                                   .padding = { 14, 10, 0, 0 }, .childGap = 10,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                       .border = { .color = COL_BORDER, .width = { .bottom = 1 } },
                       .clip = { .horizontal = true } }) {
                    label(app, "CHAT", FONT_UI, COL_MUTED);
                    label(app, app->chat_header, FONT_UI, COL_TEXT);
                    if (app->remote_badge[0]) label(app, app->remote_badge, FONT_UI, COL_WARN);
                    grow();
                    if (open_chat_stored(app)) slot(CLAY_ID("ObserverReads"), 190, 24);
                    slot(CLAY_ID("NewChatButton"), 80, 24);
                }
                if (app->memory_experiment) {
                    CLAY({ .id = CLAY_ID("MemoryStatus"),
                           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) }, .padding = { 12, 8, 4, 0 } },
                           .clip = { .horizontal = true } }) { label(app, app->memory_label, FONT_UI, COL_MUTED); }
                }
                CLAY({ .id = CLAY_ID("ChatMessages"),
                       .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
                if (app->suggest_text[0]) { /* a name noticed in chat, to confirm */
                    CLAY({ .id = CLAY_ID("NameSuggest"),
                           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34) },
                                       .padding = { 12, 10, 0, 0 }, .childGap = 8,
                                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                           .backgroundColor = COL_PANEL, .border = { .color = COL_BORDER, .width = { .top = 1 } } }) {
                        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                               .clip = { .horizontal = true } }) {
                            label(app, app->suggest_text, FONT_UI, COL_TEXT);
                        }
                        slot(CLAY_ID("NameSave"), 64, 24);
                        slot(CLAY_ID("NameSkip"), 72, 24);
                    }
                }
                CLAY({ .id = CLAY_ID("ChatFooter"),
                       .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                                   .padding = { 12, 10, 0, 0 }, .childGap = 8,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    if (app->edit_mode) {
                        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                               .clip = { .horizontal = true } }) {
                            label(app, app->edit_banner, FONT_UI, COL_WARN);
                        }
                        slot(CLAY_ID("CancelEdit"), 64, 24);
                    } else {
                        CLAY({ .id = CLAY_ID("AttachRow"),
                               .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
                    }
                    slot(CLAY_ID("SendButton"), 72, 24);
                }
                CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(input_h) },
                                   .padding = { 10, 10, 0, 0 } } }) {
                    CLAY({ .id = CLAY_ID("ChatInput"),
                           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
                }
            }

            if (app->observer_open) {
                splitter(CLAY_ID("SplitRight"), app->dragging == DRAG_OBSERVER);
                CLAY({ .id = CLAY_ID("Observer"),
                       .layout = { .sizing = { CLAY_SIZING_FIXED(app->observer_w), CLAY_SIZING_GROW(0) },
                                   .padding = CLAY_PADDING_ALL(12), .childGap = 8,
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM },
                       .backgroundColor = COL_PANEL, .clip = { .vertical = true } }) {
                    section_header(app, "OBSERVER");
                    label(app, app->observer_label, FONT_UI, COL_DIM);
                    if (app->store_inspection.capabilities & PROJECT_STORE_CAP_SEMANTIC)
                        list_item(app, CLAY_ID("RebuildIndex"), "Rebuild the index", false);
                    CLAY({ .id = CLAY_ID("Findings"),
                           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
                }
            }
        }

        /* Status bar */
        CLAY({ .id = CLAY_ID("StatusBar"),
               .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26) },
                           .padding = { 14, 14, 0, 0 }, .childGap = 16,
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = COL_PANEL,
               .border = { .color = COL_BORDER, .width = { .top = 1 } } }) {
            label(app, app->status_left, FONT_UI, COL_MUTED);
            grow();
            if (app->status[0] && GetTime() - app->status_time < 6.0)
                label(app, app->status, FONT_UI, editor_path(app->editor) && editor_is_read_only(app->editor) ? COL_WARN : COL_TEXT);
        }
    }
    Clay_RenderCommandArray commands = Clay_EndLayout();
    frame_view_model_seal(&app->frame);
    return commands;
}

static Rectangle element_rect_str(Clay_String s, uint32_t index, bool indexed)
{
    Clay_ElementId id = indexed ? Clay_GetElementIdWithIndex(s, index) : Clay_GetElementId(s);
    if (!clay_element_current(id)) return (Rectangle){ 0 }; /* not in this layout */
    Clay_ElementData d = Clay_GetElementData(id);
    return d.found ? clay_to_rect(d.boundingBox) : (Rectangle){ 0 };
}
#define ELEMENT_RECT(lit) element_rect_str(CLAY_STRING(lit), 0, false)

/* "Chat0" -> element "Chat" index 0; "ChatInput" -> "ChatInput". */
static Rectangle host_element(void *ctx, const char *name)
{
    App *app = ctx;
    if (!strcmp(name, "ReviewPanel")) return review_active(app->review) ? app->review_rect : (Rectangle){ 0 };
    if (!strcmp(name, "AcceptEdit")) return review_active(app->review) ? review_accept_rect(app->review_rect) : (Rectangle){ 0 };
    if (!strcmp(name, "ApplyLast")) return chat_view_last_apply_rect(app->chat_view);
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
    size_t n = strlen(name), k = n;
    while (k > 0 && name[k - 1] >= '0' && name[k - 1] <= '9') k--;
    Clay_String s = { .isStaticallyAllocated = false, .length = (int32_t)k, .chars = name };
    return element_rect_str(s, k < n ? (uint32_t)atoi(name + k) : 0, k < n);
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
static void host_store_restart(void *ctx)
{
    App *app = ctx;
    char database[64], why[256];
    store_database(app->store, database, sizeof(database));
    /* Deliberately no intent cancellation: exercise publication's epoch fence. */
    if (!store_start(app->store, database, why, sizeof(why))) set_status(app, why);
}
static void host_history_hold(void *ctx, bool hold) { atomic_store(&((App *)ctx)->history_hold, hold); }

static bool scripted_history_read(StoreManager *store, const char *project, const char *conversation,
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
    cap += (sizeof(app->ent_labels[0]) + 1) * ENT_SHOW + (sizeof(app->ent_row_labels[0]) + 1) * ENT_ROWS;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (size_t i = 0; i < app->ndocs; i++) strcat(strcat(s, app->docs[i]), "\n");
    for (size_t i = 0; i < app->nchats; i++) strcat(strcat(s, app->chat_labels ? app->chat_labels[i] : app->chats[i].title), "\n");
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
static void host_drop(void *ctx, const char *path)
{
    App *app = ctx; /* the same rule as a real drop: ignored behind a prompt or menu */
    if (!prompt_active(app->prompt) && !app->menu.active) drop_path(app, path);
}

static void clay_error(Clay_ErrorData err)
{
    fprintf(stderr, "clay: %.*s\n", err.errorText.length, err.errorText.chars);
}

static void style_raygui(const SFont *ui)
{
    GuiSetFont(ui->faces[0]);
    GuiSetStyle(DEFAULT, TEXT_SIZE, (int)ui->size);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    int controls[] = { BUTTON, TOGGLE, CHECKBOX };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        int c = controls[i];
        GuiSetStyle(c, BORDER_WIDTH, 1);
        GuiSetStyle(c, BASE_COLOR_NORMAL, ColorToInt((Color){ 0x31, 0x31, 0x31, 0xFF }));
        GuiSetStyle(c, BORDER_COLOR_NORMAL, ColorToInt((Color){ 0x4A, 0x4A, 0x4A, 0xFF }));
        GuiSetStyle(c, TEXT_COLOR_NORMAL, ColorToInt((Color){ 0xCC, 0xCC, 0xCC, 0xFF }));
        GuiSetStyle(c, BASE_COLOR_FOCUSED, ColorToInt((Color){ 0x3C, 0x3C, 0x3C, 0xFF }));
        GuiSetStyle(c, BORDER_COLOR_FOCUSED, ColorToInt((Color){ 0x00, 0x78, 0xD4, 0xFF }));
        GuiSetStyle(c, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
        GuiSetStyle(c, BASE_COLOR_PRESSED, ColorToInt((Color){ 0x00, 0x78, 0xD4, 0xFF }));
        GuiSetStyle(c, BORDER_COLOR_PRESSED, ColorToInt((Color){ 0x00, 0x78, 0xD4, 0xFF }));
        GuiSetStyle(c, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
        GuiSetStyle(c, BASE_COLOR_DISABLED, ColorToInt((Color){ 0x25, 0x25, 0x25, 0xFF }));
        GuiSetStyle(c, BORDER_COLOR_DISABLED, ColorToInt((Color){ 0x30, 0x30, 0x30, 0xFF }));
        GuiSetStyle(c, TEXT_COLOR_DISABLED, ColorToInt((Color){ 0x6B, 0x6B, 0x6B, 0xFF }));
    }
}

static bool load_fonts(App *app, float dpi)
{
    static const char *const ui_paths[] = {
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    };
    static const char *const prose_paths[] = {
        "/System/Library/Fonts/Supplemental/Georgia.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    };
    const char *override = getenv("SCRIBODO_PROSE_FONT");
    const char *prose_list[4];
    int np = 0;
    if (override) prose_list[np++] = override;
    for (size_t i = 0; i < sizeof(prose_paths) / sizeof(prose_paths[0]); i++) prose_list[np++] = prose_paths[i];

    bool ok = sfont_load(&app->fonts[FONT_UI], ui_paths, 3, 13.0f, 1.45f, dpi) &&
              sfont_load(&app->fonts[FONT_PROSE], prose_list, np, 18.0f, 1.6f, dpi) &&
              sfont_load(&app->fonts[FONT_CHAT], prose_list, np, 15.0f, 1.5f, dpi);
    for (int i = 0; i < FONT_COUNT; i++) app->font_ptrs[i] = &app->fonts[i];
    return ok;
}

/* ---- main ------------------------------------------------------------------------- */

static void app_destroy(App *app)
{
    if (!app) return;
    frame_view_model_destroy(&app->frame);
    recent_store_destroy(app->recent_store);
    app->recent_store = NULL;
    script_free(app->script);
    app->script = NULL;
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
    if (app->graph_job_initialized) entity_backfill_job_destroy(&app->graph_job);
    if (app->names_job_initialized) name_detection_job_destroy(&app->names_job);
    observer_destroy(app->observer); /* cancels its request before the server stops */
    chat_controller_destroy(&app->chat_controller); /* joins memory reads before dependencies below */
    editor_destroy(app->memory_sources_editor);
    semantic_client_destroy(app->semantic);
    app->semantic = NULL;
    if (app->services_initialized) (void)service_supervisor_destroy(&app->services);
    store_manager_destroy(app->store);
    app->store = NULL;
    findings_view_destroy(app->findings);
    records_free(app->flist, app->nflist);
    entity_list_free(app->ents, app->nents);
    entity_detail_free(&app->ent);
    llm_server_destroy(app->llm);
    llm_server_destroy(app->embed);
    editor_destroy(app->editor);
    explorer_snapshot_destroy(&app->explorer);
    project_session_destroy(&app->session);
    for (int i = 0; i < FONT_COUNT; i++) sfont_unload(&app->fonts[i]);
    free(app->clay_buf);
    app->clay_buf = NULL;
    if (app->window_ready) {
        CloseWindow();
        app->window_ready = false;
    }
    memset(app, 0, sizeof(*app));
}

int main(int argc, char **argv)
{
    static App app;
    atomic_init(&app.history_hold, false);
    app.store = store_manager_create();
    if (!app.store) return 1;
    frame_view_model_init(&app.frame);
    project_session_init(&app.session);
    explorer_snapshot_init(&app.explorer);
    chat_controller_init(&app.chat_controller);
    reviewed_edit_controller_init(&app.reviewed_edit);
    if (!entity_backfill_job_init(&app.graph_job, app.store)) {
        app_destroy(&app);
        return 1;
    }
    app.graph_job_initialized = true;
    if (!name_detection_job_init(&app.names_job, NULL)) {
        app_destroy(&app);
        return 1;
    }
    app.names_job_initialized = true;
    const char *path = argc > 1 ? argv[1] : NULL;

    /* UI test mode: SCRIBODO_SCRIPT=file.scribo runs a scripted session in a
     * hidden window (no focus stealing, immune to the real mouse) and exits
     * non-zero if any expectation fails. See src/app/script.h. */
    const char *script_path = getenv("SCRIBODO_SCRIPT");
    app.history_job = script_path ? chat_history_job_create_with_reader(app.store, scripted_history_read, &app)
                                  : chat_history_job_create(app.store);
    if (!app.history_job) { app_destroy(&app); return 1; }
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
        app_destroy(&app);
        return 3;
    }
    app.window_ready = true;
    SetWindowMinSize(900, 480);
    SetExitKey(KEY_NULL); /* Esc must never quit a writing app */
    SetTargetFPS(60);

    if (!backup_job_init(&app.backup, app.store)) {
        fprintf(stderr, "scribodo: couldn't initialise the backup worker\n");
        app_destroy(&app);
        return 1;
    }
    app.backup_initialized = true;
    if (!service_supervisor_init(&app.services, app.store)) {
        fprintf(stderr, "scribodo: couldn't initialise the service supervisor\n");
        app_destroy(&app);
        return 1;
    }
    app.services_initialized = true;
    app.explorer_w = 210;
    app.observer_w = 260;
    app.chat_w = 420;
    app.focus = FOCUS_DOC;

    float dpi = GetWindowScaleDPI().x;
    if (!load_fonts(&app, dpi)) {
        fprintf(stderr, "scribodo: no usable font found (set SCRIBODO_PROSE_FONT)\n");
        app_destroy(&app);
        return 1;
    }
    style_raygui(&app.fonts[FONT_UI]);

    uint32_t clay_mem = Clay_MinMemorySize();
    void *clay_buf = malloc(clay_mem);
    if (!clay_buf) {
        fprintf(stderr, "scribodo: out of memory\n");
        app_destroy(&app);
        return 1;
    }
    app.clay_buf = clay_buf;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem, clay_buf);
    Clay_Initialize(arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_error, 0 });
    Clay_SetMeasureTextFunction(clay_measure_text, app.font_ptrs);

    /* The local model. UI scripts use a fake model unless SCRIBODO_LLM=real. */
    /* The author's model settings (UI scripts keep their own). */
    if (script_path && !getenv("SCRIBODO_MODEL_SETTINGS")) {
        char mp[PATH_MAX];
        snprintf(mp, sizeof(mp), "%s/model-settings.json", getenv("SCRIBODO_SCRIPT_DIR") ? getenv("SCRIBODO_SCRIPT_DIR") : ".");
        setenv("SCRIBODO_MODEL_SETTINGS", mp, 1);
    }
    model_settings_load(&app.model);
    const char *llm_mode = getenv("SCRIBODO_LLM");
    bool fake = (llm_mode && !strcmp(llm_mode, "fake")) || (script_path && !(llm_mode && !strcmp(llm_mode, "real")));
    if (!fake) {
        LlmServerConfig cfg, ecfg;
        llm_server_default_config(&cfg, GetApplicationDirectory());
        llm_server_embed_config(&ecfg, GetApplicationDirectory());
        /* The author's context per conversation, as far as memory allows
         * (both servers are about to start). */
        const ModelSettingInfo *ci = model_setting_info(7);
        MemFit fit = memfit_context(sysmem_available(), llm_server_mem_need(&ecfg, ecfg.ctx_total), &cfg, app.model.context,
                                    (int)ci->min, (int)ci->step);
        memfit_message(&fit, app.model.context, app.mem_msg, sizeof(app.mem_msg));
        if (app.mem_msg[0]) fprintf(stderr, "scribodo: %s\n", app.mem_msg);
        cfg.ctx_total = fit.slot_ctx * (cfg.slots > 0 ? cfg.slots : 1);
        app.llm = llm_server_create(&cfg);
        if (app.llm) llm_server_start(app.llm);
        /* Embeddings for the semantic index (a second, small server). */
        app.embed = llm_server_create(&ecfg);
        if (app.embed) llm_server_start(app.embed);
        if (!app.llm || !app.embed) {
            fprintf(stderr, "scribodo: could not allocate the pinned local model supervisors (no fake fallback)\n");
            app_destroy(&app); return 1;
        }
    }
    app.semantic = semantic_client_create(app.store, app.embed);
    const char *memory_flag = getenv("SCRIBODO_MEMORY_EXPERIMENT");
    app.memory_experiment = memory_flag && !strcmp(memory_flag, "1");
    if (app.memory_experiment) {
        app.memory_job = memory_job_create(app.store, app.semantic, app.llm);
        if (!app.memory_job) { app_destroy(&app); return 1; }
    }
    ChatSession *owned_chat = chat_create(app.llm);
    name_detection_job_set_server(&app.names_job, app.llm);
    /* TLS secrets are never written out for a remote model's traffic. */
    unsetenv("SSLKEYLOGFILE");
    /* A remote model only if the author configured one; never selected here. */
    app.remote_ok = remote_load(&app.remote, app.remote_why, sizeof(app.remote_why));
    if (!app.remote_ok && app.remote_why[0]) {
        char msg[300];
        snprintf(msg, sizeof(msg), "The remote model isn't available: %s", app.remote_why);
        set_status(&app, msg);
    }
    app.editor = editor_create(&app.fonts[FONT_PROSE]);
    editor_on_will_change(app.editor, memory_editor_will_change, &app);
    ChatView *chat_view = owned_chat ? chat_view_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI], owned_chat) : NULL;
    ReviewView *review = review_create(&app.fonts[FONT_PROSE], &app.fonts[FONT_UI]);
    if (!chat_controller_adopt(&app.chat_controller, owned_chat, chat_view)) {
        chat_view_destroy(chat_view);
        chat_destroy(owned_chat);
    }
    app.memory_sources_editor = editor_create(&app.fonts[FONT_CHAT]);
    if (!app.memory_sources_editor) { app_destroy(&app); return 1; }
    editor_set_modal_owner(app.memory_sources_editor, true);
    if (!reviewed_edit_controller_adopt(&app.reviewed_edit, app.editor, review)) review_destroy(review);
    app.record = record_view_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI]);
    app.prompt = prompt_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI]);
    app.observer = observer_create(app.store, app.llm, app.semantic);
    app.findings = findings_view_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI]);
    app.findings_version = UINT64_MAX;
    /* UI scripts start with the observer off (deterministic files); a
     * script turns it on with "observer on". */
    if (app.observer && script_path) observer_set_enabled(app.observer, false);
    if (!app.chat || !app.editor || !app.chat_view || !app.review || !app.record || !app.observer || !app.semantic ||
        !app.findings || !app.prompt) {
        fprintf(stderr, "scribodo: out of memory\n");
        app_destroy(&app);
        return 1;
    }
    chat_on_message_done(app.chat, on_message_done, &app);
    /* Who is who. UI scripts keep their own profile, never the author's. */
    if (script_path && !getenv("SCRIBODO_PROFILE")) {
        char pp[PATH_MAX];
        snprintf(pp, sizeof(pp), "%s/profile.json", getenv("SCRIBODO_SCRIPT_DIR") ? getenv("SCRIBODO_SCRIPT_DIR") : ".");
        setenv("SCRIBODO_PROFILE", pp, 1);
    }
    profile_load(&app.profile);
    apply_profile(&app);
    apply_model_settings(&app, false);
    if (app.mem_msg[0]) set_status(&app, app.mem_msg);
    if (!app.profile.asked && !script_path) begin_names(&app); /* asked once, on first launch */

    /* Default to the bundled sample, located relative to the executable
     * (build/scribodo) rather than the working directory, which is "/" when
     * launched from Finder. */
    char sample[2048];
    snprintf(sample, sizeof(sample), "%s../assets/sample/sample.md", GetApplicationDirectory());
    /* Recent projects live in the app's support folder; a script keeps its
     * own list in its fixture folder. */
    {
        char state[PATH_MAX];
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') { /* Finder launches have HOME; be safe anyway */
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        if (script_path) snprintf(state, sizeof(state), "%s/.scribodo-state", getenv("SCRIBODO_SCRIPT_DIR") ? getenv("SCRIBODO_SCRIPT_DIR") : ".");
        else snprintf(state, sizeof(state), "%s/Library/Application Support/Scribodo", home ? home : ".");
        app.recent_store = recent_store_create(state);
        if (!app.recent_store) {
            fprintf(stderr, "scribodo: out of memory\n");
            app_destroy(&app);
            return 1;
        }
        app.nrecents = recent_store_load(app.recent_store, app.recents, RECENT_MAX);
    }
    dialogs_script_mode(script_path != NULL);
    ScriptHost host = { app.editor, &app.explorer_w, &app.observer_w, &app.editor_rect,
                        host_status, host_open, host_drop, &app, app.chat, host_element, host_model_ready,
                        host_record_text, host_observer_enable, host_observer_idle, host_findings_text,
                        host_attached_text, host_explorer_text, host_observer_status, host_history_cache_bytes, host_store_offline,
                        host_history_busy, host_history_hold, host_history_worker_busy, host_store_restart,
                        host_memory_settled, host_memory_enabled, host_memory_sources, host_memory_documents_settled };
    if (script_path) {
        char err[256];
        app.script = script_load(script_path, getenv("SCRIBODO_SCRIPT_DIR"), getenv("SCRIBODO_SHOT_DIR"), err, sizeof(err));
        if (!app.script) {
            fprintf(stderr, "scribodo: %s\n", err);
            app_destroy(&app);
            return 2;
        }
        printf("script %s\n", script_path);
    }
    /* The project store: on for the app (SCRIBODO_STORE=off disables it);
     * UI scripts use files unless SCRIBODO_STORE=db. */
    {
        const char *sm = getenv("SCRIBODO_STORE");
        app.store_wanted = script_path ? (sm && !strcmp(sm, "db")) : !(sm && !strcmp(sm, "off"));
        if (app.store_wanted) store_begin(&app);
    }
    if (!script_path) {
        struct stat pst;
        if (path && stat(path, &pst) == 0 && S_ISDIR(pst.st_mode)) {
            open_project(&app, path);
        } else if (path) {
            open_path(&app, path);
        } else if (app.nrecents) { /* where the author left off */
            empty_action(&app, 10, sample);
        }
        /* otherwise: the welcome screen */
    }

    bool closed_by_window = false;

    /* Debug hook: SCRIBODO_SCREENSHOT=out.png renders a few frames, saves the
     * framebuffer, and exits. */
    const char *screenshot_path = getenv("SCRIBODO_SCREENSHOT");
    int frame = 0;
    bool trace = getenv("SCRIBODO_INPUT_TRACE") != NULL;

    int cursor_shape = MOUSE_CURSOR_DEFAULT;
    /* Asked to quit by a signal (kill, logout, a terminal's ^C): leave the
     * loop so the model servers are stopped below, not left running. */
    struct sigaction sa = { 0 };
    sa.sa_handler = on_quit_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    bool running = true;
    while (running) {
        if (g_quit_signal) {
            fprintf(stderr, "scribodo: quitting (signal %d)\n", (int)g_quit_signal);
            break;
        }
        /* Scripted input is injected before anything reads input state. */
        if (app.script && !script_frame(app.script, &host)) break;

        if (app.llm) llm_server_tick(app.llm);
        if (app.embed) llm_server_tick(app.embed);
        /* While the store is wanted but offline, probe every 15 s; when it
         * answers, connect (on this thread), adopt the project, refresh. */
        if (app.store_wanted && !store_online(app.store) && !service_supervisor_busy(&app.services) && GetTime() >= app.store_next_probe) {
            app.store_next_probe = GetTime() + 15.0;
            store_try_later(&app);
        }
        ServiceSupervisorResult service_result;
        if (service_supervisor_poll(&app.services, &service_result)) {
            if (service_result.command_attempted && service_result.command_succeeded) app.services_started = true;
            if (service_result.reachable) {
                cancel_history(&app);
                app.store_why[0] = '\0';
                store_start(app.store, getenv("SCRIBODO_DB_NAME"), app.store_why, sizeof(app.store_why));
            }
            if (store_online(app.store)) {
                store_adopt_project(&app);
                app.lists_dirty = true;
                app.findings_version = UINT64_MAX;
                if (app.observer) observer_poke(app.observer);
            }
        }
        backup_poll(&app);
        names_tick(&app);
        names_poll(&app);
        /* A context size chosen while a reply was being written: now. */
        if (app.llm && app.ctx_pending && !chat_busy(app.chat) && !name_detection_job_busy(&app.names_job)) {
            app.ctx_pending = false;
            fit_context(&app, false);
        }
        bool graph_succeeded = false;
        if (entity_backfill_job_poll(&app.graph_job, &graph_succeeded)) { /* the graph is built: show it */
            app.lists_dirty = true;
        }
        chat_poll(app.chat);
        if (chat_trim_saved(app.chat)) chat_view_history_changed(app.chat_view);
        /* The observer shares the model: it yields whenever chat is active. */
        observer_set_chat_active(app.observer, chat_busy(app.chat));
        if (!history_read_busy(&app) && observer_version(app.observer) != app.findings_version) {
            app.findings_version = observer_version(app.observer);
            refresh_findings(&app);
        }
        {
            char st[200];
            observer_status(app.observer, st, sizeof(st));
            snprintf(app.observer_label, sizeof(app.observer_label), "%s", st);
        }
        const char *cn = chat_take_notice(app.chat);
        if (cn) set_status(&app, cn);
        if (app.clear_chat_when_idle && !chat_busy(app.chat)) {
            app.clear_chat_when_idle = false;
            chat_lock(app.chat);
            size_t nmsg = chat_count(app.chat);
            chat_unlock(app.chat);
            if (!app.chat_project[0] && nmsg > 0 && app.project_dir[0]) {
                adopt_chat(&app);
            } else if (strcmp(app.chat_project, app.project_dir) != 0) {
                if (unsaved_chat(&app)) {
                    set_status(&app, "Unsaved chat kept with the previous project; copy it or explicitly start a new chat");
                } else {
                    chat_clear(app.chat);
                    chat_view_history_changed(app.chat_view);
                    project_session_clear_chat(&app.session);
                    attach_reset(&app); /* the old project's documents aren't this one's */
                }
            }
        }
        history_poll(&app);
        memory_tick(&app);
        /* Changes made here mark the lists at once; this catches the rest
         * (files added in Finder, other windows). Store queries cost more
         * than a directory read, so they're polled less often. */
        double every = (app.store_inspection.capabilities & PROJECT_STORE_CAP_SLOW_REFRESH) ? 10.0 : 3.0;
        if (GetTime() - app.lists_refreshed > every) app.lists_dirty = true;

        /* Close requests: never lose unsaved prose silently. */
        if (WindowShouldClose()) {
            /* Notes save themselves; uncommitted canon text counts as unsaved. */
            bool note_lost = record_view_active(app.record) && record_view_kind(app.record) == REC_NOTE &&
                             record_view_dirty(app.record) && !save_note(&app);
            bool unsaved = editor_is_dirty(app.editor) || record_unsaved_canon(&app) || note_lost || unsaved_chat(&app) || memory_controls_pending(&app);
            if (!unsaved || (app.close_armed && GetTime() - app.close_armed_at < 5.0)) {
                running = false;
                closed_by_window = true;
                break;
            }
            glfwSetWindowShouldClose(GetWindowHandle(), 0);
            app.close_armed = true;
            app.close_armed_at = GetTime();
            set_status(&app, memory_controls_pending(&app)
                ? "Memory settings are not committed yet; wait, or close again to discard pending settings and other unsaved changes"
                : "Unsaved changes — save or copy what you need, or close again to discard");
        }

        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0 && !prompt_active(app.prompt) && !app.menu.active) drop_path(&app, dropped.paths[0]);
            UnloadDroppedFiles(dropped);
        }

        Vector2 mouse = GetMousePosition();
        bool mouse_down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y }, mouse_down);
        /* The Explorer scrolls (long History / chat lists). */
        Vector2 wheel = GetMouseWheelMoveV();
        Clay_UpdateScrollContainers(false, (Clay_Vector2){ wheel.x * 30.0f, wheel.y * 30.0f }, GetFrameTime());
        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });

        /* A prompt or menu takes all input until it closes. */
        bool modal = prompt_active(app.prompt) || app.menu.active || app.settings_open || app.memory_sources_open;
        input_block_mouse(modal);
        /* Clicks (pointer state reflects last frame's layout). */
        if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (trace) fprintf(stderr, "trace: click (%.0f, %.0f) focus=%d\n", mouse.x, mouse.y, app.focus);
            if (Clay_PointerOver(CLAY_ID("SplitLeft"))) app.dragging = DRAG_EXPLORER;
            else if (Clay_PointerOver(CLAY_ID("SplitChat"))) app.dragging = DRAG_CHAT;
            else if (app.observer_open && Clay_PointerOver(CLAY_ID("SplitRight"))) app.dragging = DRAG_OBSERVER;
            else if (record_view_active(app.record) && CheckCollisionPointRec(mouse, record_view_editor_rect(app.record)))
                app.focus = FOCUS_RECORD;
            else if (record_view_active(app.record) && CheckCollisionPointRec(mouse, app.record_rect)) {
                /* panel buttons and links: handled when it draws */
            } else if (Clay_PointerOver(CLAY_ID("Editor"))) app.focus = FOCUS_DOC;
            else if (Clay_PointerOver(CLAY_ID("ChatInput"))) app.focus = FOCUS_CHAT;
            else if (Clay_PointerOver(CLAY_ID("NewChatItem"))) new_chat(&app);
            else if (app.observer_open && Clay_PointerOver(CLAY_ID("RebuildIndex"))) {
                observer_request_rebuild(app.observer);
                set_status(&app, "Rebuilding the semantic index from the project store\xE2\x80\xA6");
            }
            else if (app.store_problem && Clay_PointerOver(CLAY_ID("StoreFixItem"))) store_resolve_problem(&app);
            else if (Clay_PointerOver(CLAY_ID("NewDocItem"))) begin_new_document(&app);
            else if (Clay_PointerOver(CLAY_ID("BackupItem"))) begin_backup(&app);
            else if (Clay_PointerOver(CLAY_ID("RestoreItem"))) begin_restore(&app);
            else if (Clay_PointerOver(CLAY_ID("NamesItem"))) begin_names(&app);
            else if (Clay_PointerOver(CLAY_ID("ImportItem"))) import_documents(&app);
            else if (Clay_PointerOver(CLAY_ID("NewNoteItem"))) {
                if (close_record(&app, false) && record_view_open(app.record, REC_NOTE, NULL, "", NULL, 0))
                    app.focus = FOCUS_RECORD;
            }
            else if (Clay_PointerOver(CLAY_ID("Explorer"))) {
                app.focus = FOCUS_NONE;
                char full[PATH_MAX + 256];
                for (size_t i = 0; i < app.ndocs; i++)
                    if (Clay_PointerOver(CLAY_IDI("DocItem", (uint32_t)i))) {
                        const char *doc = explorer_snapshot_doc(&app.explorer, app.drawn_explorer_generation,
                                                                app.session.generation, i);
                        if (doc) {
                            snprintf(full, sizeof(full), "%s/%s", app.project_dir, doc);
                            drop_path(&app, full);
                            app.focus = FOCUS_DOC;
                        }
                        break;
                    }
                for (size_t i = 0; i < app.nchats; i++)
                    if (Clay_PointerOver(CLAY_IDI("ChatItem", (uint32_t)i))) {
                        const ChatSummary *chat = explorer_snapshot_chat(&app.explorer, app.drawn_explorer_generation,
                                                                         app.session.generation, i);
                        if (chat) {
                            open_chat(&app, chat);
                            app.focus = FOCUS_CHAT;
                        }
                        break;
                    }
                for (size_t i = 0; i < app.nrevs && i < REV_SHOW; i++)
                    if (Clay_PointerOver(CLAY_IDI("RevItem", (uint32_t)i))) {
                        Revision r = app.revs[i];
                        restore_revision(&app, &r);
                        app.focus = FOCUS_DOC;
                        break;
                    }
                /* (Lists are only refreshed after click handling, so these
                 * records stay valid while the panel copies them.) */
                for (size_t i = 0; i < app.nnotes && i < REC_SHOW; i++)
                    if (app.note_labels[i][0] && Clay_PointerOver(CLAY_IDI("NoteItem", (uint32_t)i))) {
                        show_record(&app, REC_NOTE, &app.notes[i], NULL, NULL, 0);
                        break;
                    }
                for (size_t i = 0; i < app.ncanon && i < REC_SHOW; i++)
                    if (Clay_PointerOver(CLAY_IDI("CanonItem", (uint32_t)i))) {
                        show_record(&app, REC_CANON, &app.canon[i], NULL, NULL, 0);
                        break;
                    }
                /* Entities: copy what's needed first (opening refreshes the lists). */
                for (size_t i = 0; i < ENT_SHOW; i++)
                    if (app.drawn_ent[i][0] && Clay_PointerOver(CLAY_IDI("EntityItem", (uint32_t)i))) {
                        char id[40];
                        snprintf(id, sizeof(id), "%s", app.drawn_ent[i]);
                        toggle_entity(&app, id);
                        break;
                    }
                for (size_t k = 0; app.ent_open[0] && k < ENT_ROWS; k++) {
                    char id[40];
                    if (app.drawn_row_other[k][0] && Clay_PointerOver(CLAY_IDI("EntGo", (uint32_t)k))) {
                        snprintf(id, sizeof(id), "%s", app.drawn_row_other[k]);
                        toggle_entity(&app, id);
                        break;
                    }
                    if (app.drawn_row_finding[k][0] && Clay_PointerOver(CLAY_IDI("EntRow", (uint32_t)k))) {
                        snprintf(id, sizeof(id), "%s", app.drawn_row_finding[k]);
                        open_finding_source(&app, id);
                        break;
                    }
                }
            }
            /* Toolbar, chat header/footer and message list keep the focus. */
        }
        if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && Clay_PointerOver(CLAY_ID("ModelPicker"))) {
            static char items[3][240];
            const char *list[3];
            int n = 0;
            if (app.remote_ok) {
                snprintf(items[0], sizeof(items[0]), "%s Local \xC2\xB7 LFM3-1.7B BF16 (nothing leaves this Mac)",
                         chat_remote_selected(app.chat) ? "  " : "\xE2\x9C\x93");
                snprintf(items[1], sizeof(items[1]), "%s Remote \xC2\xB7 %s (sends chat and attached documents to %s)",
                         chat_remote_selected(app.chat) ? "\xE2\x9C\x93" : "  ", app.remote.name, app.remote.host);
                list[0] = items[0];
                list[1] = items[1];
                n = 2;
            } else if (app.remote_why[0]) {
                set_status(&app, app.remote_why); /* a remote is configured but unusable: say why */
            }
            snprintf(items[2], sizeof(items[2]), "   Model settings\xE2\x80\xA6");
            app.model_menu_settings = n;
            list[n++] = items[2];
            Rectangle pr = ELEMENT_RECT("ModelPicker");
            menu_open(&app.menu, MT_MODEL, 0, (Vector2){ pr.x, pr.y + pr.height + 4 }, list, n);
        }
        if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && Clay_PointerOver(CLAY_ID("Explorer"))) {
            for (size_t i = 0; i < app.nchats; i++)
                if (Clay_PointerOver(CLAY_IDI("ChatItem", (uint32_t)i))) {
                    const ChatSummary *chat = explorer_snapshot_chat(&app.explorer, app.drawn_explorer_generation,
                                                                     app.session.generation, i);
                    if (!chat) break;
                    static char reads_item[64];
                    snprintf(reads_item, sizeof(reads_item), "%s Observer reads this chat",
                             chat->observer_ignore ? "  " : "\xE2\x9C\x93");
                    const char *items[] = { "Rename\xE2\x80\xA6", reads_item, "Enable local memory (experimental)",
                                            "Disable local memory", "Rebuild local memory" };
                    snprintf(app.menu_chat, sizeof(app.menu_chat), "%s", chat->id);
                    menu_open(&app.menu, MT_CHAT, (int)i, mouse, items, app.memory_experiment ? 5 : 2);
                    break;
                }
            for (size_t i = 0; i < app.ndocs; i++)
                if (Clay_PointerOver(CLAY_IDI("DocItem", (uint32_t)i))) {
                    const char *doc = explorer_snapshot_doc(&app.explorer, app.drawn_explorer_generation,
                                                            app.session.generation, i);
                    if (!doc) break;
                    static const char *const items[] = { "Rename\xE2\x80\xA6", "Reveal in Finder" };
                    snprintf(app.menu_doc, sizeof(app.menu_doc), "%s", doc); /* copied: later snapshots may re-sort */
                    menu_open(&app.menu, MT_DOC, (int)i, mouse, items, 2);
                    break;
                }
        }
        if (!mouse_down) app.dragging = DRAG_NONE;
        float sw = (float)GetScreenWidth();
        float right = sw - (app.observer_open ? app.observer_w + SPLITTER_W : 0);
        if (app.dragging == DRAG_EXPLORER) app.explorer_w = mouse.x - SPLITTER_W / 2.0f;
        if (app.dragging == DRAG_OBSERVER) app.observer_w = sw - mouse.x - SPLITTER_W / 2.0f;
        if (app.dragging == DRAG_CHAT) app.chat_w = right - mouse.x - SPLITTER_W / 2.0f;
        /* Re-clamp every frame so shrinking the window never squeezes the editor. */
        app.explorer_w = fmaxf(140.0f, fminf(app.explorer_w, sw * 0.25f));
        app.observer_w = fmaxf(180.0f, fminf(app.observer_w, sw * 0.25f));
        app.chat_w = fmaxf(280.0f, fminf(app.chat_w, sw * 0.45f));
        /* Keep the document at least 320 px wide: shrink chat, then the
         * observer, then the explorer, and finally close the observer. */
        for (int pass = 0; pass < 4; pass++) {
            float side = app.explorer_w + app.chat_w + (app.observer_open ? app.observer_w + SPLITTER_W : 0) + 2 * SPLITTER_W;
            float deficit = 320.0f - (sw - side);
            if (deficit <= 0) break;
            if (pass == 0) app.chat_w = fmaxf(280.0f, app.chat_w - deficit);
            else if (pass == 1 && app.observer_open) app.observer_w = fmaxf(180.0f, app.observer_w - deficit);
            else if (pass == 2) app.explorer_w = fmaxf(140.0f, app.explorer_w - deficit);
            else if (pass == 3) app.observer_open = false;
        }

        bool primary = !modal && (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER));
        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (primary && shortcut_pressed('s')) {
            if (app.focus == FOCUS_RECORD && record_view_kind(app.record) == REC_NOTE) save_note(&app);
            else if (app.focus == FOCUS_RECORD)
                set_status(&app, "Canon is kept with Commit or Revise \xC2\xB7 \xE2\x8C\x98S saves notes and documents");
            else save(&app);
        }
        /* From the document only: the chat box has no selection to quote. */
        if (primary && shift && shortcut_pressed('n') && app.focus == FOCUS_DOC) note_from_selection(&app);
        if (primary && shift && shortcut_pressed('c') && app.focus == FOCUS_DOC) canon_from_selection(&app);
        if (!modal && app.focus == FOCUS_RECORD && IsKeyPressed(KEY_ESCAPE)) app.focus = FOCUS_DOC;
        if (primary && shortcut_pressed('l')) app.focus = FOCUS_CHAT;
        if (primary && shortcut_pressed('k')) begin_edit(&app);
        /* Cmd+Enter accepts only outside the chat box, where it also sends the
         * message being typed (red-team 004: one key did both). */
        if (primary && review_active(app.review) && app.focus != FOCUS_CHAT &&
            (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)))
            accept_proposal(&app);
        if (primary && shortcut_pressed('o')) choose_project(&app, false);
        if (primary && !shift && shortcut_pressed('n')) begin_new_document(&app);
        if (!modal && app.focus == FOCUS_CHAT && IsKeyPressed(KEY_ESCAPE)) {
            if (app.history_pending) {
                cancel_history(&app);
                set_status(&app, "History loading cancelled; the current window was kept");
            }
            if (app.edit_mode) {
                leave_edit_mode(&app);
                if (!app.pending.active) editor_anchor_clear(app.editor);
            }
            app.focus = FOCUS_DOC;
        }

        const char *notice = editor_take_notice(app.editor);
        if (notice) set_status(&app, notice);

        /* Refresh lists only now: clicks above were hit-tested against the
         * lists as last drawn, so a re-sort can never redirect a click. */
        if (app.lists_dirty && !history_read_busy(&app)) {
            if (refresh_lists(&app)) app.lists_dirty = false;
        }
        Clay_RenderCommandArray cmds = build_layout(&app);
        app.editor_rect = ELEMENT_RECT("Editor");
        if (record_view_active(app.record)) {
            /* Dock the note/canon panel at the bottom of the document pane. */
            Rectangle er = app.editor_rect;
            float h = record_view_height(app.record, er.width - 24, er.height * 0.45f);
            app.record_rect = (Rectangle){ er.x + 12, er.y + er.height - h - 12, er.width - 24, h };
            app.editor_rect.height = app.record_rect.y - er.y - 8 + 12;
        }
        if (review_active(app.review)) {
            /* Dock the review panel at the bottom of the document pane (above
             * the note/canon panel, if open). */
            Rectangle er = app.editor_rect;
            float h = review_height(app.review, er.width - 24, er.height * 0.5f);
            app.review_rect = (Rectangle){ er.x + 12, er.y + er.height - h - 12, er.width - 24, h };
            app.editor_rect.height = app.review_rect.y - er.y - 8;
        }
        app.chat_list_rect = ELEMENT_RECT("ChatMessages");
        app.findings_rect = app.observer_open ? ELEMENT_RECT("Findings") : (Rectangle){ 0 };
        app.chat_input_rect = ELEMENT_RECT("ChatInput");
        Rectangle save_rect = ELEMENT_RECT("SaveButton");
        Rectangle open_rect = ELEMENT_RECT("OpenButton");
        Rectangle obs_rect = ELEMENT_RECT("ObserverToggle");
        Rectangle new_rect = ELEMENT_RECT("NewChatButton");
        Rectangle send_rect = ELEMENT_RECT("SendButton");
        Rectangle attach_rect = ELEMENT_RECT("AttachRow");

        /* Always update both so queued keystrokes are consumed or discarded. */
        if (prompt_active(app.prompt)) {
            PromptResult pr = prompt_update(app.prompt, (Rectangle){ 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() });
            if (pr == PR_CANCEL) prompt_close(app.prompt);
            if (pr == PR_OK) {
                char *t = prompt_text(app.prompt);
                if (t && finish_prompt(&app, t)) prompt_close(app.prompt);
                free(t);
            }
        }
        bool idle = app.dragging == DRAG_NONE && !modal;
        editor_update(app.editor, app.editor_rect, idle && app.focus == FOCUS_DOC);
        memory_document_names(&app); /* dirty-buffer transitions fence preparation immediately */
        chat_view_set_can_apply(app.chat_view, editor_path(app.editor) != NULL && !editor_is_read_only(app.editor));
        chat_view_update(app.chat_view, app.chat_list_rect, app.chat_input_rect, idle && app.focus == FOCUS_CHAT);
        ChatPageAction page_action = chat_view_take_page(app.chat_view);
        if (idle && page_action.kind != CHAT_PAGE_NONE) page_chat(&app, page_action);
        if (review_active(app.review) && !modal) review_update(app.review, app.review_rect);
        if (record_view_active(app.record)) record_view_update(app.record, app.record_rect, idle && app.focus == FOCUS_RECORD);
        if (app.observer_open && !modal) findings_view_update(app.findings, app.findings_rect);
        input_discard_chars(); /* after both views: nothing typed now may appear later */
        long apply = chat_view_take_apply(app.chat_view);
        if (apply >= 0) {
            /* "Apply to document": propose a reply as a replacement for the
             * selection, or as an insertion at the cursor. */
            chat_lock(app.chat);
            const ChatMessage *m = chat_message(app.chat, (size_t)apply);
            char *reply = m && !m->excerpt ? strdup(m->content) : NULL;
            bool reply_cut = m && (m->truncated || m->stopped);
            bool reply_edit = m && m->is_edit;
            chat_unlock(app.chat);
            size_t a, c;
            editor_get_selection(app.editor, &a, &c);
            size_t lo = a < c ? a : c, hi = a < c ? c : a;
            if (!reply) {
            } else if (!editor_path(app.editor) || editor_is_read_only(app.editor)) {
                set_status(&app, "Open an editable document to apply a reply");
            } else if (app.pending.active) {
                set_status(&app, "Wait for the current edit to finish (or stop it) before applying a reply");
            } else if (reply_edit && hi == lo) {
                /* An edit reply rewrites a passage; inserting it would duplicate the text. */
                set_status(&app, "Select the passage this rewrite replaces, then Apply");
            } else {
                leave_edit_mode(&app);
                char *orig = editor_range_utf8(app.editor, lo, hi - lo);
                char *clean = llm_clean_edit(reply, orig ? orig : "");
                char where[64];
                if (hi > lo) snprintf(where, sizeof(where), "replace selection");
                else snprintf(where, sizeof(where), "insert at line %d", editor_stats(app.editor).line);
                if (orig && clean) propose(&app, lo, hi - lo, orig, clean, where, reply_cut);
                free(orig);
                free(clean);
            }
            free(reply);
        }
        bool rec_canon = false;
        long rec_index = chat_view_take_record(app.chat_view, &rec_canon);
        if (rec_index >= 0) record_from_reply(&app, rec_index, rec_canon);
        long sources_index = chat_view_take_sources(app.chat_view);
        if (sources_index >= 0) show_memory_sources(&app, sources_index);
        char *submitted = chat_view_take_submit(app.chat_view);
        if (submitted) send_chat(&app, submitted);
        if (chat_view_take_copied(app.chat_view)) set_status(&app, "Reply copied to the clipboard");

        int want = MOUSE_CURSOR_DEFAULT;
        if (app.dragging || Clay_PointerOver(CLAY_ID("SplitLeft")) || Clay_PointerOver(CLAY_ID("SplitChat")) ||
            (app.observer_open && Clay_PointerOver(CLAY_ID("SplitRight"))))
            want = MOUSE_CURSOR_RESIZE_EW;
        else if (CheckCollisionPointRec(mouse, app.editor_rect) || CheckCollisionPointRec(mouse, app.chat_input_rect))
            want = MOUSE_CURSOR_IBEAM;
        else if (Clay_PointerOver(CLAY_ID("Explorer")))
            want = MOUSE_CURSOR_POINTING_HAND;
        if (want != cursor_shape) {
            SetMouseCursor(want);
            cursor_shape = want;
        }

        BeginDrawing();
        ClearBackground((Color){ 0x1F, 0x1F, 0x1F, 0xFF });
        clay_render(cmds, app.font_ptrs);
        if (modal) GuiLock(); /* nothing behind a prompt or menu reacts */
        if (GuiButton(open_rect, "Open...")) choose_project(&app, false);
        if (editor_path(app.editor)) editor_draw(app.editor, app.editor_rect, app.focus == FOCUS_DOC);
        int empty_act = editor_path(app.editor) ? 0 : draw_empty(&app, app.editor_rect);
        if (review_active(app.review)) {
            review_draw(app.review, app.review_rect);
            if (GuiButton(review_accept_rect(app.review_rect), "Accept")) accept_proposal(&app);
            if (GuiButton(review_reject_rect(app.review_rect), "Reject")) reject_proposal(&app);
        }
        if (record_view_active(app.record))
            record_action(&app, record_view_draw(app.record, app.record_rect, app.focus == FOCUS_RECORD));
        chat_view_draw(app.chat_view, app.chat_list_rect, app.chat_input_rect, app.focus == FOCUS_CHAT);
        if (app.observer_open && app.findings_rect.width > 0) {
            char fid[40] = "";
            FindingAction fa = findings_view_draw(app.findings, app.findings_rect, fid, sizeof(fid));
            if (fa != FA_NONE) finding_action(&app, fa, fid);
        }

        bool can_save = (editor_is_dirty(app.editor) || editor_overwrite_armed(app.editor)) &&
                        !editor_is_read_only(app.editor);
        if (!can_save) GuiDisable();
        if (GuiButton(save_rect, "Save")) save(&app);
        GuiEnable();
        /* raygui draws with the primary face only (no fallback), so ASCII. */
        if (GuiButton(obs_rect, app.observer_open ? "Hide observer" : app.observer_button[0] ? app.observer_button : "Observer"))
            app.observer_open = !app.observer_open;
        /* Whether the observer reads the open chat (the author's choice). */
        if (open_chat_stored(&app)) {
            Rectangle orr = ELEMENT_RECT("ObserverReads");
            const ChatSummary *cs = find_chat(&app, app.chat_id);
            bool reads = cs && !cs->observer_ignore, was = reads;
            GuiCheckBox((Rectangle){ orr.x, orr.y + 4, 16, 16 }, "Observer reads this chat", &reads);
            if (reads != was) set_chat_observer(&app, app.chat_id, reads);
        }
        if (app.suggest_text[0]) {
            if (GuiButton(ELEMENT_RECT("NameSave"), "Save")) names_answer(&app, true);
            else if (GuiButton(ELEMENT_RECT("NameSkip"), "Not now")) names_answer(&app, false);
        }
        if (chat_busy(app.chat)) GuiDisable();
        if (GuiButton(new_rect, "New chat")) new_chat(&app);
        GuiEnable();
        if (app.edit_mode) {
            if (GuiButton(ELEMENT_RECT("CancelEdit"), "Cancel")) {
                leave_edit_mode(&app);
                editor_anchor_clear(app.editor);
            }
        } else if (attach_rect.width > 0) {
            int aa = draw_attach_row(&app, attach_rect);
            if (aa >= 0) {
                detach(&app, (size_t)aa);
                chat_controller_mark_attachments_changed(&app.chat_controller);
            } else if (aa == -2) {
                if (!app.project_dir[0] || app.ndocs == 0) {
                    set_status(&app, "Open a project with documents to attach them");
                } else {
                    /* The project's documents; a check mark shows what's attached. */
                    const char *items[MENU_MAX_ITEMS];
                    char labels[MENU_MAX_ITEMS][48];
                    int n = 0;
                    /* With more documents than fit, the last item asks for a name. */
                    size_t room = app.ndocs > MENU_MAX_ITEMS ? MENU_MAX_ITEMS - 1 : app.ndocs;
                    for (size_t i = 0; i < room; i++, n++) {
                        char nm[44];
                        fit_label(&app.fonts[FONT_UI], app.docs[i], 300, nm, sizeof(nm));
                        snprintf(app.menu_names[n], sizeof(app.menu_names[n]), "%s", app.docs[i]);
                        snprintf(labels[n], sizeof(labels[n]), "%s %s", is_attached(&app, app.docs[i]) ? "\xE2\x9C\x93" : "  ", nm);
                        items[n] = labels[n];
                    }
                    if (room < app.ndocs) {
                        app.menu_names[n][0] = '\0'; /* "other": a prompt */
                        snprintf(labels[n], sizeof(labels[n]), "   Other document\xE2\x80\xA6");
                        items[n] = labels[n];
                        n++;
                    }
                    menu_open(&app.menu, MT_ATTACH, 0, (Vector2){ app.attach_add.x, app.attach_add.y - 8 - (float)n * 30 }, items, n);
                }
            }
        }
        if (chat_busy(app.chat)) {
            if (GuiButton(send_rect, "Stop")) chat_cancel(app.chat);
        } else if (app.history_pending) {
            if (GuiButton(send_rect, "Cancel")) {
                cancel_history(&app);
                set_status(&app, "History loading cancelled; the current window was kept");
            }
        } else {
            bool ready = !app.llm || chat_remote_selected(app.chat) || llm_server_state(app.llm, NULL, 0) == LLM_READY;
            if (!ready) GuiDisable();
            if (GuiButton(send_rect, "Send")) {
                chat_view_submit_now(app.chat_view);
                char *t = chat_view_take_submit(app.chat_view);
                if (t) send_chat(&app, t);
                app.focus = FOCUS_CHAT;
            }
            GuiEnable();
        }

        GuiUnlock();
        if (!modal && empty_act) empty_action(&app, empty_act, sample);
        {
            Rectangle screen = { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() };
            int pick = menu_draw(&app.menu, &app.fonts[FONT_UI], screen);
            if (pick >= 0 && app.menu.tag == MT_ATTACH && pick < MENU_MAX_ITEMS && !app.menu_names[pick][0]) {
                snprintf(app.prompt_project, sizeof(app.prompt_project), "%s", app.project_dir);
                prompt_open(app.prompt, PT_ATTACH_NAME, "Attach a document", "", "Attach");
            } else if (pick >= 0 && app.menu.tag == MT_ATTACH && pick < MENU_MAX_ITEMS) {
                const char *nm = app.menu_names[pick];
                bool found = false;
                for (size_t i = 0; i < app.nattached; i++)
                    if (!strcmp(app.attached[i], nm)) {
                        detach(&app, i);
                        found = true;
                        break;
                    }
                if (!found) attach(&app, nm);
                chat_controller_mark_attachments_changed(&app.chat_controller);
            }
            if (pick >= 0 && app.menu.tag == MT_MODEL && pick == app.model_menu_settings) {
                open_model_settings(&app);
            } else if (pick >= 0 && app.menu.tag == MT_MODEL) {
                bool remote = pick == 1;
                if (remote == chat_remote_selected(app.chat)) {
                } else if (!chat_select_remote(app.chat, remote ? &app.remote : NULL)) {
                    set_status(&app, "Wait for the reply to finish before switching models");
                } else {
                    char msg[400];
                    if (remote)
                        snprintf(msg, sizeof(msg), "Using %s: new chat messages, their attached documents and edit passages go to %s. "
                                                   "Earlier messages and the observer stay on this Mac.", app.remote.name, app.remote.host);
                    else snprintf(msg, sizeof(msg), "Using the local model: nothing leaves this Mac");
                    set_status(&app, msg);
                }
            }
            if (pick == 1 && app.menu.tag == MT_CHAT) {
                const ChatSummary *cs = find_chat(&app, app.menu_chat);
                if (cs) set_chat_observer(&app, app.menu_chat, cs->observer_ignore);
            } else if (pick >= 2 && pick <= 4 && app.menu.tag == MT_CHAT && app.memory_experiment) {
                memory_control(&app, pick == 2 ? MEMORY_JOB_ENABLE : pick == 3 ? MEMORY_JOB_DISABLE : MEMORY_JOB_REBUILD);
            } else if (pick == 0 && app.menu.tag == MT_CHAT) {
                const char *title = "";
                for (size_t i = 0; i < app.nchats; i++)
                    if (!strcmp(app.chats[i].id, app.menu_chat)) title = app.chats[i].title;
                snprintf(app.prompt_project, sizeof(app.prompt_project), "%s", app.project_dir);
                prompt_open(app.prompt, PT_RENAME_CHAT, "Rename chat", title, "Rename");
            }
            if (pick >= 0 && app.menu.tag == MT_DOC) {
                if (!name_taken(&app, app.menu_doc)) set_status(&app, "That document is no longer in this project");
                else if (pick == 0) begin_rename(&app, app.menu_doc); /* a dirty open document is refused on OK */
                else reveal_document(&app, app.menu_doc);
            }
            if (app.settings_open) draw_model_settings(&app, screen);
            if (app.memory_sources_open) draw_memory_sources(&app);
            if (prompt_active(app.prompt)) {
                PromptResult pr = prompt_draw(app.prompt, screen);
                if (pr == PR_CANCEL) prompt_close(app.prompt);
                if (pr == PR_OK) {
                    char *t = prompt_text(app.prompt);
                    if (t && finish_prompt(&app, t)) prompt_close(app.prompt);
                    free(t);
                }
            }
        }
        const char *shot_path = app.script ? script_take_shot(app.script) : NULL;
        if (shot_path) {
            /* raylib batches draws until EndDrawing; flush so the capture
             * includes everything drawn this frame (e.g. raygui controls). */
            rlDrawRenderBatchActive();
            Image shot = LoadImageFromScreen();
            ExportImage(shot, shot_path);
            UnloadImage(shot);
        }
        EndDrawing();

        if (screenshot_path && ++frame == 10) {
            Image shot = LoadImageFromScreen();
            ExportImage(shot, screenshot_path);
            UnloadImage(shot);
            running = false;
        }
    }

    int rc = app.script ? script_finish(app.script, &host, closed_by_window) : 0;
    app_destroy(&app);
    return rc;
}
