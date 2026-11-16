/* Scribodo: native workbench (raylib + Clay layout + ui/widgets controls) with
 * the prose editor, a creative chat backed by the local LFM3 model, and the
 * project explorer.
 *
 *   scribodo [file.md|file.txt]
 *
 * Drop a .md/.txt file on the window to open it. ⌘S saves. ⌘L focuses the
 * chat; Esc returns to the document.
 *
 * This file is the composition root and the frame loop (AUDIT-004 F3). App and the functions shared across the app
 * are in app/app_internal.h; startup and teardown in app/app_lifecycle.c; the layout in ui/layout.c; the workflows in
 * app/chat_workflow.c, app/document_workflow.c and app/record_workflow.c; the scripted host in app/script_host_app.c.
 */
#define _DARWIN_C_SOURCE

#include "app/app_internal.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/mac_window.h"
#include "ui/motion.h"
#include "ui/backdrop.h"
#include "app/tools_dir.h"

static bool store_adopt_project(App *app);

void set_status(App *app, const char *msg)
{
    snprintf(app->status, sizeof(app->status), "%s", msg);
    app->status_time = GetTime();
}

/* A status that follows another within two seconds joins it instead of replacing it: on the native store the import
 * of a restored project finished one frame after "Restored as …" was set, so the author never saw the restore
 * reported (measured 2026-11-14, journal/store-cycle2_native_services.md). */
static void set_status_after(App *app, const char *msg)
{
    if (app->status[0] && GetTime() - app->status_time < 2.0 && !strstr(app->status, msg)) {
        char joined[sizeof(app->status)];
        snprintf(joined, sizeof(joined), "%s \xC2\xB7 %s", app->status, msg);
        snprintf(app->status, sizeof(app->status), "%s", joined);
        app->status_time = GetTime();
        return;
    }
    set_status(app, msg);
}

/* A storage failure, explained: while the project store is offline the
 * file name in `fallback` would mislead. */
const char *store_says(App *app, const char *fallback)
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

const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- project lists ------------------------------------------------------------- */

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

bool memory_controls_pending(App *app)
{
    MemoryJobStatus status;
    return app->memory_job && memory_job_status(app->memory_job, &status) && status.controls_pending;
}

void memory_editor_will_change(void *context)
{
    App *app = context;
    const char *path = editor_path(app->editor);
    if (path && chat_controller_has_attachment(&app->chat_controller, base_name(path)))
        chat_memory_attachments_changed(app->chat);
}

void memory_document_names(App *app)
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
        app->memory_label[0] = '\0'; /* no stored conversation: nothing to say (memory needs one) */
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
        snprintf(app->memory_label, sizeof(app->memory_label), "Memory \xC2\xB7 %.220s", status.why);
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
    backdrop_draw((Rectangle){ 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() });
    widget_shadow(box, RADIUS_M, 16);
    widget_round_rect(box, RADIUS_M, THEME_SURFACE);
    widget_round_outline(box, RADIUS_M, 1.0f, THEME_BORDER_STRONG);
    const char *title = "As-of retrieval sources - read only; not canon or proof of exhaustive recall";
    sfont_draw_utf8(&app->fonts[FONT_UI], title, (int)strlen(title), box.x + 14, box.y + 12, THEME_TEXT);
    app->memory_sources_close = (Rectangle){ box.x + box.width - 90, box.y + box.height - 40, 76, 28 };
    Rectangle content = { box.x + 8, box.y + 38, box.width - 16, box.height - 88 };
    editor_update(app->memory_sources_editor, content, true);
    editor_draw(app->memory_sources_editor, content, true);
    if (widget_button(app->memory_sources_close, "Close", NULL, WIDGET_OUTLINE, true) || IsKeyPressed(KEY_ESCAPE)) app->memory_sources_open = false;
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
         * writing) the files an import is about to set aside. The import runs
         * on a worker; the observer joins this project when it has finished. */
        app->observer_project_deferred = store_adopt_project(app);
        if (app->observer) observer_set_project(app->observer, app->observer_project_deferred ? "" : real);
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
    bool have_tools = tools_dir_resolve(GetApplicationDirectory(), tools, sizeof(tools));
    /* Native services have no container runtime to restart them: a store that drops after a successful start (a
     * daemon that died) is started again, at most once a minute (REDTEAM-023; `up` keeps running daemons as they are). */
    bool start = !getenv("SCRIBODO_SCRIPT") && (!app->services_started || GetTime() - app->services_started_at >= 60.0);
    if (start && !have_tools) { /* still probe: services started by hand are used */
        static const char why[] = "Scribodo's scripts weren't found beside the app (tools/services.sh); set SCRIBODO_TOOLS_DIR";
        if (strcmp(app->store_why, why)) set_status(app, why);
        snprintf(app->store_why, sizeof(app->store_why), "%s", why);
        app->services_headline = "the services script isn't installed";
        start = false;
    }
    (void)service_supervisor_start(&app->services, tools, start);
}

void store_begin(App *app)
{
    app->store_why[0] = '\0';
    if (store_start(app->store, getenv("SCRIBODO_DB_NAME"), app->store_why, sizeof(app->store_why))) return;
    store_try_later(app); /* start the services / keep probing in the background */
}

/* A project opened while the store is up is moved into it once. The move
 * runs on an owned worker (MEMORY-019 measured 0.8 s of blocked frames for a
 * 100K-word project when it ran here); the store makes every other thread see
 * the project as paused meanwhile, and import_poll() applies the outcome.
 * Returns true when the observer must wait: a move for the current project was
 * started now, or it is due and waits for the worker (import_job_next). */
static bool store_adopt_project(App *app)
{
    if (!app->project_dir[0] || !store_online(app->store)) return false;
    /* A move that died after its marker is finished here, once, on open (GAP-004 matrix, import row). */
    if (store_finish_import(app->store, app->project_dir)) set_status(app, "Finished moving this project into the project store (its old files are in .scribodo/legacy-\xE2\x80\xA6)");
    if (!app->import_job_initialized) return false;
    bool needs = store_needs_import(app->store, app->project_dir);
    /* Chat titles an older import left in .scribodo/legacy-*: copied once, on the import worker (AUDIT-004 F11). */
    bool backfill = !needs && store_backfill_pending(app->store, app->project_dir);
    switch (import_job_next(needs, backfill, import_job_busy(&app->import_job))) {
    case IMPORT_NEXT_NONE: return false;
    case IMPORT_NEXT_DEFER: return true; /* started by import_poll when the worker frees */
    case IMPORT_NEXT_BACKFILL: import_job_start_backfill(&app->import_job, app->project_dir); return false;
    case IMPORT_NEXT_IMPORT: break;
    }
    if (!import_job_start(&app->import_job, app->project_dir)) {
        set_status(app, "Could not start moving the project into the store");
        return false;
    }
    set_status(app, "Moving this project into the project store\xE2\x80\xA6 (its old files will be kept in .scribodo/legacy-\xE2\x80\xA6)");
    return true;
}

