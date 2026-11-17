/* The window as a Mac app's: the toolbar runs to the top edge with the traffic lights inside it, as in Notes or
 * Xcode, instead of a grey title bar above it.
 *
 * Done through the Objective-C runtime on the NSWindow raylib (GLFW) creates, from plain C: the window gets a
 * full-size content view, a transparent, untitled title bar and an empty toolbar in the unified compact style, which
 * makes AppKit's title bar exactly as tall as the app's toolbar with the three buttons centred in it, through resizing
 * and full screen. With the title bar gone, the empty parts of the toolbar move the window and a double-click there
 * does what the author set in System Settings ▸ Desktop & Dock (zoom, minimise or nothing). */
#ifndef SCRIBODO_MAC_WINDOW_H
#define SCRIBODO_MAC_WINDOW_H

#include <stdbool.h>

/* Unifies the title bar with a toolbar `toolbar_h` points tall. False (and nothing changed) when the handle is not a
 * window that answers the calls used. */
bool mac_window_unify(void *nswindow, float toolbar_h);
/* Once a frame: hides the toolbar in full screen (AppKit would lift it into a window over the app's toolbar band) and
 * shows it again outside; and keeps the traffic lights centred where the title bar has another height (a macOS without
 * toolbar styles), moved vertically only and only when off by more than a point. */
void mac_window_place_buttons(void *nswindow, float toolbar_h);
/* Where the app's toolbar content may start, in points: right of the zoom button as AppKit placed it. 0 when not
 * unified or in full screen (the buttons are not in the window then). */
float mac_window_toolbar_inset(void *nswindow);
/* AppKit's click count for the last left press in this window (2 for a double-click, by the system's own interval
 * and distance); 0 when there is none, as in a UI script, whose presses are raylib's, not AppKit's. */
int mac_window_last_click_count(void *nswindow);
/* A press on an empty part of the toolbar: the system's own window drag (with its snapping and tiling). False when
 * the event in hand is neither that press nor a drag following it in the same frame, is another window's, or the
 * button is already up: the caller then does nothing. */
bool mac_window_begin_drag(void *nswindow);
/* A double-click on an empty part of the toolbar: zoom, minimise or nothing, as the author's system setting says. */
void mac_window_title_double_click(void *nswindow);

/* The translucent sidebar: macOS's own sidebar material (an NSVisualEffectView, blending with what is behind the
 * window) placed beneath the app's drawing. The window is created transparent (FLAG_WINDOW_TRANSPARENT) and clears to
 * transparent where the material should show. `dark` picks the material's dark or light look to match the palette.
 * The material is installed on the first `on` and hidden when off. Every call (on or off) also: makes the window opaque
 * unless the glass is on, with the palette's page colour (bg_*, sRGB 0..1) behind anything not yet drawn; keeps its
 * shadow; and stops GLFW's view from moving the window on a press in the title band (the app's toolbar drag decides).
 * Repeated calls with the same state do nothing. False when the window cannot take it. */
bool mac_window_vibrancy(void *nswindow, bool on, bool dark, float bg_r, float bg_g, float bg_b);

#endif
