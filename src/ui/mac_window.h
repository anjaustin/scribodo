/* The window as a Mac app's: the toolbar runs to the top edge with the traffic lights inside it, as in Notes or
 * Xcode, instead of a grey title bar above it.
 *
 * Done through the Objective-C runtime on the NSWindow raylib (GLFW) creates, from plain C: the window gets a
 * full-size content view and a transparent, untitled title bar, so the app's own drawing fills it; the three buttons
 * are moved to sit centred in the app's toolbar (AppKit lays them out again on resize and full screen, so the app places
 * them every frame). With the title bar gone, the empty parts of the toolbar move the window and a double-click there
 * does what the author set in System Settings ▸ Desktop & Dock (zoom, minimise or nothing). */
#ifndef SCRIBODO_MAC_WINDOW_H
#define SCRIBODO_MAC_WINDOW_H

#include <stdbool.h>

/* Unifies the title bar with a toolbar `toolbar_h` points tall. False (and nothing changed) when the handle is not a
 * window that answers the calls used. */
bool mac_window_unify(void *nswindow, float toolbar_h);
/* Keeps the traffic lights centred in the toolbar (cheap: three frames read, moved only when AppKit reset them). */
void mac_window_place_buttons(void *nswindow, float toolbar_h);
/* Where the app's toolbar content may start, in points: right of the traffic lights. 0 when not unified. */
float mac_window_toolbar_inset(void);
/* A press on an empty part of the toolbar: the system's own window drag (with its snapping and tiling). False when
 * the event in hand is not that press, so the caller does nothing. */
bool mac_window_begin_drag(void *nswindow);
/* A double-click on an empty part of the toolbar: zoom, minimise or nothing, as the author's system setting says. */
void mac_window_title_double_click(void *nswindow);

#endif
