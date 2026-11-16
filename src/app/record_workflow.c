/* Notes, canon, entities and observer findings: listing, opening, saving and acting on them. Moved from src/main.c (AUDIT-004 F3). */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"

static void record_label(char *out, size_t outlen, const char *prefix, const char *content);
static void refresh_entities(App *app);
static bool is_blank(const char *s);
static bool selection_source(App *app, SourceRef *s);
static void store_canon(App *app, const char *status, bool use_editor_text);
static const Record *find_finding(App *app, const char *id);

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
    for (size_t i = 0; app->ents && i < app->nents && i < ENT_SHOW; i++) {
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
void toggle_entity(App *app, const char *id)
{
    if (!strcmp(app->ent_open, id)) app->ent_open[0] = '\0';
    else snprintf(app->ent_open, sizeof(app->ent_open), "%s", id);
    refresh_entities(app);
}

/* The passage a finding came from: a relationship's evidence, or a
 * mention. */
void open_finding_source(App *app, const char *finding_id)
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

void refresh_records(App *app)
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

bool record_unsaved_canon(App *app)
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

/* A new note or canon id that the project's log does not already hold (AUDIT-004 F12): records_new_id alone is
 * distinct only within this process, and records_append would file a new record under another process's id as that
 * record's next version, hiding it. False (nothing to write) when the log cannot be read or every id is taken. */
static bool new_record_id(App *app, RecordKind kind, char *id, size_t idlen)
{
    return app->project_dir[0] && records_new_id_checked(app->store, app->project_dir, kind, id, idlen);
}

/* Store the open note's text as a new version. */
bool save_note(App *app)
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
        if (!new_record_id(app, REC_NOTE, id, sizeof(id))) {
            set_status(app, store_says(app, "Could not save the note to .scribodo/notes.jsonl"));
            free(text);
            return false;
        }
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
bool close_record(App *app, bool explicit_close)
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
bool show_record(App *app, RecordKind kind, const Record *rec, const char *content, const SourceRef *src, size_t ns)
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
void note_from_selection(App *app)
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
        int ver = 0;
        if (new_record_id(app, REC_NOTE, id, sizeof(id)) &&
            records_append(app->store, app->project_dir, REC_NOTE, id, "active", s.quote, &s, 1, &ver)) {
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
void canon_from_selection(App *app)
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
void record_from_reply(App *app, long index, bool canon)
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
            int ver = 0;
            if (new_record_id(app, REC_NOTE, id, sizeof(id)) &&
                records_append(app->store, app->project_dir, REC_NOTE, id, "active", text, &s, 1, &ver)) {
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
void open_source(App *app, const SourceRef *s)
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
    if (!id[0] && !new_record_id(app, REC_CANON, id, sizeof(id))) {
        set_status(app, store_says(app, "Could not save to .scribodo/canon.jsonl"));
        free(text);
        return;
    }
    size_t ns;
    const SourceRef *src = record_view_sources(v, &ns);
    int ver = 0;
    bool first = !record_view_id(v)[0];
    FindingInfo origin = { 0 }; /* a first version drafted from a finding records which one (red-team 2026-11-14) */
    if (first && app->canon_finding[0]) snprintf(origin.related, sizeof(origin.related), "%s", app->canon_finding);
    if (app->project_dir[0] && records_append_ex(app->store, app->project_dir, REC_CANON, id, status, text, src, ns, origin.related[0] ? &origin : NULL, &ver)) {
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

void record_action(App *app, RecordAction act)
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

void refresh_findings(App *app)
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

void finding_action(App *app, FindingAction act, const char *id)
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
        int ver = 0;
        FindingInfo origin = { 0 }; /* the note's provenance: the finding it was made from (red-team 2026-11-14) */
        snprintf(origin.related, sizeof(origin.related), "%s", id);
        if (!new_record_id(app, REC_NOTE, nid, sizeof(nid)) || !records_append_ex(app->store, app->project_dir, REC_NOTE, nid, "active", v->content, v->sources, v->nsources, &origin, &ver)) {
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
