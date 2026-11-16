/* Documents: names, new, rename, import, reveal, and the name prompt's answers (finish_prompt). Moved from src/main.c (AUDIT-004 F3). */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"

static bool doc_name(const char *typed, char *out, size_t outlen, char *err, size_t errlen);
static bool same_file(const char *a, const char *b);
static bool set_aside_history(App *app, const char *name, bool linked_documents);

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

bool name_taken(App *app, const char *name)
{
    char full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    struct stat st;
    return lstat(full, &st) == 0;
}

void begin_new_document(App *app)
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

void begin_rename(App *app, const char *name)
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

bool finish_prompt(App *app, const char *typed)
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
void import_documents(App *app)
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
        /* A short read is the end of the file or an error (ferror below): no read after it. */
        for (size_t r = sizeof(buf); ok && r == sizeof(buf);) {
            r = fread(buf, 1, sizeof(buf), in);
            if (r > 0) ok = write(fd, buf, r) == (ssize_t)r;
        }
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

void reveal_document(App *app, const char *name)
{
    char full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    dialogs_reveal(full);
}
