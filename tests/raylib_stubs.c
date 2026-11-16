/* Headless stand-ins for the raylib calls the editor makes, so the real
 * editor (and its stb_textedit integration) can be driven from tests. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "ui/font.h"

double g_stub_time = 0.0;
Vector2 g_stub_mouse = { -1000.0f, -1000.0f };
float g_stub_wheel;
bool g_stub_mouse_pressed;
static char *g_clipboard = NULL;

double GetTime(void) { return g_stub_time; }
float GetFrameTime(void) { return 1.0f / 60.0f; }
void SetClipboardText(const char *text)
{
    free(g_clipboard);
    g_clipboard = text ? strdup(text) : NULL;
}
const char *GetClipboardText(void) { return g_clipboard; }

Vector2 GetMousePosition(void) { return g_stub_mouse; }
Vector2 GetMouseWheelMoveV(void) { return (Vector2){ 0, g_stub_wheel }; }
bool CheckCollisionPointRec(Vector2 p, Rectangle r)
{
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}
bool IsMouseButtonPressed(int b) { return b == MOUSE_BUTTON_LEFT && g_stub_mouse_pressed; }
bool IsMouseButtonDown(int b) { (void)b; return false; }
bool IsKeyDown(int k) { (void)k; return false; }
bool IsKeyPressed(int k) { (void)k; return false; }
bool IsKeyPressedRepeat(int k) { (void)k; return false; }
int GetCharPressed(void) { return 0; }

void BeginScissorMode(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void EndScissorMode(void) {}
void DrawRectangleRec(Rectangle r, Color c) { (void)r; (void)c; }
void DrawRectangleRounded(Rectangle r, float rd, int s, Color c) { (void)r; (void)rd; (void)s; (void)c; }
void sfont_draw(const SFont *f, uint32_t cp, float x, float y, Color c) { (void)f; (void)cp; (void)x; (void)y; (void)c; }
const char *glfwGetKeyName(int key, int scancode) { (void)key; (void)scancode; return 0; }
void sfont_draw_utf8(const SFont *f, const char *s, int len, float x, float y, Color c)
{
    (void)f; (void)s; (void)len; (void)x; (void)y; (void)c;
}
/* Used by the review panel (test_editor: review_warnings). */
void DrawRectangleLinesEx(Rectangle r, float t, Color c) { (void)r; (void)t; (void)c; }
float sfont_measure_utf8(const SFont *f, const char *s, int len) { (void)f; (void)s; return (float)len * 8.0f; }

/* ui/widgets.c, drawn by the review panel (the theme's shadow and outlines, 2026-11-16). */
Color ColorBrightness(Color c, float f) { (void)f; return c; }
void DrawLineEx(Vector2 a, Vector2 b, float t, Color c) { (void)a; (void)b; (void)t; (void)c; }
void DrawRectangleRoundedLinesEx(Rectangle r, float rf, int s, float t, Color c) { (void)r; (void)rf; (void)s; (void)t; (void)c; }
bool IsMouseButtonReleased(int b) { (void)b; return false; }
Color ColorAlpha(Color c, float a) { c.a = (unsigned char)(a * 255.0f); return c; }
void DrawTexturePro(Texture2D t, Rectangle s, Rectangle d, Vector2 o, float r, Color c) { (void)t; (void)s; (void)d; (void)o; (void)r; (void)c; }