static void import_poll(App *app)
{
    char dir[PATH_MAX], why[400] = "";
    bool ok = false;
    if (!app->import_job_initialized || !import_job_poll(&app->import_job, dir, sizeof(dir), &ok, why, sizeof(why))) return;
    bool current = strcmp(dir, app->project_dir) == 0;
    bool backfill = import_job_last_was_backfill(&app->import_job);
    if (current && backfill && why[0]) set_status(app, why); /* the chat-title backfill: a line only if it failed or skipped */
    if (current && !backfill) {
        if (ok) {
            app->store_just_moved = true;
            set_status_after(app, "This project is now kept in the project store (its old files are in .scribodo/legacy-\xE2\x80\xA6)");
        } else set_status(app, why);
        app->lists_dirty = true;
        app->findings_version = UINT64_MAX;
        if (app->observer_project_deferred && app->observer) observer_set_project(app->observer, app->project_dir);
        app->observer_project_deferred = false;
        if (app->observer) observer_poke(app->observer);
    } else if (!(current && !ok)) { /* a failed backfill of this project waits for its next open */
        /* The worker is free: the current project's deferred move, or its waiting backfill, starts now (the author moved
         * on while another project was moved or backfilled; AUDIT-004 final verification 2a). */
        if (store_adopt_project(app)) {
            app->observer_project_deferred = true;
            if (app->observer) observer_set_project(app->observer, "");
        } else if (app->observer_project_deferred) { /* deferred, and nothing to wait for any more */
            app->observer_project_deferred = false;
            if (app->observer) observer_set_project(app->observer, app->project_dir);
        }
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

static bool tools_dir(App *app, char *out, size_t outlen)
{
    if (tools_dir_resolve(GetApplicationDirectory(), out, outlen)) return true;
    set_status(app, "Backups need Scribodo's scripts, which weren't found beside the app (tools/backup.sh); set SCRIBODO_TOOLS_DIR");
    return false;
}

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
    if (!tools_dir(app, tools, sizeof(tools))) return;
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
    if (!tools_dir(app, tools, sizeof(tools))) return;
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

void leave_edit_mode(App *app)
{
    reviewed_edit_controller_leave_mode(&app->reviewed_edit);
}

/* Show a proposal for review. Takes ownership of nothing; copies. */
void propose(App *app, size_t start, size_t len, const char *original, const char *replacement, const char *note,
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

/* The project folder a document belongs to (its directory, resolved). */
void project_of(const char *doc_path, char *out, size_t outlen)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", doc_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    char real[PATH_MAX];
    snprintf(out, outlen, "%s", realpath(dir, real) ? real : dir);
}

void open_path(App *app, const char *path)
{
    char msg[512];
    /* An open note or canon entry belongs to its project: settle it before
     * switching to another one. */
    if (record_view_active(app->record)) {
        char proj[PATH_MAX];
        project_of(path, proj, sizeof(proj));
        if (strcmp(proj, app->project_dir) != 0 && !close_record(app, false)) return;
    }
    /* No draft write may be in flight while the load recovers strays beside the document (draft_controller.h). */
    const char *open_doc = editor_path(app->editor);
    draft_controller_before_load(&app->draft, open_doc, open_doc && editor_is_dirty(app->editor));
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
void drop_path(App *app, const char *path)
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
void open_project(App *app, const char *dir)
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
void memfit_message(const MemFit *f, int wanted, char *out, size_t outlen)
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
void apply_model_settings(App *app, bool context)
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

/* The panel (drawn over everything). */
static void draw_model_settings(App *app, Rectangle screen)
{
    backdrop_draw(screen);
    float w = 620, rowh = 44, h = 110 + rowh * MODEL_SETTING_COUNT + 56 + (app->llm ? 26 : 0);
    Rectangle p = { (screen.width - w) / 2, (screen.height - h) / 2, w, h };
    widget_shadow(p, RADIUS_M, 16);
    widget_round_rect(p, RADIUS_M, THEME_SURFACE);
    widget_round_outline(p, RADIUS_M, 1.0f, THEME_BORDER_STRONG);
    const SFont *ui = &app->fonts[FONT_UI];
    widget_text(ui, "Model settings (chat)", p.x + 24, p.y + 16, w - 48, THEME_TEXT_STRONG);
    widget_text(ui, "The observer always uses temperature 0, so its findings stay reproducible.", p.x + 24, p.y + 44,
                w - 48, THEME_TEXT_MUTED);
    for (int i = 0; i < MODEL_SETTING_COUNT; i++) {
        const ModelSettingInfo *in = model_setting_info(i);
        float y = p.y + 80 + rowh * (float)i;
        widget_text(ui, in->label, p.x + 24, y, 230, THEME_TEXT);
        widget_text(ui, in->hint, p.x + 24, y + 18, 330, THEME_TEXT_DIM);
        double v = model_setting_get(&app->model_edit, i);
        char val[32];
        if (in->integer) snprintf(val, sizeof(val), "%d", (int)v);
        else snprintf(val, sizeof(val), "%.2f", v);
        app->set_minus[i] = (Rectangle){ p.x + w - 200, y + 4, 36, 28 };
        app->set_plus[i] = (Rectangle){ p.x + w - 60, y + 4, 36, 28 };
        if (widget_button(app->set_minus[i], "\xE2\x88\x92", NULL, WIDGET_OUTLINE, true)) model_setting_set(&app->model_edit, i, v - in->step);
        const SFont *mono = &app->fonts[FONT_MONO];
        float vw = sfont_measure_utf8(mono, val, (int)strlen(val));
        widget_text(mono, val, p.x + w - 110 - vw / 2, y + 4 + (28 - mono->line_height) / 2, 0, THEME_TEXT);
        if (widget_button(app->set_plus[i], "+", NULL, WIDGET_OUTLINE, true)) model_setting_set(&app->model_edit, i, v + in->step);
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
        widget_text(ui, line, p.x + 24, by - 32, w - 48, THEME_TEXT_MUTED);
    }
    app->set_defaults = (Rectangle){ p.x + 24, by, 100, 30 };
    app->set_cancel = (Rectangle){ p.x + w - 228, by, 96, 30 };
    app->set_apply = (Rectangle){ p.x + w - 120, by, 96, 30 };
    if (widget_button(app->set_defaults, "Defaults", NULL, WIDGET_QUIET, true)) model_settings_defaults(&app->model_edit);
    if (widget_button(app->set_cancel, "Cancel", NULL, WIDGET_OUTLINE, true) || IsKeyPressed(KEY_ESCAPE)) app->settings_open = false;
    if (widget_button(app->set_apply, "Apply", NULL, WIDGET_PRIMARY, true)) {
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
void apply_profile(App *app)
{
    if (app->chat) chat_set_names(app->chat, app->profile.author, app->profile.assistant);
    if (app->chat_view) {
        chat_view_set_assistant_name(app->chat_view, profile_assistant_name(&app->profile));
        chat_view_set_author_name(app->chat_view, app->profile.author);
    }
    if (app->observer) observer_set_names(app->observer, app->profile.author, app->profile.assistant);
}

void begin_names(App *app)
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

void save(App *app)
{
    char msg[512];
    bool ok = editor_save(app->editor, msg, sizeof(msg));
    set_status(app, msg);
    if (ok) record_revision(app, "save", NULL);
    if (ok && app->observer) observer_poke(app->observer); /* a saved revision: new text to read */
    app->lists_dirty = true;
}

/* ---- drafts: the unsaved text survives a dead battery (app/draft_controller.h) ------------------- */

static char *draft_editor_text(void *ctx)
{
    return editor_text_utf8((const Editor *)ctx);
}

static void draft_tick(App *app)
{
    const char *path = editor_path(app->editor);
    bool dirty = path && editor_is_dirty(app->editor);
    (void)draft_controller_tick(&app->draft, path, dirty, GetTime(), draft_editor_text, app->editor);
    int error = 0;
    if (draft_controller_take_failure(&app->draft, &error)) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Couldn't keep a draft of the unsaved text beside the document (%s); save to keep it",
                 error ? strerror(error) : "write failed");
        set_status(app, msg);
    }
}

/* ---- the welcome screen ---------------------------------------------------------------- */

/* The document area when nothing is open: a welcome screen (no project)
 * or a prompt to pick or create a document. Returns the button pressed:
 * 1 open folder, 2 new project, 3 sample, 10+i recent project i,
 * 20 new document, 21 import; 0 for none. */
static int draw_empty(App *app, Rectangle r)
{
    const SFont *big = &app->fonts[FONT_PROSE], *ui = &app->fonts[FONT_UI];
    Color text = THEME_TEXT_STRONG, muted = THEME_TEXT_MUTED;
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
            if (widget_button(app->welcome_btn[i], labels[i], NULL, i == 0 ? WIDGET_PRIMARY : WIDGET_OUTLINE, true)) act = 1 + i;
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
                if (hot) DrawRectangleRounded(rr, 0.2f, 4, THEME_HOVER);
                sfont_draw_utf8(ui, name, (int)strlen(name), x, y, THEME_LINK);
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
        if (widget_button(app->empty_btn[0], "New document", NULL, WIDGET_PRIMARY, true)) act = 20;
        if (widget_button(app->empty_btn[1], "Import\xE2\x80\xA6", NULL, WIDGET_OUTLINE, true)) act = 21;
    }
    return act;
}

void empty_action(App *app, int act, const char *sample)
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

/* ---- Recall panel (MEMORY-015 S1) ------------------------------------- */

static void toggle_recall(App *app)
{
    if (app->focus_mode) app_set_focus_mode(app, false); /* a pane is not shown in focus mode */
    app->recall_open = !app->recall_open;
    if (app->recall_open) {
        app->observer_open = false;
        app->focus = FOCUS_RECALL;
    } else if (app->focus == FOCUS_RECALL) app->focus = FOCUS_DOC;
}

/* A clicked row opens the canon entry's panel, or selects the excerpt's
 * exact source passage through the same path findings and canon use. */
static void open_recall_row(App *app, size_t index)
{
    const RecallResult *r = recall_view_result(app->recall);
    if (!r || index >= r->count) return;
    const RecallRow *row = &r->rows[index];
    if (row->kind == RECALL_CANON) {
        for (size_t i = 0; i < app->ncanon; i++)
            if (!strcmp(app->canon[i].id, row->record_id)) {
                show_record(app, REC_CANON, &app->canon[i], NULL, NULL, 0);
                return;
            }
        set_status(app, "That canon entry is no longer listed in this project");
        return;
    }
    SourceRef s = { 0 };
    s.start = row->codepoint_start;
    s.len = row->codepoints;
    s.quote = row->text;
    if (row->kind == RECALL_CHAT_EXCERPT) {
        s.kind = SRC_CHAT;
        snprintf(s.chat, sizeof(s.chat), "%s", row->conversation_id);
        s.message = (long)row->sequence;
    } else {
        s.kind = SRC_DOC;
        snprintf(s.doc, sizeof(s.doc), "%s", row->document_name);
        snprintf(s.revision, sizeof(s.revision), "%s", row->revision_id);
    }
    open_source(app, &s);
}

/* Each frame: drop results for a project that is no longer open, start a
 * submitted query off the frame thread, and publish a finished result. */
static void recall_frame(App *app)
{
    if (strcmp(app->recall_project, app->project_dir) != 0) {
        snprintf(app->recall_project, sizeof(app->recall_project), "%s", app->project_dir);
        recall_view_clear(app->recall);
        app->recall_stale = recall_job_busy(&app->recall_job);
    }
    char *query = recall_view_take_query(app->recall);
    if (query) {
        if (!app->project_dir[0]) set_status(app, "Open a project to search its record");
        else if (recall_job_busy(&app->recall_job)) set_status(app, "Recall is still searching; try again in a moment");
        else if (recall_job_start(&app->recall_job, app->project_dir, query)) {
            recall_view_set_busy(app->recall, true);
            app->recall_stale = false;
        } else set_status(app, "Recall could not start");
        free(query);
    }
    RecallResult result;
    bool ok = false;
    if (recall_job_poll(&app->recall_job, &result, &ok)) {
        if (app->recall_stale) {
            recall_result_free(&result);
            app->recall_stale = false;
            recall_view_set_busy(app->recall, false);
        } else {
            recall_view_set_result(app->recall, &result, ok);
            if (!ok) set_status(app, "Recall failed; the panel says why");
        }
    }
}

/* ---- main(): startup phases, the frame loop and shutdown (AUDIT-004 F3) ------------------------------------- */

/* Asked to quit by a signal (kill, logout, a terminal's ^C): leave the
 * loop so the model servers are stopped below, not left running. */
static void install_quit_signals(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_quit_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

/* The frame's worker polls and timers, after the service supervisor's. */
static void frame_poll_workers(App *app)
{
    backup_poll(app);
    draft_tick(app);
    names_tick(app);
    names_poll(app);
    import_poll(app);
    /* A context size chosen while a reply was being written: now. */
    if (app->llm && app->ctx_pending && !chat_busy(app->chat) && !name_detection_job_busy(&app->names_job)) {
        app->ctx_pending = false;
        fit_context(app, false);
    }
    bool graph_succeeded = false;
    if (entity_backfill_job_poll(&app->graph_job, &graph_succeeded)) { /* the graph is built: show it */
        app->lists_dirty = true;
    }
    chat_poll(app->chat);
    if (chat_trim_saved(app->chat)) chat_view_history_changed(app->chat_view);
    /* The observer shares the model: it yields whenever chat is active. */
    observer_set_chat_active(app->observer, chat_busy(app->chat));
    if (!history_read_busy(app) && observer_version(app->observer) != app->findings_version) {
        app->findings_version = observer_version(app->observer);
        refresh_findings(app);
    }
    {
        char st[200];
        observer_status(app->observer, st, sizeof(st));
        snprintf(app->observer_label, sizeof(app->observer_label), "%s", st);
    }
    const char *cn = chat_take_notice(app->chat);
    if (cn) set_status(app, cn);
    if (app->clear_chat_when_idle && !chat_busy(app->chat)) {
        app->clear_chat_when_idle = false;
        chat_lock(app->chat);
        size_t nmsg = chat_count(app->chat);
        chat_unlock(app->chat);
        if (!app->chat_project[0] && nmsg > 0 && app->project_dir[0]) {
            adopt_chat(app);
        } else if (strcmp(app->chat_project, app->project_dir) != 0) {
            if (unsaved_chat(app)) {
                set_status(app, "Unsaved chat kept with the previous project; copy it or explicitly start a new chat");
            } else {
                chat_clear(app->chat);
                chat_view_history_changed(app->chat_view);
                project_session_clear_chat(&app->session);
                attach_reset(app); /* the old project's documents aren't this one's */
            }
        }
    }
    history_poll(app);
    memory_tick(app);
    /* Changes made here mark the lists at once; this catches the rest
     * (files added in Finder, other windows). Store queries cost more
     * than a directory read, so they're polled less often. */
    double every = (app->store_inspection.capabilities & PROJECT_STORE_CAP_SLOW_REFRESH) ? 10.0 : 3.0;
    if (GetTime() - app->lists_refreshed > every) app->lists_dirty = true;
}

/* True when the window may close now. */
static bool frame_close_requested(App *app)
{
    /* Close requests: never lose unsaved prose silently. */
    if (WindowShouldClose()) {
        /* Notes save themselves; uncommitted canon text counts as unsaved. */
        bool note_lost = record_view_active(app->record) && record_view_kind(app->record) == REC_NOTE &&
                         record_view_dirty(app->record) && !save_note(app);
        bool unsaved = editor_is_dirty(app->editor) || record_unsaved_canon(app) || note_lost || unsaved_chat(app) || memory_controls_pending(app);
        if (!unsaved || (app->close_armed && GetTime() - app->close_armed_at < 5.0)) {
            return true;
        }
        glfwSetWindowShouldClose(GetWindowHandle(), 0);
        app->close_armed = true;
        app->close_armed_at = GetTime();
        set_status(app, memory_controls_pending(app)
            ? "Memory settings are not committed yet; wait, or close again to discard pending settings and other unsaved changes"
            : "Unsaved changes — save or copy what you need, or close again to discard");
    }
    return false;
}

static void frame_take_dropped(App *app)
{
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        if (dropped.count > 0 && !prompt_active(app->prompt) && !app->menu.active) drop_path(app, dropped.paths[0]);
        UnloadDroppedFiles(dropped);
    }
}

/* The toolbar where nothing answers a click: with the title bar gone, these parts move the window. */
static bool toolbar_empty(void)
{
    static const char *const controls[] = { "AppMenu", "OpenButton", "SaveButton", "ModelPicker", "RecallToggle", "ObserverToggle" };
    if (!Clay_PointerOver(CLAY_ID("Toolbar"))) return false;
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
        if (Clay_PointerOver(Clay_GetElementId((Clay_String){ .length = (int32_t)strlen(controls[i]), .chars = controls[i] }))) return false;
    return true;
}

static void frame_left_click(App *app, Vector2 mouse, bool modal, bool trace)
{
    /* An empty part of the toolbar: the window's title bar (ui/mac_window.h). A double-click does what the author's
     * system setting says; a press starts the system's own window drag. */
    if (!modal && app->unified_titlebar && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && toolbar_empty()) {
        double now = GetTime();
        if (now - app->toolbar_click_t < 0.4) {
            app->toolbar_click_t = 0;
            mac_window_title_double_click(GetWindowHandle());
        } else {
            app->toolbar_click_t = now;
            mac_window_begin_drag(GetWindowHandle());
        }
        return;
    }
    /* Clicks (pointer state reflects last frame's layout). */
    if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (trace) fprintf(stderr, "trace: click (%.0f, %.0f) focus=%d\n", mouse.x, mouse.y, app->focus);
        if (Clay_PointerOver(CLAY_ID("SplitLeft"))) app->dragging = DRAG_EXPLORER;
        else if (Clay_PointerOver(CLAY_ID("SplitChat"))) app->dragging = DRAG_CHAT;
        else if ((app->observer_open || app->recall_open) && Clay_PointerOver(CLAY_ID("SplitRight"))) app->dragging = DRAG_OBSERVER;
        else if (app->recall_open && CheckCollisionPointRec(mouse, recall_view_input_rect(app->recall))) app->focus = FOCUS_RECALL;
        else if (record_view_active(app->record) && CheckCollisionPointRec(mouse, record_view_editor_rect(app->record)))
            app->focus = FOCUS_RECORD;
        else if (record_view_active(app->record) && CheckCollisionPointRec(mouse, app->record_rect)) {
            /* panel buttons and links: handled when it draws */
        } else if (Clay_PointerOver(CLAY_ID("Editor"))) app->focus = FOCUS_DOC;
        else if (Clay_PointerOver(CLAY_ID("ChatInput"))) app->focus = FOCUS_CHAT;
        else if (Clay_PointerOver(CLAY_ID("NewChatItem"))) new_chat(app);
        else if (app->observer_open && Clay_PointerOver(CLAY_ID("RebuildIndex"))) {
            observer_request_rebuild(app->observer);
            set_status(app, "Rebuilding the semantic index from the project store\xE2\x80\xA6");
        }
        else if (app->store_problem && Clay_PointerOver(CLAY_ID("StoreFixItem"))) store_resolve_problem(app);
        else if (Clay_PointerOver(CLAY_ID("NewDocItem"))) begin_new_document(app);
        else if (Clay_PointerOver(CLAY_ID("ImportItem"))) import_documents(app);
        else if (Clay_PointerOver(CLAY_ID("NewNoteItem"))) {
            if (close_record(app, false) && record_view_open(app->record, REC_NOTE, NULL, "", NULL, 0))
                app->focus = FOCUS_RECORD;
        }
        else if (Clay_PointerOver(CLAY_ID("Explorer"))) {
            app->focus = FOCUS_NONE;
            char full[PATH_MAX + 256];
            for (size_t i = 0; i < app->ndocs; i++)
                if (Clay_PointerOver(CLAY_IDI("DocItem", (uint32_t)i))) {
                    const char *doc = explorer_snapshot_doc(&app->explorer, app->drawn_explorer_generation,
                                                            app->session.generation, i);
                    if (doc) {
                        snprintf(full, sizeof(full), "%s/%s", app->project_dir, doc);
                        drop_path(app, full);
                        app->focus = FOCUS_DOC;
                    }
                    break;
                }
            for (size_t i = 0; i < app->nchats; i++)
                if (Clay_PointerOver(CLAY_IDI("ChatItem", (uint32_t)i))) {
                    const ChatSummary *chat = explorer_snapshot_chat(&app->explorer, app->drawn_explorer_generation,
                                                                     app->session.generation, i);
                    if (chat) {
                        open_chat(app, chat);
                        app->focus = FOCUS_CHAT;
                    }
                    break;
                }
            for (size_t i = 0; i < app->nrevs && i < REV_SHOW; i++)
                if (Clay_PointerOver(CLAY_IDI("RevItem", (uint32_t)i))) {
                    Revision r = app->revs[i];
                    restore_revision(app, &r);
                    app->focus = FOCUS_DOC;
                    break;
                }
            /* (Lists are only refreshed after click handling, so these
             * records stay valid while the panel copies them.) */
            for (size_t i = 0; i < app->nnotes && i < REC_SHOW; i++)
                if (app->note_labels[i][0] && Clay_PointerOver(CLAY_IDI("NoteItem", (uint32_t)i))) {
                    show_record(app, REC_NOTE, &app->notes[i], NULL, NULL, 0);
                    break;
                }
            for (size_t i = 0; i < app->ncanon && i < REC_SHOW; i++)
                if (Clay_PointerOver(CLAY_IDI("CanonItem", (uint32_t)i))) {
                    show_record(app, REC_CANON, &app->canon[i], NULL, NULL, 0);
                    break;
                }
            /* Entities: copy what's needed first (opening refreshes the lists). */
            for (size_t i = 0; i < ENT_SHOW; i++)
                if (app->drawn_ent[i][0] && Clay_PointerOver(CLAY_IDI("EntityItem", (uint32_t)i))) {
                    char id[40];
                    snprintf(id, sizeof(id), "%s", app->drawn_ent[i]);
                    toggle_entity(app, id);
                    break;
                }
            for (size_t k = 0; app->ent_open[0] && k < ENT_ROWS; k++) {
                char id[40];
                if (app->drawn_row_other[k][0] && Clay_PointerOver(CLAY_IDI("EntGo", (uint32_t)k))) {
                    snprintf(id, sizeof(id), "%s", app->drawn_row_other[k]);
                    toggle_entity(app, id);
                    break;
                }
                if (app->drawn_row_finding[k][0] && Clay_PointerOver(CLAY_IDI("EntRow", (uint32_t)k))) {
                    snprintf(id, sizeof(id), "%s", app->drawn_row_finding[k]);
                    open_finding_source(app, id);
                    break;
                }
            }
        }
        /* Toolbar, chat header/footer and message list keep the focus. */
    }
}

/* Scribodo in the toolbar: the app's own settings. */
static void frame_app_menu(App *app, bool modal)
{
    if (modal || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || !Clay_PointerOver(CLAY_ID("AppMenu"))) return;
    /* The app's settings, then the open project's own: back up, restore, names (once in the Explorer). */
    const char *items[] = { "Appearance\xE2\x80\xA6     \xE2\x8C\x98,", app->focus_mode ? "Leave focus mode  \xE2\x8C\x98\\" : "Focus mode        \xE2\x8C\x98\\",
                            "Model settings\xE2\x80\xA6",
                            backup_job_busy(&app->backup) ? "Backing up\xE2\x80\xA6" : "Back up now",
                            "Restore from a backup\xE2\x80\xA6", "Names\xE2\x80\xA6" };
    Rectangle r = ELEMENT_RECT("AppMenu");
    menu_open(&app->menu, MT_APP, 0, (Vector2){ r.x, r.y + r.height + 4 }, items, app->project_dir[0] ? 6 : 3);
}

static void frame_model_picker(App *app, bool modal)
{
    if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && Clay_PointerOver(CLAY_ID("ModelPicker"))) {
        static char items[3][240];
        const char *list[3];
        int n = 0;
        if (app->remote_ok) {
            snprintf(items[0], sizeof(items[0]), "%s Local \xC2\xB7 LFM3-1.7B BF16 (nothing leaves this Mac)",
                     chat_remote_selected(app->chat) ? "  " : "\xE2\x9C\x93");
            snprintf(items[1], sizeof(items[1]), "%s Remote \xC2\xB7 %s (sends chat and attached documents to %s)",
                     chat_remote_selected(app->chat) ? "\xE2\x9C\x93" : "  ", app->remote.name, app->remote.host);
            list[0] = items[0];
            list[1] = items[1];
            n = 2;
        } else if (app->remote_why[0]) {
            set_status(app, app->remote_why); /* a remote is configured but unusable: say why */
        }
        snprintf(items[2], sizeof(items[2]), "   Model settings\xE2\x80\xA6");
        app->model_menu_settings = n;
        list[n++] = items[2];
        Rectangle pr = ELEMENT_RECT("ModelPicker");
        menu_open(&app->menu, MT_MODEL, 0, (Vector2){ pr.x, pr.y + pr.height + 4 }, list, n);
    }
}

