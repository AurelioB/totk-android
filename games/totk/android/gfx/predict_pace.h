/* Bounded, deadline-predicted acquisition lead. Pure policy: no clocks,
 * allocation, waits or I/O. The caller serializes each path/interval history,
 * reserves S on vk_present_slots, and keeps that SAME S through presentation.
 * All supplied timestamps use one monotonic nanosecond clock. */
#pragma once
#include <stdint.h>

enum {
    PREDICT_PACE_SAMPLES = 300,
    PREDICT_PACE_WARMUP = 8,
    PREDICT_PACE_PPM = 1000000,
    PREDICT_PACE_DEFAULT_MISS_PPM = 20000
};
#define PREDICT_PACE_MARGIN_NS 500000ull
#define PREDICT_PACE_STEP_NS 125000ull

typedef struct {
    struct {
        uint64_t ready_ns, allowance_ns;
        unsigned char miss;
    } samples[PREDICT_PACE_SAMPLES];
    int64_t ready_order[PREDICT_PACE_SAMPLES], allowance_order[PREDICT_PACE_SAMPLES];
    int64_t phases[PREDICT_PACE_SAMPLES], phase_order[PREDICT_PACE_SAMPLES];
    uint64_t period_ns, first_slot_ns, last_slot_ns, last_display_slot_ns;
    uint64_t cost_ns, allowance_ns, margin_ns;
    uint64_t service_allowance_ns, display_protection_ns, display_refresh_ns;
    int64_t display_phase_ns;
    unsigned count, next, ready_misses;
    unsigned phase_count, phase_next, display_count, display_misses;
    unsigned miss_budget_ppm, budget_configured;
} predict_pacer;

typedef struct {
    uint64_t slot_ns, period_ns, deadline_ns, release_ns, lead_ns;
    uint64_t cost_ns, allowance_ns, margin_ns;
} predict_decision;

typedef struct {
    int valid, miss;
    int64_t error_ns;
    uint64_t late_ns;
} predict_observation;

/* Zero is a configured zero-miss budget, not the unconfigured default. Budget
 * changes take effect on the next feedback, never by recalculating in choose. */
static inline void predict_pacer_set_budget(predict_pacer *p, unsigned ppm) {
    if (!p) return;
    p->miss_budget_ppm = ppm > PREDICT_PACE_PPM ? PREDICT_PACE_PPM : ppm;
    p->budget_configured = 1;
}
static inline void predict_pacer_reset(predict_pacer *p) {
    if (!p) return;
    unsigned ppm = p->budget_configured ? p->miss_budget_ppm : PREDICT_PACE_DEFAULT_MISS_PPM;
    *p = (predict_pacer){.miss_budget_ppm = ppm, .budget_configured = 1};
}

/* Relative caps also keep tiny synthetic periods meaningful. Margin is fixed:
 * readiness already includes wake/prediction variation, so adding positive
 * error again would count the same demand twice and create a ratchet. */
static inline uint64_t predict_pace_scaled(uint64_t ns, uint64_t period, unsigned divisor) {
    uint64_t cap = period / divisor;
    return !cap ? 1 : ns < cap ? ns : cap;
}
static inline uint64_t predict_pace_margin_limit(uint64_t period) {
    return predict_pace_scaled(PREDICT_PACE_MARGIN_NS, period, 16);
}
static inline uint64_t predict_pace_add_capped(uint64_t a, uint64_t b, uint64_t cap) {
    return a >= cap || b >= cap - a ? cap : a + b;
}
static inline uint64_t predict_pace_follow(uint64_t value, uint64_t target,
        uint64_t step, int fast) {
    if (!value || (fast && target > value)) return target;
    if (target > value) return predict_pace_add_capped(value, step, target);
    return value - (value - target < step ? value - target : step);
}
static inline int predict_pace_over_budget(unsigned misses, unsigned count, unsigned ppm) {
    return (uint64_t)misses * PREDICT_PACE_PPM > (uint64_t)count * ppm;
}

/* Exact nearest-rank (1-budget) index: ceil(n * (1-b)) - 1. At 100% use the
 * minimum, at 0% the maximum. With 300 samples, 2% excludes exactly six tails.
 * Products are widened before multiplication, including the rate comparison. */
