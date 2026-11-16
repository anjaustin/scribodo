/* The Fonts panel. See fonts_panel.h. */
#define _DARWIN_C_SOURCE
#include "app/app_internal.h"
#include "ui/input.h"
#include "ui/theme.h"
#include "ui/widgets.h"

static const char *const k_samples[APPEARANCE_ROLE_COUNT] = {
    "Documents \xC2\xB7 Chats \xC2\xB7 History \xC2\xB7 Open\xE2\x80\xA6",
    "The fog came in off the fjord. Zo\xC3\xAB \xC3\x85ngstr\xC3\xB6m, Dvo\xC5\x99\xC3\xA1k, 1897.",
    "Who stands at the top of the lighthouse stairs?",
    "Ln 12, Col 40 \xC2\xB7 3,482 words \xC2\xB7 0123456789",
};

static void scan(App *app)
{
    FontsPanel *p = &app->fonts_panel;
    if (p->scanned) return;
    char bundled[PATH_MAX + 8], home[PATH_MAX];
    snprintf(bundled, sizeof(bundled), "%s/fonts", app->assets_dir);
    snprintf(home, sizeof(home), "%s/Library/Fonts", getenv("HOME") ? getenv("HOME") : "");
    const char *dirs[] = { app->assets_dir[0] ? bundled : "", "/System/Library/Fonts", "/Library/Fonts", home };
    if (!font_catalog_scan(&p->catalog, dirs, 4, bundled)) set_status(app, "Not enough memory to list the fonts");
    p->scanned = true;
    free(p->matches);
    p->matches = p->catalog.count ? calloc(p->catalog.count, sizeof(*p->matches)) : NULL;
    p->nmatch = 0;
}

static void refilter(FontsPanel *p)
{
    p->nmatch = p->matches ? font_catalog_filter(&p->catalog, p->filter, p->matches, p->catalog.count) : 0;
    p->sel = p->nmatch ? 1 : 0;
    p->top = 0;
}

void fonts_panel_open(App *app)
{
    FontsPanel *p = &app->fonts_panel;
    p->saved = app->appearance; /* Cancel returns to what was shown, which a failed save may have left unsaved */
    font_catalog_free(&p->catalog); /* listed afresh each time: a font installed since shows up */
    p->scanned = false;
    p->open = true;
    p->picking = -1;
}

void fonts_panel_destroy(FontsPanel *p)
{
    font_catalog_free(&p->catalog);
    free(p->matches);
    memset(p, 0, sizeof(*p));
}

static unsigned changed_roles(const Appearance *a, const Appearance *b)
{
    unsigned m = 0;
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++)
        if (strcmp(a->role[r].path, b->role[r].path) || a->role[r].index != b->role[r].index || a->role[r].size != b->role[r].size)
            m |= 1u << r;
    return m;
}

/* Shows `next` (only the roles that differ are reloaded). A newly chosen face that does not load is put back to the
 * role's previous choice; a choice that stopped loading since (the file was removed) is dropped for the default. */
static void show(App *app, const Appearance *next)
{
    Appearance before = app->appearance;
    unsigned roles = changed_roles(&before, next);
    if (!roles) return;
    app->appearance = *next;
    char why[PATH_MAX + 128];
    unsigned failed = app_fonts_apply(app, roles, why, sizeof(why));
    if (!failed) return;
    set_status(app, why);
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) {
        if (!(failed & (1u << r))) continue;
        AppearanceFont *f = &app->appearance.role[r];
        bool was_chosen = strcmp(before.role[r].path, f->path) || before.role[r].index != f->index;
        if (was_chosen) {
            snprintf(f->path, sizeof(f->path), "%s", before.role[r].path);
            f->index = before.role[r].index;
        } else {
            f->path[0] = '\0';
            f->index = 0;
        }
    }
    app_fonts_apply(app, failed, NULL, 0);
}

static void close_panel(App *app, bool keep)
{
    FontsPanel *p = &app->fonts_panel;
    if (keep) {
        if (appearance_save(&app->appearance)) set_status(app, "Fonts saved");
        else set_status(app, "Couldn't save the fonts; they apply until Scribodo quits");
    } else {
        show(app, &p->saved);
    }
    p->open = false;
    p->picking = -1;
}

/* The role's face as the list shows it: "Georgia", "Georgia Bold" (a Regular face by its family alone). */
static void role_face_name(const App *app, int r, char *out, size_t cap)
{
    const char *n = sfont_primary_name(&app->fonts[r]);
    snprintf(out, cap, "%s", n && n[0] ? n : "(none)");
    size_t len = strlen(out);
    if (len > 8 && !strcmp(out + len - 8, " Regular")) out[len - 8] = '\0';
}