static void frame_explorer_menu(App *app, Vector2 mouse, bool modal)
{
    if (!modal && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && Clay_PointerOver(CLAY_ID("Explorer"))) {
        for (size_t i = 0; i < app->nchats; i++)
            if (Clay_PointerOver(CLAY_IDI("ChatItem", (uint32_t)i))) {
                const ChatSummary *chat = explorer_snapshot_chat(&app->explorer, app->drawn_explorer_generation,
                                                                 app->session.generation, i);
                if (!chat) break;
                static char reads_item[64];
                snprintf(reads_item, sizeof(reads_item), "%s Observer reads this chat",
                         chat->observer_ignore ? "  " : "\xE2\x9C\x93");
                const char *items[] = { "Rename\xE2\x80\xA6", reads_item, "Enable local memory (experimental)",
                                        "Disable local memory", "Rebuild local memory" };
                snprintf(app->menu_chat, sizeof(app->menu_chat), "%s", chat->id);
                menu_open(&app->menu, MT_CHAT, (int)i, mouse, items, app->memory_experiment ? 5 : 2);
                break;
            }
        for (size_t i = 0; i < app->ndocs; i++)
            if (Clay_PointerOver(CLAY_IDI("DocItem", (uint32_t)i))) {
                const char *doc = explorer_snapshot_doc(&app->explorer, app->drawn_explorer_generation,
                                                        app->session.generation, i);
                if (!doc) break;
                static const char *const items[] = { "Rename\xE2\x80\xA6", "Reveal in Finder" };
                snprintf(app->menu_doc, sizeof(app->menu_doc), "%s", doc); /* copied: later snapshots may re-sort */
                menu_open(&app->menu, MT_DOC, (int)i, mouse, items, 2);
                break;
            }
    }
}

