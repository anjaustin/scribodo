#ifndef SCRIBODO_FRAME_METRICS_H
#define SCRIBODO_FRAME_METRICS_H

#include <stdbool.h>
#include <stdint.h>

/* Allocation-free, bounded logarithmic histogram. Quantiles are bucket upper
 * bounds (at most 1/64 of a power-of-two interval above the sample), capped by
 * the exact maximum. Sub-microsecond intervals use the first bucket. */
#define FRAME_METRICS_BINS 4096
/* The proposed worst-frame budget (MEMORY-019): intervals above it are counted
 * exactly, so a long run reports how often the budget was crossed, not only
 * the single worst interval. */
#define FRAME_METRICS_BUDGET_MS 50.0
typedef struct {
    uint64_t bins[FRAME_METRICS_BINS];
    uint64_t count, over_budget;
    double previous_s, total_s, max_ms;
    bool anchored;
} FrameMetrics;

typedef struct {
    uint64_t count, over_budget; /* intervals above FRAME_METRICS_BUDGET_MS */
    double p50_ms, p95_ms, p99_ms, max_ms, sample_s;
} FrameMetricsReport;

void frame_metrics_clear(FrameMetrics *metrics);
/* Supply monotonic seconds once per frame. The first valid timestamp anchors;
 * zero-length intervals are ignored, backwards time reanchors, and invalid
 * timestamps break the chain. No allocation, platform clock or UI dependency. */
void frame_metrics_record(FrameMetrics *metrics, double monotonic_s);
FrameMetricsReport frame_metrics_report(const FrameMetrics *metrics);

#endif
