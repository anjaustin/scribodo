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
#include "app/records.h"
#include "app/revisions.h"
#include "llm/llm_text.h"
#include "app/script.h"
#include "llm/chat.h"
#include "llm/server.h"
#include "ui/chat_view.h"
#include "ui/clay_render.h"
#include "ui/editor.h"
#include "ui/font.h"
#include "ui/input.h"
#include "ui/record_view.h"
#include "ui/review_view.h"

/* raylib re-reads GLFW's close flag every frame, so clearing it cancels a
 * close request. GLFWwindow is opaque; raylib hands us the pointer. */
void glfwSetWindowShouldClose(void *window, int value);

enum { FONT_UI = 0, FONT_PROSE = 1, FONT_CHAT = 2, FONT_COUNT };
enum { FOCUS_NONE, FOCUS_DOC, FOCUS_CHAT, FOCUS_RECORD };
enum { DRAG_NONE, DRAG_EXPLORER, DRAG_OBSERVER, DRAG_CHAT };

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
    bool include_doc;
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
            if ((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")))
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
        { "source", "Quoted in a note/canon" },
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

static void set_project_from(App *app, const char *doc_path)
{
    char real[PATH_MAX];
    project_of(doc_path, real, sizeof(real));
    if (strcmp(real, app->project_dir) != 0) {
        snprintf(app->project_dir, sizeof(app->project_dir), "%s", real);
        /* A different project has different chats: start fresh (after the
         * current reply finishes, so it is saved with its own project). */
        if (app->chat) app->clear_chat_when_idle = true;
    }
    app->lists_dirty = true;
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
    } else {
        set_status(app, msg);
    }
}

/* File-drop handler (also used by the Explorer and UI scripts). */
static void drop_path(App *app, const char *path)
{
    if (editor_is_dirty(app->editor)) set_status(app, "Save the current document before opening another");
    else open_path(app, path);
}

static void save(App *app)
{
    char msg[512];
    bool ok = editor_save(app->editor, msg, sizeof(msg));
    set_status(app, msg);
    if (ok) record_revision(app, "save", NULL);
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

static void show_record(App *app, RecordKind kind, const Record *rec, const char *content, const SourceRef *src, size_t ns)
{
    if (!close_record(app, false)) return;
    if (!record_view_open(app->record, kind, rec, content, src, ns)) {
        set_status(app, "Could not open that entry");
        return;
    }
    app->record_close_armed = false;
    app->focus = FOCUS_RECORD;
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
    show_record(app, REC_NOTE, NULL, have ? s.quote : "", have ? &s : NULL, have ? 1 : 0);
    if (have && record_view_active(app->record)) {
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
        show_record(app, REC_NOTE, NULL, text, &s, 1);
        if (record_view_active(app->record)) {
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
                snprintf(msg, sizeof(msg), "Source: reply %ld of this chat", s->message + 1);
                if (!strcmp(app->chat_id, s->chat)) set_status(app, msg);
                return;
            }
        set_status(app, "The source chat is no longer in this project");
        return;
    }
    char full[PATH_MAX + 300];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, s->doc);
    if (!editor_path(app->editor) || strcmp(base_name(editor_path(app->editor)), s->doc) != 0) {
        if (editor_is_dirty(app->editor)) {
            set_status(app, "Save the current document before opening the source");
            return;
        }
        open_path(app, full);
        if (!editor_path(app->editor) || strcmp(base_name(editor_path(app->editor)), s->doc) != 0) return;
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
    ChatContext ctx = { 0 };
    char *doc = NULL;
    if (app->include_doc && editor_path(app->editor)) {
        doc = editor_text_utf8(app->editor);
        ctx.doc_title = base_name(editor_path(app->editor));
        ctx.doc_text = doc;
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

static void wrapped_label(const char *s, Clay_Color color)
{
    CLAY_TEXT(cstr(s), CLAY_TEXT_CONFIG({ .fontId = FONT_UI, .textColor = color,
                                         .wrapMode = CLAY_TEXT_WRAP_WORDS }));
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
           .cornerRadius = CLAY_CORNER_RADIUS(4),
           .clip = { .horizontal = true } }) {
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
    if (!path)
        snprintf(app->doc_label, sizeof(app->doc_label), "No document \xE2\x80\x94 drop a .md or .txt file here");
    else
        snprintf(app->doc_label, sizeof(app->doc_label), "%s%s%s", base_name(path),
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
                   .backgroundColor = COL_PANEL, .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
                section_header("DOCUMENTS");
                const char *cur = editor_path(app->editor) ? base_name(editor_path(app->editor)) : "";
                if (app->ndocs == 0) coming_soon("No .md or .txt files here");
                for (size_t i = 0; i < app->ndocs; i++)
                    list_item(CLAY_IDI("DocItem", (uint32_t)i), app->docs[i], !strcmp(cur, app->docs[i]));
                section_header("CHATS");
                list_item(CLAY_ID("NewChatItem"), "+ New chat", false);
                for (size_t i = 0; i < app->nchats; i++)
                    list_item(CLAY_IDI("ChatItem", (uint32_t)i), app->chats[i].title, !strcmp(app->chat_id, app->chats[i].id));
                if (!app->project_dir[0]) coming_soon("Open a document to save chats");
                section_header("HISTORY");
                if (app->nrevs == 0) coming_soon(editor_path(app->editor) ? "No revisions yet" : "Open a document");
                for (size_t i = 0; i < app->nrevs && i < REV_SHOW; i++)
                    list_item(CLAY_IDI("RevItem", (uint32_t)i), app->rev_labels[i], false);
                if (app->nrevs > REV_SHOW) coming_soon("Older revisions are in .scribodo/revisions");
                section_header("NOTES");
                const char *open_id = record_view_active(app->record) ? record_view_id(app->record) : "";
                if (app->project_dir[0]) list_item(CLAY_ID("NewNoteItem"), "+ New note", false);
                else coming_soon("Open a document to keep notes");
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
                        slot(CLAY_ID("IncludeDoc"), 16, 16);
                        label("Include document", FONT_UI, COL_MUTED);
                        grow();
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
                    wrapped_label("Coming soon. The observer will list characters, events, relationships, open "
                                  "questions, and possible continuity conflicts here \xE2\x80\x94 it never writes "
                                  "into your chat or documents.", COL_DIM);
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
                label(app->status, FONT_UI, editor_is_read_only(app->editor) ? COL_WARN : COL_TEXT);
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

static bool host_model_ready(void *ctx)
{
    App *app = ctx;
    return !app->llm || llm_server_state(app->llm, NULL, 0) == LLM_READY;
}
static void host_drop(void *ctx, const char *path) { drop_path(ctx, path); }

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
    app.include_doc = true;

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
    if (!app.chat || !app.editor || !app.chat_view || !app.review || !app.record) {
        fprintf(stderr, "scribodo: out of memory\n");
        CloseWindow();
        return 1;
    }
    chat_on_message_done(app.chat, on_message_done, &app);

    /* Default to the bundled sample, located relative to the executable
     * (build/scribodo) rather than the working directory, which is "/" when
     * launched from Finder. */
    char sample[2048];
    if (!path) {
        snprintf(sample, sizeof(sample), "%s../assets/sample/sample.md", GetApplicationDirectory());
        path = sample;
    }
    if (!script_path) open_path(&app, path);

    Script *script = NULL;
    ScriptHost host = { app.editor, &app.explorer_w, &app.observer_w, &app.editor_rect,
                        host_status, host_open, host_drop, &app, app.chat, host_element, host_model_ready,
                        host_record_text };
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
            if (dropped.count > 0) drop_path(&app, dropped.paths[0]);
            UnloadDroppedFiles(dropped);
        }

        Vector2 mouse = GetMousePosition();
        bool mouse_down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y }, mouse_down);
        /* The Explorer scrolls (long History / chat lists). */
        Vector2 wheel = GetMouseWheelMoveV();
        Clay_UpdateScrollContainers(false, (Clay_Vector2){ wheel.x * 30.0f, wheel.y * 30.0f }, GetFrameTime());
        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });

        /* Clicks (pointer state reflects last frame's layout). */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
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

        bool primary = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (primary && shortcut_pressed('s')) {
            if (app.focus == FOCUS_RECORD && record_view_kind(app.record) == REC_NOTE) save_note(&app);
            else if (app.focus != FOCUS_RECORD) save(&app);
        }
        if (primary && shift && shortcut_pressed('n')) note_from_selection(&app);
        if (primary && shift && shortcut_pressed('c') && app.focus == FOCUS_DOC) canon_from_selection(&app);
        if (app.focus == FOCUS_RECORD && IsKeyPressed(KEY_ESCAPE)) app.focus = FOCUS_DOC;
        if (primary && shortcut_pressed('l')) app.focus = FOCUS_CHAT;
        if (primary && shortcut_pressed('k')) begin_edit(&app);
        /* Cmd+Enter accepts only outside the chat box, where it also sends the
         * message being typed (red-team 004: one key did both). */
        if (primary && review_active(app.review) && app.focus != FOCUS_CHAT &&
            (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)))
            accept_proposal(&app);
        if (app.focus == FOCUS_CHAT && IsKeyPressed(KEY_ESCAPE)) {
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
        app.chat_input_rect = ELEMENT_RECT("ChatInput");
        Rectangle save_rect = ELEMENT_RECT("SaveButton");
        Rectangle obs_rect = ELEMENT_RECT("ObserverToggle");
        Rectangle new_rect = ELEMENT_RECT("NewChatButton");
        Rectangle send_rect = ELEMENT_RECT("SendButton");
        Rectangle inc_rect = ELEMENT_RECT("IncludeDoc");

        /* Always update both so queued keystrokes are consumed or discarded. */
        bool idle = app.dragging == DRAG_NONE;
        editor_update(app.editor, app.editor_rect, idle && app.focus == FOCUS_DOC);
        chat_view_update(app.chat_view, app.chat_list_rect, app.chat_input_rect, idle && app.focus == FOCUS_CHAT);
        if (review_active(app.review)) review_update(app.review, app.review_rect);
        if (record_view_active(app.record)) record_view_update(app.record, app.record_rect, idle && app.focus == FOCUS_RECORD);
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
        editor_draw(app.editor, app.editor_rect, app.focus == FOCUS_DOC);
        if (review_active(app.review)) {
            review_draw(app.review, app.review_rect);
            if (GuiButton(review_accept_rect(app.review_rect), "Accept")) accept_proposal(&app);
            if (GuiButton(review_reject_rect(app.review_rect), "Reject")) reject_proposal(&app);
        }
        if (record_view_active(app.record))
            record_action(&app, record_view_draw(app.record, app.record_rect, app.focus == FOCUS_RECORD));
        chat_view_draw(app.chat_view, app.chat_list_rect, app.chat_input_rect, app.focus == FOCUS_CHAT);

        bool can_save = (editor_is_dirty(app.editor) || editor_overwrite_armed(app.editor)) &&
                        !editor_is_read_only(app.editor);
        if (!can_save) GuiDisable();
        if (GuiButton(save_rect, "Save")) save(&app);
        GuiEnable();
        /* raygui draws with the primary face only (no fallback), so ASCII. */
        if (GuiButton(obs_rect, app.observer_open ? "Hide observer" : "Observer"))
            app.observer_open = !app.observer_open;
        if (chat_busy(app.chat)) GuiDisable();
        if (GuiButton(new_rect, "New chat")) new_chat(&app);
        GuiEnable();
        if (app.edit_mode) {
            if (GuiButton(ELEMENT_RECT("CancelEdit"), "Cancel")) {
                leave_edit_mode(&app);
                editor_anchor_clear(app.editor);
            }
        } else {
            GuiCheckBox(inc_rect, NULL, &app.include_doc);
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
    records_free(app.notes, app.nnotes);
    records_free(app.canon, app.ncanon);
    free(app.revs);
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