static void frame_pane_widths(App *app, Vector2 mouse, bool mouse_down)
{
    if (!mouse_down) app->dragging = DRAG_NONE;
    float sw = (float)GetScreenWidth();
    float right = sw - (app->observer_open || app->recall_open ? app->observer_w + SPLITTER_W : 0);
    if (app->dragging == DRAG_EXPLORER) app->explorer_w = mouse.x - SPLITTER_W / 2.0f;
    if (app->dragging == DRAG_OBSERVER) app->observer_w = sw - mouse.x - SPLITTER_W / 2.0f;
    if (app->dragging == DRAG_CHAT) app->chat_w = right - mouse.x - SPLITTER_W / 2.0f;
    /* Re-clamp every frame so shrinking the window never squeezes the editor. */
    app->explorer_w = fmaxf(140.0f, fminf(app->explorer_w, sw * 0.25f));
    app->observer_w = fmaxf(180.0f, fminf(app->observer_w, sw * 0.25f));
    app->chat_w = fmaxf(280.0f, fminf(app->chat_w, sw * 0.45f));
    /* Keep the document at least 320 px wide: shrink chat, then the
     * observer, then the explorer, and finally close the observer. */
    for (int pass = 0; pass < 4; pass++) {
        float side = app->explorer_w + app->chat_w + (app->observer_open || app->recall_open ? app->observer_w + SPLITTER_W : 0) + 2 * SPLITTER_W;
        float deficit = 320.0f - (sw - side);
        if (deficit <= 0) break;
        if (pass == 0) app->chat_w = fmaxf(280.0f, app->chat_w - deficit);
        else if (pass == 1 && (app->observer_open || app->recall_open)) app->observer_w = fmaxf(180.0f, app->observer_w - deficit);
        else if (pass == 2) app->explorer_w = fmaxf(140.0f, app->explorer_w - deficit);
        else if (pass == 3) app->observer_open = app->recall_open = false;
    }
    /* The right pane glides when it opens or shuts (the target crosses zero); otherwise, a splitter drag or a window
     * resize, it is placed at once. The glide runs until the pane reaches its target (red team: a check of "already
     * open" ended it after one frame). */
    float target = app->observer_open || app->recall_open ? app->observer_w : 0.0f;
    if ((target > 0.0f) != (app->side_target > 0.0f)) app->side_gliding = true;
    app->side_target = target;
    if (app->dragging == DRAG_OBSERVER) app->side_gliding = false;
    app->side_shown = app->side_gliding ? motion_approach(app->side_shown, target, GetFrameTime(), MOTION_PANE_MS) : target;
    if (app->side_shown == target) app->side_gliding = false;
}

