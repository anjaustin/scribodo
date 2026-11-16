/* src/ui/widgets.c's press rule, with the mouse stubbed: a click needs the press and the release on the same control
 * (its rectangle and its label), a press left over from a control that vanished clicks nothing later, and a locked
 * or disabled control never clicks (red team of 8d5d6a4). */
#include <stdio.h>

#include "raylib.h"
#include "test_util.h"
#include "ui/font.h"
#include "ui/widgets.h"

static Vector2 g_m;
static bool g_pr, g_dn, g_rl;
Vector2 GetMousePosition(void) { return g_m; }
bool IsMouseButtonPressed(int b) { (void)b; return g_pr; }
bool IsMouseButtonDown(int b) { (void)b; return g_dn; }
bool IsMouseButtonReleased(int b) { (void)b; return g_rl; }
bool CheckCollisionPointRec(Vector2 p, Rectangle r) { return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height; }
void DrawRectangleRounded(Rectangle r, float a, int s, Color c) { (void)r, (void)a, (void)s, (void)c; }
void DrawRectangleRoundedLinesEx(Rectangle r, float a, int s, float t, Color c) { (void)r, (void)a, (void)s, (void)t, (void)c; }
void DrawLineEx(Vector2 a, Vector2 b, float t, Color c) { (void)a, (void)b, (void)t, (void)c; }
Color ColorBrightness(Color c, float f) { (void)f; return c; }
void DrawTexturePro(Texture2D t, Rectangle s, Rectangle d, Vector2 o, float r, Color c) { (void)t, (void)s, (void)d, (void)o, (void)r, (void)c; }
float sfont_measure_utf8(const SFont *f, const char *s, int n) { (void)f, (void)s; return (float)n * 7; }
void sfont_draw_utf8(const SFont *f, const char *s, int n, float x, float y, Color c) { (void)f, (void)s, (void)n, (void)x, (void)y, (void)c; }
size_t utf8_decode_one(const uint8_t *p, size_t n, uint32_t *cp) { (void)n; *cp = *p; return 1; }

/* One frame of mouse state, then the frame's start as the app runs it. */
static void frame(bool pressed, bool down, bool released)
{
    g_pr = pressed, g_dn = down, g_rl = released;
    widget_frame_begin();
}

static const Rectangle SEND = { 500, 700, 72, 24 };

static void test_an_ordinary_click(void)
{
    g_m = (Vector2){ 520, 710 };
    frame(true, true, false);
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true));
    frame(false, false, true);
    CHECK(widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true));
    frame(false, false, false);
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true)); /* once */
}

static void test_stop_does_not_become_send(void)
{
    g_m = (Vector2){ 520, 710 };
    frame(true, true, false);
    CHECK(!widget_button(SEND, "Stop", NULL, WIDGET_OUTLINE, true));
    frame(false, true, false); /* the reply ends: Send in the same place, the button still held */
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true));
    frame(false, false, true);
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true));
}

static void test_a_stale_press_clicks_nothing_later(void)
{
    Rectangle accept = { 100, 100, 60, 24 };
    g_m = (Vector2){ 120, 110 };
    frame(true, true, false);
    widget_button(accept, "Accept", NULL, WIDGET_PRIMARY, true);
    frame(false, false, true); /* released while Accept was not drawn */
    frame(false, false, false);
    g_m = (Vector2){ 400, 400 };
    frame(true, true, false); /* a press in the editor */
    g_m = (Vector2){ 120, 110 };
    frame(false, true, false);
    widget_button(accept, "Accept", NULL, WIDGET_PRIMARY, true);
    frame(false, false, true);
    CHECK(!widget_button(accept, "Accept", NULL, WIDGET_PRIMARY, true));
}

static void test_a_release_elsewhere_and_disabled_and_locked(void)
{
    g_m = (Vector2){ 520, 710 };
    frame(true, true, false);
    widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true);
    g_m = (Vector2){ 10, 10 };
    frame(false, false, true);
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true)); /* dragged off before release */
    g_m = (Vector2){ 520, 710 };
    frame(true, true, false);
    widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, false);
    frame(false, false, true);
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, false));
    widget_set_locked(true);
    frame(true, true, false);
    widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true);
    frame(false, false, true);
    CHECK(!widget_button(SEND, "Send", NULL, WIDGET_PRIMARY, true));
    widget_set_locked(false);
}

static void test_a_check_box_flips_once(void)
{
    Rectangle box = { 50, 50, 190, 24 };
    bool on = false;
    g_m = (Vector2){ 60, 60 };
    frame(true, true, false);
    CHECK(!widget_checkbox(box, "Observer reads this chat", NULL, &on));
    frame(false, false, true);
    CHECK(widget_checkbox(box, "Observer reads this chat", NULL, &on) && on);
    frame(false, false, false);
    CHECK(!widget_checkbox(box, "Observer reads this chat", NULL, &on) && on);
}

int main(void)
{
    static SFont font;
    font.line_height = 18;
    widget_set_font(&font);
    static const TestCase tests[] = {
        { "an_ordinary_click", test_an_ordinary_click },
        { "stop_does_not_become_send", test_stop_does_not_become_send },
        { "a_stale_press_clicks_nothing_later", test_a_stale_press_clicks_nothing_later },
        { "a_release_elsewhere_and_disabled_and_locked", test_a_release_elsewhere_and_disabled_and_locked },
        { "a_check_box_flips_once", test_a_check_box_flips_once },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
