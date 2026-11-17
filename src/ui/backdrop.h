/* Frosted glass behind a dialog: the window as it was when the dialog opened, blurred on the GPU and tinted with the
 * palette's scrim, instead of a flat dark veil.
 *
 * backdrop_draw takes one snapshot of the frame drawn so far (the panes, before the dialog), deferred by a frame when
 * that frame holds a menu or an appearance change (a lighter flat veil meanwhile), runs it through a two-pass Gaussian blur at half resolution, and draws the result over the window; later
 * frames reuse it until backdrop_release (the dialog closed) or a window resize. The panes behind a dialog take no
 * input, so a still picture of them loses nothing but a reply's streaming, which resumes on screen when the dialog
 * closes. Without a shader (a GL that refuses it), and with the translucent sidebar on (the frame is not opaque, and a
 * readback forces it so), there is no snapshot: a veil over the live panes. */
#ifndef SCRIBODO_BACKDROP_H
#define SCRIBODO_BACKDROP_H

#include "raylib.h"

/* Draws the frosted backdrop over `screen` (the window, in logical px). */
void backdrop_draw(Rectangle screen);
/* The dialog closed: the snapshot is dropped, so the next dialog sees the window as it is then. */
void backdrop_release(void);
/* The window behind changed (the Appearance panel): a new snapshot is taken on the next frame, the old one shown
 * until then. */
void backdrop_invalidate(void);
/* Once per frame, before anything is drawn. */
void backdrop_frame_begin(void);
/* This frame holds a menu (drawn before the dialog that a pick opens): no snapshot is taken in it. An appearance change
 * (backdrop_invalidate) blocks its frame the same way, since its text was laid out in the font being replaced. */
void backdrop_defer(void);

#endif