static void frame_shortcuts(App *app, bool modal)
{
    bool primary = !modal && (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER));
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (primary && shortcut_pressed('s')) {
        if (app->focus == FOCUS_RECORD && record_view_kind(app->record) == REC_NOTE) save_note(app);
        else if (app->focus == FOCUS_RECORD)
            set_status(app, "Canon is kept with Commit or Revise \xC2\xB7 \xE2\x8C\x98S saves notes and documents");
        else save(app);
    }
    /* From the document only: the chat box has no selection to quote. */
    if (primary && shift && shortcut_pressed('n') && app->focus == FOCUS_DOC) note_from_selection(app);
    /* In the composer the same keys toggle the Note mark (red-team 2026-11-14: it was mouse-only). */
    if (primary && shift && shortcut_pressed('n') && app->focus == FOCUS_CHAT && !app->edit_mode) chat_view_toggle_note(app->chat_view);
    if (primary && shift && shortcut_pressed('c') && app->focus == FOCUS_DOC) canon_from_selection(app);
    if (!modal && app->focus == FOCUS_RECORD && IsKeyPressed(KEY_ESCAPE)) app->focus = FOCUS_DOC;
    if (!modal && app->focus == FOCUS_RECALL && IsKeyPressed(KEY_ESCAPE)) app->focus = FOCUS_DOC;
    /* The chat is not on screen in focus mode: reaching for it leaves focus mode first. */
    if (primary && (shortcut_pressed('l') || shortcut_pressed('k')) && app->focus_mode) app_set_focus_mode(app, false);
    if (primary && shortcut_pressed('l')) app->focus = FOCUS_CHAT;
    if (primary && shortcut_pressed('k')) begin_edit(app);
    /* Cmd+Enter accepts only outside the chat box, where it also sends the
     * message being typed (red-team 004: one key did both). */
    if (primary && review_active(app->review) && app->focus != FOCUS_CHAT &&
        (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)))
        accept_proposal(app);
    if (primary && shortcut_pressed('o')) choose_project(app, false);
    if (primary && IsKeyPressed(KEY_COMMA)) fonts_panel_open(app);
    if (primary && IsKeyPressed(KEY_BACKSLASH)) app_set_focus_mode(app, !app->focus_mode);
    if (primary && !shift && shortcut_pressed('n')) begin_new_document(app);
    if (!modal && app->focus == FOCUS_CHAT && IsKeyPressed(KEY_ESCAPE)) {
        if (app->history_pending) {
            cancel_history(app);
            set_status(app, "History loading cancelled; the current window was kept");
        }
        if (app->edit_mode) {
            leave_edit_mode(app);
            if (!app->pending.active) editor_anchor_clear(app->editor);
        }
        app->focus = FOCUS_DOC;
    }
}

/* The layout for this frame, and the rects the views and controls take from it. */
static Clay_RenderCommandArray frame_layout(App *app, FrameRects *rects)
{
    const char *notice = editor_take_notice(app->editor);
    if (notice) set_status(app, notice);

    /* Refresh lists only now: clicks above were hit-tested against the
     * lists as last drawn, so a re-sort can never redirect a click. */
    if (app->lists_dirty && !history_read_busy(app)) {
        if (refresh_lists(app)) app->lists_dirty = false;
    }
    Clay_RenderCommandArray cmds = build_layout(app);
    app->editor_rect = ELEMENT_RECT("Editor");
    if (record_view_active(app->record)) {
        /* Dock the note/canon panel at the bottom of the document pane. */
        Rectangle er = app->editor_rect;
        float h = record_view_height(app->record, er.width - 24, er.height * 0.45f);
        app->record_rect = (Rectangle){ er.x + 12, er.y + er.height - h - 12, er.width - 24, h };
        app->editor_rect.height = app->record_rect.y - er.y - 8 + 12;
    }
    if (review_active(app->review)) {
        /* Dock the review panel at the bottom of the document pane (above
         * the note/canon panel, if open). */
        Rectangle er = app->editor_rect;
        float h = review_height(app->review, er.width - 24, er.height * 0.5f);
        app->review_rect = (Rectangle){ er.x + 12, er.y + er.height - h - 12, er.width - 24, h };
        app->editor_rect.height = app->review_rect.y - er.y - 8;
    }
    app->chat_list_rect = ELEMENT_RECT("ChatMessages");
    app->findings_rect = app->observer_open ? ELEMENT_RECT("Findings") : (Rectangle){ 0 };
    app->recall_rect = app->recall_open ? ELEMENT_RECT("RecallBody") : (Rectangle){ 0 };
    app->chat_input_rect = ELEMENT_RECT("ChatInput");
    rects->save_rect = ELEMENT_RECT("SaveButton");
    rects->open_rect = ELEMENT_RECT("OpenButton");
    rects->obs_rect = ELEMENT_RECT("ObserverToggle");
    rects->new_rect = ELEMENT_RECT("NewChatButton");
    rects->send_rect = ELEMENT_RECT("SendButton");
    rects->attach_rect = ELEMENT_RECT("AttachRow");
    return cmds;
}

