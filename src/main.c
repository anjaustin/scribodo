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
#include "app/script.h"
#include "llm/chat.h"
#include "llm/server.h"
#include "ui/chat_view.h"
#include "ui/clay_render.h"
#include "ui/editor.h"
#include "ui/font.h"
#include "ui/input.h"

/* raylib re-reads GLFW's close flag every frame, so clearing it cancels a
 * close request. GLFWwindow is opaque; raylib hands us the pointer. */
void glfwSetWindowShouldClose(void *window, int value);

enum { FONT_UI = 0, FONT_PROSE = 1, FONT_CHAT = 2, FONT_COUNT };
enum { FOCUS_NONE, FOCUS_DOC, FOCUS_CHAT };
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
}

static void set_project_from(App *app, const char *doc_path)
{
    char real[PATH_MAX];
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", doc_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    if (!realpath(dir, real)) snprintf(real, sizeof(real), "%s", dir);
    if (strcmp(real, app->project_dir) != 0) {
        snprintf(app->project_dir, sizeof(app->project_dir), "%s", real);
        /* A different project has different chats: start fresh (after the
         * current reply finishes, so it is saved with its own project). */
        if (app->chat) app->clear_chat_when_idle = true;
    }
    app->lists_dirty = true;
}

/* ---- documents ------------------------------------------------------------------ */

static void open_path(App *app, const char *path)
{
    char msg[512];
    if (editor_load_file(app->editor, path, msg, sizeof(msg))) {
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
    editor_save(app->editor, msg, sizeof(msg));
    set_status(app, msg);
    app->lists_dirty = true;
}

/* ---- chat ---------------------------------------------------------------------- */

static void on_message_done(ChatSession *s, size_t index, void *ctx)
{
    App *app = ctx;
    if (!app->chat_project[0]) return; /* no project: conversation stays in memory */
    chat_lock(s);
    const ChatMessage *m = chat_message(s, index);
    ChatMessage copy = { 0 };
    if (m) {
        copy = *m;
        copy.content = strdup(m->content ? m->content : "");
    }
    chat_unlock(s);
    if (!m || !copy.content) return;
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
    if (chat_send(app->chat, text, &ctx, err, sizeof(err))) {
        chat_view_clear_input(app->chat_view);
        if (err[0]) set_status(app, err); /* e.g. document truncated */
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
                   .backgroundColor = COL_PANEL, .clip = { .vertical = true } }) {
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
                section_header("NOTES");
                coming_soon("Coming soon");
                section_header("CANON");
                coming_soon("Coming soon");
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
                    slot(CLAY_ID("IncludeDoc"), 16, 16);
                    label("Include document", FONT_UI, COL_MUTED);
                    grow();
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
    (void)ctx;
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
    if (!app.chat || !app.editor || !app.chat_view) {
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
                        host_status, host_open, host_drop, &app, app.chat, host_element, host_model_ready };
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
            if (!editor_is_dirty(app.editor) || (app.close_armed && GetTime() - app.close_armed_at < 5.0)) {
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
        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });

        /* Clicks (pointer state reflects last frame's layout). */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (trace) fprintf(stderr, "trace: click (%.0f, %.0f) focus=%d\n", mouse.x, mouse.y, app.focus);
            if (Clay_PointerOver(CLAY_ID("SplitLeft"))) app.dragging = DRAG_EXPLORER;
            else if (Clay_PointerOver(CLAY_ID("SplitChat"))) app.dragging = DRAG_CHAT;
            else if (app.observer_open && Clay_PointerOver(CLAY_ID("SplitRight"))) app.dragging = DRAG_OBSERVER;
            else if (Clay_PointerOver(CLAY_ID("Editor"))) app.focus = FOCUS_DOC;
            else if (Clay_PointerOver(CLAY_ID("ChatInput"))) app.focus = FOCUS_CHAT;
            else if (Clay_PointerOver(CLAY_ID("NewChatItem"))) new_chat(&app);
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
        if (primary && shortcut_pressed('s')) save(&app);
        if (primary && shortcut_pressed('l')) app.focus = FOCUS_CHAT;
        if (app.focus == FOCUS_CHAT && IsKeyPressed(KEY_ESCAPE)) app.focus = FOCUS_DOC;

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
        input_discard_chars(); /* after both views: nothing typed now may appear later */
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
        GuiCheckBox(inc_rect, NULL, &app.include_doc);
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
