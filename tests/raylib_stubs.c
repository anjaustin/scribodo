/* Headless stand-ins for the raylib calls the editor makes, so the real
 * editor (and its stb_textedit integration) can be driven from tests. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "ui/font.h"

double g_stub_time = 0.0;
static char *g_clipboard = NULL;

double GetTime(void) { return g_stub_time; }
void SetClipboardText(const char *text)
{
    free(g_clipboard);
    g_clipboard = text ? strdup(text) : NULL;
}
const char *GetClipboardText(void) { return g_clipboard; }

Vector2 GetMousePosition(void) { return (Vector2){ -1000.0f, -1000.0f }; }
Vector2 GetMouseWheelMoveV(void) { return (Vector2){ 0 }; }
bool CheckCollisionPointRec(Vector2 p, Rectangle r)
{
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}
bool IsMouseButtonPressed(int b) { (void)b; return false; }
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
