/* Scribodo — editor feasibility spike.
 *
 * Native workbench shell (raylib + Clay layout + raygui controls) around the
 * prose editor. Explorer and Observer panes are placeholders for now.
 *
 *   scribodo [file.md|file.txt]
 *
 * Drop a .md/.txt file on the window to open it. ⌘S saves.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clay.h"
#include "raygui.h"
#include "raylib.h"

#include "ui/clay_render.h"
#include "ui/editor.h"
#include "ui/font.h"
#include "ui/keys.h"

/* raylib re-reads GLFW's close flag every frame, so clearing it cancels a
 * close request. GLFWwindow is opaque; raylib hands us the pointer. */
void glfwSetWindowShouldClose(void *window, int value);

enum { FONT_UI = 0, FONT_PROSE = 1, FONT_COUNT };

#define RGB(r, g, b) ((Clay_Color){ r, g, b, 255 })
static const Clay_Color COL_BG      = RGB(0x1F, 0x1F, 0x1F);
static const Clay_Color COL_PANEL   = RGB(0x18, 0x18, 0x18);
static const Clay_Color COL_BORDER  = RGB(0x2B, 0x2B, 0x2B);
static const Clay_Color COL_TEXT    = RGB(0xCC, 0xCC, 0xCC);
static const Clay_Color COL_MUTED   = RGB(0x8B, 0x8B, 0x8B);
static const Clay_Color COL_ACCENT  = RGB(0x00, 0x78, 0xD4);
static const Clay_Color COL_ACTIVE  = RGB(0x37, 0x37, 0x3D);
static const Clay_Color COL_WARN    = RGB(0xCC, 0xA7, 0x00);

#define SPLITTER_W 5

typedef struct {
    Editor *editor;
    SFont fonts[FONT_COUNT];
    SFont *font_ptrs[FONT_COUNT];
    float explorer_w, observer_w;
    int dragging; /* 0 none, 1 explorer splitter, 2 observer splitter */
    bool editor_focused;
    char status[512];
    double status_time;
    bool close_armed;
    double close_armed_at;
    /* Strings referenced by Clay must outlive the frame's render pass. */
    char doc_label[512];
    char status_left[256];
} App;

static void set_status(App *app, const char *msg)
{
    snprintf(app->status, sizeof(app->status), "%s", msg);
    app->status_time = GetTime();
}

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
    CLAY({ .layout = { .padding = { 0, 0, 8, 2 } } }) { label(s, FONT_UI, COL_MUTED); }
}

