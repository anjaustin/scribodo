#include "prompt_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raygui.h"
#include "ui/editor.h"

#define W 440.0f
#define H 150.0f

struct PromptView {
    SFont *text, *label;
    Editor *field;
    bool active;
    int tag;
    char title[128], ok[32], error[200];
    Rectangle box, field_r, ok_r, cancel_r;
};

PromptView *prompt_create(SFont *text_font, SFont *label_font)
{
    PromptView *v = calloc(1, sizeof(PromptView));
    if (!v) return NULL;
    v->text = text_font;
    v->label = label_font;
    v->field = editor_create(text_font);
    if (!v->field) {
        free(v);
        return NULL;
    }
    editor_set_scratch(v->field, "");
    editor_set_layout(v->field, 8.0f, 6.0f, 4000.0f);
    editor_set_submit_on_enter(v->field, true);
    editor_set_single_line(v->field, true);
    editor_set_modal_owner(v->field, true);
    editor_set_max_chars(v->field, 200);
    return v;
}

void prompt_destroy(PromptView *v)
{
    if (!v) return;
    editor_destroy(v->field);
    free(v);
}

void prompt_open(PromptView *v, int tag, const char *title, const char *initial, const char *ok_label)
{
    v->active = true;
    v->tag = tag;
    snprintf(v->title, sizeof(v->title), "%s", title);
    snprintf(v->ok, sizeof(v->ok), "%s", ok_label ? ok_label : "OK");
    v->error[0] = '\0';
    editor_set_text_utf8(v->field, initial ? initial : "");
    /* Select the name, not its extension. */
    size_t n = editor_length(v->field), stem = n;
    const char *dot = initial ? strrchr(initial, '.') : NULL;
    if (dot && dot != initial) {
        size_t cps = 0;
        for (const char *p = initial; p < dot; p++) cps += ((unsigned char)*p & 0xC0) != 0x80;
        stem = cps;
    }
    editor_set_selection(v->field, 0, stem);
    editor_take_submit(v->field);
}

void prompt_close(PromptView *v) { v->active = false; }
bool prompt_active(const PromptView *v) { return v->active; }
int prompt_tag(const PromptView *v) { return v->tag; }
void prompt_set_error(PromptView *v, const char *msg) { snprintf(v->error, sizeof(v->error), "%s", msg ? msg : ""); }

char *prompt_text(const PromptView *v)
{
    char *t = editor_text_utf8(v->field);
    if (!t) return NULL;
    /* One line: keep only what precedes a pasted line break, trimmed. */
    t[strcspn(t, "\n")] = '\0';
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) t[--n] = '\0';
    size_t lead = strspn(t, " \t");
    if (lead) memmove(t, t + lead, strlen(t + lead) + 1);
    return t;
}

static void layout(PromptView *v, Rectangle s)
{
    v->box = (Rectangle){ s.x + (s.width - W) / 2, s.y + s.height * 0.28f, W, H };
    v->field_r = (Rectangle){ v->box.x + 16, v->box.y + 42, W - 32, v->text->line_height + 12 };
    v->ok_r = (Rectangle){ v->box.x + W - 16 - 90, v->box.y + H - 16 - 26, 90, 26 };
    v->cancel_r = (Rectangle){ v->ok_r.x - 8 - 90, v->ok_r.y, 90, 26 };
}

PromptResult prompt_update(PromptView *v, Rectangle screen)
{
    if (!v->active) return PR_NONE;
    layout(v, screen);
    editor_update(v->field, v->field_r, true);
    if (IsKeyPressed(KEY_ESCAPE)) return PR_CANCEL;
    if (editor_take_submit(v->field)) return PR_OK;
    return PR_NONE;
}

PromptResult prompt_draw(PromptView *v, Rectangle screen)
{
    if (!v->active) return PR_NONE;
    layout(v, screen);
    DrawRectangleRec(screen, (Color){ 0, 0, 0, 0x80 }); /* dim everything behind */
    DrawRectangleRounded(v->box, 0.05f, 6, (Color){ 0x25, 0x27, 0x2B, 0xFF });
    DrawRectangleRoundedLinesEx(v->box, 0.05f, 6, 1.0f, (Color){ 0x3D, 0x6F, 0xA8, 0xFF });
    sfont_draw_utf8(v->label, v->title, (int)strlen(v->title), v->box.x + 16, v->box.y + 14, (Color){ 0xDD, 0xDD, 0xDD, 0xFF });
    DrawRectangleRec(v->field_r, (Color){ 0x1B, 0x1C, 0x1F, 0xFF });
    DrawRectangleLinesEx(v->field_r, 1.0f, (Color){ 0x3D, 0x6F, 0xA8, 0xFF });
    editor_draw(v->field, v->field_r, true);
    if (v->error[0])
        sfont_draw_utf8(v->label, v->error, (int)strlen(v->error), v->box.x + 16, v->field_r.y + v->field_r.height + 8,
                        (Color){ 0xF2, 0x8B, 0x82, 0xFF });
    PromptResult r = PR_NONE;
    if (GuiButton(v->cancel_r, "Cancel")) r = PR_CANCEL;
    if (GuiButton(v->ok_r, v->ok)) r = PR_OK;
    return r;
}

Rectangle prompt_element(const PromptView *v, const char *name)
{
    if (!v->active) return (Rectangle){ 0 };
    if (!strcmp(name, "PromptField")) return v->field_r;
    if (!strcmp(name, "PromptOK")) return v->ok_r;
    if (!strcmp(name, "PromptCancel")) return v->cancel_r;
    return (Rectangle){ 0 };
}
