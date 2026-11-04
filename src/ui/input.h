/* Input seams: shortcut matching, typed characters, and the clipboard.
 *
 * Shortcuts: GLFW (and therefore raylib) key codes name *physical* keys by
 * their US QWERTY position. Matching shortcuts on them puts ⌘Z on the wrong
 * key for QWERTZ/AZERTY/Dvorak users (QWERTZ even swaps undo and redo), so
 * we ask GLFW which letter each key types in the current layout.
 *
 * Characters and clipboard go through here so the UI test runner can inject
 * typed text (raylib's event playback cannot) and use a private clipboard
 * instead of the user's.
 */
#ifndef SCRIBODO_INPUT_H
#define SCRIBODO_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if the key that types `letter` ('a'..'z') was pressed (or
 * auto-repeated) this frame. */
bool shortcut_pressed(char letter);

/* Next typed codepoint this frame (injected first, then real), or 0. */
int input_get_char(void);

/* --- test hooks ------------------------------------------------------- */

/* Queue codepoints to be returned by input_get_char() this frame. */
void input_inject_chars(const uint32_t *cps, size_t n);

/* Scripted mode: match shortcuts by physical (US) key position so scripts
 * behave the same on every keyboard layout, and keep the clipboard private. */
void input_set_scripted(bool scripted);

const char *input_get_clipboard(void);
void input_set_clipboard(const char *utf8);

#endif
