/* src/ui/motion.c: the ease reaches 95% of the way in its time, lands exactly, never overshoots, survives a stalled
 * frame, and is instant with motion off. */
#include <math.h>

#include "test_util.h"
#include "ui/motion.h"

static float run(float from, float to, float ms, float frame_s, float total_s)
{
    float v = from;
    for (float t = 0; t < total_s - 1e-6f; t += frame_s) v = motion_approach(v, to, frame_s, ms);
    return v;
}

static void test_reaches_95_percent_in_its_time(void)
{
    motion_set_enabled(true);
    float v = run(0, 1000, 180, 1.0f / 60, 0.180f);
    CHECK(v > 940 && v < 960); /* about 95% at 60 fps */
    CHECK(run(0, 1000, 180, 1.0f / 120, 0.180f) > 940); /* the same at 120 fps */
}

static void test_lands_exactly_and_stops(void)
{
    float v = run(0, 260, 180, 1.0f / 60, 1.0f);
    CHECK(v == 260.0f);
    CHECK(!motion_moving(v, 260.0f));
    CHECK(run(1, 0, 120, 1.0f / 60, 1.0f) == 0.0f); /* small values land too */
    CHECK(run(500, -300, 140, 1.0f / 60, 1.0f) == -300.0f);
}

static void test_never_overshoots(void)
{
    float v = 0;
    for (int i = 0; i < 200; i++) {
        float next = motion_approach(v, 100, 1.0f / 60, 120);
        CHECK(next >= v && next <= 100);
        v = next;
    }
    v = motion_approach(0, 100, 5.0f, 120); /* a five-second stall moves at most one 100 ms step */
    CHECK(v > 0 && v < 100);
}

static void test_bad_inputs(void)
{
    CHECK(motion_approach(NAN, 7, 0.016f, 120) == 7);
    CHECK(motion_approach(3, 7, -1, 120) == 7);
    CHECK(motion_approach(3, 7, NAN, 120) == 7);
    CHECK(motion_approach(3, 7, 0.016f, 0) == 7);
}

static void test_motion_off_is_instant(void)
{
    motion_set_enabled(false);
    CHECK(motion_approach(0, 500, 0.016f, 180) == 500);
    CHECK(motion_fade_in(10.0, 10.0, 260) == 1.0f);
    motion_set_enabled(true);
    CHECK(motion_fade_in(10.0, 10.0, 260) == 0.0f);
    float mid = motion_fade_in(10.0, 10.13, 260);
    CHECK(mid > 0.5f && mid < 1.0f); /* ease out: past half at half time */
    CHECK(motion_fade_in(10.0, 11.0, 260) == 1.0f);
    CHECK(motion_fade_in(10.0, 9.0, 260) == 0.0f);
}

int main(void)
{
    static const TestCase tests[] = {
        { "reaches_95_percent_in_its_time", test_reaches_95_percent_in_its_time },
        { "lands_exactly_and_stops", test_lands_exactly_and_stops },
        { "never_overshoots", test_never_overshoots },
        { "bad_inputs", test_bad_inputs },
        { "motion_off_is_instant", test_motion_off_is_instant },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
