/* Frosted glass behind a dialog: the window as it was when the dialog opened, blurred on the GPU and tinted with the
 * palette's scrim, instead of a flat dark veil.
 *
 * backdrop_draw takes one snapshot of the frame drawn so far (the panes, before the dialog) the first time it is
 * called, runs it through a two-pass Gaussian blur at half resolution, and draws the result over the window; later
 * frames reuse it until backdrop_release (the dialog closed) or a window resize. The panes behind a dialog take no
 * input, so a still picture of them loses nothing but a reply's streaming, which resumes on screen when the dialog
 * closes. Without a shader (a GL that refuses it) it falls back to the flat scrim. */
#ifndef SCRIBODO_BACKDROP_H
#define SCRIBODO_BACKDROP_H

#include "raylib.h"

/* Draws the frosted backdrop over `screen` (the window, in logical px). */
void backdrop_draw(Rectangle screen);
/* The dialog closed: the snapshot is dropped, so the next dialog sees the window as it is then. */
void backdrop_release(void);

#endif