static void list_item(const char *s, bool active)
{
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) },
                       .padding = { 8, 8, 0, 0 },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = active ? COL_ACTIVE : (Clay_Color){ 0 },
           .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
        label(s, FONT_UI, active ? COL_TEXT : COL_MUTED);
    }
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

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static Clay_RenderCommandArray build_layout(App *app)
{
    Editor *ed = app->editor;
    const char *path = editor_path(ed) ? editor_path(ed) : "untitled";
    snprintf(app->doc_label, sizeof(app->doc_label), "%s%s%s", base_name(path),
             editor_is_dirty(ed) ? "  \xE2\x97\x8F" : "",
             editor_is_read_only(ed) ? "  (read-only)" : "");
    if (!editor_path(ed)) snprintf(app->doc_label, sizeof(app->doc_label), "No document \xE2\x80\x94 drop a .md or .txt file here");

    EditorStats st = editor_stats(ed);
    if (st.selected)
        snprintf(app->status_left, sizeof(app->status_left), "Ln %d, Col %d (%zu selected)   %zu words   UTF-8   %s",
                 st.line, st.column, st.selected, st.words, st.crlf ? "CRLF" : "LF");
    else
        snprintf(app->status_left, sizeof(app->status_left), "Ln %d, Col %d   %zu words   UTF-8   %s",
                 st.line, st.column, st.words, st.crlf ? "CRLF" : "LF");

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
            CLAY({ .id = CLAY_ID("SaveButton"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(24) } } }) {}
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
            label("LFM3-1.7B \xC2\xB7 BF16 \xC2\xB7 local \xC2\xB7 not connected", FONT_UI, COL_MUTED);
        }

        /* Panes */
        CLAY({ .id = CLAY_ID("Main"),
               .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {

            CLAY({ .id = CLAY_ID("Explorer"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(app->explorer_w), CLAY_SIZING_GROW(0) },
                               .padding = CLAY_PADDING_ALL(12), .childGap = 2,
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = COL_PANEL,
                   .clip = { .vertical = true } }) {
                section_header("DOCUMENTS");
                list_item(base_name(path), true);
                section_header("CHATS");
                list_item("No chats yet", false);
                section_header("NOTES");
                list_item("No notes yet", false);
                section_header("CANON");
                list_item("No canon entries", false);
            }

            splitter(CLAY_ID("SplitLeft"), app->dragging == 1);

            CLAY({ .id = CLAY_ID("Editor"),
                   .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                   .backgroundColor = COL_BG }) {}

            splitter(CLAY_ID("SplitRight"), app->dragging == 2);

            CLAY({ .id = CLAY_ID("Observer"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(app->observer_w), CLAY_SIZING_GROW(0) },
                               .padding = CLAY_PADDING_ALL(12), .childGap = 8,
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = COL_PANEL,
                   .clip = { .vertical = true } }) {
                section_header("OBSERVER");
                wrapped_label("No findings yet. The observer will list characters, events, "
                              "relationships, open questions, and possible continuity "
                              "conflicts here \xE2\x80\x94 it never writes into your chat or documents.",
                              COL_MUTED);
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
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
            if (app->status[0] && GetTime() - app->status_time < 6.0)
                label(app->status, FONT_UI, editor_is_read_only(ed) ? COL_WARN : COL_TEXT);
        }
    }
    return Clay_EndLayout();
}

static Rectangle element_rect(const char *name, int32_t len)
{
    Clay_String s = { .isStaticallyAllocated = true, .length = len, .chars = name };
    Clay_ElementData d = Clay_GetElementData(Clay_GetElementId(s));
    return d.found ? clay_to_rect(d.boundingBox) : (Rectangle){ 0 };
}
#define ELEMENT_RECT(lit) element_rect(lit, (int32_t)(sizeof(lit) - 1))

static void clay_error(Clay_ErrorData err)
{
    fprintf(stderr, "clay: %.*s\n", err.errorText.length, err.errorText.chars);
}

static void style_raygui(const SFont *ui)
{
    GuiSetFont(ui->faces[0]);
    GuiSetStyle(DEFAULT, TEXT_SIZE, (int)ui->size);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    GuiSetStyle(BUTTON, BORDER_WIDTH, 1);
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt((Color){ 0x31, 0x31, 0x31, 0xFF }));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt((Color){ 0x3C, 0x3C, 0x3C, 0xFF }));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt((Color){ 0xCC, 0xCC, 0xCC, 0xFF }));
    GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, ColorToInt((Color){ 0x3C, 0x3C, 0x3C, 0xFF }));
    GuiSetStyle(BUTTON, BORDER_COLOR_FOCUSED, ColorToInt((Color){ 0x00, 0x78, 0xD4, 0xFF }));
    GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, ColorToInt((Color){ 0x00, 0x78, 0xD4, 0xFF }));
    GuiSetStyle(BUTTON, BORDER_COLOR_PRESSED, ColorToInt((Color){ 0x00, 0x78, 0xD4, 0xFF }));
    GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BASE_COLOR_DISABLED, ColorToInt((Color){ 0x25, 0x25, 0x25, 0xFF }));
    GuiSetStyle(BUTTON, BORDER_COLOR_DISABLED, ColorToInt((Color){ 0x30, 0x30, 0x30, 0xFF }));
    GuiSetStyle(BUTTON, TEXT_COLOR_DISABLED, ColorToInt((Color){ 0x6B, 0x6B, 0x6B, 0xFF }));
}

