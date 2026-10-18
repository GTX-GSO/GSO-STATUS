#ifndef GSO_MODEL_H
#define GSO_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GSO_SNAPSHOT_TARGET_BYTES 16384u
#define GSO_SNAPSHOT_MAX_BYTES 32768u
#define GSO_MAX_DISKS 3u
#define GSO_MAX_SESSIONS 4u
#define GSO_GAME_HISTORY_POINTS 24u

typedef enum {
    GSO_OK, GSO_STALE, GSO_OFFLINE, GSO_UNSUPPORTED,
    GSO_UNCONFIGURED, GSO_ERROR
} gso_quality;

/* valid=false is absence, never a fabricated zero. reason is a static code. */
typedef struct { bool valid; double value; const char *reason; } gso_metric;
typedef struct {
    gso_quality quality;
    bool received;
    uint64_t received_monotonic_ms;
    uint64_t age_at_receive_ms;
} gso_freshness;

gso_metric gso_metric_missing(const char *reason);
gso_metric gso_metric_number(double value);
bool gso_metric_percent_valid(gso_metric metric);
uint64_t gso_source_age(gso_freshness source, uint64_t now_ms);
gso_quality gso_source_quality(gso_freshness source, uint64_t now_ms,
                               uint64_t stale_ms, uint64_t offline_ms);
bool gso_snapshot_size_allowed(size_t length);
#endif
