/* Drawn controls. See widgets.h. */
#include "ui/widgets.h"

#include <math.h>
#include <string.h>

#include "core/utf8.h"
#include "ui/theme.h"

/* The control the current press began on: its rectangle and its label, so a release elsewhere, or on another control
 * drawn in the same place (Stop becoming Send when a reply ends mid-press; Back and Done), clicks nothing. */
static Rectangle g_pressed;
static uint64_t g_pressed_label;
static bool g_has_pressed;
static const SFont *g_font;
static bool g_locked;

void widget_set_font(const SFont *f) { g_font = f; }

void widget_frame_begin(void)
{
    /* A press is claimed by the control under it this frame, or by none: one left over from a control that was not
     * drawn on its release frame must not click whatever appears there later. */
    bool pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool idle = !IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    if (pressed || idle) g_has_pressed = false;
}

static uint64_t label_key(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}
void widget_set_locked(bool locked) { g_locked = locked; }
bool widget_locked(void) { return g_locked; }

static bool same_rect(Rectangle a, Rectangle b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }

void widget_round_rect(Rectangle r, float radius, Color fill)
{
    float shortest = r.width < r.height ? r.width : r.height;
    if (shortest <= 0) return;
    float rf = radius * 2.0f / shortest;
    DrawRectangleRounded(r, rf > 1.0f ? 1.0f : rf, 16, fill);
}

void widget_shadow(Rectangle r, float radius, float depth)
{
    Color c = THEME_SHADOW;
    int layers = depth > 4 ? (int)(depth / 2) : 2;
    for (int k = layers; k >= 1; k--) {
        float spread = (float)k * depth / (float)layers;
        float fall = spread * 0.35f;
        Color layer = c;
        float w = 1.0f - (float)k / (float)(layers + 1);
        layer.a = (unsigned char)((float)c.a * w * w * 2.2f / (float)layers);
        widget_round_rect((Rectangle){ r.x - spread, r.y - spread + fall, r.width + 2 * spread, r.height + 2 * spread },
                          radius + spread, layer);
    }
}

void widget_round_outline(Rectangle r, float radius, float thick, Color line)
{
    float shortest = r.width < r.height ? r.width : r.height;
    if (shortest <= 0) return;
    float rf = radius * 2.0f / shortest;
    DrawRectangleRoundedLinesEx(r, rf > 1.0f ? 1.0f : rf, 16, thick, line);
}

float widget_text(const SFont *f, const char *s, float x, float y, float max_w, Color c)
{
    int len = (int)strlen(s);
    float w = sfont_measure_utf8(f, s, len);
    if (max_w <= 0 || w <= max_w) {
        sfont_draw_utf8(f, s, len, x, y, c);
        return w;
    }
    static const char ell[] = "\xE2\x80\xA6";
    float ew = sfont_measure_utf8(f, ell, 3);
    /* The longest prefix of whole characters that leaves room for the ellipsis. */
    int cut = 0;
    float at = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (cut < len) {
        uint32_t cp;
        int n = (int)utf8_decode_one(p + cut, (size_t)(len - cut), &cp);
        float a = sfont_advance(f, cp);
        if (at + a + ew > max_w) break;
        at += a;
        cut += n;
    }
    sfont_draw_utf8(f, s, cut, x, y, c);
    sfont_draw_utf8(f, ell, 3, x + at, y, c);
    return at + ew;
}

void widget_text_in(const SFont *f, const char *s, Rectangle r, Color c)
{
    widget_text(f, s, r.x, r.y + (r.height - f->line_height) * 0.5f, r.width, c);
}

/* The press/release rule shared by every control; `held` reports a press in progress on `r`. */
static bool press_rule(Rectangle r, const char *label, bool enabled, bool *held)
{
    enabled = enabled && !g_locked;
    bool inside = CheckCollisionPointRec(GetMousePosition(), r);
    bool clicked = false;
    uint64_t key = label_key(label);
    if (enabled && inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_pressed = r;
        g_pressed_label = key;
        g_has_pressed = true;
    }
    bool mine = g_has_pressed && same_rect(g_pressed, r) && g_pressed_label == key;
    if (held) *held = enabled && mine && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && mine) {
        clicked = enabled && inside;
        g_has_pressed = false;
    }
    return clicked;
}

bool widget_clicked(Rectangle r) { return press_rule(r, "", true, NULL); }