static inline unsigned predict_pace_quantile_index(unsigned count, unsigned ppm) {
    unsigned rank = (unsigned)(((uint64_t)count * (PREDICT_PACE_PPM - ppm) +
        PREDICT_PACE_PPM - 1) / PREDICT_PACE_PPM);
    return rank ? rank - 1 : 0;
}

/* A ring preserves expiry order; an ordered multiset preserves exact ranks.
 * Update only on accepted feedback, O(300), with no heap or scratch copies.
 * Duplicate values remove one occurrence. No scans occur in choose. */
static inline void predict_pace_order_update(int64_t *order, unsigned count,
        int replacing, int64_t old, int64_t value) {
    if (replacing) {
        unsigned i = 0;
        while (order[i] < old) i++;
        for (; i + 1 < count; i++) order[i] = order[i + 1];
        count--;
    }
    unsigned i = count;
    while (i && order[i - 1] > value) { order[i] = order[i - 1]; i--; }
    order[i] = value;
}
static inline void predict_pace_allowance(predict_pacer *p) {
    p->allowance_ns = predict_pace_add_capped(p->service_allowance_ns,
        p->display_protection_ns, (uint64_t)INT64_MAX);
}

/* O(1), including repeated calls during a gate wait. Eight complete feedback
 * samples use full lead, then cached quantiles select fractional lead. Forecasts
 * converge every feedback; only a realized rate STRICTLY beyond budget allows
 * a fast rise. There is no consecutive-quiet timer. D is S minus separately
 * measured path allowance, never a new presentation phase. Release stays in
 * [S-2P,S-P], including overload. Invalid input does not mutate history. */
static inline predict_decision predict_pacer_choose(predict_pacer *p,
        uint64_t slot_ns, uint64_t period_ns) {
    predict_decision r = {0};
    if (!p || !period_ns || slot_ns > (uint64_t)INT64_MAX ||
        period_ns > (uint64_t)INT64_MAX / 2 || slot_ns <= 2 * period_ns) return r;
    if (p->period_ns != period_ns) {
        predict_pacer_reset(p);
        p->period_ns = period_ns;
    }
    if (slot_ns < p->first_slot_ns || slot_ns <= p->last_slot_ns) return r;
    if (!p->first_slot_ns) p->first_slot_ns = slot_ns;
    uint64_t floor = predict_pace_margin_limit(period_ns);
    uint64_t cost = p->count ? p->cost_ns : period_ns;
    uint64_t allowance = p->count ? p->allowance_ns : floor;
    uint64_t margin = floor;
    if (!cost || !allowance || allowance >= slot_ns) return r;
    uint64_t demand = predict_pace_add_capped(cost, allowance, 2 * period_ns);
    demand = predict_pace_add_capped(demand, margin, 2 * period_ns);
    if (demand < period_ns) demand = period_ns;
    if (p->count < PREDICT_PACE_WARMUP) demand = 2 * period_ns;
    uint64_t release = slot_ns - demand;
    if (cost > (uint64_t)INT64_MAX - release) return r;
    return (predict_decision){.slot_ns = slot_ns, .period_ns = period_ns,
        .deadline_ns = slot_ns - allowance, .release_ns = release,
        .lead_ns = demand - period_ns, .cost_ns = cost,
        .allowance_ns = allowance, .margin_ns = margin};
}

/* Observe once with the ORIGINAL release target, not the actual wake. work_ns
 * includes required-resource waits, but excludes deliberate gate/slot/FIFO
 * holds. actual_ready_ns is the actual wake + that complete wait-stripped work
 * (or an independently measured causal endpoint without intentional holds).
 * Train the COMPLETE span actual_ready-release_target, including both early
 * and late wake variation. A nonpositive span is represented by 1ns, not by
 * adding positive prediction errors to an already refreshed cost quantile.
 * A miss is actual_ready>D, distinct from prediction error or display timing.
 *
 * Zero, overflowing, malformed, duplicate/out-of-order and wrong-period data
 * are invalid and do not mutate the model. ready>S+P is a whole-slot stall:
 * discard the sample and reset to warmup, retaining a stale-slot watermark and
 * configured budget. The owner MUST discard old-generation feedback, and reset
 * on path/mode/refresh/reanchor changes; value-only plans cannot identify a
 * prior generation with the same period. */
