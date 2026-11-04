/* Layout-aware shortcut detection.
 *
 * GLFW (and therefore raylib) key codes name *physical* keys by their US
 * QWERTY position. Matching shortcuts on them puts ⌘Z on the wrong key for
 * QWERTZ/AZERTY/Dvorak users (QWERTZ even swaps undo and redo). Instead we
 * ask GLFW what letter each physical key produces in the current layout.
 */
#ifndef SCRIBODO_KEYS_H
#define SCRIBODO_KEYS_H

#include <stdbool.h>

/* True if the key that types `letter` ('a'..'z') in the active keyboard
 * layout was pressed (or auto-repeated) this frame. */
bool shortcut_pressed(char letter);

#endif