static void frame_update_views(App *app, bool modal)
{
    /* Always update both so queued keystrokes are consumed or discarded. */
    if (prompt_active(app->prompt)) {
        PromptResult pr = prompt_update(app->prompt, (Rectangle){ 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() });
        if (pr == PR_CANCEL) prompt_close(app->prompt);
        if (pr == PR_OK) {
            char *t = prompt_text(app->prompt);
            if (t && finish_prompt(app, t)) prompt_close(app->prompt);
            free(t);
        }
    }
    bool idle = app->dragging == DRAG_NONE && !modal;
    /* In focus mode only the document (and a note or canon panel under it) take the keyboard. */
    if (app->focus_mode && app->focus != FOCUS_DOC && app->focus != FOCUS_RECORD) app->focus = FOCUS_DOC;
    editor_update(app->editor, app->editor_rect, idle && app->focus == FOCUS_DOC);
    memory_document_names(app); /* dirty-buffer transitions fence preparation immediately */
    chat_view_set_can_apply(app->chat_view, editor_path(app->editor) != NULL && !editor_is_read_only(app->editor));
    chat_view_set_note_disabled(app->chat_view, app->edit_mode); /* before the frame's hit test and draw */
    chat_view_update(app->chat_view, app->chat_list_rect, app->chat_input_rect, idle && app->focus == FOCUS_CHAT);
    ChatPageAction page_action = chat_view_take_page(app->chat_view);
    if (idle && page_action.kind != CHAT_PAGE_NONE) page_chat(app, page_action);
    if (review_active(app->review) && !modal) review_update(app->review, app->review_rect);
    if (record_view_active(app->record)) record_view_update(app->record, app->record_rect, idle && app->focus == FOCUS_RECORD);
    if (app->observer_open && !modal) findings_view_update(app->findings, app->findings_rect);
    if (app->recall_open && !modal) recall_view_update(app->recall, app->recall_rect, idle && app->focus == FOCUS_RECALL);
    fonts_panel_update(app);
    input_discard_chars(); /* after both views: nothing typed now may appear later */
    recall_frame(app);
}

static void frame_apply_reply(App *app)
{
    long apply = chat_view_take_apply(app->chat_view);
    if (apply >= 0) {
        /* "Apply to document": propose a reply as a replacement for the
         * selection, or as an insertion at the cursor. */
        chat_lock(app->chat);
        const ChatMessage *m = chat_message(app->chat, (size_t)apply);
        char *reply = m && !m->excerpt ? strdup(m->content) : NULL;
        bool reply_cut = m && (m->truncated || m->stopped);
        bool reply_edit = m && m->is_edit;
        chat_unlock(app->chat);
        size_t a, c;
        editor_get_selection(app->editor, &a, &c);
        size_t lo = a < c ? a : c, hi = a < c ? c : a;
        if (!reply) {
        } else if (!editor_path(app->editor) || editor_is_read_only(app->editor)) {
            set_status(app, "Open an editable document to apply a reply");
        } else if (app->pending.active) {
            set_status(app, "Wait for the current edit to finish (or stop it) before applying a reply");
        } else if (reply_edit && hi == lo) {
            /* An edit reply rewrites a passage; inserting it would duplicate the text. */
            set_status(app, "Select the passage this rewrite replaces, then Apply");
        } else {
            leave_edit_mode(app);
            char *orig = editor_range_utf8(app->editor, lo, hi - lo);
            char *clean = llm_clean_edit(reply, orig ? orig : "");
            char where[64];
            if (hi > lo) snprintf(where, sizeof(where), "replace selection");
            else snprintf(where, sizeof(where), "insert at line %d", editor_stats(app->editor).line);
            if (orig && clean) propose(app, lo, hi - lo, orig, clean, where, reply_cut);
            free(orig);
            free(clean);
        }
        free(reply);
    }
}

static void frame_chat_requests(App *app)
{
    bool rec_canon = false;
    long rec_index = chat_view_take_record(app->chat_view, &rec_canon);
    if (rec_index >= 0) record_from_reply(app, rec_index, rec_canon);
    long sources_index = chat_view_take_sources(app->chat_view);
    if (sources_index >= 0) show_memory_sources(app, sources_index);
    char *submitted = chat_view_take_submit(app->chat_view);
    if (submitted) send_chat(app, submitted);
    if (chat_view_take_copied(app->chat_view)) set_status(app, "Reply copied to the clipboard");
}

static void frame_cursor(App *app, MainLoop *loop, Vector2 mouse)
{
    int want = MOUSE_CURSOR_DEFAULT;
    if (app->dragging || Clay_PointerOver(CLAY_ID("SplitLeft")) || Clay_PointerOver(CLAY_ID("SplitChat")) ||
        (app->observer_open && Clay_PointerOver(CLAY_ID("SplitRight"))))
        want = MOUSE_CURSOR_RESIZE_EW;
    else if (CheckCollisionPointRec(mouse, app->editor_rect) || CheckCollisionPointRec(mouse, app->chat_input_rect))
        want = MOUSE_CURSOR_IBEAM;
    else if (Clay_PointerOver(CLAY_ID("Explorer")))
        want = MOUSE_CURSOR_POINTING_HAND;
    if (want != loop->cursor_shape) {
        SetMouseCursor(want);
        loop->cursor_shape = want;
    }
}

/* Begins the frame's drawing; returns the welcome screen's action, if any. */
static int frame_draw_panes(App *app, Clay_RenderCommandArray cmds, const FrameRects *rects, bool modal)
{
    BeginDrawing();
    ClearBackground(THEME_BG);
    clay_render(cmds, app->font_ptrs);
    widget_frame_begin();
    widget_set_locked(modal); /* nothing behind a prompt or menu reacts */
    if (widget_icon_button(rects->open_rect, &(WidgetIcon){ icon_texture(ICON_OPEN), icon_size(ICON_OPEN) }, "Open\xE2\x80\xA6", NULL,
                           WIDGET_QUIET, true, false))
        choose_project(app, false);
    if (editor_path(app->editor)) editor_draw(app->editor, app->editor_rect, app->focus == FOCUS_DOC);
    int empty_act = editor_path(app->editor) ? 0 : draw_empty(app, app->editor_rect);
    if (review_active(app->review)) {
        review_draw(app->review, app->review_rect);
        if (widget_button(review_accept_rect(app->review_rect), "Accept", NULL, WIDGET_PRIMARY, true)) accept_proposal(app);
        if (widget_button(review_reject_rect(app->review_rect), "Reject", NULL, WIDGET_OUTLINE, true)) reject_proposal(app);
    }
    if (record_view_active(app->record))
        record_action(app, record_view_draw(app->record, app->record_rect, app->focus == FOCUS_RECORD));
    chat_view_draw(app->chat_view, app->chat_list_rect, app->chat_input_rect, app->focus == FOCUS_CHAT);
    if (app->observer_open && app->findings_rect.width > 0) {
        char fid[40] = "";
        FindingAction fa = findings_view_draw(app->findings, app->findings_rect, fid, sizeof(fid));
        if (fa != FA_NONE) finding_action(app, fa, fid);
    }
    if (app->recall_open && app->recall_rect.width > 0) {
        RecallAction ra = recall_view_draw(app->recall, app->recall_rect, app->focus == FOCUS_RECALL);
        if (ra.kind == RECALL_ACTION_OPEN) open_recall_row(app, ra.row);
    }
    return empty_act;
}

