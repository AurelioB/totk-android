/* Standalone deterministic test; no Android, GPU, wall clock or sleeping. */
#include "display_pace.h"
#include <assert.h>
#include <stdio.h>

static uint64_t stamp(unsigned tick, unsigned hz) {
    return 1000000000ull + (uint64_t)tick * 1000000000ull / hz;
}
static void cadence(unsigned hz, unsigned interval) {
    display_pacer p = {0};
    unsigned frames = 0;
    assert(display_pace_pending(&p, interval) == interval);
    for (unsigned tick = 1; tick <= hz * 10; tick++) {
        assert(display_pace_tick(&p, stamp(tick, hz)));
        int ready = display_pace_take(&p, interval);
        assert(ready == (tick % interval == 0));
        frames += ready;
        /* An acquire loop or a duplicate/stale callback cannot spend a tick twice. */
        assert(!display_pace_take(&p, interval));
        assert(!display_pace_tick(&p, stamp(tick, hz)));
        assert(!display_pace_tick(&p, stamp(tick - 1, hz)));
    }
    assert(frames == hz * 10 / interval);
}
int main(void) {
    for (unsigned interval = 1; interval <= 3; interval++) {
        cadence(60, interval);
        cadence(90, interval);
        cadence(120, interval);
    }
    display_pacer p = {0};
    assert(!display_pace_tick(&p, 0));
    assert(!display_pace_take(&p, 0));
    assert(display_pace_tick(&p, stamp(1, 60)));
    assert(display_pace_take(&p, 0)); /* interval 0 still capped to display */
    assert(!display_pace_take(&p, 1));
    for (unsigned tick = 2; tick <= 20; tick++)
        assert(display_pace_tick(&p, stamp(tick, 60)));
    assert(display_pace_take(&p, 2)); /* slow frame: only ONE credit, no catch-up */
    assert(display_pace_pending(&p, 2) == 2);
    assert(!display_pace_take(&p, 2));
    assert(display_pace_tick(&p, stamp(21, 60)));
    assert(display_pace_pending(&p, 2) == 1);
    assert(display_pace_take(&p, 1)); /* controller changed 30 -> 60 */
    assert(display_pace_tick(&p, stamp(22, 60)));
    assert(!display_pace_take(&p, 2)); /* controller changed 60 -> 30 */
    assert(display_pace_tick(&p, stamp(23, 60)));
    assert(display_pace_take(&p, 2));
    /* Missing callbacks / a suspended Looper do not fabricate refresh credits.
     * The next callback resumes pacing instead of bursting through the gap. */
    assert(display_pace_tick(&p, stamp(600, 60)));
    assert(!display_pace_take(&p, 2));
    assert(display_pace_tick(&p, stamp(601, 60)));
    assert(display_pace_take(&p, 2));
    assert(!display_pace_take(&p, 2));
    /* A refresh change affects callback cadence, not the requested tick count. */
    uint64_t base = p.frame_ns;
    assert(display_pace_tick(&p, base + 8333333));
    assert(!display_pace_take(&p, 2));
    assert(display_pace_tick(&p, base + 16666666));
    assert(display_pace_take(&p, 2));
    puts("display pace: 60/90/120 Hz x intervals 1/2/3, stale ticks, stalls, interval and refresh changes passed");
    return 0;
}
