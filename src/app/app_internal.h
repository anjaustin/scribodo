/* The application's state and the functions the composition root (src/main.c) shares with the workflow and
 * layout modules it was split into (AUDIT-004 F3, red team on remed/main). Internal to the app: included only by
 * src/main.c and the files listed below, never by a library module or a test. */
#ifndef SCRIBODO_APP_INTERNAL_H
#define SCRIBODO_APP_INTERNAL_H

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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clay.h"
#include "core/experiments.h"
#include "raygui.h"
#include "raylib.h"
#include "rlgl.h"
#include "app/chat_store.h"
#include "core/utf8.h"
#include "core/doc_io.h"
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
#include "app/recall_job.h"
#include "app/import_job.h"
#include "app/name_detection_job.h"
#include "app/project_session.h"
#include "app/explorer_snapshot.h"
#include "app/chat_controller.h"
#include "app/draft_controller.h"
#include "app/local_models.h"
#include "app/startup_env.h"
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
#include "llm/rerank.h"
#include "ui/chat_view.h"
#include "ui/clay_render.h"
#include "ui/editor.h"
#include "ui/findings_view.h"
#include "ui/recall_view.h"
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
enum { FOCUS_NONE, FOCUS_DOC, FOCUS_CHAT, FOCUS_RECORD, FOCUS_RECALL };
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
    LlmServer *rerank;            /* the cross-encoder (verified-answer pipeline experiment) */
    RerankClient *rerank_client;
    bool answer_pipeline;
    SemanticClient *semantic;
    MemoryJob *memory_job;
    MemoryDocumentCache *memory_document_cache;
    GenerativeGate *generative_gate;
    bool memory_experiment, memory_sources_open;
    bool memory_briefs_experiment;
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

    /* Drafts of unsaved text (app/draft_controller.h) */
    DraftController draft;

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
    ImportJob import_job;         /* moving a legacy project into the store, off the frame thread */
    bool observer_project_deferred; /* the observer joins the project once its import has finished */
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

    /* Recall (MEMORY-015 S1): deterministic canon and original excerpts, no model. */
    bool recall_open;
    RecallView *recall;
    RecallJob recall_job;
    bool recall_job_initialized, recall_stale;
    Rectangle recall_rect;
    char recall_project[PATH_MAX];

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
    const char *services_headline; /* why the last `services.sh up` failed (static), NULL once one succeeds */
    char services_warning[SERVICES_REASON_MAX]; /* the last `up` warning shown, so each is shown once */
    /* A backup running in the background (Explorer: PROJECT). */
    BackupJob backup;
    char backup_label[160];
    double store_next_probe;      /* while offline: when to probe again */
    bool store_just_moved;        /* the project was just moved into the store (for the status) */
    bool services_started;
    double services_started_at;   /* when the services were last started (a dead daemon is restarted, rate-limited) */
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
    bool import_job_initialized;
    bool names_job_initialized;
    void *clay_buf;
    Script *script;
} App;

/* The four anonymous unions above alias extracted controllers back to flat names, field by field. Their layout is
 * checked here (AUDIT-004 F3): a controller field added, removed, moved, retyped or resized fails the build until
 * its mirror is updated. Each mirror must sit at its field's offset with the same type and size, and the last
 * mirror must end where the controller ends (a field slotted into existing interior padding is the one change these
 * cannot see; it would not shift a mirror). */
