/* Drawn controls. See widgets.h. */
#include "ui/widgets.h"

#include <math.h>
#include <string.h>

#include "core/utf8.h"
#include "ui/theme.h"

/* The button the current press began on (its rectangle), so a release elsewhere clicks nothing. */
static Rectangle g_pressed;
static bool g_has_pressed;

static bool same_rect(Rectangle a, Rectangle b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }

void widget_round_rect(Rectangle r, float radius, Color fill)
{
    float shortest = r.width < r.height ? r.width : r.height;
    if (shortest <= 0) return;
    float rf = radius * 2.0f / shortest;
    DrawRectangleRounded(r, rf > 1.0f ? 1.0f : rf, 16, fill);
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
static bool press_rule(Rectangle r, bool enabled, bool *held)
{
    bool inside = CheckCollisionPointRec(GetMousePosition(), r);
    bool clicked = false;
    if (enabled && inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_pressed = r;
        g_has_pressed = true;
    }
    if (held) *held = enabled && g_has_pressed && same_rect(g_pressed, r) && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && g_has_pressed && same_rect(g_pressed, r)) {
        clicked = enabled && inside;
        g_has_pressed = false;
    }
    return clicked;
}

bool widget_clicked(Rectangle r) { return press_rule(r, true, NULL); }

bool widget_button(Rectangle r, const char *label, const SFont *f, WidgetKind kind, bool enabled)
{
    bool inside = CheckCollisionPointRec(GetMousePosition(), r);
    bool held = false;
    bool clicked = press_rule(r, enabled, &held);

    Color fill = BLANK, text = enabled ? THEME_TEXT : THEME_TEXT_DIM;
    switch (kind) {
    case WIDGET_PRIMARY:
        fill = enabled ? THEME_ACCENT : THEME_CARD;
        if (enabled && (inside || held)) fill = ColorBrightness(THEME_ACCENT, held ? -0.15f : 0.12f);
        if (enabled) text = THEME_ON_ACCENT;
        break;
    case WIDGET_OUTLINE:
        fill = enabled && held ? THEME_ACTIVE : enabled && inside ? THEME_HOVER : BLANK;
        break;
    case WIDGET_QUIET:
    default:
        fill = enabled && held ? THEME_ACTIVE : enabled && inside ? THEME_HOVER : BLANK;
        break;
    }
    if (fill.a) widget_round_rect(r, RADIUS_S, fill);
    if (kind == WIDGET_OUTLINE) widget_round_outline(r, RADIUS_S, 1.0f, enabled ? THEME_BORDER_STRONG : THEME_BORDER);
    float w = sfont_measure_utf8(f, label, (int)strlen(label));
    float x = r.x + (r.width - w) * 0.5f;
    if (x < r.x + SP_2) x = r.x + SP_2;
    widget_text(f, label, roundf(x), roundf(r.y + (r.height - f->line_height) * 0.5f), r.width - SP_4, text);
    return clicked;
}
