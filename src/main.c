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
#include <dirent.h>
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
#include "app/revisions.h"
#include "llm/llm_text.h"
#include "app/script.h"
#include "llm/chat.h"
#include "llm/observer.h"
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
enum { PT_NEW_DOC = 1, PT_RENAME_DOC, PT_RENAME_CHAT, PT_ATTACH_NAME };  /* what the name prompt is for */
enum { MT_DOC = 1, MT_CHAT, MT_ATTACH };  /* what a context menu is for */
#define MAX_ATTACH 8

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
#define MAX_DOCS 200

/* Revisions listed in History (newest first). */
#define REV_SHOW 100
/* Notes / canon entries listed in the Explorer (most recent first). */
#define REC_SHOW 200

typedef struct {
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

    /* Model and chat */
    LlmServer *llm;
    ChatSession *chat;
    ChatView *chat_view;
    /* A conversation belongs to the project it started in, even if the author
     * opens a document elsewhere while a reply is still streaming. */
    char chat_project[PATH_MAX];
    bool clear_chat_when_idle;
    char chat_id[40];
    char chat_title[160];

    /* Reviewed AI edits */
    ReviewView *review;
    bool edit_mode;              /* next chat message is an edit instruction */
    char *edit_text;             /* the selected passage (its position is the editor's anchor) */
    char edit_banner[240];
    struct {
        bool active;             /* an edit request is streaming */
        size_t index;            /* its assistant message */
        char *original, *instruction;
    } pending;
    struct {
        bool active;
        char *original, *replacement, *note;
        char doc[PATH_MAX];
    } proposal;
    Rectangle review_rect;
    Revision *revs;
    size_t nrevs;
    char rev_labels[REV_SHOW][64];

    /* Notes and canon (SOW §4.4) */
    Record *notes, *canon;
    size_t nnotes, ncanon;
    char note_labels[REC_SHOW][72];
    char canon_labels[REC_SHOW][72];
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
    char attached[MAX_ATTACH][256];
    size_t nattached;
    Rectangle attach_chip[MAX_ATTACH], attach_x[MAX_ATTACH], attach_add;
    bool attach_touched;          /* the author changed them: opening a document no longer resets them */
    RecentProject recents[RECENT_MAX];
    size_t nrecents;
    Rectangle welcome_btn[3], recent_rect[RECENT_MAX], empty_btn[2];
    bool record_close_armed;     /* a discard of uncommitted canon text was asked for... */
    double record_close_armed_at; /* ...at this time (a second press within 5 s confirms) */

    /* Project (the folder holding the open document) */
    char project_dir[PATH_MAX];
    char *docs[MAX_DOCS];
    size_t ndocs;
    ChatSummary *chats;
    size_t nchats;
    double lists_refreshed;
    bool lists_dirty; /* refreshed between click handling and layout (stable hit-testing) */

    /* Strings referenced by Clay must outlive the frame's render pass. */
    char doc_label[512];
    char status_left[256];
    char model_label[512];
    char chat_header[200];
    Clay_Color model_color;
} App;

