/* Motion: how values glide instead of jumping (panes opening, scrolling, a finding arriving).
 *
 * Every animated value is a pair (shown, target) moved each frame by motion_approach, an exponential ease whose
 * `ms` is the time to cover about 95% of the distance. With motion off the shown value is the target at once:
 * UI test runs (scripted, so their checks see the end state on the next frame) and authors who set macOS's
 * Reduce motion (Accessibility ▸ Display), read once at launch. */
#ifndef SCRIBODO_MOTION_H
#define SCRIBODO_MOTION_H

#include <stdbool.h>

#define MOTION_FAST_MS 120.0f /* hover fills, a toggle */
#define MOTION_PANE_MS 180.0f /* a pane opening or closing */
#define MOTION_SCROLL_MS 140.0f
#define MOTION_FADE_MS 260.0f /* something new appearing */

void motion_set_enabled(bool on);
bool motion_enabled(void);
/* macOS's Reduce motion setting (com.apple.universalaccess reduceMotion); false when unset or unreadable. */
bool motion_system_reduced(void);

/* `cur` moved toward `target` over `dt` seconds; it lands exactly on the target once within half a unit (or 0.001
 * for values under 2), so an animation ends instead of creeping. Without motion: `target`. */
float motion_approach(float cur, float target, float dt, float ms);
/* True while `cur` has not reached `target`: the frame should be drawn again soon. */
bool motion_moving(float cur, float target);

/* 0..1 for something that appeared at `born` (GetTime seconds), eased; 1 without motion. */
float motion_fade_in(double born, double now, float ms);

#endif
