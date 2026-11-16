/* Motion. See motion.h. */
#include "ui/motion.h"

#include <math.h>

static bool g_enabled = true;

void motion_set_enabled(bool on) { g_enabled = on; }
bool motion_enabled(void) { return g_enabled; }

static float snap_distance(float target) { return fabsf(target) < 2.0f ? 0.001f : 0.5f; }

float motion_approach(float cur, float target, float dt, float ms)
{
    if (!g_enabled || ms <= 0.0f || !isfinite(cur) || !(dt >= 0.0f)) return target;
    /* 95% of the way in `ms`: e^(-k * ms/1000) = 0.05, k = 3 / (ms/1000). */
    float k = 3.0f / (ms / 1000.0f);
    float t = 1.0f - expf(-k * (dt > 0.1f ? 0.1f : dt)); /* a stalled frame does not overshoot or teleport */
    float next = cur + (target - cur) * t;
    return fabsf(target - next) < snap_distance(target) ? target : next;
}

bool motion_moving(float cur, float target) { return cur != target; }

float motion_fade_in(double born, double now, float ms)
{
    if (!g_enabled || ms <= 0.0f) return 1.0f;
    float t = (float)((now - born) * 1000.0 / ms);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); /* ease out (cubic) */
}
