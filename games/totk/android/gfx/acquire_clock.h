/* Fixed acquisition releases on the real Choreographer lattice. Pure policy:
 * no clock reads, allocation or waits. Callers serialize state and supply
 * CLOCK_MONOTONIC callback/arrival timestamps, never present or texture history. */
#pragma once
#include <stdint.h>
#include "../../runtime/present_clock.h"

typedef struct {
    uint64_t target_ns, refresh_ns;
    unsigned interval;
} acquire_clock;

enum { ACQUIRE_CLOCK_OFF, ACQUIRE_CLOCK_NO_CALLBACK, ACQUIRE_CLOCK_ACTIVE };
enum {
    ACQUIRE_CLOCK_START = 1,
    ACQUIRE_CLOCK_STALL = 2,
    ACQUIRE_CLOCK_REFRESH = 4
};
typedef struct {
    uint64_t target_ns, late_ns;
    unsigned reanchor;
    int mode, reached;
} acquire_clock_release;

/* Latest projected physical slot at/before arrival. Usually this is simply
 * the latest callback; projecting a stale callback prevents suspended Android
 * callbacks from granting an unlimited bank of immediate acquisitions. Only a
 * real callback satisfying runtime_present_slot_reached can release the slot. */
static inline uint64_t acquire_clock_anchor(uint64_t callback_ns, uint64_t refresh_ns,
        uint64_t arrival_ns) {
    return arrival_ns > callback_ns
        ? callback_ns + (arrival_ns - callback_ns) / refresh_ns * refresh_ns : callback_ns;
}

/* Poll an already reserved release after a callback or refresh-mode change.
 * Keep the ORIGINAL arrival timestamp while waiting: scheduler wakeup latency
 * is not a new acquisition. Callback lateness still detects a suspended Looper.
 * An EXACT one-game-period delay preserves phase; only strictly greater stalls
 * reanchor. Ordinary late/short frames can catch up by at most one game period. */
static inline acquire_clock_release acquire_clock_observe(acquire_clock *p,
        uint64_t callback_ns, uint64_t refresh_ns, uint64_t arrival_ns) {
    acquire_clock_release r = {.mode = ACQUIRE_CLOCK_NO_CALLBACK};
    if (!callback_ns || !refresh_ns || !p->target_ns || !p->interval ||
        refresh_ns > UINT64_MAX / p->interval) return r;
    uint64_t period = refresh_ns * p->interval;
    uint64_t latest = arrival_ns > callback_ns ? arrival_ns : callback_ns;
    r.late_ns = latest > p->target_ns ? latest - p->target_ns : 0;
    if (p->refresh_ns != refresh_ns) {
        r.reanchor = ACQUIRE_CLOCK_REFRESH;
        p->target_ns = acquire_clock_anchor(callback_ns, refresh_ns, arrival_ns);
    } else if (r.late_ns > period) {
        r.reanchor = ACQUIRE_CLOCK_STALL;
        p->target_ns = acquire_clock_anchor(callback_ns, refresh_ns, arrival_ns);
    }
    p->refresh_ns = refresh_ns;
    r.target_ns = p->target_ns;
    r.mode = ACQUIRE_CLOCK_ACTIVE;
    r.reached = runtime_present_slot_reached(callback_ns, r.target_ns, refresh_ns);
    return r;
}

/* Exactly one reserve per acquisition; observe() never advances a frame.
 * First release may use the latest callback. Thereafter target[k] is the prior
 * target plus the NEW requested interval, not arrival/present/availability time.
 * Interval changes preserve the lattice; refresh-mode changes reanchor it.
 * Disabled/unavailable calls leave state untouched, distinct from active mode. */
static inline acquire_clock_release acquire_clock_reserve(acquire_clock *p, int enabled,
        uint64_t callback_ns, uint64_t refresh_ns, unsigned interval, uint64_t arrival_ns) {
    acquire_clock_release r = {.mode = enabled ? ACQUIRE_CLOCK_NO_CALLBACK : ACQUIRE_CLOCK_OFF};
    if (!enabled || !callback_ns || !refresh_ns) return r;
    if (!interval) interval = 1;
    if (refresh_ns > UINT64_MAX / interval) return r;
    uint64_t period = refresh_ns * interval;
    if (p->target_ns && p->refresh_ns == refresh_ns) {
        if (p->target_ns > UINT64_MAX - period) return r;
        p->target_ns += period;
    } else {
        r.reanchor = p->target_ns ? ACQUIRE_CLOCK_REFRESH : ACQUIRE_CLOCK_START;
        p->target_ns = acquire_clock_anchor(callback_ns, refresh_ns, arrival_ns);
    }
    p->refresh_ns = refresh_ns;
    p->interval = interval;
    acquire_clock_release observed = acquire_clock_observe(p, callback_ns, refresh_ns, arrival_ns);
    observed.reanchor |= r.reanchor;
    return observed;
}

/* All lead states reserve the SAME virtual lattice. Lead L admits L periods
 * earlier with pending < 1+L (L <= 2). Removing lead drains on that same clock,
 * never by skipping a reservation, resetting phase or dropping a picture.
 * Keep this budget even when a surface drops a picture before enqueue: actual
 * pending credits alone cannot enforce the average simulation rate. */
static inline int acquire_clock_slack_ready(const acquire_clock *p,
        uint64_t callback_ns, unsigned pending, unsigned lead) {
    if (!p->interval || !p->refresh_ns || !p->target_ns ||
        p->refresh_ns > UINT64_MAX / p->interval) return 0;
    uint64_t period = p->refresh_ns * p->interval;
    if (lead > 2) lead = 2;
    /* Saturate subtraction without overflowing period * lead. */
    uint64_t target = p->target_ns;
    for (unsigned i = 0; i < lead; i++) target = target > period ? target - period : 0;
    return pending < 1u + lead &&
        runtime_present_slot_reached(callback_ns, target, p->refresh_ns);
}