static inline predict_observation predict_pacer_observe(predict_pacer *p,
        const predict_decision *d, uint64_t work_ns, uint64_t allowance_ns,
        uint64_t actual_ready_ns) {
    predict_observation r = {0};
    if (!p || !d || !d->period_ns || d->period_ns != p->period_ns ||
        d->period_ns > (uint64_t)INT64_MAX / 2 ||
        d->slot_ns > (uint64_t)INT64_MAX || d->slot_ns <= 2 * d->period_ns ||
        d->slot_ns < p->first_slot_ns || d->slot_ns <= p->last_slot_ns ||
        !d->allowance_ns || d->allowance_ns >= d->slot_ns ||
        d->deadline_ns != d->slot_ns - d->allowance_ns ||
        d->release_ns < d->slot_ns - 2 * d->period_ns ||
        d->release_ns > d->slot_ns - d->period_ns ||
        d->lead_ns != d->slot_ns - d->period_ns - d->release_ns ||
        !d->cost_ns || d->cost_ns > (uint64_t)INT64_MAX - d->release_ns ||
        !d->margin_ns || d->margin_ns > predict_pace_margin_limit(d->period_ns) ||
        !work_ns || work_ns > (uint64_t)INT64_MAX - d->release_ns ||
        !allowance_ns || allowance_ns >= d->slot_ns ||
        !actual_ready_ns || actual_ready_ns > (uint64_t)INT64_MAX ||
        work_ns > actual_ready_ns) return r;
    if (actual_ready_ns > d->slot_ns && actual_ready_ns - d->slot_ns > d->period_ns) {
        predict_pacer_reset(p);
        p->period_ns = d->period_ns;
        p->first_slot_ns = p->last_slot_ns = d->slot_ns;
        return r;
    }
    uint64_t predicted_ready = d->release_ns + d->cost_ns;
    r.valid = 1;
    r.miss = actual_ready_ns > d->deadline_ns;
    r.late_ns = r.miss ? actual_ready_ns - d->deadline_ns : 0;
    r.error_ns = actual_ready_ns >= predicted_ready
        ? (int64_t)(actual_ready_ns - predicted_ready)
        : -(int64_t)(predicted_ready - actual_ready_ns);

    uint64_t span = actual_ready_ns > d->release_ns ? actual_ready_ns - d->release_ns : 1;
    int replacing = p->count == PREDICT_PACE_SAMPLES;
    predict_pace_order_update(p->ready_order, p->count, replacing,
        (int64_t)p->samples[p->next].ready_ns, (int64_t)span);
    predict_pace_order_update(p->allowance_order, p->count, replacing,
        (int64_t)p->samples[p->next].allowance_ns, (int64_t)allowance_ns);
    if (replacing) p->ready_misses -= p->samples[p->next].miss;
    p->samples[p->next].ready_ns = span;
    p->samples[p->next].allowance_ns = allowance_ns;
    p->samples[p->next].miss = (unsigned char)r.miss;
    p->ready_misses += r.miss;
    p->next = (p->next + 1) % PREDICT_PACE_SAMPLES;
    if (!replacing) p->count++;
    p->last_slot_ns = d->slot_ns;
    unsigned q = predict_pace_quantile_index(p->count, p->miss_budget_ppm);
    uint64_t step = predict_pace_scaled(PREDICT_PACE_STEP_NS, d->period_ns, 128);
    int fast = predict_pace_over_budget(p->ready_misses, p->count, p->miss_budget_ppm);
    p->cost_ns = predict_pace_follow(p->cost_ns, (uint64_t)p->ready_order[q], step, fast);
    p->service_allowance_ns = predict_pace_follow(p->service_allowance_ns,
        (uint64_t)p->allowance_order[q], step, fast);
    p->margin_ns = predict_pace_margin_limit(d->period_ns);
    predict_pace_allowance(p);
    return r;
}

