/* MIT. Actual-release cap, independent of presenter buffer lead or phase.
 * The consumer calls AFTER every texture/GPU wait, sleeps until the returned
 * deadline, then stores the actual monotonic time observed AFTER that sleep.
 * Preserve that last actual release across refresh changes and temporal resets;
 * a stale planned deadline is never release authority or catch-up credit.
 */
#pragma once
#include <stdint.h>

#define FG_ADAPTIVE_PACE_MIN_INTERVAL_NS UINT64_C(16666667)
static inline uint64_t fg_adaptive_pace_deadline(uint64_t last_actual_release_ns,
        uint64_t now_ns,uint64_t refresh_ns) {
    if (!last_actual_release_ns) return now_ns;
    const uint64_t interval=refresh_ns>FG_ADAPTIVE_PACE_MIN_INTERVAL_NS ?
        refresh_ns : FG_ADAPTIVE_PACE_MIN_INTERVAL_NS;
    const uint64_t deadline=last_actual_release_ns>UINT64_MAX-interval ?
        UINT64_MAX : last_actual_release_ns+interval;
    return deadline>now_ns ? deadline : now_ns;
}