static bool draw_button(Rectangle r, const char *label, const SFont *f, WidgetKind kind, bool enabled, bool on,
                        const WidgetIcon *icon)
{
    if (!f) f = g_font;
    if (!f || r.width <= 0 || r.height <= 0) return false; /* not laid out this frame (focus mode, a closed pane) */
    bool inside = CheckCollisionPointRec(GetMousePosition(), r) && !g_locked;
    bool held = false;
    bool clicked = press_rule(r, label, enabled, &held);

    Color fill = BLANK, text = enabled ? THEME_TEXT : THEME_TEXT_DIM;
    switch (kind) {
    case WIDGET_PRIMARY:
        fill = enabled ? THEME_ACCENT : THEME_CARD;
        /* Darker under the pointer, never lighter: the text on it keeps its contrast in every palette. */
        if (enabled && (inside || held)) fill = ColorBrightness(THEME_ACCENT, held ? -0.18f : -0.08f);
        if (enabled) text = THEME_ON_ACCENT;
        break;
    case WIDGET_OUTLINE:
        fill = enabled && held ? THEME_ACTIVE : enabled && inside ? THEME_HOVER : BLANK;
        break;
    case WIDGET_QUIET:
    default:
        fill = enabled && held ? THEME_ACTIVE : on ? (enabled && inside ? THEME_CONTROL_HOVER : THEME_ACTIVE)
                                               : enabled && inside ? THEME_HOVER : BLANK;
        if (on && enabled) text = THEME_TEXT_STRONG;
        break;
    }
    if (fill.a) widget_round_rect(r, RADIUS_S, fill);
    if (kind == WIDGET_OUTLINE) widget_round_outline(r, RADIUS_S, 1.0f, enabled ? THEME_BORDER_STRONG : THEME_BORDER);
    float w = sfont_measure_utf8(f, label, (int)strlen(label));
    bool has_icon = icon && icon->texture && icon->texture->id && icon->size.x > 0;
    float iw = has_icon ? icon->size.x + (label[0] ? 6.0f : 0.0f) : 0.0f;
    float x = r.x + (r.width - w - iw) * 0.5f;
    if (x < r.x + SP_2) x = r.x + SP_2;
    if (has_icon) { /* the symbol, then its label, centred together; the icon in the label's colour */
        const Texture2D *t = icon->texture;
        DrawTexturePro(*t, (Rectangle){ 0, 0, (float)t->width, (float)t->height },
                       (Rectangle){ roundf(x), roundf(r.y + (r.height - icon->size.y) * 0.5f), icon->size.x, icon->size.y },
                       (Vector2){ 0, 0 }, 0, text);
        x += iw;
    }
    widget_text(f, label, roundf(x), roundf(r.y + (r.height - f->line_height) * 0.5f), r.x + r.width - SP_2 - x, text);
    return clicked;
}

bool widget_icon_button(Rectangle r, const WidgetIcon *icon, const char *label, const SFont *f, WidgetKind kind,
                        bool enabled, bool on)
{
    return draw_button(r, label, f, kind, enabled, on, icon);
}

bool widget_button(Rectangle r, const char *label, const SFont *f, WidgetKind kind, bool enabled)
{
    return draw_button(r, label, f, kind, enabled, false, NULL);
}

bool widget_toggle(Rectangle r, const char *label, const SFont *f, bool on)
{
    return draw_button(r, label, f, WIDGET_QUIET, true, on, NULL);
}

bool widget_checkbox(Rectangle r, const char *label, const SFont *f, bool *value)
{
    if (!f) f = g_font;
    if (!f || r.width <= 0 || r.height <= 0) return false;
    bool clicked = press_rule(r, label, true, NULL);
    if (clicked) *value = !*value;
    float s = 14;
    Rectangle box = { r.x, roundf(r.y + (r.height - s) * 0.5f), s, s };
    bool hover = CheckCollisionPointRec(GetMousePosition(), r) && !g_locked;
    if (*value) {
        widget_round_rect(box, 3, THEME_ACCENT);
        /* A check drawn as two strokes, so it does not depend on the font having one. */
        Vector2 a = { box.x + 3.5f, box.y + 7.5f }, b = { box.x + 6.0f, box.y + 10.0f }, c = { box.x + 10.5f, box.y + 4.0f };
        DrawLineEx(a, b, 1.8f, THEME_ON_ACCENT);
        DrawLineEx(b, c, 1.8f, THEME_ON_ACCENT);
    } else {
        widget_round_outline(box, 3, 1.0f, hover ? THEME_TEXT_MUTED : THEME_BORDER_STRONG);
    }
    widget_text(f, label, box.x + s + SP_2, roundf(r.y + (r.height - f->line_height) * 0.5f), r.width - s - SP_2,
                hover ? THEME_TEXT : THEME_TEXT_MUTED);
    return clicked;
}