static void frame_draw_toolbar(App *app, const FrameRects *rects)
{
    bool can_save = (editor_is_dirty(app->editor) || editor_overwrite_armed(app->editor)) &&
                    !editor_is_read_only(app->editor);
    if (widget_icon_button(rects->save_rect, &(WidgetIcon){ icon_texture(ICON_SAVE), icon_size(ICON_SAVE) }, "Save", NULL, WIDGET_QUIET,
                           can_save, false))
        save(app);
    if (widget_icon_button(rects->obs_rect, &(WidgetIcon){ icon_texture(ICON_OBSERVER), icon_size(ICON_OBSERVER) },
                           app->observer_button[0] ? app->observer_button : "Observer", NULL, WIDGET_QUIET, true, app->observer_open)) {
        if (app->focus_mode) app_set_focus_mode(app, false); /* a pane is not shown in focus mode */
        app->observer_open = !app->observer_open;
        if (app->observer_open && app->recall_open) toggle_recall(app);
    }
    if (widget_icon_button(ELEMENT_RECT("RecallToggle"), &(WidgetIcon){ icon_texture(ICON_RECALL), icon_size(ICON_RECALL) }, "Recall",
                           NULL, WIDGET_QUIET, true, app->recall_open))
        toggle_recall(app);
    /* Whether the observer reads the open chat (the author's choice). */
    if (open_chat_stored(app)) {
        Rectangle orr = ELEMENT_RECT("ObserverReads");
        const ChatSummary *cs = find_chat(app, app->chat_id);
        bool reads = cs && !cs->observer_ignore, was = reads;
        widget_checkbox(orr, "Observer reads this chat", NULL, &reads);
        if (reads != was) set_chat_observer(app, app->chat_id, reads);
    }
}

static void frame_draw_chat_controls(App *app, const FrameRects *rects)
{
    if (app->suggest_text[0]) {
        if (widget_button(ELEMENT_RECT("NameSave"), "Save", NULL, WIDGET_PRIMARY, true)) names_answer(app, true);
        else if (widget_button(ELEMENT_RECT("NameSkip"), "Not now", NULL, WIDGET_QUIET, true)) names_answer(app, false);
    }
    if (widget_icon_button(rects->new_rect, &(WidgetIcon){ icon_texture(ICON_NEW_CHAT), icon_size(ICON_NEW_CHAT) }, "New chat", NULL,
                           WIDGET_QUIET, !chat_busy(app->chat), false))
        new_chat(app);
    if (app->edit_mode) {
        if (widget_button(ELEMENT_RECT("CancelEdit"), "Cancel", NULL, WIDGET_OUTLINE, true)) {
            leave_edit_mode(app);
            editor_anchor_clear(app->editor);
        }
    } else if (rects->attach_rect.width > 0) {
        int aa = draw_attach_row(app, rects->attach_rect);
        if (aa >= 0) {
            detach(app, (size_t)aa);
            chat_controller_mark_attachments_changed(&app->chat_controller);
        } else if (aa == -2) {
            if (!app->project_dir[0] || app->ndocs == 0) {
                set_status(app, "Open a project with documents to attach them");
            } else {
                /* The project's documents; a check mark shows what's attached. */
                const char *items[MENU_MAX_ITEMS];
                char labels[MENU_MAX_ITEMS][48];
                int n = 0;
                /* With more documents than fit, the last item asks for a name. */
                size_t room = app->ndocs > MENU_MAX_ITEMS ? MENU_MAX_ITEMS - 1 : app->ndocs;
                for (size_t i = 0; i < room; i++, n++) {
                    char nm[44];
                    fit_label(&app->fonts[FONT_UI], app->docs[i], 300, nm, sizeof(nm));
                    snprintf(app->menu_names[n], sizeof(app->menu_names[n]), "%s", app->docs[i]);
                    snprintf(labels[n], sizeof(labels[n]), "%s %s", is_attached(app, app->docs[i]) ? "\xE2\x9C\x93" : "  ", nm);
                    items[n] = labels[n];
                }
                if (room < app->ndocs) {
                    app->menu_names[n][0] = '\0'; /* "other": a prompt */
                    snprintf(labels[n], sizeof(labels[n]), "   Other document\xE2\x80\xA6");
                    items[n] = labels[n];
                    n++;
                }
                menu_open(&app->menu, MT_ATTACH, 0, (Vector2){ app->attach_add.x, app->attach_add.y - 8 - (float)n * 30 }, items, n);
            }
        }
    }
    if (chat_busy(app->chat)) {
        if (widget_button(rects->send_rect, "Stop", NULL, WIDGET_OUTLINE, true)) chat_cancel(app->chat);
    } else if (app->history_pending) {
        if (widget_button(rects->send_rect, "Cancel", NULL, WIDGET_OUTLINE, true)) {
            cancel_history(app);
            set_status(app, "History loading cancelled; the current window was kept");
        }
    } else {
        bool ready = !app->llm || chat_remote_selected(app->chat) || llm_server_state(app->llm, NULL, 0) == LLM_READY;
        if (widget_button(rects->send_rect, "Send", NULL, WIDGET_PRIMARY, ready)) {
            chat_view_submit_now(app->chat_view);
            char *t = chat_view_take_submit(app->chat_view);
            if (t) send_chat(app, t);
            app->focus = FOCUS_CHAT;
        }
    }
}

static void frame_draw_overlays(App *app, MainLoop *loop, int empty_act, bool modal)
{
    widget_set_locked(false);
    if (!modal && empty_act) empty_action(app, empty_act, loop->sample);
    {
        Rectangle screen = { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() };
        int pick = menu_draw(&app->menu, &app->fonts[FONT_UI], screen);
        if (pick >= 0 && app->menu.tag == MT_ATTACH && pick < MENU_MAX_ITEMS && !app->menu_names[pick][0]) {
            snprintf(app->prompt_project, sizeof(app->prompt_project), "%s", app->project_dir);
            prompt_open(app->prompt, PT_ATTACH_NAME, "Attach a document", "", "Attach");
        } else if (pick >= 0 && app->menu.tag == MT_ATTACH && pick < MENU_MAX_ITEMS) {
            const char *nm = app->menu_names[pick];
            bool found = false;
            for (size_t i = 0; i < app->nattached; i++)
                if (!strcmp(app->attached[i], nm)) {
                    detach(app, i);
                    found = true;
                    break;
                }
            if (!found) attach(app, nm);
            chat_controller_mark_attachments_changed(&app->chat_controller);
        }
        if (pick == 0 && app->menu.tag == MT_APP) fonts_panel_open(app);
        if (pick == 1 && app->menu.tag == MT_APP) app_set_focus_mode(app, !app->focus_mode);
        if (pick == 2 && app->menu.tag == MT_APP) open_model_settings(app);
        if (pick == 3 && app->menu.tag == MT_APP) begin_backup(app);
        if (pick == 4 && app->menu.tag == MT_APP) begin_restore(app);
        if (pick == 5 && app->menu.tag == MT_APP) begin_names(app);
        if (pick >= 0 && app->menu.tag == MT_MODEL && pick == app->model_menu_settings) {
            open_model_settings(app);
        } else if (pick >= 0 && app->menu.tag == MT_MODEL) {
            bool remote = pick == 1;
            if (remote == chat_remote_selected(app->chat)) {
            } else if (!chat_select_remote(app->chat, remote ? &app->remote : NULL)) {
                set_status(app, "Wait for the reply to finish before switching models");
            } else {
                char msg[400];
                if (remote)
                    snprintf(msg, sizeof(msg), "Using %s: new chat messages, their attached documents and edit passages go to %s. "
                                               "Earlier messages and the observer stay on this Mac.", app->remote.name, app->remote.host);
                else snprintf(msg, sizeof(msg), "Using the local model: nothing leaves this Mac");
                set_status(app, msg);
            }
        }
        if (pick == 1 && app->menu.tag == MT_CHAT) {
            const ChatSummary *cs = find_chat(app, app->menu_chat);
            if (cs) set_chat_observer(app, app->menu_chat, cs->observer_ignore);
        } else if (pick >= 2 && pick <= 4 && app->menu.tag == MT_CHAT && app->memory_experiment) {
            memory_control(app, pick == 2 ? MEMORY_JOB_ENABLE : pick == 3 ? MEMORY_JOB_DISABLE : MEMORY_JOB_REBUILD);
        } else if (pick == 0 && app->menu.tag == MT_CHAT) {
            const char *title = "";
            for (size_t i = 0; i < app->nchats; i++)
                if (!strcmp(app->chats[i].id, app->menu_chat)) title = app->chats[i].title;
            snprintf(app->prompt_project, sizeof(app->prompt_project), "%s", app->project_dir);
            prompt_open(app->prompt, PT_RENAME_CHAT, "Rename chat", title, "Rename");
        }
        if (pick >= 0 && app->menu.tag == MT_DOC) {
            if (!name_taken(app, app->menu_doc)) set_status(app, "That document is no longer in this project");
            else if (pick == 0) begin_rename(app, app->menu_doc); /* a dirty open document is refused on OK */
            else reveal_document(app, app->menu_doc);
        }
        /* Frosted glass behind a dialog (ui/backdrop.h): its snapshot goes when the last dialog closes. */
        if (!app->settings_open && !app->fonts_panel.open && !prompt_active(app->prompt) && !app->memory_sources_open)
            backdrop_release();
        if (app->settings_open) draw_model_settings(app, screen);
        if (app->fonts_panel.open) fonts_panel_draw(app, screen);
        if (app->memory_sources_open) draw_memory_sources(app);
        if (prompt_active(app->prompt)) {
            PromptResult pr = prompt_draw(app->prompt, screen);
            if (pr == PR_CANCEL) prompt_close(app->prompt);
            if (pr == PR_OK) {
                char *t = prompt_text(app->prompt);
                if (t && finish_prompt(app, t)) prompt_close(app->prompt);
                free(t);
            }
        }
    }
}