/* --- the list of fonts ------------------------------------------------------------------------------------------ */

static void pick(App *app, int row)
{
    FontsPanel *p = &app->fonts_panel;
    int r = p->picking;
    Appearance next = app->appearance;
    if (row <= 0) {
        next.role[r].path[0] = '\0';
        next.role[r].index = 0;
    } else if ((size_t)(row - 1) < p->nmatch) {
        const FontCatalogEntry *e = &p->catalog.items[p->matches[row - 1]];
        snprintf(next.role[r].path, sizeof(next.role[r].path), "%s", e->path);
        next.role[r].index = e->index;
    }
    show(app, &next);
    p->picking = -1;
    p->picked = true;
}

/* The list's keys and typed filter: read in the frame's update phase, before unread keystrokes are dropped
 * (input_discard_chars), so typing reaches the filter. */
void fonts_panel_update(App *app)
{
    app->fonts_panel.picked = false;
    if (!app->fonts_panel.open || app->fonts_panel.picking < 0) return;
    FontsPanel *p = &app->fonts_panel;
    int c;
    bool typed = false;
    while ((c = input_get_char()) != 0) {
        size_t n = strlen(p->filter);
        if (c >= 0x20 && c < 0x7F && n + 1 < sizeof(p->filter)) {
            p->filter[n] = (char)c;
            p->filter[n + 1] = '\0';
            typed = true;
        }
    }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && p->filter[0]) {
        p->filter[strlen(p->filter) - 1] = '\0';
        typed = true;
    }
    if (typed) refilter(p);
    int rows = (int)p->nmatch + 1;
    if ((IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) && p->sel + 1 < rows) p->sel++;
    if ((IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) && p->sel > 0) p->sel--;
    int fit = p->fit > 0 ? p->fit : FONTS_PANEL_ROWS;
    if (p->sel < p->top) p->top = p->sel;
    if (p->sel >= p->top + fit) p->top = p->sel - fit + 1;
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        p->top -= (int)(wheel > 0 ? ceilf(wheel) : floorf(wheel));
        int max_top = rows > fit ? rows - fit : 0;
        p->top = p->top < 0 ? 0 : p->top > max_top ? max_top : p->top;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) pick(app, p->sel);
}

static void draw_list(App *app, Rectangle d)
{
    FontsPanel *p = &app->fonts_panel;
    const SFont *ui = &app->fonts[FONT_UI];
    int r = p->picking;
    const AppearanceRoleInfo *in = appearance_role_info(r);

    char title[96];
    snprintf(title, sizeof(title), "%s font", in->label);
    widget_text(ui, title, d.x + SP_6, d.y + SP_5, d.width - 2 * SP_6, THEME_TEXT);

    /* The filter, as typed. */
    Rectangle field = { d.x + SP_6, d.y + 52, d.width - 2 * SP_6, 32 };
    widget_round_rect(field, RADIUS_S, THEME_FIELD);
    widget_round_outline(field, RADIUS_S, 1.0f, THEME_ACCENT);
    if (p->filter[0]) {
        float w = widget_text(ui, p->filter, field.x + SP_3, field.y + (field.height - ui->line_height) * 0.5f, field.width - SP_6, THEME_TEXT);
        DrawRectangle((int)(field.x + SP_3 + w + 1), (int)(field.y + 8), 1, (int)(field.height - 16), THEME_TEXT);
    } else {
        widget_text_in(ui, "Type to filter the installed fonts", (Rectangle){ field.x + SP_3, field.y, field.width - SP_6, field.height },
                       THEME_TEXT_DIM);
    }

    float row_h = 30, y0 = field.y + field.height + SP_3;
    float footer = d.y + d.height - 60; /* the count and Back sit below; rows never reach them */
    int fit = (int)((footer - y0) / row_h);
    if (fit > FONTS_PANEL_ROWS) fit = FONTS_PANEL_ROWS;
    if (fit < 1) fit = 1;
    p->fit = fit;
    int rows = (int)p->nmatch + 1;
    p->nitem = 0;
    const char *current = app->appearance.role[r].path;
    int cur_index = app->appearance.role[r].index;
    for (int i = p->top; i < rows && p->nitem < fit; i++) {
        Rectangle row = { d.x + SP_4, y0 + row_h * (float)p->nitem, d.width - 2 * SP_4, row_h };
        p->item[p->nitem++] = row;
        char label[260];
        bool chosen;
        const char *note = "";
        if (i == 0) {
            snprintf(label, sizeof(label), "Default \xC2\xB7 %s", in->default_name);
            chosen = !current[0];
        } else {
            const FontCatalogEntry *e = &p->catalog.items[p->matches[i - 1]];
            snprintf(label, sizeof(label), "%s", e->display);
            chosen = current[0] && e->index == cur_index && !strcmp(e->path, current);
            note = e->bundled ? "bundled" : "";
        }
        bool hover = CheckCollisionPointRec(GetMousePosition(), row);
        if (i == p->sel || hover) widget_round_rect(row, RADIUS_S, i == p->sel ? THEME_ACTIVE : THEME_HOVER);
        if (chosen) widget_text(ui, "\xE2\x9C\x93", row.x + SP_2, row.y + (row_h - ui->line_height) * 0.5f, 0, THEME_OK);
        float nw = note[0] ? sfont_measure_utf8(ui, note, (int)strlen(note)) : 0;
        widget_text(ui, label, row.x + SP_8, row.y + (row_h - ui->line_height) * 0.5f, row.width - SP_8 - nw - SP_4,
                    i == p->sel ? THEME_TEXT : THEME_TEXT_MUTED);
        if (note[0]) widget_text(ui, note, row.x + row.width - nw - SP_2, row.y + (row_h - ui->line_height) * 0.5f, 0, THEME_TEXT_DIM);
        if (widget_clicked(row)) {
            pick(app, i);
            return;
        }
    }
    if (rows == 1 && p->filter[0])
        widget_text(ui, "No installed font matches", d.x + SP_6, y0 + row_h + SP_2, 0, THEME_TEXT_DIM);

    char count[64];
    snprintf(count, sizeof(count), "%zu of %zu fonts", p->nmatch, p->catalog.count);
    widget_text(ui, count, d.x + SP_6, d.y + d.height - 44, 0, THEME_TEXT_DIM);
    p->back = (Rectangle){ d.x + d.width - SP_6 - 96, d.y + d.height - 50, 96, 30 };
    if (widget_button(p->back, "Back", ui, WIDGET_OUTLINE, true) || IsKeyPressed(KEY_ESCAPE)) p->picking = -1;
}