static void set_status(App *app, const char *msg)
{
    snprintf(app->status, sizeof(app->status), "%s", msg);
    app->status_time = GetTime();
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- project lists ------------------------------------------------------------- */

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void refresh_records(App *app);
static void refresh_findings(App *app);

static void refresh_lists(App *app)
{
    for (size_t i = 0; i < app->ndocs; i++) free(app->docs[i]);
    app->ndocs = 0;
    free(app->chats);
    app->chats = NULL;
    app->nchats = 0;
    app->lists_refreshed = GetTime();
    if (!app->project_dir[0]) return;
    DIR *d = opendir(app->project_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && app->ndocs < MAX_DOCS) {
            size_t n = strlen(ent->d_name);
            if (ent->d_name[0] == '.') continue;
            bool ctl = false; /* a name with a line break would break records and requests */
            for (const unsigned char *c = (const unsigned char *)ent->d_name; *c && !ctl; c++) ctl = *c < 0x20;
            if (ctl) continue;
            if ((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")) ||
                (n > 9 && !strcasecmp(ent->d_name + n - 9, ".markdown")))
                app->docs[app->ndocs++] = strdup(ent->d_name);
        }
        closedir(d);
        qsort(app->docs, app->ndocs, sizeof(char *), cmp_str);
    }
    app->nchats = chat_store_list(app->project_dir, &app->chats);
    free(app->revs);
    app->revs = NULL;
    app->nrevs = editor_path(app->editor) ? revisions_list(app->project_dir, editor_path(app->editor), &app->revs) : 0;
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
    refresh_findings(app); /* conflicts are shown against the current canon */
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

static void refresh_records(App *app)
{
    records_free(app->notes, app->nnotes);
    records_free(app->canon, app->ncanon);
    app->notes = app->canon = NULL;
    app->nnotes = app->ncanon = 0;
    if (!app->project_dir[0]) return;
    app->nnotes = records_list(app->project_dir, REC_NOTE, &app->notes);
    app->ncanon = records_list(app->project_dir, REC_CANON, &app->canon);
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

static void set_project(App *app, const char *real)
{
    if (strcmp(real, app->project_dir) != 0) {
        /* A prompt or menu was for the old project's files. */
        if (app->prompt) prompt_close(app->prompt);
        app->menu.active = false;
        snprintf(app->project_dir, sizeof(app->project_dir), "%s", real);
        if (app->observer) observer_set_project(app->observer, real);
        app->findings_version = UINT64_MAX; /* re-list */
        /* A different project has different chats: start fresh (after the
         * current reply finishes, so it is saved with its own project). */
        if (app->chat) app->clear_chat_when_idle = true;
    }
    app->lists_dirty = true;
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
    free(app->proposal.original);
    free(app->proposal.replacement);
    free(app->proposal.note);
    memset(&app->proposal, 0, sizeof(app->proposal));
    review_clear(app->review);
    editor_anchor_clear(app->editor);
}

static void clear_pending(App *app)
{
    free(app->pending.original);
    free(app->pending.instruction);
    memset(&app->pending, 0, sizeof(app->pending));
}

static void leave_edit_mode(App *app)
{
    app->edit_mode = false;
    free(app->edit_text);
    app->edit_text = NULL;
}

/* Show a proposal for review. Takes ownership of nothing; copies. */
static void propose(App *app, size_t start, size_t len, const char *original, const char *replacement, const char *note,
                    bool truncated)
{
    clear_proposal(app);
    if (!editor_path(app->editor)) return;
    if (!editor_anchor_set(app->editor, start, len)) return;
    if (!replacement[0]) {
        editor_anchor_clear(app->editor);
        /* An empty reply is not a proposal to delete the passage. */
        set_status(app, "The model returned no text for this edit; nothing was proposed");
        return;
    }
    app->proposal.active = true;
    app->proposal.original = strdup(original);
    app->proposal.replacement = strdup(replacement);
    app->proposal.note = strdup(note ? note : "");
    snprintf(app->proposal.doc, sizeof(app->proposal.doc), "%s", editor_path(app->editor));
    if (!app->proposal.original || !app->proposal.replacement || !review_set(app->review, original, replacement, note, truncated)) {
        clear_proposal(app);
        set_status(app, "Could not show the proposed edit");
        return;
    }
    editor_reveal(app->editor, start);
}

/* Returns false (and says so) if the revision could not be written. */
static bool record_revision(App *app, const char *origin, const char *note)
{
    if (!app->project_dir[0] || !editor_path(app->editor)) return true; /* no history to keep */
    char *text = editor_text_utf8(app->editor);
    bool ok = text && revisions_record(app->project_dir, editor_path(app->editor), text, origin, note, NULL);
    free(text);
    if (!ok) set_status(app, "Could not record a revision in .scribodo/revisions");
    app->lists_dirty = true;
    return ok;
}

static void accept_proposal(App *app)
{
    if (!app->proposal.active) return;
    if (!editor_path(app->editor) || strcmp(app->proposal.doc, editor_path(app->editor)) != 0) {
        clear_proposal(app);
        set_status(app, "That edit was for another document");
        return;
    }
    if (editor_is_read_only(app->editor)) {
        set_status(app, "This document is read-only; the edit can't be applied");
        return;
    }
    /* Never apply over text that changed since the proposal was made. The
     * editor tracks the passage through every edit (red-team 004: matching by
     * offset or by text could land on a different copy of the same words, or
     * insert mid-word after earlier text changed). */
    size_t start = 0, len = 0;
    bool same = editor_anchor_get(app->editor, &start, &len);
    if (same) {
        char *now = editor_range_utf8(app->editor, start, len);
        same = now && !strcmp(now, app->proposal.original);
        free(now);
    }
    if (!same) {
        clear_proposal(app);
        set_status(app, "The passage changed since this edit was proposed. Select it again and request a new edit.");
        return;
    }
    /* The text before the edit must be safely in History before it changes. */
    if (!record_revision(app, "before-ai-edit", app->proposal.note)) {
        set_status(app, "The edit was not applied: could not save the current text to History (.scribodo/revisions)");
        return;
    }
    if (!editor_replace_range_utf8(app->editor, start, len, app->proposal.replacement)) {
        set_status(app, "Could not apply the edit");
        return;
    }
    bool after = record_revision(app, "ai-edit", app->proposal.note);
    clear_proposal(app);
    if (after)
        set_status(app, "Edit applied \xC2\xB7 \xE2\x8C\x98Z undoes it \xC2\xB7 the file isn't saved until you press \xE2\x8C\x98S");
    else
        set_status(app, "Edit applied, but it could not be recorded in History \xC2\xB7 \xE2\x8C\x98Z undoes it");
    app->focus = FOCUS_DOC;
}

static void reject_proposal(App *app)
{
    clear_proposal(app);
    set_status(app, "Edit rejected \xE2\x80\x94 the document is unchanged");
}

/* ⌘K: the next chat message becomes an edit request for the selection. */
static void begin_edit(App *app)
{
    size_t a, c;
    editor_get_selection(app->editor, &a, &c);
    size_t lo = a < c ? a : c, hi = a < c ? c : a;
    if (!editor_path(app->editor) || editor_is_read_only(app->editor)) {
        set_status(app, "Open an editable document to request edits");
        return;
    }
    if (hi == lo) {
        set_status(app, "Select the passage you want edited, then press \xE2\x8C\x98K");
        return;
    }
    if (app->pending.active) {
        set_status(app, "Wait for the current edit to finish (or stop it) before requesting another");
        return;
    }
    leave_edit_mode(app);
    clear_proposal(app); /* one passage at a time: a new request replaces the open proposal */
    app->edit_text = editor_range_utf8(app->editor, lo, hi - lo);
    if (!app->edit_text) return;
    app->edit_mode = true;
    /* Banner: the first words of the passage. */
    char first[80];
    snprintf(first, sizeof(first), "%.60s", app->edit_text);
    for (char *p = first; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    /* don't cut a UTF-8 sequence in half */
    size_t fl = strlen(first);
    while (fl > 0 && ((unsigned char)first[fl - 1] & 0xC0) == 0x80) fl--;
    if (fl > 0 && ((unsigned char)first[fl - 1] & 0xC0) == 0xC0) fl--;
    first[fl] = '\0';
    snprintf(app->edit_banner, sizeof(app->edit_banner), "\xE2\x9C\x8E Editing \xE2\x80\x9C%s%s\xE2\x80\x9D \xE2\x80\x94 describe the change",
             first, strlen(app->edit_text) > fl ? "\xE2\x80\xA6" : "");
    editor_anchor_set(app->editor, lo, hi - lo);
    app->focus = FOCUS_CHAT;
}

static void restore_revision(App *app, const Revision *r)
{
    if (!editor_path(app->editor)) return;
    if (editor_is_read_only(app->editor)) {
        set_status(app, "This document is read-only");
        return;
    }
    char *text = revisions_load(app->project_dir, editor_path(app->editor), r);
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
        recent_touch(app->project_dir, base_name(path));
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
    recent_touch(real, NULL);
    app->nrecents = recent_load(app->recents, RECENT_MAX);
    char msg[PATH_MAX + 32];
    snprintf(msg, sizeof(msg), "Opened %s", base_name(real));
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
static bool set_aside_history(App *app, const char *name)
{
    char rev[PATH_MAX + 300];
    snprintf(rev, sizeof(rev), "%s/.scribodo/revisions/%s", app->project_dir, name);
    struct stat st;
    if (stat(rev, &st) != 0) return true;
    char aside[PATH_MAX + 340];
    snprintf(aside, sizeof(aside), "%s.earlier-%ld", rev, (long)time(NULL));
    return rename(rev, aside) == 0;
}

/* Returns true when the prompt can close. */
static bool finish_prompt(App *app, const char *typed)
{
    if (prompt_tag(app->prompt) == PT_ATTACH_NAME) {
        if (strcmp(app->prompt_project, app->project_dir) != 0) return true;
        if (!listed_doc(app, typed)) {
            prompt_set_error(app->prompt, "No document by that name in this project");
            return false;
        }
        attach(app, typed);
        app->attach_touched = true;
        return true;
    }
    if (prompt_tag(app->prompt) == PT_RENAME_CHAT) {
        if (!typed[0]) {
            prompt_set_error(app->prompt, "Type a title");
            return false;
        }
        /* Titles are kept to 150 bytes, cut on a character boundary. */
        char title[160];
        snprintf(title, sizeof(title), "%.150s", typed);
        size_t tl = strlen(title);
        while (tl > 0 && ((unsigned char)title[tl] & 0xC0) == 0x80) title[tl--] = '\0';
        if (tl < strlen(typed) && tl > 0 && ((unsigned char)title[tl - 1] & 0xC0) == 0xC0) title[--tl] = '\0';
        typed = title;
        if (strcmp(app->prompt_project, app->project_dir) != 0) return true; /* project changed: nothing done */
        if (!chat_store_rename(app->project_dir, app->menu_chat, typed)) {
            prompt_set_error(app->prompt, "Couldn't rename that chat");
            return false;
        }
        if (!strcmp(app->chat_id, app->menu_chat)) snprintf(app->chat_title, sizeof(app->chat_title), "%s", typed);
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
        int fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            prompt_set_error(app->prompt, strerror(errno));
            return false;
        }
        close(fd);
        bool aside = set_aside_history(app, name);
        open_path(app, full);
        char msg[320];
        snprintf(msg, sizeof(msg), aside ? "Created %s" : "Created %s (older History under this name couldn't be set aside)", name);
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
    if (renamex_np(old_full, full, self ? 0 : RENAME_EXCL) != 0) {
        prompt_set_error(app->prompt, strerror(errno));
        return false;
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
        if (!same_dir && !set_aside_history(app, name)) snprintf(problems, sizeof(problems), "History couldn't be moved");
        else if (renamex_np(rev_old, rev_new, same_dir ? 0 : RENAME_EXCL) != 0)
            snprintf(problems, sizeof(problems), "History couldn't be moved");
    }
    if (!renames_record(app->project_dir, app->rename_from, name))
        snprintf(problems + strlen(problems), sizeof(problems) - strlen(problems), "%sthe rename couldn't be recorded",
                 problems[0] ? "; " : "");
    observer_rename_doc(app->observer, app->project_dir, app->rename_from, name);
    for (size_t i = 0; i < app->nattached; i++)
        if (!strcmp(app->attached[i], app->rename_from)) snprintf(app->attached[i], sizeof(app->attached[i]), "%s", name);
    recent_rename_doc(app->project_dir, app->rename_from, name);
    if (is_open) editor_set_path(app->editor, full);
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
    size_t done = 0;
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
            done++;
            if (!first[0]) snprintf(first, sizeof(first), "%s", dst);
        }
    }
    dialogs_free_paths(paths, n);
    if (n == 0) return;
    app->lists_dirty = true;
    if (first[0] && !editor_is_dirty(app->editor)) open_path(app, first);
    char msg[200];
    snprintf(msg, sizeof(msg), "Imported %zu of %zu document%s", done, n, n == 1 ? "" : "s");
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

static void on_message_done(ChatSession *s, size_t index, void *ctx)
{
    App *app = ctx;
    chat_lock(s);
    const ChatMessage *m = chat_message(s, index);
    ChatMessage copy = { 0 };
    if (m) {
        copy = *m;
        copy.content = strdup(m->content ? m->content : "");
    }
    chat_unlock(s);
    if (!m || !copy.content) return;
    if (app->pending.active && index == app->pending.index) {
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
            char *clean = llm_clean_edit(copy.content, app->pending.original);
            if (clean) propose(app, start, len, app->pending.original, clean, app->pending.instruction, copy.truncated);
            else editor_anchor_clear(app->editor);
            free(clean);
        }
        clear_pending(app);
    }
    if (!app->chat_project[0]) { free(copy.content); return; }
    if (!chat_store_append(app->chat_project, app->chat_id, app->chat_title, &copy))
        set_status(app, "Could not save the chat to .scribodo/chats");
    else if (app->observer && copy.role == CHAT_ASSISTANT)
        observer_poke(app->observer); /* a finished exchange is stored */
    free(copy.content);
    app->lists_dirty = true;
}

/* A conversation started before any project was open has nowhere to be
 * saved; when a project appears, adopt it rather than discard it (red-team
 * 003: it used to be cleared, silently losing the author's text). */
static void adopt_chat(App *app)
{
    snprintf(app->chat_project, sizeof(app->chat_project), "%s", app->project_dir);
    if (!app->chat_id[0]) chat_store_new_id(app->chat_id, sizeof(app->chat_id));
    if (!app->chat_title[0]) snprintf(app->chat_title, sizeof(app->chat_title), "Chat");
    chat_lock(app->chat);
    size_t n = chat_count(app->chat);
    ChatMessage *copy = calloc(n ? n : 1, sizeof(ChatMessage));
    for (size_t i = 0; copy && i < n; i++) {
        copy[i] = *chat_message(app->chat, i);
        copy[i].content = strdup(copy[i].content ? copy[i].content : "");
    }
    chat_unlock(app->chat);
    bool ok = copy != NULL;
    for (size_t i = 0; ok && i < n; i++)
        ok = copy[i].content && chat_store_append(app->chat_project, app->chat_id, app->chat_title, &copy[i]);
    for (size_t i = 0; copy && i < n; i++) free(copy[i].content);
    free(copy);
    set_status(app, ok ? "Chat saved to this project" : "Could not save the chat to .scribodo/chats");
    app->lists_dirty = true;
}

/* ---- attachments ---------------------------------------------------------------- */

static bool is_attached(App *app, const char *name)
{
    for (size_t i = 0; i < app->nattached; i++)
        if (!strcmp(app->attached[i], name)) return true;
    return false;
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
    snprintf(app->attached[app->nattached++], sizeof(app->attached[0]), "%s", name);
    return true;
}

static void detach(App *app, size_t i)
{
    if (i >= app->nattached) return;
    memmove(&app->attached[i], &app->attached[i + 1], (app->nattached - i - 1) * sizeof(app->attached[0]));
    app->nattached--;
}

/* A new conversation starts with the open document attached. */
static void attach_reset(App *app)
{
    app->nattached = 0;
    app->attach_touched = false;
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

static void new_chat(App *app)
{
    if (chat_busy(app->chat)) {
        set_status(app, "Wait for the reply to finish (or press Stop)");
        return;
    }
    chat_clear(app->chat);
    app->chat_id[0] = '\0';
    app->chat_title[0] = '\0';
    app->chat_project[0] = '\0';
    attach_reset(app);
    app->focus = FOCUS_CHAT;
}

static void open_chat(App *app, const ChatSummary *c)
{
    if (chat_busy(app->chat)) {
        set_status(app, "Wait for the reply to finish (or press Stop)");
        return;
    }
    ChatMessage *msgs;
    size_t n;
    if (!chat_store_load(app->project_dir, c->id, &msgs, &n) || !chat_load_messages(app->chat, msgs, n)) {
        set_status(app, "Could not open that chat");
        return;
    }
    /* Its attachments are those of its newest user message, exactly: if
     * that one was sent with nothing, nothing is attached (red-team 007).
     * Renames are followed only if they came after the message. */
    app->nattached = 0;
    app->attach_touched = true;
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
    snprintf(app->chat_id, sizeof(app->chat_id), "%s", c->id);
    snprintf(app->chat_title, sizeof(app->chat_title), "%s", c->title);
    snprintf(app->chat_project, sizeof(app->chat_project), "%s", app->project_dir);
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
    bool ok = app->project_dir[0] && records_append(app->project_dir, REC_NOTE, id, "active", text, src, ns, &ver);
    if (ok) {
        record_view_stored(v, id, ver, "active", text);
        set_status(app, "Note saved");
    } else {
        set_status(app, "Could not save the note to .scribodo/notes.jsonl");
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
    bool ok = full && revisions_snapshot(app->project_dir, editor_path(app->editor), full, s->revision, sizeof(s->revision));
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
        if (records_append(app->project_dir, REC_NOTE, id, "active", s.quote, &s, 1, &ver)) {
            record_view_stored(v, id, ver, "active", s.quote);
            set_status(app, "Saved as a note \xC2\xB7 edit it below");
        } else {
            set_status(app, "Could not save the note to .scribodo/notes.jsonl");
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
    char *text = m && !m->streaming ? strdup(m->content) : NULL;
    chat_unlock(app->chat);
    if (!text) return;
    SourceRef s = { .kind = SRC_CHAT, .message = index, .start = 0, .quote = text };
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
            if (records_append(app->project_dir, REC_NOTE, id, "active", text, &s, 1, &ver)) {
                record_view_stored(app->record, id, ver, "active", text);
                set_status(app, "Saved the reply as a note");
            } else {
                set_status(app, "Could not save the note to .scribodo/notes.jsonl");
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
        for (size_t i = 0; i < app->nchats; i++)
            if (!strcmp(app->chats[i].id, s->chat)) {
                if (strcmp(app->chat_id, s->chat) != 0) open_chat(app, &app->chats[i]);
                char msg[120];
                snprintf(msg, sizeof(msg), "Source: message %ld of this chat", s->message + 1);
                if (!strcmp(app->chat_id, s->chat)) {
                    chat_view_reveal(app->chat_view, s->message);
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
    if (app->project_dir[0] && records_append(app->project_dir, REC_CANON, id, status, text, src, ns, &ver)) {
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
        set_status(app, "Could not save to .scribodo/canon.jsonl");
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
                  records_append(app->project_dir, REC_NOTE, record_view_id(v), "deleted", text ? text : "", src, ns, NULL);
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
    app->nflist = app->project_dir[0] ? records_list(app->project_dir, REC_FINDING, &app->flist) : 0;
    findings_view_set(app->findings, app->flist, app->nflist, app->canon, app->ncanon);
    size_t fresh = 0;
    for (size_t i = 0; i < app->nflist; i++) fresh += !strcmp(record_current(&app->flist[i])->status, "new");
    if (fresh) snprintf(app->observer_button, sizeof(app->observer_button), "Observer (%zu)", fresh);
    else snprintf(app->observer_button, sizeof(app->observer_button), "Observer");
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
        if (!records_append(app->project_dir, REC_NOTE, nid, "active", v->content, v->sources, v->nsources, &ver)) {
            set_status(app, "Could not save the note to .scribodo/notes.jsonl");
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
            set_status(app, "Could not update .scribodo/findings.jsonl");
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
    if (!app->chat_id[0]) {
        chat_store_new_id(app->chat_id, sizeof(app->chat_id));
        snprintf(app->chat_project, sizeof(app->chat_project), "%s", app->project_dir);
        /* Title: the first line of the first message, shortened. */
        size_t n = strcspn(text, "\n");
        if (n > 48) n = 48;
        snprintf(app->chat_title, sizeof(app->chat_title), "%.*s%s", (int)n, text, strlen(text) > n ? "\xE2\x80\xA6" : "");
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
        cdocs[ncd] = (ChatDoc){ app->attached[i], texts[ncd] };
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
            clear_proposal(app);
            clear_pending(app);
            app->pending.active = true;
            chat_lock(app->chat);
            app->pending.index = chat_count(app->chat) - 1;
            chat_unlock(app->chat);
            app->pending.original = strdup(app->edit_text);
            app->pending.instruction = strdup(text);
            editor_anchor_set(app->editor, a_start, a_len); /* clear_proposal dropped it */
            leave_edit_mode(app);
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

static Clay_String cstr(const char *s)
{
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(s), .chars = s };
}

static void label(const char *s, int font, Clay_Color color)
{
    CLAY_TEXT(cstr(s), CLAY_TEXT_CONFIG({ .fontId = (uint16_t)font, .textColor = color,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
}


static void section_header(const char *s)
{
    CLAY({ .layout = { .padding = { 0, 0, 10, 4 } } }) { label(s, FONT_UI, COL_MUTED); }
}

/* A clickable list row. */
static void list_item(Clay_ElementId id, const char *s, bool active)
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
        label(s, FONT_UI, active ? COL_TEXT : COL_MUTED);
    }
}

/* Inactive placeholder text: deliberately not styled like a row. */
static void coming_soon(const char *s)
{
    CLAY({ .layout = { .padding = { 8, 8, 2, 2 } } }) { label(s, FONT_UI, COL_DIM); }
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
    app->model_color = ls == LLM_READY ? COL_OK : ls == LLM_FAILED ? COL_ERR : COL_WARN;

    snprintf(app->chat_header, sizeof(app->chat_header), "%s", app->chat_title[0] ? app->chat_title : "New chat");
}

static Clay_RenderCommandArray build_layout(App *app)
{
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
            label("Scribodo", FONT_UI, COL_TEXT);
            slot(CLAY_ID("OpenButton"), 64, 24);
            label(app->doc_label, FONT_UI, COL_MUTED);
            slot(CLAY_ID("SaveButton"), 64, 24);
            grow();
            label(app->model_label, FONT_UI, app->model_color);
            slot(CLAY_ID("ObserverToggle"), 96, 24);
        }

        CLAY({ .id = CLAY_ID("Main"), .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
            /* Explorer */
            CLAY({ .id = CLAY_ID("Explorer"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(app->explorer_w), CLAY_SIZING_GROW(0) },
                               .padding = CLAY_PADDING_ALL(12), .childGap = 2,
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = COL_PANEL, .clip = { .horizontal = true, .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
                section_header("DOCUMENTS");
                const char *cur = editor_path(app->editor) ? base_name(editor_path(app->editor)) : "";
                if (!app->project_dir[0]) coming_soon("Open a folder (\xE2\x8C\x98O)");
                else if (app->ndocs == 0) coming_soon("No .md or .txt files yet");
                for (size_t i = 0; i < app->ndocs; i++)
                    list_item(CLAY_IDI("DocItem", (uint32_t)i), app->docs[i], !strcmp(cur, app->docs[i]));
                if (app->project_dir[0]) {
                    list_item(CLAY_ID("NewDocItem"), "+ New document", false);
                    list_item(CLAY_ID("ImportItem"), "Import\xE2\x80\xA6", false);
                }
                section_header("CHATS");
                list_item(CLAY_ID("NewChatItem"), "+ New chat", false);
                for (size_t i = 0; i < app->nchats; i++)
                    list_item(CLAY_IDI("ChatItem", (uint32_t)i), app->chats[i].title, !strcmp(app->chat_id, app->chats[i].id));
                if (!app->project_dir[0]) coming_soon("Open a project to save chats");
                section_header("HISTORY");
                if (app->nrevs == 0) coming_soon(editor_path(app->editor) ? "No revisions yet" : "Open a document");
                for (size_t i = 0; i < app->nrevs && i < REV_SHOW; i++)
                    list_item(CLAY_IDI("RevItem", (uint32_t)i), app->rev_labels[i], false);
                if (app->nrevs > REV_SHOW) coming_soon("Older revisions are in .scribodo/revisions");
                section_header("NOTES");
                const char *open_id = record_view_active(app->record) ? record_view_id(app->record) : "";
                if (app->project_dir[0]) list_item(CLAY_ID("NewNoteItem"), "+ New note", false);
                else coming_soon("Open a project to keep notes");
                for (size_t i = 0; i < app->nnotes && i < REC_SHOW; i++)
                    if (app->note_labels[i][0])
                        list_item(CLAY_IDI("NoteItem", (uint32_t)i), app->note_labels[i], !strcmp(open_id, app->notes[i].id));
                section_header("CANON");
                if (app->project_dir[0] && app->ncanon == 0) coming_soon("Select a passage, then \xE2\x8C\x98\xE2\x87\xA7" "C");
                for (size_t i = 0; i < app->ncanon && i < REC_SHOW; i++)
                    list_item(CLAY_IDI("CanonItem", (uint32_t)i), app->canon_labels[i], !strcmp(open_id, app->canon[i].id));
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
                    label("CHAT", FONT_UI, COL_MUTED);
                    label(app->chat_header, FONT_UI, COL_TEXT);
                    grow();
                    slot(CLAY_ID("NewChatButton"), 80, 24);
                }
                CLAY({ .id = CLAY_ID("ChatMessages"),
                       .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
                CLAY({ .id = CLAY_ID("ChatFooter"),
                       .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                                   .padding = { 12, 10, 0, 0 }, .childGap = 8,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    if (app->edit_mode) {
                        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                               .clip = { .horizontal = true } }) {
                            label(app->edit_banner, FONT_UI, COL_WARN);
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
                    section_header("OBSERVER");
                    label(app->observer_label, FONT_UI, COL_DIM);
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
            label(app->status_left, FONT_UI, COL_MUTED);
            grow();
            if (app->status[0] && GetTime() - app->status_time < 6.0)
                label(app->status, FONT_UI, editor_path(app->editor) && editor_is_read_only(app->editor) ? COL_WARN : COL_TEXT);
        }
    }
    return Clay_EndLayout();
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
    if (!strncmp(name, "Record", 6)) return record_view_element(app->record, name);
    if (!strncmp(name, "Prompt", 6)) return prompt_element(app->prompt, name);
    if (!strcmp(name, "AttachAdd")) return app->attach_add;
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

static char *host_explorer_text(void *ctx)
{
    App *app = ctx;
    size_t cap = 2;
    for (size_t i = 0; i < app->ndocs; i++) cap += strlen(app->docs[i]) + 1;
    for (size_t i = 0; i < app->nchats; i++) cap += strlen(app->chats[i].title) + 1;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (size_t i = 0; i < app->ndocs; i++) strcat(strcat(s, app->docs[i]), "\n");
    for (size_t i = 0; i < app->nchats; i++) strcat(strcat(s, app->chats[i].title), "\n");
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;

    /* UI test mode: SCRIBODO_SCRIPT=file.scribo runs a scripted session in a
     * hidden window (no focus stealing, immune to the real mouse) and exits
     * non-zero if any expectation fails. See src/app/script.h. */
    const char *script_path = getenv("SCRIBODO_SCRIPT");
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
    SetWindowMinSize(900, 480);
    SetExitKey(KEY_NULL); /* Esc must never quit a writing app */
    SetTargetFPS(60);

    static App app;
    app.explorer_w = 210;
    app.observer_w = 260;
    app.chat_w = 420;
    app.focus = FOCUS_DOC;

    float dpi = GetWindowScaleDPI().x;
    if (!load_fonts(&app, dpi)) {
        fprintf(stderr, "scribodo: no usable font found (set SCRIBODO_PROSE_FONT)\n");
        CloseWindow();
        return 1;
    }
    style_raygui(&app.fonts[FONT_UI]);

    uint32_t clay_mem = Clay_MinMemorySize();
    void *clay_buf = malloc(clay_mem);
    if (!clay_buf) {
        fprintf(stderr, "scribodo: out of memory\n");
        CloseWindow();
        return 1;
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem, clay_buf);
    Clay_Initialize(arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_error, 0 });
    Clay_SetMeasureTextFunction(clay_measure_text, app.font_ptrs);

    /* The local model. UI scripts use a fake model unless SCRIBODO_LLM=real. */
    const char *llm_mode = getenv("SCRIBODO_LLM");
    bool fake = (llm_mode && !strcmp(llm_mode, "fake")) || (script_path && !(llm_mode && !strcmp(llm_mode, "real")));
    if (!fake) {
        LlmServerConfig cfg;
        llm_server_default_config(&cfg, GetApplicationDirectory());
        app.llm = llm_server_create(&cfg);
        if (app.llm) llm_server_start(app.llm);
    }
    app.chat = chat_create(app.llm);
    app.editor = editor_create(&app.fonts[FONT_PROSE]);
    app.chat_view = app.chat ? chat_view_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI], app.chat) : NULL;
    app.review = review_create(&app.fonts[FONT_PROSE], &app.fonts[FONT_UI]);
    app.record = record_view_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI]);
    app.prompt = prompt_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI]);
    app.observer = observer_create(app.llm);
    app.findings = findings_view_create(&app.fonts[FONT_CHAT], &app.fonts[FONT_UI]);
    app.findings_version = UINT64_MAX;
    /* UI scripts start with the observer off (deterministic files); a
     * script turns it on with "observer on". */
    if (app.observer && script_path) observer_set_enabled(app.observer, false);
    if (!app.chat || !app.editor || !app.chat_view || !app.review || !app.record || !app.observer || !app.findings || !app.prompt) {
        fprintf(stderr, "scribodo: out of memory\n");
        CloseWindow();
        return 1;
    }
    chat_on_message_done(app.chat, on_message_done, &app);

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
        recent_set_dir(state);
        app.nrecents = recent_load(app.recents, RECENT_MAX);
    }
    dialogs_script_mode(script_path != NULL);
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

    Script *script = NULL;
    ScriptHost host = { app.editor, &app.explorer_w, &app.observer_w, &app.editor_rect,
                        host_status, host_open, host_drop, &app, app.chat, host_element, host_model_ready,
                        host_record_text, host_observer_enable, host_observer_idle, host_findings_text,
                        host_attached_text, host_explorer_text };
    if (script_path) {
        char err[256];
        script = script_load(script_path, getenv("SCRIBODO_SCRIPT_DIR"), getenv("SCRIBODO_SHOT_DIR"), err, sizeof(err));
        if (!script) {
            fprintf(stderr, "scribodo: %s\n", err);
            CloseWindow();
            return 2;
        }
        printf("script %s\n", script_path);
    }
    bool closed_by_window = false;

    /* Debug hook: SCRIBODO_SCREENSHOT=out.png renders a few frames, saves the
     * framebuffer, and exits. */
    const char *screenshot_path = getenv("SCRIBODO_SCREENSHOT");
    int frame = 0;
    bool trace = getenv("SCRIBODO_INPUT_TRACE") != NULL;

    int cursor_shape = MOUSE_CURSOR_DEFAULT;
    bool running = true;
    while (running) {
        /* Scripted input is injected before anything reads input state. */
        if (script && !script_frame(script, &host)) break;

        if (app.llm) llm_server_tick(app.llm);
        chat_poll(app.chat);
        /* The observer shares the model: it yields whenever chat is active. */
        observer_set_chat_active(app.observer, chat_busy(app.chat));
        if (observer_version(app.observer) != app.findings_version) {
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
                chat_clear(app.chat);
                app.chat_id[0] = app.chat_title[0] = app.chat_project[0] = '\0';
                attach_reset(&app); /* the old project's documents aren't this one's */
            }
        }
        if (GetTime() - app.lists_refreshed > 3.0) app.lists_dirty = true;

        /* Close requests: never lose unsaved prose silently. */
        if (WindowShouldClose()) {
            /* Notes save themselves; uncommitted canon text counts as unsaved. */
            bool note_lost = record_view_active(app.record) && record_view_kind(app.record) == REC_NOTE &&
                             record_view_dirty(app.record) && !save_note(&app);
            bool unsaved = editor_is_dirty(app.editor) || record_unsaved_canon(&app) || note_lost;
            if (!unsaved || (app.close_armed && GetTime() - app.close_armed_at < 5.0)) {
                running = false;
                closed_by_window = true;
                break;
            }
            glfwSetWindowShouldClose(GetWindowHandle(), 0);
            app.close_armed = true;
            app.close_armed_at = GetTime();
            set_status(&app, "Unsaved changes \xE2\x80\x94 press \xE2\x8C\x98S to save, or close again to discard");
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
        bool modal = prompt_active(app.prompt) || app.menu.active;
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
            else if (Clay_PointerOver(CLAY_ID("NewDocItem"))) begin_new_document(&app);
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
                        snprintf(full, sizeof(full), "%s/%s", app.project_dir, app.docs[i]);
                        drop_path(&app, full);
                        app.focus = FOCUS_DOC;
                        break;
                    }
                for (size_t i = 0; i < app.nchats; i++)
                    if (Clay_PointerOver(CLAY_IDI("ChatItem", (uint32_t)i))) {
                        open_chat(&app, &app.chats[i]);
                        app.focus = FOCUS_CHAT;
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
            }
            /* Toolbar, chat header/footer and message list keep the focus. */
        }
        if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && Clay_PointerOver(CLAY_ID("Explorer"))) {
            for (size_t i = 0; i < app.nchats; i++)
                if (Clay_PointerOver(CLAY_IDI("ChatItem", (uint32_t)i))) {
                    static const char *const items[] = { "Rename\xE2\x80\xA6" };
                    snprintf(app.menu_chat, sizeof(app.menu_chat), "%s", app.chats[i].id);
                    menu_open(&app.menu, MT_CHAT, (int)i, mouse, items, 1);
                    break;
                }
            for (size_t i = 0; i < app.ndocs; i++)
                if (Clay_PointerOver(CLAY_IDI("DocItem", (uint32_t)i))) {
                    static const char *const items[] = { "Rename\xE2\x80\xA6", "Reveal in Finder" };
                    snprintf(app.menu_doc, sizeof(app.menu_doc), "%s", app.docs[i]); /* by name: the list may re-sort */
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
        if (app.lists_dirty) {
            refresh_lists(&app);
            app.lists_dirty = false;
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
        chat_view_update(app.chat_view, app.chat_list_rect, app.chat_input_rect, idle && app.focus == FOCUS_CHAT);
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
            char *reply = m ? strdup(m->content) : NULL;
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
                app.attach_touched = true;
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
        } else {
            bool ready = !app.llm || llm_server_state(app.llm, NULL, 0) == LLM_READY;
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
                app.attach_touched = true;
            }
            if (pick >= 0 && app.menu.tag == MT_CHAT) {
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
        const char *shot_path = script ? script_take_shot(script) : NULL;
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

    int rc = script ? script_finish(script, &host, closed_by_window) : 0;
    script_free(script);
    chat_view_destroy(app.chat_view);
    clear_proposal(&app);
    clear_pending(&app);
    leave_edit_mode(&app);
    review_destroy(app.review);
    record_view_destroy(app.record);
    prompt_destroy(app.prompt);
    records_free(app.notes, app.nnotes);
    records_free(app.canon, app.ncanon);
    free(app.revs);
    observer_destroy(app.observer);  /* cancels its request before the server stops */
    findings_view_destroy(app.findings);
    records_free(app.flist, app.nflist);
    chat_destroy(app.chat);          /* cancels any reply in progress */
    llm_server_destroy(app.llm);     /* stops llama-server and removes its socket */
    editor_destroy(app.editor);
    for (size_t i = 0; i < app.ndocs; i++) free(app.docs[i]);
    free(app.chats);
    for (int i = 0; i < FONT_COUNT; i++) sfont_unload(&app.fonts[i]);
    free(clay_buf);
    CloseWindow();
    return rc;
}
