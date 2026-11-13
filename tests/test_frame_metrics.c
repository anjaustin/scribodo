#include "app/frame_metrics.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    FrameMetrics m;
    frame_metrics_clear(&m);
    FrameMetricsReport r = frame_metrics_report(&m);
    assert(r.count == 0 && r.p99_ms == 0 && r.max_ms == 0);
    frame_metrics_record(&m, 0); /* zero is a valid clock origin */
    for (int i = 1; i <= 100; i++) frame_metrics_record(&m, i * .010);
    r = frame_metrics_report(&m);
    assert(r.count == 100 && fabs(r.sample_s - 1) < 1e-9);
    assert(r.over_budget == 0); /* 10 ms frames are under the 50 ms budget */
    assert(r.p50_ms >= 9.999 && r.p99_ms <= 10.16);
    assert(fabs(r.max_ms - 10) < 1e-9);
    frame_metrics_record(&m, 1); /* duplicate */
    frame_metrics_record(&m, .5); /* backwards, reanchor */
    frame_metrics_record(&m, NAN);
    frame_metrics_record(&m, INFINITY);
    frame_metrics_record(&m, -1);
    frame_metrics_record(&m, 20); /* anchor, no artificial long interval */
    assert(m.count == 100);
    frame_metrics_record(&m, 21);
    assert(m.count == 101 && m.max_ms == 1000);
    frame_metrics_clear(&m);
    double now = 0;
    frame_metrics_record(&m, now);
    for (int i = 1; i <= 100; i++) {
        now += (double)i / 1000;
        frame_metrics_record(&m, now);
    }
    r = frame_metrics_report(&m);
    assert(r.p50_ms >= 49.999 && r.p50_ms <= 50.5);
    assert(r.p95_ms >= 94.999 && r.p95_ms <= 96);
    assert(r.p99_ms >= 98.999 && r.p99_ms <= 100);
    assert(fabs(r.max_ms - 100) < 1e-8);
    /* Intervals of 1..100 ms: those above 50 ms (51..100) count; the accumulated
     * clock puts the 50 ms interval a rounding error either side of the line. */
    assert(r.over_budget == 50 || r.over_budget == 51);
    frame_metrics_clear(&m);
    frame_metrics_record(&m, 0);
    frame_metrics_record(&m, 0.050);
    frame_metrics_record(&m, 0.1001); /* 50.1 ms */
    r = frame_metrics_report(&m);
    assert(r.count == 2 && r.over_budget == 1);
    frame_metrics_clear(&m);
    frame_metrics_record(&m, 0);
    frame_metrics_record(&m, 1e-15);
    frame_metrics_record(&m, 1e20); /* histogram overflow bucket, exact max */
    r = frame_metrics_report(&m);
    assert(r.count == 2 && r.p99_ms == r.max_ms && isfinite(r.max_ms));
    /* Long runs consume exactly the same storage. */
    frame_metrics_clear(&m);
    for (int i = 0; i <= 1000000; i++) frame_metrics_record(&m, i * .016);
    assert(m.count == 1000000);
    puts("frame_metrics: passed");
    return 0;
}