/* --- the roles ------------------------------------------------------------------------------------------------- */

/* A role's row: its label, then (when the window has room) a line in the role's own font. */
static float role_row_height(const App *app, int r, bool previews)
{
    return previews ? 24 + app->fonts[r].line_height + SP_4 + 22 : 44;
}

static void draw_roles(App *app, Rectangle d, bool previews)
{
    FontsPanel *p = &app->fonts_panel;
    const SFont *ui = &app->fonts[FONT_UI];
    widget_text(ui, "Fonts", d.x + SP_6, d.y + SP_5, 0, THEME_TEXT);
    widget_text(ui, "A typeface and size for each part of Scribodo. Changes show at once; Done keeps them.",
                d.x + SP_6, d.y + SP_5 + ui->line_height + 2, d.width - 2 * SP_6, THEME_TEXT_MUTED);
    float y = d.y + 76;
    Appearance next = app->appearance;
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) {
        const AppearanceRoleInfo *in = appearance_role_info(r);
        const SFont *rf = &app->fonts[r];
        DrawRectangle((int)(d.x + SP_6), (int)y, (int)(d.width - 2 * SP_6), 1, THEME_BORDER);
        widget_text(ui, in->label, d.x + SP_6, y + SP_2, 0, THEME_TEXT);
        float lw = sfont_measure_utf8(ui, in->label, (int)strlen(in->label));
        widget_text(ui, in->hint, d.x + SP_6 + lw + SP_3, y + SP_2, 260, THEME_TEXT_DIM);

        /* The face, then the size. */
        float bx = d.x + d.width - SP_6 - 132 - SP_2 - 230;
        p->pick[r] = (Rectangle){ bx, y + SP_2, 230, 28 };
        char name[200], face[220];
        role_face_name(app, r, name, sizeof(name));
        snprintf(face, sizeof(face), "%s  \xE2\x96\xBE", name);
        if (widget_button(p->pick[r], face, ui, WIDGET_OUTLINE, true)) {
            scan(app);
            p->picking = r;
            p->filter[0] = '\0';
            refilter(p);
            p->sel = 0;
            long at = app->appearance.role[r].path[0]
                          ? font_catalog_find(&p->catalog, app->appearance.role[r].path, app->appearance.role[r].index) : -1;
            for (size_t i = 0; at >= 0 && i < p->nmatch; i++)
                if (p->matches[i] == (size_t)at) p->sel = (int)i + 1;
            p->top = p->sel > FONTS_PANEL_ROWS / 2 ? p->sel - FONTS_PANEL_ROWS / 2 : 0;
        }
        float sx = d.x + d.width - SP_6 - 132;
        p->minus[r] = (Rectangle){ sx, y + SP_2, 36, 28 };
        p->plus[r] = (Rectangle){ sx + 96, y + SP_2, 36, 28 };
        if (widget_button(p->minus[r], "\xE2\x88\x92", ui, WIDGET_OUTLINE, next.role[r].size > in->min_size))
            appearance_set_size(&next, r, next.role[r].size - 1.0f);
        char size[16];
        snprintf(size, sizeof(size), "%g", (double)next.role[r].size);
        float zw = sfont_measure_utf8(&app->fonts[FONT_MONO], size, (int)strlen(size));
        widget_text(&app->fonts[FONT_MONO], size, sx + 66 - zw / 2, y + SP_2 + (28 - app->fonts[FONT_MONO].line_height) / 2, 0, THEME_TEXT);
        if (widget_button(p->plus[r], "+", ui, WIDGET_OUTLINE, next.role[r].size < in->max_size))
            appearance_set_size(&next, r, next.role[r].size + 1.0f);

        /* A line in the role's own font, as it will look. */
        if (previews) {
            BeginScissorMode((int)(d.x + SP_6), (int)(y + 40), (int)(d.width - 2 * SP_6), (int)(rf->line_height + 4));
            widget_text(rf, k_samples[r], d.x + SP_6, y + 40, d.width - 2 * SP_6, THEME_TEXT_PROSE);
            EndScissorMode();
        }
        y += role_row_height(app, r, previews);
    }
    show(app, &next);

    float by = d.y + d.height - 50;
    p->defaults = (Rectangle){ d.x + SP_6, by, 100, 30 };
    p->cancel = (Rectangle){ d.x + d.width - SP_6 - 196, by, 92, 30 };
    p->done = (Rectangle){ d.x + d.width - SP_6 - 96, by, 96, 30 };
    if (widget_button(p->defaults, "Defaults", ui, WIDGET_QUIET, true)) {
        Appearance def;
        appearance_defaults(&def);
        show(app, &def);
    }
    if (widget_button(p->cancel, "Cancel", ui, WIDGET_OUTLINE, true) || (IsKeyPressed(KEY_ESCAPE) && !p->picked)) {
        close_panel(app, false);
        return;
    }
    if (widget_button(p->done, "Done", ui, WIDGET_PRIMARY, true) || (IsKeyPressed(KEY_ENTER) && !p->picked)) close_panel(app, true);
}

