#include "gso_model.h"
#include <math.h>
#include <stdint.h>

gso_metric gso_metric_missing(const char *reason) {
    return (gso_metric){false, 0, reason && reason[0] ? reason : "unavailable"};
}
gso_metric gso_metric_number(double value) {
    if (!isfinite(value)) return gso_metric_missing("invalid_number");
    return (gso_metric){true, value, NULL};
}
bool gso_metric_percent_valid(gso_metric metric) {
    return metric.valid && isfinite(metric.value) && metric.value >= 0 && metric.value <= 100;
}
uint64_t gso_source_age(gso_freshness source, uint64_t now_ms) {
    if (!source.received) return UINT64_MAX;
    uint64_t elapsed = now_ms >= source.received_monotonic_ms ? now_ms - source.received_monotonic_ms : 0;
    if (UINT64_MAX - source.age_at_receive_ms < elapsed) return UINT64_MAX;
    return source.age_at_receive_ms + elapsed;
}
gso_quality gso_source_quality(gso_freshness source, uint64_t now_ms,
                               uint64_t stale_ms, uint64_t offline_ms) {
    if (source.quality != GSO_OK && source.quality != GSO_STALE) return source.quality;
    if (!source.received) return GSO_UNCONFIGURED;
    if (offline_ms < stale_ms) return GSO_ERROR;
    uint64_t age = gso_source_age(source, now_ms);
    if (age > offline_ms) return GSO_OFFLINE;
    if (age > stale_ms) return GSO_STALE;
    return source.quality; /* A stale upstream cannot become OK just by receiving it. */
}
bool gso_snapshot_size_allowed(size_t length) {
    return length > 0 && length <= GSO_SNAPSHOT_MAX_BYTES;
}
