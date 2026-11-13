#include "frame_metrics.h"

#include <math.h>
#include <string.h>

void frame_metrics_clear(FrameMetrics *m)
{
    memset(m, 0, sizeof(*m));
}

void frame_metrics_record(FrameMetrics *m, double now)
{
    if (!isfinite(now) || now < 0) { m->anchored = false; return; }
    if (!m->anchored) { m->previous_s = now; m->anchored = true; return; }
    double delta = now - m->previous_s;
    m->previous_s = now;
    double ms = delta * 1000.0;
    if (!(delta > 0) || !isfinite(ms) || m->count == UINT64_MAX) return;
    int exponent;
    double fraction = frexp(ms, &exponent);
    int bin = (exponent + 19) * 64 + (int)((fraction * 2.0 - 1.0) * 64.0);
    if (bin < 0) bin = 0;
    if (bin >= FRAME_METRICS_BINS) bin = FRAME_METRICS_BINS - 1;
    m->bins[bin]++;
    m->count++;
    if (ms > FRAME_METRICS_BUDGET_MS) m->over_budget++;
    m->total_s += delta;
    if (ms > m->max_ms) m->max_ms = ms;
}

static double quantile(const FrameMetrics *m, unsigned percent)
{
    if (!m->count) return 0;
    /* ceil(count * percent / 100), without overflowing the product. */
    uint64_t rank = (m->count / 100) * percent + ((m->count % 100) * percent + 99) / 100;
    uint64_t seen = 0;
    for (int i = 0; i < FRAME_METRICS_BINS; i++) {
        seen += m->bins[i];
        if (seen >= rank) {
            if (i == FRAME_METRICS_BINS - 1) return m->max_ms;
            double upper = ldexp(1.0 + (double)(i % 64 + 1) / 64.0, i / 64 - 20);
            return fmin(upper, m->max_ms);
        }
    }
    return m->max_ms;
}

FrameMetricsReport frame_metrics_report(const FrameMetrics *m)
{
    return (FrameMetricsReport){ .count = m->count, .over_budget = m->over_budget, .p50_ms = quantile(m, 50),
        .p95_ms = quantile(m, 95), .p99_ms = quantile(m, 99),
        .max_ms = m->max_ms, .sample_s = m->total_s };
}