/* Screenshots, then the end of the frame. False when the SCRIBODO_SCREENSHOT run is done. */
static bool frame_end(App *app, MainLoop *loop)
{
    const char *shot_path = app->script ? script_take_shot(app->script) : NULL;
    if (shot_path) {
        /* raylib batches draws until EndDrawing; flush so the capture
         * includes everything drawn this frame (e.g. the widgets' controls). */
        rlDrawRenderBatchActive();
        Image shot = LoadImageFromScreen();
        ExportImage(shot, shot_path);
        UnloadImage(shot);
    }
    EndDrawing();

    if (loop->screenshot_path && ++loop->frame == 10) {
        Image shot = LoadImageFromScreen();
        ExportImage(shot, loop->screenshot_path);
        UnloadImage(shot);
        return false;
    }
    return true;
}

/* One frame after the worker polls in main(). False to leave the loop. */
static bool frame_step(App *app, MainLoop *loop)
{
    if (app->unified_titlebar) mac_window_place_buttons(GetWindowHandle(), TOOLBAR_H); /* AppKit re-lays them out */
    static double refresh_checked;
    if (GetTime() - refresh_checked > 3.0) { /* the window may have moved to another display */
        refresh_checked = GetTime();
        app_match_refresh_rate(app);
    }
    if (app->font_notice[0]) { /* a font problem found at launch (app_lifecycle.c load_fonts) */
        set_status(app, app->font_notice);
        app->font_notice[0] = '\0';
    }
    frame_poll_workers(app);
    if (frame_close_requested(app)) {
        loop->closed_by_window = true;
        return false;
    }
    frame_take_dropped(app);

    Vector2 mouse = GetMousePosition();
    bool mouse_down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y }, mouse_down);
    /* The Explorer scrolls (long History / chat lists). */
    Vector2 wheel = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ wheel.x * 30.0f, wheel.y * 30.0f }, GetFrameTime());
    Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });

    /* A prompt or menu takes all input until it closes. */
    bool modal = prompt_active(app->prompt) || app->menu.active || app->settings_open || app->memory_sources_open ||
                 app->fonts_panel.open;
    input_block_mouse(modal);
    frame_left_click(app, mouse, modal, loop->trace);
    frame_app_menu(app, modal);
    frame_model_picker(app, modal);
    frame_explorer_menu(app, mouse, modal);
    frame_pane_widths(app, mouse, mouse_down);
    frame_shortcuts(app, modal);

    FrameRects rects;
    Clay_RenderCommandArray cmds = frame_layout(app, &rects);
    frame_update_views(app, modal);
    frame_apply_reply(app);
    frame_chat_requests(app);
    frame_cursor(app, loop, mouse);

    int empty_act = frame_draw_panes(app, cmds, &rects, modal);
    frame_draw_toolbar(app, &rects);
    frame_draw_chat_controls(app, &rects);
    frame_draw_overlays(app, loop, empty_act, modal);
    return frame_end(app, loop);
}

int main(int argc, char **argv)
{
    static App app;
    /* Before any thread: the environment is read and rewritten here only (app/startup_env.h). */
    static StartupEnv env;
    static MainLoop loop;
    (void)startup_env_prepare(&env);
    /* A font probe (ui/font.h): rasterise one face and exit, before anything else starts. */
    if (argc == 4 && !strcmp(argv[1], "--font-probe")) return sfont_probe_main(argv[2], atoi(argv[3]));
    atomic_init(&app.history_hold, false);
    app.store = store_manager_create();
    if (!app.store) return 1;
    const char *path = argc > 1 ? argv[1] : NULL;

    /* UI test mode: SCRIBODO_SCRIPT=file.scribo runs a scripted session in a
     * hidden window (no focus stealing, immune to the real mouse) and exits
     * non-zero if any expectation fails. See src/app/script.h. */
    const char *script_path = env.scripted ? env.script_path : NULL;
    int rc = app_init_jobs(&app, script_path);
    if (!rc) rc = app_open_window(&app, script_path);
    if (!rc) rc = app_start_models(&app, script_path);
    if (!rc) rc = app_create_views(&app, script_path);
    if (!rc) rc = app_load_recents(&app, &loop, &env, script_path);
    if (!rc) rc = script_host_setup(&app, &loop, &env, script_path);
    if (rc) {
        app_destroy(&app);
        return rc;
    }
    app_open_initial(&app, path, script_path, loop.sample);

    /* Debug hook: SCRIBODO_SCREENSHOT=out.png renders a few frames, saves the
     * framebuffer, and exits. */
    loop.screenshot_path = getenv("SCRIBODO_SCREENSHOT");
    loop.trace = getenv("SCRIBODO_INPUT_TRACE") != NULL;
    loop.cursor_shape = MOUSE_CURSOR_DEFAULT;
    install_quit_signals();
    while (true) {
        if (g_quit_signal) {
            fprintf(stderr, "scribodo: quitting (signal %d)\n", (int)g_quit_signal);
            break;
        }
        /* Scripted input is injected before anything reads input state. */
        if (app.script && !script_frame(app.script, &loop.host)) break;

        if (app.llm) llm_server_tick(app.llm);
        if (app.embed) llm_server_tick(app.embed);
        if (app.rerank) llm_server_tick(app.rerank);
        /* While the store is wanted but offline, probe every 15 s; when it
         * answers, connect (on this thread), adopt the project, refresh. */
        if (app.store_wanted && !store_online(app.store) && !service_supervisor_busy(&app.services) && GetTime() >= app.store_next_probe) {
            app.store_next_probe = GetTime() + 15.0;
            store_try_later(&app);
        }
        ServiceSupervisorResult service_result;
        if (service_supervisor_poll(&app.services, &service_result)) {
            if (service_result.command_attempted && service_result.command_succeeded) {
                app.services_started = true;
                app.services_started_at = GetTime();
                const char *warning = service_supervisor_warning(&service_result);
                if (warning && strcmp(warning, app.services_warning)) {
                    snprintf(app.services_warning, sizeof(app.services_warning), "%s", warning);
                    set_status(&app, warning);
                }
            }
            if (service_result.reachable || service_result.command_succeeded) {
                app.services_headline = NULL;
            } else if (service_result.command_attempted) {
                /* The script's reason reaches the author: once in the status line, and its sentence on the badge. */
                char why[sizeof(app.store_why)];
                service_supervisor_describe(&service_result, why, sizeof(why));
                if (strcmp(why, app.store_why)) set_status(&app, why);
                snprintf(app.store_why, sizeof(app.store_why), "%s", why);
                app.services_headline = service_supervisor_headline(&service_result);
            }
            if (service_result.reachable) {
                cancel_history(&app);
                app.store_why[0] = '\0';
                store_start(app.store, getenv("SCRIBODO_DB_NAME"), app.store_why, sizeof(app.store_why));
            }
            if (store_online(app.store)) {
                resave_unsaved_chat(&app);
                if (store_adopt_project(&app) && app.observer) {
                    app.observer_project_deferred = true;
                    observer_set_project(app.observer, "");
                }
                app.lists_dirty = true;
                app.findings_version = UINT64_MAX;
                if (app.observer) observer_poke(app.observer);
            }
        }
        if (!frame_step(&app, &loop)) break;
    }

    rc = app.script ? script_finish(app.script, &loop.host, loop.closed_by_window) : 0;
    app_destroy(&app);
    return rc;
}
