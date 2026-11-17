/* Chat: sending, replies done, history windows, attachments, names noticed in chat. Moved from src/main.c (AUDIT-004 F3). */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"

static void names_check(App *app, size_t reply_index);

void cancel_history(App *app)
{
    if (app->history_job) chat_history_job_cancel(app->history_job);
    app->history_pending = false;
    app->history_ticket++;
}

/* Routing hint only: a stored project always goes through the worker, including
 * paused/malformed markers. Actual authorization stays in ProjectStore there. */
bool database_history_project(const char *project)
{
    char marker[PATH_MAX + 64];
    struct stat st;
    snprintf(marker, sizeof(marker), "%s/.scribodo/store.json", project);
    return lstat(marker, &st) == 0;
}

bool history_read_busy(App *app)
{
    return app->history_pending || chat_history_job_busy(app->history_job);
}

const ChatSummary *find_chat(const App *app, const char *id)
{
    for (size_t i = 0; id && id[0] && i < app->nchats; i++)
        if (!strcmp(app->chats[i].id, id)) return &app->chats[i];
    return NULL;
}

/* The open chat has been saved (the setting belongs to a stored chat). */
bool open_chat_stored(const App *app) { return app->project_dir[0] && find_chat(app, app->chat_id) != NULL; }

void set_chat_observer(App *app, const char *id, bool read)
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

void set_chat_archived(App *app, const char *id, bool archived)
{
    char cid[40];
    snprintf(cid, sizeof(cid), "%s", id); /* the list is refreshed below */
    if (!chat_store_set_archived(app->store, app->project_dir, cid, archived)) {
        set_status(app, store_says(app, archived ? "Couldn't archive that chat" : "Couldn't restore that chat"));
        return;
    }
    set_status(app, archived ? "Chat archived: memory and the observer set it aside until you restore it (Archived, at the end of Chats)"
                             : "Chat restored: memory and the observer use it again");
    app->lists_dirty = true; /* refreshed after this frame is drawn (rows point into the lists) */
    if (!archived && app->observer) observer_poke(app->observer);
}

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

void names_poll(App *app)
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

void names_answer(App *app, bool save)
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

void on_message_done(ChatSession *s, size_t index, void *ctx)
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
    if (!chat_store_append_sequenced(app->store, app->chat_project, app->chat_id, app->chat_title, &copy, &sequence)) {
        chat_mark_save_failed(s, index);
        set_status(app, store_says(app, "Could not save the chat to .scribodo/chats"));
    } else {
        if (sequence >= 0) chat_mark_saved(s, index, sequence);
        if (app->observer && copy.role == CHAT_ASSISTANT) observer_poke(app->observer);
    }
    free(copy.content);
    app->lists_dirty = true;
}

/* A conversation started before any project was open has nowhere to be
 * saved; when a project appears, adopt it rather than discard it (red-team
 * 003: it used to be cleared, silently losing the author's text). */
void adopt_chat(App *app)
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

/* Messages whose append the store refused are saved when it answers again (red-team 2026-11-14: the status told
 * the author to keep them and nothing re-appended them, so sending stayed refused). Exactly the marked ones: a reply
 * whose done callback has not run yet carries no mark, and neither does a chat loaded from files before its project
 * moved into the store (second pass: inferring "unsaved" from a missing sequence re-appended both). */
void resave_unsaved_chat(App *app)
{
    if (!app->chat_project[0] || !app->chat_id[0]) return;
    bool ok = false;
    if (!chat_store_resave_failed(app->store, app->chat, app->chat_project, app->chat_id, app->chat_title, &ok)) return;
    if (chat_trim_saved(app->chat)) chat_view_history_changed(app->chat_view);
    set_status(app, ok ? "Saved the messages kept while the store was offline" : store_says(app, "Could not save the chat to .scribodo/chats"));
    app->lists_dirty = true;
}

bool is_attached(App *app, const char *name)
{
    return chat_controller_has_attachment(&app->chat_controller, name);
}

/* A document the Explorer lists for this project: never a path, a hidden
 * file or anything outside the folder (red-team 007: a chat log could name
 * "../other/secret.md"). */
bool listed_doc(App *app, const char *name)
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

bool attach(App *app, const char *name)
{
    if (is_attached(app, name)) return true;
    if (!listed_doc(app, name)) return false;
    if (app->nattached == MAX_ATTACH) {
        set_status(app, "A chat can have up to 8 documents attached");
        return false;
    }
    return chat_controller_add_attachment(&app->chat_controller, name);
}

