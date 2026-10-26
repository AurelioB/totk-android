/* Standalone pure policy test: timestamps in, release deadlines out. No
 * Android, GPU, environment, wall-clock reads or sleeping. */
#include "acquire_clock.h"
#include "acquire_slack.h"
#include "vk_present_slots.h"
#include <assert.h>
#include <stdio.h>

static const uint64_t base = 1000000000ull;
static uint64_t stamp(unsigned tick, unsigned hz) {
    return base + (uint64_t)tick * 1000000000ull / hz;
}
static void stable(unsigned hz, unsigned interval) {
    acquire_clock p = {0};
    uint64_t refresh = (1000000000ull + hz / 2) / hz;
    uint64_t period = refresh * interval;
    acquire_clock_release r = acquire_clock_reserve(&p, 1, base, refresh, interval, base);
    assert(r.mode == ACQUIRE_CLOCK_ACTIVE && r.reached && r.target_ns == base);
    assert(r.reanchor == ACQUIRE_CLOCK_START);
    for (unsigned frame = 1; frame <= 1200; frame++) {
        uint64_t target = base + frame * period;
        uint64_t arrival = target - 1000000;
        r = acquire_clock_reserve(&p, 1, stamp(frame * interval - 1, hz), refresh, interval, arrival);
        assert(r.target_ns == target && !r.reanchor && !r.reached);
        /* Fractional callbacks round differently from refresh multiplication.
         * Production's actual tolerance, not target>=callback, releases here. */
        r = acquire_clock_observe(&p, stamp(frame * interval, hz), refresh, arrival);
        assert(r.target_ns == target && !r.reanchor && r.reached);
    }
}
static void jitter(void) {
    const uint64_t refresh = 16666667, period = 2 * refresh;
    const uint64_t work[] = {26000000, 35000000, 17000000, 26000000};
    acquire_clock p = {0};
    uint64_t arrival = base;
    for (unsigned frame = 0; frame < 1200; frame++) {
        unsigned tick = (unsigned)((arrival - base) * 60 / 1000000000ull);
        acquire_clock_release r = acquire_clock_reserve(&p, 1, stamp(tick, 60), refresh, 2, arrival);
        uint64_t expected = base + frame * period;
        assert(r.target_ns == expected);
        assert(r.reanchor == (frame ? 0u : ACQUIRE_CLOCK_START));
        if (!r.reached) {
            tick = frame * 2;
            r = acquire_clock_observe(&p, stamp(tick, 60), refresh, arrival);
            assert(r.reached && !r.reanchor && r.target_ns == expected);
            if (arrival < stamp(tick, 60)) arrival = stamp(tick, 60);
        }
        arrival += work[frame % 4];
    }
}
static void callback_jitter(void) {
    const uint64_t refresh = 16666667, period = 2 * refresh;
    const int64_t offsets[] = {-250000, 180000, -100000, 300000};
    acquire_clock p = {0};
    acquire_clock_reserve(&p, 1, base, refresh, 2, base);
    for (unsigned frame = 1; frame <= 600; frame++) {
        uint64_t target = base + frame * period;
        uint64_t arrival = target - 2000000;
        acquire_clock_release r = acquire_clock_reserve(&p, 1, target - refresh,
                                                       refresh, 2, arrival);
        assert(!r.reanchor && !r.reached && r.target_ns == target);
        uint64_t callback = (uint64_t)((int64_t)target + offsets[frame % 4]);
        r = acquire_clock_observe(&p, callback, refresh, arrival);
        assert(!r.reanchor && r.reached && r.target_ns == target);
    }
}
static void late_short(void) {
    const uint64_t refresh = 10000000, period = 2 * refresh;
    acquire_clock p = {0};
    acquire_clock_reserve(&p, 1, base, refresh, 2, base);
    acquire_clock_release r = acquire_clock_reserve(&p, 1, base + 3 * refresh,
                                                   refresh, 2, base + period + 15000000);
    assert(r.reached && !r.reanchor && r.target_ns == base + period);
    /* A 15ms-late frame followed by 3ms of work does not reset the phase. */
    r = acquire_clock_reserve(&p, 1, base + 3 * refresh, refresh, 2, base + period + 18000000);
    assert(!r.reached && !r.reanchor && r.target_ns == base + 2 * period);
    r = acquire_clock_observe(&p, base + 4 * refresh, refresh, base + period + 18000000);
    assert(r.reached && !r.reanchor);
}
static void stall_boundary(int callback_late, uint64_t extra) {
    const uint64_t refresh = 10000000, period = 2 * refresh;
    acquire_clock p = {0};
    acquire_clock_reserve(&p, 1, base, refresh, 2, base);
    uint64_t latest = base + 2 * period + extra;
    uint64_t callback = callback_late ? latest : base + 2 * period;
    uint64_t arrival = callback_late ? base + period : latest;
    acquire_clock_release r = acquire_clock_reserve(&p, 1, callback, refresh, 2, arrival);
    assert(r.late_ns == period + extra && r.reached);
    assert(r.reanchor == (extra ? ACQUIRE_CLOCK_STALL : 0));
    assert(r.target_ns == (extra ? callback : base + period));
    /* Exactly one period late permits one extra immediate release, NOT a bank
     * proportional to stall duration. +1ns drops that catch-up credit. */
    r = acquire_clock_reserve(&p, 1, callback, refresh, 2, latest);
    assert(!r.reanchor && r.reached == !extra);
    if (!extra) {
        r = acquire_clock_reserve(&p, 1, callback, refresh, 2, latest);
        assert(!r.reanchor && !r.reached);
    }
}
static void changes(void) {
    const uint64_t refresh = 10000000;
    acquire_clock p = {0};
    acquire_clock_reserve(&p, 1, base, refresh, 2, base);
    acquire_clock_release r = acquire_clock_reserve(&p, 1, base, refresh, 1, base);
    assert(!r.reanchor && r.target_ns == base + refresh);
    r = acquire_clock_observe(&p, base + refresh, refresh, base);
    assert(r.reached && !r.reanchor);
    r = acquire_clock_reserve(&p, 1, base + refresh, refresh, 3, base + refresh);
    assert(!r.reanchor && r.target_ns == base + 4 * refresh);
    /* A real mode change, not an ordinary interval change, starts a new real
     * lattice; both reserve-time and in-wait refresh changes are covered. */
    r = acquire_clock_observe(&p, base + 25000000, 5000000, base + refresh);
    assert(r.reanchor == ACQUIRE_CLOCK_REFRESH && r.reached && r.target_ns == base + 25000000);
    r = acquire_clock_reserve(&p, 1, base + 25000000, 5000000, 2, base + 25000000);
    assert(!r.reanchor && r.target_ns == base + 35000000 && !r.reached);
    r = acquire_clock_observe(&p, base + 35000000, 5000000, base + 25000000);
    assert(!r.reanchor && r.reached);
    r = acquire_clock_reserve(&p, 1, base + 40000000, refresh, 2, base + 40000000);
    assert(r.reanchor == ACQUIRE_CLOCK_REFRESH && r.target_ns == base + 40000000 && r.reached);
    r = acquire_clock_reserve(&p, 1, base + 40000000, refresh, 0, base + 40000000);
    assert(!r.reanchor && r.target_ns == base + 50000000 && p.interval == 1);
}
static void suspend_recovery(void) {
    const uint64_t refresh = 10000000, period = 2 * refresh;
    acquire_clock p = {0};
    acquire_clock_reserve(&p, 1, base, refresh, 2, base);
    acquire_clock_release r = acquire_clock_reserve(&p, 1, base, refresh, 2, base + 1);
    assert(!r.reached && r.target_ns == base + period);
    r = acquire_clock_observe(&p, base + 1000 * period, refresh, base + 1);
    assert(r.reanchor == ACQUIRE_CLOCK_STALL && r.reached && r.target_ns == base + 1000 * period);
    r = acquire_clock_reserve(&p, 1, base + 1000 * period, refresh, 2, base + 1000 * period);
    assert(!r.reanchor && !r.reached && r.target_ns == base + 1001 * period);
    /* Arrival while callbacks are suspended cannot spend projected slots:
     * it waits for a fresh real callback rather than falling back to unpaced. */
    p = (acquire_clock){0};
    acquire_clock_reserve(&p, 1, base + 1000 * period, refresh, 2, base + 1000 * period);
    uint64_t arrival = base + 2000 * period + 3000000;
    r = acquire_clock_reserve(&p, 1, base + 1000 * period, refresh, 2, arrival);
    assert(r.reanchor == ACQUIRE_CLOCK_STALL && !r.reached);
    assert(r.target_ns == base + 2000 * period);
    r = acquire_clock_observe(&p, base + 1000 * period, refresh, arrival);
    assert(!r.reanchor && !r.reached && r.target_ns == base + 2000 * period);
    r = acquire_clock_observe(&p, base + 2000 * period, refresh, arrival);
    assert(!r.reanchor && r.reached);
    r = acquire_clock_reserve(&p, 1, base + 2000 * period, refresh, 2, arrival);
    assert(!r.reanchor && !r.reached && r.target_ns == base + 2001 * period);
}
static void disabled_and_fractional(void) {
    acquire_clock p = {0};
    acquire_clock_release r = acquire_clock_reserve(&p, 0, base, 16666667, 2, base);
    assert(r.mode == ACQUIRE_CLOCK_OFF && !r.target_ns && !r.reached && !p.target_ns);
    r = acquire_clock_reserve(&p, 1, 0, 16666667, 2, base);
    assert(r.mode == ACQUIRE_CLOCK_NO_CALLBACK && !p.target_ns);
    r = acquire_clock_observe(&p, base, 16666667, base);
    assert(r.mode == ACQUIRE_CLOCK_NO_CALLBACK && !p.target_ns);
    r = acquire_clock_reserve(&p, 1, base, 16666667, 2, base);
    uint64_t old = p.target_ns;
    r = acquire_clock_reserve(&p, 0, base + 999999999, 8333333, 1, base + 999999999);
    assert(r.mode == ACQUIRE_CLOCK_OFF && p.target_ns == old && p.refresh_ns == 16666667);
    r = acquire_clock_reserve(&p, 1, base, 0, 2, base);
    assert(r.mode == ACQUIRE_CLOCK_NO_CALLBACK && p.target_ns == old);
    r = acquire_clock_reserve(&p, 1, base, UINT64_MAX, 2, base);
    assert(r.mode == ACQUIRE_CLOCK_NO_CALLBACK && p.target_ns == old);
    r = acquire_clock_reserve(&p, 1, stamp(1, 60), 16666667, 2, stamp(1, 60));
    assert(!r.reached && r.target_ns == base + 33333334);
    r = acquire_clock_observe(&p, stamp(2, 60), 16666667, stamp(1, 60));
    assert(r.reached && r.target_ns == stamp(2, 60) + 1);
    assert(runtime_present_slot_reached(base, base + 2500000, 10000000));
    assert(!runtime_present_slot_reached(base, base + 2500001, 10000000));
}
static void bounded_lead(void) {
    const uint64_t refresh = 10000000, period = 2 * refresh;
    acquire_clock p = {0};
    unsigned pending = 0;
    assert(!runtime_present_credit_update(&pending, -1));
    acquire_clock_reserve(&p, 1, base, refresh, 2, base);
    assert(acquire_clock_slack_ready(&p, base, pending, 1));
    assert(runtime_present_credit_update(&pending, 1));
    /* A short 5ms picture banks time: the 25ms next picture starts at +5,
     * finishes at +30, and meets its +40 slot. Without lead it starts at +20
     * and misses that slot at +45. No GPU lifetime wait is modeled away. */
    acquire_clock_reserve(&p, 1, base, refresh, 2, base + 5000000);
    assert(acquire_clock_slack_ready(&p, base, pending, 1));
    assert(base + 5000000 + 25000000 < base + 2 * period);
    assert(base + period + 25000000 > base + 2 * period);
    assert(runtime_present_credit_update(&pending, 1));
    acquire_clock_reserve(&p, 1, base + period, refresh, 2, base + 30000000);
    assert(!acquire_clock_slack_ready(&p, base + period, pending, 1));
    assert(runtime_present_credit_update(&pending, -1));
    assert(acquire_clock_slack_ready(&p, base + period, pending, 1));
    assert(runtime_present_credit_update(&pending, -1));
    assert(!runtime_present_credit_update(&pending, -1));
    /* A missing surface / pre-enqueue drop creates no actual pending credit.
     * Even then only two startup releases and one per game period are allowed. */
    p = (acquire_clock){0};
    for (unsigned frame = 0; frame < 1200; frame++) {
        uint64_t target = base + frame * period;
        uint64_t release = frame ? target - period : base;
        acquire_clock_release r = acquire_clock_reserve(&p, 1, release, refresh, 2, release);
        assert(!r.reanchor || frame == 0);
        assert(p.target_ns == target && acquire_clock_slack_ready(&p, release, 0, 1));
        if (frame > 1) assert(!acquire_clock_slack_ready(&p, release - refresh, 0, 1));
    }
    uint64_t arrival = base + 2000 * period;
    acquire_clock_release r = acquire_clock_reserve(&p, 1, base + 1200 * period,
                                                   refresh, 2, arrival);
    assert(r.reanchor == ACQUIRE_CLOCK_STALL);
    assert(!acquire_clock_slack_ready(&p, base + 1200 * period, 0, 1));
    acquire_clock_observe(&p, arrival, refresh, arrival);
    assert(acquire_clock_slack_ready(&p, arrival, 0, 1));
    acquire_clock_reserve(&p, 1, arrival, refresh, 2, arrival);
    assert(acquire_clock_slack_ready(&p, arrival, 0, 1));
    acquire_clock_reserve(&p, 1, arrival, refresh, 2, arrival);
    assert(!acquire_clock_slack_ready(&p, arrival, 0, 1));
}
static void adaptive_modes(void) {
    assert(acquire_slack_parse(NULL) == ACQUIRE_SLACK_OFF);
    assert(acquire_slack_parse("0") == ACQUIRE_SLACK_OFF);
    assert(acquire_slack_parse("1") == ACQUIRE_SLACK_ALWAYS);
    assert(acquire_slack_parse("2") == ACQUIRE_SLACK_TWO);
    assert(acquire_slack_parse("auto") == ACQUIRE_SLACK_AUTO);
    assert(acquire_slack_parse("predict") == ACQUIRE_SLACK_PREDICT);
    const char *invalid[] = {"", "00", "01", "3", "-1", "off", "always",
                             "adaptive", "AUTO", "auto1", " auto", "1 "};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert(acquire_slack_parse(invalid[i]) == ACQUIRE_SLACK_INVALID);
}
static void adaptive_hysteresis(unsigned hz, unsigned interval) {
    const uint64_t target = ((1000000000ull + hz / 2) / hz) * interval;
    uint64_t now = base;
    acquire_slack_policy p = {0};
    acquire_slack_observation r = acquire_slack_observe(&p, now, target);
    assert(!r.interval_ns && !r.reason && !p.lead);
    now += target + ACQUIRE_SLACK_LATE_NS;
    r = acquire_slack_observe(&p, now, target);
    assert(r.interval_ns == target + ACQUIRE_SLACK_LATE_NS && !r.reason && !p.lead);
    now += target + ACQUIRE_SLACK_LATE_NS + 1;
    r = acquire_slack_observe(&p, now, target);
    assert(r.reason == ACQUIRE_SLACK_LATE && p.lead == 1 && !p.quiet_frames);
    for (unsigned i = 1; i < ACQUIRE_SLACK_QUIET_FRAMES; i++) {
        r = acquire_slack_observe(&p, now += target, target);
        assert(!r.reason && p.lead == 1 && p.quiet_frames == i);
    }
    /* A late interval at the end of the window renews the ENTIRE window. */
    now += target + ACQUIRE_SLACK_LATE_NS + 1;
    r = acquire_slack_observe(&p, now, target);
    assert(!r.reason && p.lead == 1 && !p.quiet_frames);
    for (unsigned i = 1; i <= ACQUIRE_SLACK_QUIET_FRAMES; i++) {
        now += target + ACQUIRE_SLACK_LATE_NS; /* Equality is quiet. */
        r = acquire_slack_observe(&p, now, target);
        assert(r.reason == (i == ACQUIRE_SLACK_QUIET_FRAMES ? ACQUIRE_SLACK_QUIET : 0));
        assert(p.lead == (i < ACQUIRE_SLACK_QUIET_FRAMES));
    }
    assert(!p.quiet_frames && p.drain_next);
    r = acquire_slack_observe(&p, now += 2 * target, target);
    assert(r.reason == ACQUIRE_SLACK_DRAIN && r.interval_ns == 2 * target);
    assert(!p.lead && !p.drain_next); /* Planned drain must not retrigger lead. */
    for (unsigned i = 0; i < 300; i++) {
        r = acquire_slack_observe(&p, now += target, target);
        assert(!r.reason && !p.lead && !p.quiet_frames);
    }
    r = acquire_slack_observe(&p, now += target + ACQUIRE_SLACK_LATE_NS + 1, target);
    assert(r.reason == ACQUIRE_SLACK_LATE && p.lead == 1);
    for (unsigned i = 1; i < ACQUIRE_SLACK_QUIET_FRAMES; i++)
        acquire_slack_observe(&p, now += target, target);
    /* A target change retains active lead, but requires a fresh quiet window. */
    const uint64_t next_target = target / 2;
    r = acquire_slack_observe(&p, now += next_target, next_target);
    assert(!r.reason && p.lead == 1 && p.quiet_frames == 1);
    for (unsigned i = 1; i < ACQUIRE_SLACK_QUIET_FRAMES; i++)
        r = acquire_slack_observe(&p, now += next_target, next_target);
    assert(r.reason == ACQUIRE_SLACK_QUIET && !p.lead);
}
static void adaptive_drain_guard(void) {
    const uint64_t target = 20000000;
    acquire_slack_policy p = {.last_present_ns = base, .target_ns = target,
        .lead = 1, .quiet_frames = ACQUIRE_SLACK_QUIET_FRAMES - 1};
    uint64_t now = base + target;
    acquire_slack_observation r = acquire_slack_observe(&p, now, target);
    assert(r.reason == ACQUIRE_SLACK_QUIET && !p.lead && p.drain_next);
    /* Even a coincident real hitch is exempted in this ONE known interval,
     * but its full raw duration remains available to the diagnostic log. */
    const uint64_t drain = 2 * target + 50000000;
    r = acquire_slack_observe(&p, now += drain, target);
    assert(r.reason == ACQUIRE_SLACK_DRAIN && r.interval_ns == drain);
    assert(!p.lead && !p.drain_next);
    r = acquire_slack_observe(&p, now += target + ACQUIRE_SLACK_LATE_NS + 1, target);
    assert(r.reason == ACQUIRE_SLACK_LATE && p.lead == 1);
}
static void adaptive_short_long(unsigned interval) {
    const uint64_t refresh = 10000000, period = refresh * interval;
    const uint64_t short_work = period / 4, long_work = period * 5 / 4;
    acquire_clock clock = {0};
    acquire_slack_policy policy = {0};
    acquire_clock_reserve(&clock, 1, base, refresh, interval, base);
    assert(acquire_clock_slack_ready(&clock, base, 0, policy.lead));
    acquire_slack_observe(&policy, base + short_work, period);
    acquire_clock_release r = acquire_clock_reserve(&clock, 1, base, refresh,
                                                   interval, base + short_work);
    assert(!r.reanchor && !acquire_clock_slack_ready(&clock, base, 0, policy.lead));
    r = acquire_clock_observe(&clock, base + period, refresh, base + short_work);
    assert(!r.reanchor && acquire_clock_slack_ready(&clock, base + period, 0, policy.lead));
    uint64_t present = base + period + long_work;
    acquire_slack_observation sample = acquire_slack_observe(&policy, present, period);
    assert(sample.reason == ACQUIRE_SLACK_LATE && policy.lead == 1);
    uint64_t callback = base + (present - base) / refresh * refresh;
    r = acquire_clock_reserve(&clock, 1, callback, refresh, interval, present);
    assert(!r.reanchor && r.target_ns == base + 2 * period);
    assert(acquire_clock_slack_ready(&clock, callback, 0, policy.lead));
    sample = acquire_slack_observe(&policy, present + short_work, period);
    assert(sample.interval_ns == short_work && !sample.reason && policy.lead == 1);
    assert(policy.quiet_frames == 1); /* A short recovery cannot flap lead off. */
}
static void adaptive_gate_phase(unsigned interval) {
    const uint64_t refresh = 10000000, period = refresh * interval;
    acquire_clock p = {0};
    acquire_clock_reserve(&p, 1, base, refresh, interval, base);
    acquire_clock_release r = acquire_clock_reserve(&p, 1, base, refresh, interval, base);
    assert(r.target_ns == base + period && !r.reanchor);
    assert(!acquire_clock_slack_ready(&p, base, 0, 0));
    assert(acquire_clock_slack_ready(&p, base, 0, 1));
    assert(acquire_clock_slack_ready(&p, base, 1, 1));
    assert(!acquire_clock_slack_ready(&p, base, 2, 1));
    assert(!acquire_clock_slack_ready(&p, base + period, 1, 0));
    assert(acquire_clock_slack_ready(&p, base + period, 0, 0));
    /* The next reservation is identical in both states. Removing lead waits
     * one extra period AND drains actual pending pictures, rather than skipping
     * a reservation or returning to texture-derived synthetic availability. */
    r = acquire_clock_reserve(&p, 1, base + period, refresh, interval, base + period);
    uint64_t target = base + 2 * period;
    assert(r.target_ns == target && !r.reanchor);
    assert(acquire_clock_slack_ready(&p, base + period, 1, 1));
    assert(!acquire_clock_slack_ready(&p, base + period, 0, 0));
    r = acquire_clock_observe(&p, target, refresh, base + period);
    assert(r.target_ns == target && !r.reanchor);
    assert(!acquire_clock_slack_ready(&p, target, 1, 0));
    assert(acquire_clock_slack_ready(&p, target, 0, 0));
    assert(p.target_ns == target); /* Readiness/toggles never mutate phase. */
}
static void adaptive_rate_bound(unsigned interval) {
    const uint64_t refresh = 10000000, period = refresh * interval;
    acquire_clock p = {0};
    uint64_t callback = base;
    unsigned immediate = 0;
    for (unsigned frame = 0; frame < 2400; frame++) {
        unsigned lead = (frame / 37) % 2;
        uint64_t arrival = callback;
        acquire_clock_release r = acquire_clock_reserve(&p, 1, callback, refresh, interval, arrival);
        assert(r.reanchor == (frame ? 0u : ACQUIRE_CLOCK_START));
        int waited = 0;
        while (!acquire_clock_slack_ready(&p, callback, 0, lead)) {
            callback += refresh;
            r = acquire_clock_observe(&p, callback, refresh, arrival);
            assert(!r.reanchor);
            waited = 1;
        }
        immediate = waited ? 1 : immediate + 1;
        assert(immediate <= 2);
        assert(p.target_ns == base + frame * period);
        assert(callback == base + (frame - (frame && lead)) * period);
        /* Even with no surface/enqueue credits, mode switches cannot bank
         * unbounded acquisitions or increase long-run simulation speed. */
        assert((uint64_t)frame <= (callback - base) / period + 1);
    }
}
static void adaptive_present_drain(unsigned interval, int adaptive) {
    enum { FRAMES = 400 };
    const uint64_t refresh = 10000000, period = refresh * interval, work = period / 4;
    acquire_clock acquire = {0};
    vk_present_slots presenter = {0};
    vk_present_slot pictures[FRAMES];
    acquire_slack_policy policy = {0};
    unsigned quiet_transitions = 0, drains = 0, previous_lead = 1;
    if (adaptive) {
        acquire_slack_observe(&policy, base - 3 * period, period);
        acquire_slack_observation sample = acquire_slack_observe(&policy, base - period, period);
        assert(sample.reason == ACQUIRE_SLACK_LATE && policy.lead == 1);
    }
    unsigned pending = 0, retired = 0;
    uint64_t callback = base, now = base, previous_admission = 0;
    for (unsigned frame = 0; frame < FRAMES; frame++) {
        unsigned lead = adaptive ? policy.lead : (frame / 40) % 2 == 0;
        uint64_t arrival = now;
        acquire_clock_release r = acquire_clock_reserve(&acquire, 1, callback,
                                                        refresh, interval, arrival);
        assert(r.reanchor == (frame ? 0u : ACQUIRE_CLOCK_START));
        while (!acquire_clock_slack_ready(&acquire, callback, pending, lead)) {
            callback += refresh;
            if (now < callback) now = callback;
            while (retired < frame && runtime_present_can_submit(callback,
                    pictures[retired].desired_ns, refresh, now)) {
                assert(pictures[retired].desired_ns == base + (retired + 1) * period);
                assert(runtime_present_credit_update(&pending, -1));
                retired++;
            }
            r = acquire_clock_observe(&acquire, callback, refresh, arrival);
            assert(!r.reanchor);
        }
        assert(acquire.target_ns == base + frame * period);
        if (frame && lead != previous_lead) {
            if (!lead) {
                assert(now == previous_admission + 2 * period);
                assert(!pending); /* Both already scheduled pictures drained. */
                assert(retired == frame);
            } else {
                assert(now == previous_admission + work);
                assert(pending == 1); /* Only one bounded extra admission. */
            }
        }
        previous_admission = now;
        previous_lead = lead;
        now += work;
        if (adaptive) {
            acquire_slack_observation sample = acquire_slack_observe(&policy, now, period);
            assert(sample.reason != ACQUIRE_SLACK_LATE);
            if (sample.reason == ACQUIRE_SLACK_QUIET) quiet_transitions++;
            if (sample.reason == ACQUIRE_SLACK_DRAIN) {
                drains++;
                assert(sample.interval_ns == 2 * period && !policy.lead);
            }
            if (frame >= 122) assert(!policy.lead); /* No 120-frame sawtooth. */
        }
        pictures[frame] = vk_present_slots_reserve(&presenter, 1, callback,
                                                    refresh, interval, now);
        assert(pictures[frame].mode == VK_PRESENT_SLOTS_ACTIVE && !pictures[frame].skipped);
        assert(pictures[frame].desired_ns == base + (frame + 1) * period);
        assert(now < pictures[frame].desired_ns);
        assert(runtime_present_credit_update(&pending, 1));
        assert(pending <= 1u + lead);
        /* Previous reservations stay queued at their exact original times
         * across both transitions: the drain consumes them, never drops them. */
        for (unsigned queued = retired; queued <= frame; queued++)
            assert(pictures[queued].desired_ns == base + (queued + 1) * period);
    }
    while (retired < FRAMES) {
        callback += refresh;
        if (now < callback) now = callback;
        while (retired < FRAMES && runtime_present_can_submit(callback,
                pictures[retired].desired_ns, refresh, now)) {
            assert(pictures[retired].desired_ns == base + (retired + 1) * period);
            assert(runtime_present_credit_update(&pending, -1));
            retired++;
        }
    }
    assert(!pending && retired == FRAMES);
    if (adaptive) assert(quiet_transitions == 1 && drains == 1 && !policy.lead);
}
int main(void) {
    stable(60, 1); /* 60 fps */
    stable(60, 2); /* 30 fps */
    stable(120, 2); stable(120, 4);
    jitter(); callback_jitter(); late_short();
    stall_boundary(0, 0); stall_boundary(0, 1);
    stall_boundary(1, 0); stall_boundary(1, 1);
    changes(); suspend_recovery(); disabled_and_fractional(); bounded_lead();
    adaptive_modes(); adaptive_drain_guard();
    adaptive_hysteresis(60, 1); adaptive_hysteresis(60, 2);
    adaptive_hysteresis(120, 2); adaptive_hysteresis(120, 4);
    for (unsigned interval = 1; interval <= 2; interval++) {
        adaptive_short_long(interval); adaptive_gate_phase(interval);
        adaptive_rate_bound(interval);
        adaptive_present_drain(interval, 0); adaptive_present_drain(interval, 1);
    }
    puts("acquire clock: stable 30/60, jitter/late-short phase, stalls/refresh/suspend, bounded lead, adaptive hysteresis, non-reset transitions and gap-free present-slot drain passed");
    return 0;
}