static float roles_height(const App *app, bool previews)
{
    float h = 76 + 70;
    for (int r = 0; r < APPEARANCE_ROLE_COUNT; r++) h += role_row_height(app, r, previews);
    return h;
}

void fonts_panel_draw(App *app, Rectangle screen)
{
    FontsPanel *p = &app->fonts_panel;
    if (!p->open) return;
    DrawRectangleRec(screen, THEME_SCRIM);
    float w = 720;
    /* A short window drops the preview lines rather than let rows run under the buttons. */
    bool previews = roles_height(app, true) <= screen.height - 40;
    float h = p->picking >= 0 ? 56 + 52 + 30 * FONTS_PANEL_ROWS + 80 : roles_height(app, previews);
    if (h > screen.height - 40) h = screen.height - 40;
    if (w > screen.width - 40) w = screen.width - 40;
    Rectangle d = { roundf((screen.width - w) / 2), roundf((screen.height - h) / 2), w, h };
    widget_shadow(d, RADIUS_M, 16);
    widget_round_rect(d, RADIUS_M, THEME_SURFACE);
    widget_round_outline(d, RADIUS_M, 1.0f, THEME_BORDER_STRONG);
    if (p->picking >= 0) draw_list(app, d);
    else draw_roles(app, d, previews);
}

Rectangle fonts_panel_element(const FontsPanel *p, const char *name)
{
    Rectangle none = { 0 };
    if (!p->open) return none;
    if (p->picking >= 0) {
        if (!strcmp(name, "FontsBack")) return p->back;
        if (!strncmp(name, "FontsItem", 9)) {
            int i = atoi(name + 9);
            return i >= 0 && i < p->nitem ? p->item[i] : none;
        }
        return none;
    }
    if (!strcmp(name, "FontsDone")) return p->done;
    if (!strcmp(name, "FontsCancel")) return p->cancel;
    if (!strcmp(name, "FontsDefaults")) return p->defaults;
    static const char *const kinds[] = { "FontsPick", "FontsMinus", "FontsPlus" };
    for (int k = 0; k < 3; k++) {
        size_t n = strlen(kinds[k]);
        if (strncmp(name, kinds[k], n) || !name[n]) continue;
        int r = atoi(name + n);
        if (r < 0 || r >= APPEARANCE_ROLE_COUNT) return none;
        return k == 0 ? p->pick[r] : k == 1 ? p->minus[r] : p->plus[r];
    }
    return none;
}