#define APP_MIRRORS(member, Controller, field, mirror)                                                              \
    _Static_assert(offsetof(App, mirror) == offsetof(App, member) + offsetof(Controller, field),                    \
                   "App." #mirror " must sit at " #Controller "." #field);                                          \
    _Static_assert(sizeof(((App *)0)->mirror) == sizeof(((Controller *)0)->field),                                  \
                   "App." #mirror " must be the size of " #Controller "." #field);                                  \
    _Static_assert(__builtin_types_compatible_p(__typeof__(((App *)0)->mirror), __typeof__(((Controller *)0)->field)), \
                   "App." #mirror " must have the type of " #Controller "." #field)
#define APP_MIRROR_ENDS(member, Controller, last)                                                                   \
    _Static_assert(sizeof(((App *)0)->member) == sizeof(Controller), "App." #member " must be a " #Controller);    \
    _Static_assert((offsetof(App, last) + sizeof(((App *)0)->last) - offsetof(App, member) + _Alignof(Controller) - 1) \
                           / _Alignof(Controller) * _Alignof(Controller) == sizeof(Controller),                     \
                   #Controller " has a field after " #last " that App does not mirror")

APP_MIRRORS(chat_controller, ChatController, chat, chat);
APP_MIRRORS(chat_controller, ChatController, chat_view, chat_view);
APP_MIRRORS(chat_controller, ChatController, attached, attached);
APP_MIRRORS(chat_controller, ChatController, nattached, nattached);
APP_MIRRORS(chat_controller, ChatController, attach_touched, attach_touched);
APP_MIRRORS(chat_controller, ChatController, attachment_revision, attachment_revision);
APP_MIRROR_ENDS(chat_controller, ChatController, attachment_revision);

APP_MIRRORS(session, ProjectSession, project_dir, project_dir);
APP_MIRRORS(session, ProjectSession, chat_project, chat_project);
APP_MIRRORS(session, ProjectSession, clear_chat_when_idle, clear_chat_when_idle);
APP_MIRRORS(session, ProjectSession, chat_id, chat_id);
APP_MIRRORS(session, ProjectSession, chat_title, chat_title);
APP_MIRRORS(session, ProjectSession, generation, session_generation);
APP_MIRROR_ENDS(session, ProjectSession, session_generation);

APP_MIRRORS(reviewed_edit, ReviewedEditController, review, review);
APP_MIRRORS(reviewed_edit, ReviewedEditController, editor, review_editor);
APP_MIRRORS(reviewed_edit, ReviewedEditController, edit_mode, edit_mode);
APP_MIRRORS(reviewed_edit, ReviewedEditController, edit_text, edit_text);
APP_MIRRORS(reviewed_edit, ReviewedEditController, edit_banner, edit_banner);
APP_MIRRORS(reviewed_edit, ReviewedEditController, pending, pending);
APP_MIRRORS(reviewed_edit, ReviewedEditController, proposal, proposal);
APP_MIRROR_ENDS(reviewed_edit, ReviewedEditController, proposal);

APP_MIRRORS(explorer, ExplorerSnapshot, docs, docs);
APP_MIRRORS(explorer, ExplorerSnapshot, ndocs, ndocs);
APP_MIRRORS(explorer, ExplorerSnapshot, chats, chats);
APP_MIRRORS(explorer, ExplorerSnapshot, chat_labels, chat_labels);
APP_MIRRORS(explorer, ExplorerSnapshot, nchats, nchats);
APP_MIRRORS(explorer, ExplorerSnapshot, generation, explorer_generation);
APP_MIRRORS(explorer, ExplorerSnapshot, project_generation, explorer_project_generation);
APP_MIRROR_ENDS(explorer, ExplorerSnapshot, explorer_project_generation);

/* What lives across frames of the loop, besides App. */
typedef struct {
    ScriptHost host;
    char sample[2048];           /* the bundled sample, the welcome screen's fallback */
    const char *screenshot_path; /* SCRIBODO_SCREENSHOT: render a few frames, save the framebuffer, exit */
    int frame;
    bool trace;                  /* SCRIBODO_INPUT_TRACE */
    int cursor_shape;
    bool closed_by_window;
} MainLoop;

/* The screen rects of raygui controls drawn over the layout this frame. */
typedef struct {
    Rectangle save_rect, open_rect, obs_rect, new_rect, send_rect, attach_rect;
} FrameRects;

#define ELEMENT_RECT(lit) element_rect_str(CLAY_STRING(lit), 0, false)

/* src/main.c */
void set_status(App *app, const char *msg);
const char *store_says(App *app, const char *fallback);
const char *base_name(const char *path);
bool memory_controls_pending(App *app);
void memory_editor_will_change(void *context);
void memory_document_names(App *app);
void store_begin(App *app);
void leave_edit_mode(App *app);
void propose(App *app, size_t start, size_t len, const char *original, const char *replacement, const char *note, bool truncated);
void project_of(const char *doc_path, char *out, size_t outlen);
void open_path(App *app, const char *path);
void drop_path(App *app, const char *path);
void open_project(App *app, const char *dir);
void memfit_message(const MemFit *f, int wanted, char *out, size_t outlen);
void apply_model_settings(App *app, bool context);
void apply_profile(App *app);
void begin_names(App *app);
void save(App *app);
void empty_action(App *app, int act, const char *sample);

/* src/ui/layout.c */
void label(App *app, const char *s, int font, Clay_Color color);
Clay_RenderCommandArray build_layout(App *app);
Rectangle element_rect_str(Clay_String s, uint32_t index, bool indexed);

/* src/app/script_host_app.c */
bool scripted_history_read(StoreManager *store, const char *project, const char *conversation, const ChatWindowRequest *request, ChatWindowPage *page, void *context);
int script_host_setup(App *app, MainLoop *loop, const StartupEnv *env, const char *script_path);

/* src/app/app_lifecycle.c */
void app_destroy(App *app);
int app_init_jobs(App *app, const char *script_path);
int app_open_window(App *app, const char *script_path);
int app_start_models(App *app, const char *script_path);
int app_create_views(App *app, const char *script_path);
int app_load_recents(App *app, MainLoop *loop, const StartupEnv *env, const char *script_path);
void app_open_initial(App *app, const char *path, const char *script_path, const char *sample);

/* src/app/document_workflow.c */
bool name_taken(App *app, const char *name);
void begin_new_document(App *app);
void begin_rename(App *app, const char *name);
bool finish_prompt(App *app, const char *typed);
void import_documents(App *app);
void reveal_document(App *app, const char *name);

/* src/app/chat_workflow.c */
void cancel_history(App *app);
bool database_history_project(const char *project);
bool history_read_busy(App *app);
const ChatSummary *find_chat(const App *app, const char *id);
bool open_chat_stored(const App *app);
void set_chat_observer(App *app, const char *id, bool read);
void names_poll(App *app);
void names_answer(App *app, bool save);
void on_message_done(ChatSession *s, size_t index, void *ctx);
void adopt_chat(App *app);
void resave_unsaved_chat(App *app);
bool is_attached(App *app, const char *name);
bool listed_doc(App *app, const char *name);
bool attach(App *app, const char *name);
void detach(App *app, size_t i);
void attach_reset(App *app);
char *attached_text(App *app, const char *name, size_t cap, bool *cut);
bool unsaved_chat(App *app);
void new_chat(App *app);
bool load_chat_window(App *app, const char *project, const char *id, const char *title, ChatWindowRequest request, bool restore);
void history_poll(App *app);
void page_chat(App *app, ChatPageAction action);
void open_chat(App *app, const ChatSummary *c);
void fit_label(const SFont *f, const char *s, float maxw, char *out, size_t outlen);
int draw_attach_row(App *app, Rectangle r);
void send_chat(App *app, char *text);

/* src/app/record_workflow.c */
void toggle_entity(App *app, const char *id);
void open_finding_source(App *app, const char *finding_id);
void refresh_records(App *app);
bool record_unsaved_canon(App *app);
bool save_note(App *app);
bool close_record(App *app, bool explicit_close);
bool show_record(App *app, RecordKind kind, const Record *rec, const char *content, const SourceRef *src, size_t ns);
void note_from_selection(App *app);
void canon_from_selection(App *app);
void record_from_reply(App *app, long index, bool canon);
void open_source(App *app, const SourceRef *s);
void record_action(App *app, RecordAction act);
void refresh_findings(App *app);
void finding_action(App *app, FindingAction act, const char *id);

#endif
