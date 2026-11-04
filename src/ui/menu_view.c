#include "menu_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void menu_open(MenuView *m, int tag, int target, Vector2 at, const char *const *items, int n)
{
    memset(m, 0, sizeof(*m));
    m->active = true;
    m->tag = tag;
    m->target = target;
    m->at = at;
    m->n = n > MENU_MAX_ITEMS ? MENU_MAX_ITEMS : n;
    for (int i = 0; i < m->n; i++) snprintf(m->items[i], sizeof(m->items[i]), "%s", items[i]);
    m->fresh = true;
}

int menu_draw(MenuView *m, const SFont *font, Rectangle screen)
{
    if (!m->active) return -1;
    float w = 160, ih = font->line_height + 10;
    for (int i = 0; i < m->n; i++) {
        float tw = sfont_measure_utf8(font, m->items[i], (int)strlen(m->items[i])) + 28;
        if (tw > w) w = tw;
    }
    float x = m->at.x, y = m->at.y, h = ih * (float)m->n + 8;
    if (x + w > screen.x + screen.width) x = screen.x + screen.width - w;
    if (y + h > screen.y + screen.height) y = screen.y + screen.height - h;
    Rectangle box = { x, y, w, h };
    DrawRectangleRounded(box, 0.08f, 6, (Color){ 0x2A, 0x2C, 0x30, 0xFF });
    DrawRectangleRoundedLinesEx(box, 0.08f, 6, 1.0f, (Color){ 0x45, 0x48, 0x4E, 0xFF });
    Vector2 mouse = GetMousePosition();
    int picked = -1;
    for (int i = 0; i < m->n; i++) {
        m->rects[i] = (Rectangle){ x + 4, y + 4 + ih * (float)i, w - 8, ih };
        bool hot = CheckCollisionPointRec(mouse, m->rects[i]);
        if (hot) DrawRectangleRounded(m->rects[i], 0.2f, 4, (Color){ 0x0E, 0x63, 0x9C, 0xFF });
        sfont_draw_utf8(font, m->items[i], (int)strlen(m->items[i]), m->rects[i].x + 10, m->rects[i].y + 5,
                        (Color){ 0xE0, 0xE0, 0xE0, 0xFF });
        if (hot && !m->fresh && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) picked = i;
    }
    bool outside_click = !m->fresh && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) &&
                         !CheckCollisionPointRec(mouse, box);
    if (picked >= 0 || outside_click || IsKeyPressed(KEY_ESCAPE)) m->active = false;
    m->fresh = false;
    return picked;
}

Rectangle menu_element(const MenuView *m, const char *name)
{
    if (!m->active || strncmp(name, "MenuItem", 8)) return (Rectangle){ 0 };
    int i = atoi(name + 8);
    return i >= 0 && i < m->n ? m->rects[i] : (Rectangle){ 0 };
}