void detach(App *app, size_t i)
{
    (void)chat_controller_remove_attachment(&app->chat_controller, i);
}

/* A new conversation starts with the open document attached. */
void attach_reset(App *app)
{
    chat_controller_clear_attachments(&app->chat_controller);
    if (editor_path(app->editor)) attach(app, base_name(editor_path(app->editor)));
}

/* Read an attached document as the editor shows it: the open one from the
 * buffer (unsaved text included), others from disk with BOM and CRLF
 * normalised. NULL if it can't be read. */
char *attached_text(App *app, const char *name, size_t cap, bool *cut)
{
    *cut = false;
    if (!listed_doc(app, name)) return NULL;
    if (editor_path(app->editor) && !strcmp(base_name(editor_path(app->editor)), name)) {
        size_t length = editor_length(app->editor);
        size_t keep = cap && length > cap ? cap : length;
        char *text = editor_range_utf8(app->editor, 0, keep);
        if (!text) return NULL;
        *cut = keep < length;
        if (cap && strlen(text) > cap) {
            size_t end = cap;
            while (end && ((unsigned char)text[end] & 0xc0) == 0x80) end--;
            text[end] = 0;
            *cut = true;
        }
        return text;
    }
    char full[PATH_MAX + 260];
    snprintf(full, sizeof(full), "%s/%s", app->project_dir, name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (16 << 20)) return NULL;
    FILE *f = fopen(full, "rb");
    if (!f) return NULL;
    size_t wanted = (size_t)st.st_size;
    if (cap && wanted > cap) { wanted = cap; *cut = true; }
    char *buf = malloc(wanted + 1);
    size_t got = buf ? fread(buf, 1, wanted, f) : 0;
    bool read_ok = !ferror(f) && got == wanted;
    fclose(f);
    if (!buf || !read_ok) { free(buf); return NULL; }
    if (*cut) {
        /* Drop a potentially partial final UTF-8 sequence without reading the
         * rest of a manuscript on the UI thread. */
        size_t start = got;
        while (start && ((unsigned char)buf[start - 1] & 0xc0) == 0x80) start--;
        if (start) {
            unsigned char lead = (unsigned char)buf[start - 1];
            size_t need = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            if (got - (start - 1) < need) got = start - 1;
        }
    }
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

bool unsaved_chat(App *app)
{
    chat_lock(app->chat);
    bool unsaved = chat_window_info(app->chat).unsaved;
    chat_unlock(app->chat);
    return unsaved;
}

void new_chat(App *app)
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

bool load_chat_window(App *app, const char *project, const char *id, const char *title,
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

void history_poll(App *app)
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
            chat_context_used_reset(app->chat); /* another conversation: its figure comes with its next reply */
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

void page_chat(App *app, ChatPageAction action)
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

void open_chat(App *app, const ChatSummary *c)
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

/* `s` shortened with an ellipsis to fit `maxw` (whole UTF-8 characters). */
void fit_label(const SFont *f, const char *s, float maxw, char *out, size_t outlen)
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
int draw_attach_row(App *app, Rectangle r)
{
    const SFont *ui = &app->fonts[FONT_UI];
    Color muted = THEME_TEXT_MUTED, chip_bg = THEME_CHIP, text = THEME_TEXT;
    Color link = THEME_LINK;
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
        sfont_draw_utf8(ui, "\xC3\x97", 2, xr.x + 4, y, hot ? THEME_ERR : muted);
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

void send_chat(App *app, char *text)
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
    ChatContext ctx = { .note = chat_view_take_submit_note(app->chat_view) }; /* the author's declared turn kind */
    char *doc = NULL;
    ChatDoc cdocs[MAX_ATTACH];
    char *texts[MAX_ATTACH] = { 0 };
    size_t ncd = 0;
    char missing[300] = "";
    for (size_t i = 0; i < app->nattached; i++) {
        bool cut = false;
        size_t cap = app->memory_experiment ? (64u * 1024u) / (app->nattached ? app->nattached : 1) : 0;
        texts[ncd] = attached_text(app, app->attached[i], cap, &cut);
        if (!texts[ncd]) {
            snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s%s", missing[0] ? ", " : "", app->attached[i]);
            continue;
        }
        cdocs[ncd] = (ChatDoc){ .title = app->attached[i], .text = texts[ncd],
            .unsaved = editor_path(app->editor) && !strcmp(base_name(editor_path(app->editor)), app->attached[i]) &&
                       editor_is_dirty(app->editor), .truncated = cut };
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