static bool load_fonts(App *app, float dpi)
{
    static const char *const ui_paths[] = {
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    };
    static const char *const prose_paths[] = {
        "/System/Library/Fonts/Supplemental/Georgia.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    const char *override = getenv("SCRIBODO_PROSE_FONT");
    const char *prose_list[5];
    int np = 0;
    if (override) prose_list[np++] = override;
    for (size_t i = 0; i < sizeof(prose_paths) / sizeof(prose_paths[0]); i++) prose_list[np++] = prose_paths[i];

    bool ok = sfont_load(&app->fonts[FONT_UI], ui_paths, 4, 13.0f, 1.45f, dpi) &&
              sfont_load(&app->fonts[FONT_PROSE], prose_list, np, 18.0f, 1.6f, dpi);
    for (int i = 0; i < FONT_COUNT; i++) app->font_ptrs[i] = &app->fonts[i];
    return ok;
}

static void open_path(App *app, const char *path)
{
    char msg[512];
    if (editor_load_file(app->editor, path, msg, sizeof(msg))) {
        if (msg[0]) set_status(app, msg);
        else set_status(app, "Opened");
    } else {
        set_status(app, msg);
    }
}

static void save(App *app)
{
    char msg[512];
    editor_save(app->editor, msg, sizeof(msg));
    set_status(app, msg);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(1280, 820, "Scribodo");
    SetWindowMinSize(720, 420);
    SetExitKey(KEY_NULL); /* Esc must never quit a writing app */
    SetTargetFPS(60);

    static App app;
    app.explorer_w = 220;
    app.observer_w = 280;
    app.editor_focused = true;

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

    app.editor = editor_create(&app.fonts[FONT_PROSE]);
    if (!app.editor) {
        fprintf(stderr, "scribodo: out of memory\n");
        CloseWindow();
        return 1;
    }
    /* Default to the bundled sample, located relative to the executable
     * (build/scribodo) rather than the working directory, which is "/" when
     * launched from Finder. */
    char sample[2048];
    if (!path) {
        snprintf(sample, sizeof(sample), "%s../assets/sample/sample.md", GetApplicationDirectory());
        path = sample;
    }
    open_path(&app, path);

    /* Debug hook: SCRIBODO_SCREENSHOT=out.png renders a few frames, saves the
     * framebuffer, and exits. Used to check rendering without interaction. */
    const char *screenshot_path = getenv("SCRIBODO_SCREENSHOT");
    int frame = 0;

    int cursor_shape = MOUSE_CURSOR_DEFAULT;
    bool running = true;
    while (running) {
        /* Close requests: never lose unsaved prose silently. */
        if (WindowShouldClose()) {
            if (!editor_is_dirty(app.editor) || (app.close_armed && GetTime() - app.close_armed_at < 5.0)) {
                running = false;
                break;
            }
            glfwSetWindowShouldClose(GetWindowHandle(), 0);
            app.close_armed = true;
            app.close_armed_at = GetTime();
            set_status(&app, "Unsaved changes \xE2\x80\x94 press \xE2\x8C\x98S to save, or close again to discard");
        }

        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0) {
                if (editor_is_dirty(app.editor)) set_status(&app, "Save the current document before opening another");
                else open_path(&app, dropped.paths[0]);
            }
            UnloadDroppedFiles(dropped);
        }

        Vector2 mouse = GetMousePosition();
        bool mouse_down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y }, mouse_down);
        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });

        /* Splitter dragging (pointer state reflects last frame's layout). */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (Clay_PointerOver(CLAY_ID("SplitLeft"))) app.dragging = 1;
            else if (Clay_PointerOver(CLAY_ID("SplitRight"))) app.dragging = 2;
            /* Clicks in the toolbar (e.g. Save) keep the editor focused. */
            else if (!Clay_PointerOver(CLAY_ID("Toolbar")))
                app.editor_focused = Clay_PointerOver(CLAY_ID("Editor"));
        }
        if (!mouse_down) app.dragging = 0;
        float sw = (float)GetScreenWidth();
        if (app.dragging == 1) app.explorer_w = mouse.x - SPLITTER_W / 2.0f;
        if (app.dragging == 2) app.observer_w = sw - mouse.x - SPLITTER_W / 2.0f;
        /* Re-clamp every frame so shrinking the window can never squeeze the
         * editor below a usable width. */
        app.explorer_w = fmaxf(140.0f, fminf(app.explorer_w, sw * 0.30f));
        app.observer_w = fmaxf(180.0f, fminf(app.observer_w, sw * 0.30f));

        bool primary = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#ifndef __APPLE__
        primary = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
        if (primary && shortcut_pressed('s')) save(&app);

        const char *notice = editor_take_notice(app.editor);
        if (notice) set_status(&app, notice);

        Clay_RenderCommandArray cmds = build_layout(&app);
        Rectangle editor_rect = ELEMENT_RECT("Editor");
        Rectangle save_rect = ELEMENT_RECT("SaveButton");

        /* Always update so queued keystrokes are consumed or discarded. */
        editor_update(app.editor, editor_rect, app.editor_focused && app.dragging == 0);

        int want = MOUSE_CURSOR_DEFAULT;
        if (app.dragging || Clay_PointerOver(CLAY_ID("SplitLeft")) || Clay_PointerOver(CLAY_ID("SplitRight")))
            want = MOUSE_CURSOR_RESIZE_EW;
        else if (CheckCollisionPointRec(mouse, editor_rect))
            want = MOUSE_CURSOR_IBEAM;
        if (want != cursor_shape) {
            SetMouseCursor(want);
            cursor_shape = want;
        }

        BeginDrawing();
        ClearBackground((Color){ 0x1F, 0x1F, 0x1F, 0xFF });
        clay_render(cmds, app.font_ptrs);
        editor_draw(app.editor, editor_rect, app.editor_focused);

        bool can_save = (editor_is_dirty(app.editor) || editor_overwrite_armed(app.editor)) &&
                        !editor_is_read_only(app.editor);
        if (!can_save) GuiDisable();
        if (GuiButton(save_rect, "Save")) save(&app);
        GuiEnable();
        EndDrawing();

        if (screenshot_path && ++frame == 10) {
            Image shot = LoadImageFromScreen();
            ExportImage(shot, screenshot_path);
            UnloadImage(shot);
            running = false;
        }
    }

    editor_destroy(app.editor);
    for (int i = 0; i < FONT_COUNT; i++) sfont_unload(&app.fonts[i]);
    free(clay_buf);
    CloseWindow();
    return 0;
}