/* Only actual display fences, never latch substitutes. S is an app callback
 * slot, NOT a hardware-vsync timestamp: calibrate the same-epoch signed
 * display-S baseline at the rolling 10th percentile. This tolerates extra-
 * refresh upper tails and rejects a stable 10-14ms compositor phase as misses.
 * First seven phase samples return -1 (unclassified), as do invalid signals;
 * phase_count distinguishes absent/warming/calibrated. Once calibrated all
 * retained phases are classified against baseline+R/2, and display_count is
 * their rolling denominator. Negative baseline phases are valid.
 *
 * Beyond-budget physical tails protect allowance by the (1-budget) phase
 * excess ABOVE the half-refresh tolerance, capped at P/2. This is a target,
 * not an additive error accumulator. At/below budget it decays every feedback,
 * including an isolated permitted miss. Work cost and S never change here.
 * Calibration cannot distinguish a constant whole-refresh shift from phase;
 * the owner must reset on epoch changes. Initial offsets are bounded by P+R;
 * calibrated deviations by P+R/2, so one extra refresh plus normal phase
 * variation remains observable at interval 1. Pause-sized offsets are ignored. */
static inline int predict_pacer_presented(predict_pacer *p, uint64_t slot, uint64_t period,
        uint64_t refresh, uint64_t display) {
    if (!p || !p->count || !refresh || refresh > period || period != p->period_ns ||
        period > (uint64_t)INT64_MAX / 2 || slot > (uint64_t)INT64_MAX ||
        slot < p->first_slot_ns || slot <= p->last_display_slot_ns ||
        !display || display > (uint64_t)INT64_MAX ||
        (p->display_refresh_ns && p->display_refresh_ns != refresh)) return -1;
    uint64_t distance = display >= slot ? display - slot : slot - display;
    if (distance > period + refresh) return -1;
    int64_t phase = display >= slot ? (int64_t)distance : -(int64_t)distance;
    if (p->phase_count >= PREDICT_PACE_WARMUP) {
        uint64_t deviation = phase >= p->display_phase_ns
            ? (uint64_t)phase - (uint64_t)p->display_phase_ns
            : (uint64_t)p->display_phase_ns - (uint64_t)phase;
        if (deviation > period + refresh / 2) return -1;
    }
    int replacing = p->phase_count == PREDICT_PACE_SAMPLES;
    predict_pace_order_update(p->phase_order, p->phase_count, replacing,
        p->phases[p->phase_next], phase);
    p->phases[p->phase_next] = phase;
    p->phase_next = (p->phase_next + 1) % PREDICT_PACE_SAMPLES;
    if (!replacing) p->phase_count++;
    p->last_display_slot_ns = slot;
    p->display_refresh_ns = refresh;
    if (p->phase_count < PREDICT_PACE_WARMUP) return -1;
    p->display_phase_ns = p->phase_order[(p->phase_count - 1) / 10];
    p->display_count = p->phase_count;
    p->display_misses = 0;
    int64_t tolerance = (int64_t)(refresh / 2);
    int64_t threshold = p->display_phase_ns > INT64_MAX - tolerance
        ? INT64_MAX : p->display_phase_ns + tolerance;
    for (unsigned i = p->phase_count; i && p->phase_order[i - 1] > threshold; i--)
        p->display_misses++;
    uint64_t target = 0;
    if (predict_pace_over_budget(p->display_misses, p->display_count, p->miss_budget_ppm)) {
        int64_t tail = p->phase_order[predict_pace_quantile_index(p->phase_count, p->miss_budget_ppm)];
        if (tail > threshold) target = (uint64_t)tail - (uint64_t)threshold;
        uint64_t cap = period / 2 ? period / 2 : 1;
        if (target > cap) target = cap;
    }
    uint64_t step = predict_pace_scaled(PREDICT_PACE_STEP_NS, period, 128);
    p->display_protection_ns = predict_pace_follow(p->display_protection_ns, target, step,
        predict_pace_over_budget(p->display_misses, p->display_count, p->miss_budget_ppm));
    predict_pace_allowance(p);
    return phase > threshold;
}
