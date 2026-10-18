#include "gso_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    assert(!gso_metric_missing(NULL).valid);
    assert(gso_metric_percent_valid(gso_metric_number(0)));
    assert(!gso_metric_percent_valid(gso_metric_number(101)));
    assert(!gso_metric_number(NAN).valid);
    gso_freshness s = {GSO_OK, true, 1000, 2000};
    assert(gso_source_quality(s, 2000, 3000, 10000) == GSO_OK);
    assert(gso_source_quality(s, 2001, 3000, 10000) == GSO_STALE);
    assert(gso_source_quality(s, 9001, 3000, 10000) == GSO_OFFLINE);
    assert(gso_source_age(s, 999) == 2000);
    s.quality = GSO_STALE;
    assert(gso_source_quality(s, 1000, 3000, 10000) == GSO_STALE);
    s.quality = GSO_ERROR;
    assert(gso_source_quality(s, 999999, 3000, 10000) == GSO_ERROR);
    s.received = false;
    assert(gso_source_age(s, 1000) == UINT64_MAX);
    s = (gso_freshness){GSO_OK, true, 0, UINT64_MAX-1};
    assert(gso_source_age(s, 9) == UINT64_MAX);
    assert(!gso_snapshot_size_allowed(0));
    assert(gso_snapshot_size_allowed(32768));
    assert(!gso_snapshot_size_allowed(32769));
    puts("PASS: portable model boundaries and freshness");
    return 0;
}
