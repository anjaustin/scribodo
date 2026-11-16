/* The frame's Clay layout: every CLAY( element of the window, pane by pane, and the strings it shows. Moved from src/main.c (AUDIT-004 F3). */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"

static Clay_String cstr(FrameViewModel *frame, const char *s);
static void section_header(App *app, const char *s);
static void list_item(App *app, Clay_ElementId id, const char *s, bool active);
static void coming_soon(App *app, const char *s);
static void splitter(Clay_ElementId id, bool active);
static void slot(Clay_ElementId id, float w, float h);
static void grow(void);
static void update_labels(App *app);
static void layout_toolbar(App *app);
static void layout_entities(App *app);
static void layout_explorer(App *app);
static void layout_chat(App *app);
static void layout_side_panel(App *app);
static void layout_status_bar(App *app);

static Clay_String cstr(FrameViewModel *frame, const char *s)
{
    const char *copy = frame_view_model_copy(frame, s);
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(copy), .chars = copy };
}

void label(App *app, const char *s, int font, Clay_Color color)
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
        snprintf(app->model_label, sizeof(app->model_label), "\xE2\x97\x8F Remote: %s \xC2\xB7 chat, attached documents, edit passages and your profile name go to %s",
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
        bool paused = app->store_inspection.blocked_by != PROJECT_STORE_BLOCK_NONE;
        const char *why = paused ? "notes, canon, chats and History paused" : app->services_headline;
        snprintf(app->store_label, sizeof(app->store_label), "\xE2\x97\x8F Project store offline%s%s", why ? " \xC2\xB7 " : "",
                 why ? why : "");
        app->store_color = app->store_inspection.blocked_by != PROJECT_STORE_BLOCK_NONE ? COL_ERR : COL_WARN;
    }
    app->model_color = ls == LLM_READY ? COL_OK : ls == LLM_FAILED ? COL_ERR : COL_WARN;
    if (remote_on) app->model_color = COL_WARN; /* project content leaves this Mac */

    snprintf(app->chat_header, sizeof(app->chat_header), "%s", app->chat_title[0] ? app->chat_title : "New chat");
}

/* The toolbar: open, the document, save, the store and model badges, recall and observer toggles. */
static void layout_toolbar(App *app)
{
    /* Toolbar */
    CLAY({ .id = CLAY_ID("Toolbar"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40) },
                       .padding = { 14, 14, 0, 0 }, .childGap = 14,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = COL_PANEL,
           .border = { .color = COL_BORDER, .width = { .bottom = 1 } } }) {
        /* Scribodo: the app's menu (Fonts, Model settings). */
        CLAY({ .id = CLAY_ID("AppMenu"),
               .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(26) }, .padding = { 8, 8, 0, 0 },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = Clay_PointerOver(CLAY_ID("AppMenu")) ? COL_HOVER : (Clay_Color){ 0 },
               .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
            label(app, "Scribodo", FONT_UI, COL_TEXT);
        }
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
        slot(CLAY_ID("RecallToggle"), 72, 24);
        slot(CLAY_ID("ObserverToggle"), 96, 24);
    }
}

/* The Explorer's entity graph (store projects), recording what each row held when it was laid out. */
static void layout_entities(App *app)
{
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

/* The Explorer: documents, chats, History, notes, canon, the project actions and entities. */
static void layout_explorer(App *app)
{
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
        /* Back up, Restore and Names live in the Scribodo menu; the last backup's outcome stays in view here. */
        if (app->project_dir[0] && app->backup_label[0]) coming_soon(app, app->backup_label);
        layout_entities(app);
    }
}

/* The chat pane: header, memory line, messages, a name to confirm, footer and input. */
static void layout_chat(App *app)
{
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
        if (app->memory_experiment && app->memory_label[0]) {
            CLAY({ .id = CLAY_ID("MemoryStatus"),
                   .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) }, .padding = { 12, 8, 4, 0 } },
                   .clip = { .horizontal = true } }) { label(app, app->memory_label, FONT_UI, COL_DIM); }
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
}

/* The right pane: recall or the observer, whichever is open. */
static void layout_side_panel(App *app)
{
    if (app->recall_open) {
        splitter(CLAY_ID("SplitRight"), app->dragging == DRAG_OBSERVER);
        CLAY({ .id = CLAY_ID("Recall"),
               .layout = { .sizing = { CLAY_SIZING_FIXED(app->observer_w), CLAY_SIZING_GROW(0) },
                           .padding = CLAY_PADDING_ALL(12), .childGap = 8,
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = COL_PANEL, .clip = { .vertical = true } }) {
            section_header(app, "RECALL");
            label(app, "Your committed canon and verified original text", FONT_UI, COL_DIM);
            CLAY({ .id = CLAY_ID("RecallBody"),
                   .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
        }
    } else if (app->observer_open) {
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

/* The status bar. */
static void layout_status_bar(App *app)
{
    /* Status bar */
    CLAY({ .id = CLAY_ID("StatusBar"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26) },
                       .padding = { 14, 14, 0, 0 }, .childGap = 16,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = COL_PANEL,
           .border = { .color = COL_BORDER, .width = { .top = 1 } } }) {
        label(app, app->status_left, FONT_MONO, COL_MUTED); /* position, words, encoding: figures */
        grow();
        if (app->status[0] && GetTime() - app->status_time < 6.0)
            label(app, app->status, FONT_UI, editor_path(app->editor) && editor_is_read_only(app->editor) ? COL_WARN : COL_TEXT);
    }
}

Clay_RenderCommandArray build_layout(App *app)
{
    app->drawn_explorer_generation = app->explorer.generation;
    frame_view_model_begin(&app->frame);
    update_labels(app);
    Clay_BeginLayout();
    CLAY({ .id = CLAY_ID("Root"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = COL_BG }) {

        layout_toolbar(app);

        CLAY({ .id = CLAY_ID("Main"), .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
            layout_explorer(app);
            splitter(CLAY_ID("SplitLeft"), app->dragging == DRAG_EXPLORER);

            CLAY({ .id = CLAY_ID("Editor"), .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                   .backgroundColor = COL_BG }) {}

            splitter(CLAY_ID("SplitChat"), app->dragging == DRAG_CHAT);

            layout_chat(app);
            layout_side_panel(app);
        }

        layout_status_bar(app);
    }
    Clay_RenderCommandArray commands = Clay_EndLayout();
    frame_view_model_seal(&app->frame);
    return commands;
}

Rectangle element_rect_str(Clay_String s, uint32_t index, bool indexed)
{
    Clay_ElementId id = indexed ? Clay_GetElementIdWithIndex(s, index) : Clay_GetElementId(s);
    if (!clay_element_current(id)) return (Rectangle){ 0 }; /* not in this layout */
    Clay_ElementData d = Clay_GetElementData(id);
    return d.found ? clay_to_rect(d.boundingBox) : (Rectangle){ 0 };
}
