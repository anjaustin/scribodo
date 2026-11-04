/* A context menu: a short list of items at the mouse. A click on an item
 * picks it; a click elsewhere or Esc closes the menu. */
#ifndef SCRIBODO_MENU_VIEW_H
#define SCRIBODO_MENU_VIEW_H

#include <stdbool.h>

#include "raylib.h"
#include "ui/font.h"

#define MENU_MAX_ITEMS 8

typedef struct {
    bool active;
    int tag;         /* what it is for (the app's code) */
    int target;      /* e.g. the index of the item right-clicked */
    Vector2 at;
    int n;
    char items[MENU_MAX_ITEMS][48];
    Rectangle rects[MENU_MAX_ITEMS];
    bool fresh;      /* opened this frame: ignore the opening click */
} MenuView;

void menu_open(MenuView *m, int tag, int target, Vector2 at, const char *const *items, int n);
/* Returns the item picked this frame (0..n-1), or -1. Draws the menu. */
int menu_draw(MenuView *m, const SFont *font, Rectangle screen);
Rectangle menu_element(const MenuView *m, const char *name); /* "MenuItem0".. */

#endif
