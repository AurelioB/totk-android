/* Standalone pure policy tests: synthetic timestamps only. No Android, GPU,
 * clocks, allocation, waiting or I/O. The owner runs this host test. */
#include "predict_pace.h"
#include "vk_present_slots.h"
#include "../../runtime/present_predict.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

static const uint64_t base = 1000000000ull, refresh60 = 16666667;

static void same_state(const predict_pacer *a, const predict_pacer *b) {
    assert(a->period_ns == b->period_ns && a->first_slot_ns == b->first_slot_ns);
    assert(a->last_slot_ns == b->last_slot_ns && a->last_display_slot_ns == b->last_display_slot_ns);
    assert(a->cost_ns == b->cost_ns && a->allowance_ns == b->allowance_ns);
    assert(a->margin_ns == b->margin_ns && a->service_allowance_ns == b->service_allowance_ns);
    assert(a->display_protection_ns == b->display_protection_ns);
    assert(a->display_refresh_ns == b->display_refresh_ns && a->display_phase_ns == b->display_phase_ns);
    assert(a->count == b->count && a->next == b->next && a->ready_misses == b->ready_misses);
    assert(a->phase_count == b->phase_count && a->phase_next == b->phase_next);
    assert(a->display_count == b->display_count && a->display_misses == b->display_misses);
    assert(a->miss_budget_ppm == b->miss_budget_ppm && a->budget_configured == b->budget_configured);
    assert(!memcmp(a->ready_order, b->ready_order, sizeof a->ready_order));
    assert(!memcmp(a->allowance_order, b->allowance_order, sizeof a->allowance_order));
    assert(!memcmp(a->phases, b->phases, sizeof a->phases));
    assert(!memcmp(a->phase_order, b->phase_order, sizeof a->phase_order));
    for (unsigned i = 0; i < PREDICT_PACE_SAMPLES; i++) {
        assert(a->samples[i].ready_ns == b->samples[i].ready_ns);
        assert(a->samples[i].allowance_ns == b->samples[i].allowance_ns);
        assert(a->samples[i].miss == b->samples[i].miss);
    }
}
static void bounds(predict_decision d, uint64_t slot, uint64_t period) {
    assert(d.slot_ns == slot && d.period_ns == period);
    assert(d.cost_ns && d.allowance_ns && d.margin_ns);
    assert(d.deadline_ns == slot - d.allowance_ns);
    assert(d.release_ns >= slot - 2 * period && d.release_ns <= slot - period);
    assert(d.lead_ns == slot - period - d.release_ns && d.lead_ns <= period);
    assert(d.margin_ns == predict_pace_margin_limit(period));
}
static predict_decision choose(predict_pacer *p, uint64_t slot, uint64_t period) {
    predict_decision d = predict_pacer_choose(p, slot, period);
    bounds(d, slot, period);
    return d;
}
static predict_observation finish(predict_pacer *p, const predict_decision *d,
        uint64_t work, uint64_t allowance) {
    predict_observation o = predict_pacer_observe(p, d, work, allowance, d->release_ns + work);
    assert(o.valid);
    return o;
}
static void train(predict_pacer *p, uint64_t *slot, uint64_t period,
        uint64_t work, uint64_t allowance, unsigned frames) {
    for (unsigned i = 0; i < frames; i++, *slot += period) {
        predict_decision d = choose(p, *slot, period);
        finish(p, &d, work, allowance);
    }
}
static void invalid(predict_pacer *p, const predict_decision *d,
        uint64_t work, uint64_t allowance, uint64_t ready) {
    predict_pacer before = *p;
    predict_observation o = predict_pacer_observe(p, d, work, allowance, ready);
    assert(!o.valid && !o.miss && !o.error_ns && !o.late_ns);
    same_state(p, &before);
}
static int present(predict_pacer *p, uint64_t *slot, uint64_t period, int64_t phase) {
    uint64_t display = phase >= 0 ? *slot + (uint64_t)phase : *slot - (uint64_t)-phase;
    int missed = predict_pacer_presented(p, *slot, period, refresh60, display);
    *slot += period;
    return missed;
}

static void budget_configuration(void) {
    predict_pacer p = {0};
    uint64_t period = 2 * refresh60, slot = base;
    predict_pacer_set_budget(NULL, 0);
    predict_pacer_reset(NULL);
    predict_decision d = choose(&p, slot, period);
    assert(p.budget_configured && p.miss_budget_ppm == 20000);
    finish(&p, &d, period, 500000);
    predict_pacer_set_budget(&p, 0);
    predict_pacer_reset(&p);
    assert(p.budget_configured && !p.miss_budget_ppm && !p.count && !p.phase_count);
    slot += period;
    train(&p, &slot, period, period, 500000, 10);
    d = choose(&p, slot, refresh60); /* period reset retains explicit zero */
    assert(!p.count && !p.miss_budget_ppm && d.lead_ns == refresh60);
    predict_pacer_set_budget(&p, 50000);
    predict_pacer_reset(&p);
    assert(p.miss_budget_ppm == 50000);
    predict_pacer_set_budget(&p, UINT32_MAX);
    assert(p.miss_budget_ppm == PREDICT_PACE_PPM);
    predict_pacer_reset(&p);
    assert(p.miss_budget_ppm == PREDICT_PACE_PPM);
    predict_pacer fresh = {0};
    predict_pacer_reset(&fresh);
    assert(fresh.miss_budget_ppm == PREDICT_PACE_DEFAULT_MISS_PPM);
}

static void warmup_does_not_bypass_rate_gate(void) {
    const uint64_t period = 2 * refresh60, low = 40000000, high = 50000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, low, 1000000, 1);
    for (unsigned i = 1; i < PREDICT_PACE_WARMUP; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        assert(d.lead_ns == period);
        assert(!finish(&p, &d, high, 1000000).miss);
        assert(!p.ready_misses && p.cost_ns == low + i * PREDICT_PACE_STEP_NS);
    }
    predict_decision d = choose(&p, slot, period);
    assert(p.count == PREDICT_PACE_WARMUP && d.lead_ns < period);
    assert(finish(&p, &d, high, 1000000).miss);
    assert(p.cost_ns == high && p.ready_misses == 1); /* now above budget */
}

static void warmup_and_fractional(unsigned interval) {
    const uint64_t period = refresh60 * interval;
    const uint64_t work = period + period / 4, allowance = period / 16;
    const uint64_t floor = predict_pace_margin_limit(period);
    predict_pacer p = {0};
    uint64_t slot = base;
    for (unsigned i = 0; i < PREDICT_PACE_WARMUP; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        assert(d.lead_ns == period && d.release_ns == slot - 2 * period);
        assert(d.cost_ns == (i ? work : period));
        assert(d.allowance_ns == (i ? allowance : floor));
        predict_observation o = finish(&p, &d, work, allowance);
        assert(!o.miss && !o.late_ns);
        assert(o.error_ns == (i ? 0 : (int64_t)(work - period)));
        assert(p.margin_ns == floor); /* refreshed cost is not charged twice */
    }
    predict_decision d = choose(&p, slot, period);
    assert(d.cost_ns == work && d.allowance_ns == allowance && d.margin_ns == floor);
    assert(d.lead_ns == work + allowance + floor - period);
    assert(d.lead_ns > 0 && d.lead_ns < period);
    assert(d.release_ns + work + floor == d.deadline_ns);
    predict_pacer before = p;
    for (unsigned i = 0; i < 100; i++) {
        predict_decision repeated = choose(&p, slot, period);
        assert(repeated.release_ns == d.release_ns && repeated.deadline_ns == d.deadline_ns);
        same_state(&p, &before); /* no feedback, no scan or quantile training */
    }
    train(&p, &slot, period, work, allowance, 600);
    d = choose(&p, slot, period);
    assert(d.lead_ns == work + allowance + floor - period);
    assert(p.count == 300 && !p.ready_misses && p.margin_ns == floor);
    predict_pacer_reset(&p);
    train(&p, &slot, period, period / 2, allowance, PREDICT_PACE_WARMUP);
    d = choose(&p, slot, period);
    assert(!d.lead_ns && d.release_ns == slot - period);
}

static void sampled_demand_not_full_lead(void) {
    const uint64_t period = 2 * refresh60, allowance = 1000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    /* 40..50ms complete readiness demand, not a hardcoded lead cap. Its 98th
     * percentile plus path allowance/margin calls for about 18ms extra lead. */
    for (unsigned i = 0; i < 1200; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        finish(&p, &d, 40000000 + (i % 101) * 100000, allowance);
    }
    predict_decision d = choose(&p, slot, period);
    unsigned q = predict_pace_quantile_index(p.count, p.miss_budget_ppm);
    assert(d.cost_ns == (uint64_t)p.ready_order[q]);
    assert(d.lead_ns == d.cost_ns + allowance + d.margin_ns - period);
    assert(d.lead_ns >= 15000000 && d.lead_ns <= 20000000 && d.lead_ns < period);
    for (uint64_t demand = 48000000; demand <= 50000000; demand += 1000000) {
        predict_pacer_reset(&p);
        train(&p, &slot, period, demand, allowance, 600);
        d = choose(&p, slot, period);
        assert(d.lead_ns == demand + allowance + d.margin_ns - period);
        assert(d.lead_ns >= 15000000 && d.lead_ns <= 20000000);
    }
    /* Real overload remains legal full lead; no device-goal safety cap. */
    train(&p, &slot, period, 2 * period, allowance, 600);
    assert(choose(&p, slot, period).lead_ns == period);
}

static void nearest_rank_and_expiry(void) {
    assert(predict_pace_quantile_index(1, 20000) == 0);
    assert(predict_pace_quantile_index(49, 20000) == 48);
    assert(predict_pace_quantile_index(50, 20000) == 48);
    assert(predict_pace_quantile_index(100, 10000) == 98);
    assert(predict_pace_quantile_index(300, 0) == 299);
    assert(predict_pace_quantile_index(300, 10000) == 296);
    assert(predict_pace_quantile_index(300, 20000) == 293);
    assert(predict_pace_quantile_index(300, 50000) == 284);
    assert(predict_pace_quantile_index(300, 1000000) == 0);
    const uint64_t period = 2 * refresh60;
    predict_pacer p = {0};
    uint64_t slot = base;
    for (unsigned i = 0; i < 300; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        finish(&p, &d, 10000000 + i, 250000 + i);
    }
    assert(p.count == 300 && !p.next && !p.ready_misses);
    assert(p.ready_order[293] == 10000293 && p.allowance_order[293] == 250293);
    assert(p.cost_ns == 10000293 && p.allowance_ns == 250293);
    for (unsigned i = 0; i < 300; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        finish(&p, &d, 1000000, 100000);
        assert(p.count == 300 && p.next == (i + 1) % 300);
        /* The oldest ascending sample expires, not the largest or an EMA. */
        unsigned remaining = 299 - i;
        assert(p.ready_order[299] == (remaining ? 10000299 : 1000000));
        for (unsigned j = 1; j < 300; j++) {
            assert(p.ready_order[j - 1] <= p.ready_order[j]);
            assert(p.allowance_order[j - 1] <= p.allowance_order[j]);
        }
    }
    for (unsigned i = 0; i < 300; i++) {
        assert(p.ready_order[i] == 1000000 && p.allowance_order[i] == 100000);
    }
    train(&p, &slot, period, 1000000, 100000, 100);
    assert(p.cost_ns == 1000000 && p.allowance_ns == 100000);
}

static void fast_raise_requires_budget(unsigned ppm) {
    const uint64_t period = 2 * refresh60, low = 45000000, high = 50000000;
    const uint64_t allowance = 1000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    predict_pacer_set_budget(&p, ppm);
    train(&p, &slot, period, low, allowance, 300);
    unsigned permitted = (unsigned)((uint64_t)300 * ppm / PREDICT_PACE_PPM);
    for (unsigned i = 0; i <= permitted; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        predict_observation o = finish(&p, &d, high, allowance);
        assert(o.miss && o.error_ns == (int64_t)(high - low));
        assert(p.ready_misses == i + 1);
        if (i < permitted) {
            assert(!predict_pace_over_budget(p.ready_misses, p.count, ppm));
            assert(p.cost_ns == low && p.margin_ns == d.margin_ns);
        } else {
            assert(predict_pace_over_budget(p.ready_misses, p.count, ppm));
            assert(p.cost_ns == high); /* exact quantile fast rise, no error add */
            assert(p.margin_ns == d.margin_ns);
        }
    }
    predict_decision d = choose(&p, slot, period);
    predict_observation o = finish(&p, &d, high, allowance);
    assert(!o.miss && !o.error_ns && p.cost_ns == high);
    slot += period;
    train(&p, &slot, period, low, allowance, 500);
    assert(!p.ready_misses && p.cost_ns == low); /* rolling flags expire */
}

static void budget_changes_on_feedback(void) {
    const uint64_t period = 2 * refresh60, low = 45000000, high = 50000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, low, 1000000, 300);
    for (unsigned i = 0; i < 6; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        assert(finish(&p, &d, high, 1000000).miss);
    }
    assert(p.cost_ns == low && p.ready_misses == 6);
    predict_pacer_set_budget(&p, 0);
    predict_decision d = choose(&p, slot, period);
    assert(d.cost_ns == low); /* setter/choose do not train a quantile */
    finish(&p, &d, low, 1000000);
    assert(p.cost_ns == high);
    slot += period;
    predict_pacer_set_budget(&p, 50000);
    d = choose(&p, slot, period);
    finish(&p, &d, low, 1000000);
    assert(p.cost_ns == high - PREDICT_PACE_STEP_NS);
    slot += period;
    train(&p, &slot, period, low, 1000000, 100);
    assert(p.cost_ns == low && p.ready_misses <= 6);
    predict_pacer_set_budget(&p, 1000000);
    slot += period;
    d = choose(&p, slot, period);
    finish(&p, &d, high, 1000000);
    assert(p.cost_ns == low); /* 100% budget: minimum rank, no fast miss raise */
}

static void permitted_misses_do_not_stop_decay(void) {
    const uint64_t period = 2 * refresh60, high = 58000000, low = 40000000;
    const uint64_t allowance = 1000000, step = PREDICT_PACE_STEP_NS;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, high, allowance, 300);
    int decayed_on_miss = 0;
    for (unsigned i = 0; i < 1000; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        uint64_t before = p.cost_ns;
        predict_observation o = i % 100 == 0
            ? predict_pacer_observe(&p, &d, low, allowance, d.deadline_ns + 1000000)
            : finish(&p, &d, low, allowance);
        assert(o.valid && p.ready_misses <= 3);
        assert(p.cost_ns <= before && before - p.cost_ns <= step);
        if (o.miss && p.cost_ns < before) decayed_on_miss = 1;
        assert(p.margin_ns == predict_pace_margin_limit(period));
    }
    assert(decayed_on_miss && p.cost_ns == low && p.allowance_ns == allowance);
    assert(p.ready_misses == 3 && !predict_pace_over_budget(p.ready_misses, p.count, 20000));
    predict_decision d = choose(&p, slot, period);
    assert(d.lead_ns == low + allowance + d.margin_ns - period && d.lead_ns < period);
}

static void complete_readiness_and_wait_exclusion(void) {
    const uint64_t period = 2 * refresh60, work = 40000000;
    const uint64_t allowance = 1000000, wake = 8000000, resource = 2000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    for (unsigned i = 0; i < 600; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        const uint64_t credit_hold = 3 * period;
        runtime_present_prediction plan = {.release_target_ns = d.release_ns,
            .release_ns = d.release_ns + wake + credit_hold, .credit_hold_ns = credit_hold};
        uint64_t causal_ready = runtime_present_prediction_ready(&plan, work + resource);
        assert(causal_ready == d.release_ns + wake + work + resource);
        uint64_t wall_post_ready = plan.release_ns + work + resource + 4 * period;
        assert(wall_post_ready > d.slot_ns + period);
        predict_observation o = predict_pacer_observe(&p, &d, work + resource,
            allowance, causal_ready); /* required resources included, FIFO not */
        assert(o.valid);
    }
    assert(p.cost_ns == work + resource + wake && p.margin_ns == 500000);
    assert(p.ready_order[293] == (int64_t)(work + resource + wake));
    predict_decision d = choose(&p, slot, period);
    predict_observation o = predict_pacer_observe(&p, &d, work + resource, allowance,
        d.release_ns + work + resource + wake);
    assert(o.valid && !o.miss && !o.error_ns); /* no second wake/error insurance */
    runtime_present_prediction invalid = {.release_target_ns = 10, .release_ns = 20,
        .credit_hold_ns = 11};
    assert(!runtime_present_prediction_ready(&invalid, 5));
    invalid.credit_hold_ns = 0; invalid.release_ns = UINT64_MAX;
    assert(!runtime_present_prediction_ready(&invalid, 5));
    slot += period;
    /* Early wakes are negative variation, not positive-only accumulated error. */
    for (unsigned i = 0; i < 700; i++, slot += period) {
        d = choose(&p, slot, period);
        o = predict_pacer_observe(&p, &d, work, allowance, d.release_ns - 2000000 + work);
        assert(o.valid);
    }
    assert(p.cost_ns == work - 2000000 && p.margin_ns == 500000);
    /* Nonpositive complete spans remain valid 1ns samples (not unsigned wrap). */
    d = choose(&p, slot, period);
    o = predict_pacer_observe(&p, &d, 1, allowance, d.release_ns - 1);
    assert(o.valid && o.error_ns < 0 && p.ready_order[0] == 1);

    predict_pacer_reset(&p);
    slot += period;
    train(&p, &slot, period, 2 * period, allowance, 300);
    d = choose(&p, slot, period);
    assert(d.lead_ns == period);
    o = finish(&p, &d, 2 * period - allowance / 2, allowance);
    assert(o.miss && o.error_ns == -(int64_t)(allowance / 2));
    assert(o.late_ns == allowance - allowance / 2); /* miss != prediction error */
}

static void allowance_quantile_and_recovery(void) {
    const uint64_t period = 2 * refresh60, work = 45000000, allowance = 1000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, work, allowance, 300);
    /* Six expensive platform calls fit the 2% tail budget; no work inflation. */
    for (unsigned i = 0; i < 6; i++, slot += period) {
        predict_decision d = choose(&p, slot, period);
        finish(&p, &d, work, 12000000);
        assert(p.allowance_ns == allowance && p.cost_ns == work);
    }
    predict_decision d = choose(&p, slot, period);
    finish(&p, &d, work, 12000000);
    assert(p.allowance_ns == allowance + PREDICT_PACE_STEP_NS);
    assert(!p.ready_misses); /* rising allowance without realized misses is slow */
    slot += period;
    train(&p, &slot, period, work, 12000000, 400);
    assert(p.allowance_ns == 12000000 && p.cost_ns == work);
    train(&p, &slot, period, work, allowance, 600);
    assert(p.allowance_ns == allowance && !p.display_protection_ns);
}

static void independent_histories(void) {
    predict_pacer models[2][3] = {0};
    uint64_t slots[2][3];
    for (unsigned path = 0; path < 2; path++) {
        for (unsigned interval = 1; interval <= 3; interval++) {
            uint64_t period = refresh60 * interval;
            slots[path][interval - 1] = base;
            predict_pacer_set_budget(&models[path][interval - 1], path ? 0 : 50000);
            train(&models[path][interval - 1], &slots[path][interval - 1],
                period, period + period / 4, period / 32, 300);
        }
    }
    predict_pacer saved[2][3];
    memcpy(saved, models, sizeof models);
    predict_pacer_reset(&models[0][0]);
    train(&models[1][1], &slots[1][1], refresh60 * 2, refresh60, 500000, 800);
    for (unsigned path = 0; path < 2; path++) {
        for (unsigned i = 0; i < 3; i++) {
            if ((path == 0 && !i) || (path == 1 && i == 1)) continue;
            same_state(&models[path][i], &saved[path][i]);
        }
    }
    assert(!models[0][0].count && models[0][0].miss_budget_ppm == 50000);
    assert(models[1][1].cost_ns == refresh60 && !models[1][1].miss_budget_ppm);
}

static void fixed_slot_sequence(unsigned interval) {
    const uint64_t period = refresh60 * interval, allowance = period / 32;
    vk_present_slots slots = {0};
    predict_pacer p = {0};
    vk_present_slot primed = vk_present_slots_reserve(&slots, 1, base, refresh60, interval, base);
    assert(primed.desired_ns == base + period);
    int full = 0, fractional = 0, none = 0, changed = 0;
    uint64_t last_lead = period;
    for (unsigned frame = 0; frame < 1400; frame++) {
        uint64_t callback = base + (uint64_t)frame * period;
        vk_present_slot s = vk_present_slots_reserve(&slots, 1, callback, refresh60, interval, callback);
        uint64_t expected = base + (uint64_t)(frame + 2) * period;
        assert(s.mode == VK_PRESENT_SLOTS_ACTIVE && !s.skipped && s.desired_ns == expected);
        predict_decision d = choose(&p, s.desired_ns, period);
        assert(d.slot_ns == expected && slots.desired_ns == expected);
        full |= d.lead_ns == period;
        fractional |= d.lead_ns > 0 && d.lead_ns < period;
        none |= !d.lead_ns;
        changed |= d.lead_ns != last_lead;
        last_lead = d.lead_ns;
        uint64_t work = frame < 40 ? period + period / 4 :
            frame < 80 ? period + period / 2 : period / 2;
        finish(&p, &d, work, allowance);
        assert(d.slot_ns == base + (uint64_t)(frame + 2) * period);
    }
    assert(full && fractional && none && changed);
    uint64_t now = base + 2000 * period;
    vk_present_slot s = vk_present_slots_reserve(&slots, 1, now, refresh60, interval, now);
    assert(s.mode == VK_PRESENT_SLOTS_ACTIVE && s.skipped);
    assert((s.desired_ns - base) % period == 0 && s.desired_ns >= now);
    predict_pacer_reset(&p);
    predict_decision d = choose(&p, s.desired_ns, period);
    assert(d.lead_ns == period && !p.count);
}

static void stalls_and_mode_resets(void) {
    const uint64_t period = 2 * refresh60, work = period + period / 4, allowance = period / 32;
    predict_pacer p = {0};
    uint64_t slot = base;
    predict_pacer_set_budget(&p, 10000);
    train(&p, &slot, period, work, allowance, 300);
    predict_decision d = choose(&p, slot, period);
    predict_observation o = predict_pacer_observe(&p, &d, work, allowance, slot + period);
    assert(o.valid && o.miss); /* exactly one-period lateness is not a stall */
    slot += period;
    d = choose(&p, slot, period);
    o = predict_pacer_observe(&p, &d, work, allowance, slot + period + 1);
    assert(!o.valid && !o.miss && !o.error_ns && !o.late_ns);
    assert(!p.count && !p.cost_ns && !p.allowance_ns && !p.margin_ns && !p.phase_count);
    assert(p.last_slot_ns == slot && p.period_ns == period && p.miss_budget_ppm == 10000);
    invalid(&p, &d, work, allowance, d.release_ns + work);
    slot += period;
    d = choose(&p, slot, period);
    assert(d.lead_ns == period);
    finish(&p, &d, work, allowance);
    slot += period;
    train(&p, &slot, period, work, allowance, 300);
    predict_decision old = choose(&p, slot, period);
    d = choose(&p, slot + period, refresh60);
    assert(d.lead_ns == refresh60 && !p.count && p.miss_budget_ppm == 10000);
    invalid(&p, &old, work, allowance, old.release_ns + work);
    finish(&p, &d, refresh60, allowance);
    predict_pacer_reset(&p); /* owner invalidates old generation before feedback */
    assert(!p.count && !p.period_ns && !p.last_display_slot_ns && p.miss_budget_ppm == 10000);
    d = choose(&p, slot + 10 * period, refresh60);
    invalid(&p, &old, work, allowance, old.release_ns + work);
}

static void invalid_and_overflow(void) {
    const uint64_t period = 2 * refresh60, work = period + period / 4, allowance = period / 32;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, work, allowance, 300);
    predict_pacer before = p;
    const uint64_t bad_slots[] = {0, 2 * period, UINT64_MAX, (uint64_t)INT64_MAX + 1};
    for (unsigned i = 0; i < sizeof bad_slots / sizeof *bad_slots; i++) {
        predict_decision d = predict_pacer_choose(&p, bad_slots[i], period);
        assert(!d.slot_ns && !d.period_ns && !d.release_ns);
        same_state(&p, &before);
    }
    const uint64_t bad_periods[] = {0, UINT64_MAX, (uint64_t)INT64_MAX / 2 + 1};
    for (unsigned i = 0; i < sizeof bad_periods / sizeof *bad_periods; i++) {
        predict_decision d = predict_pacer_choose(&p, slot, bad_periods[i]);
        assert(!d.slot_ns && !d.period_ns && !d.release_ns);
        same_state(&p, &before);
    }
    predict_decision d = choose(&p, slot, period), bad;
    uint64_t ready = d.release_ns + work;
    invalid(&p, NULL, work, allowance, ready);
    invalid(&p, &d, 0, allowance, ready);
    invalid(&p, &d, UINT64_MAX, allowance, ready);
    invalid(&p, &d, work, 0, ready);
    invalid(&p, &d, work, UINT64_MAX, ready);
    invalid(&p, &d, work, allowance, 0);
    invalid(&p, &d, work, allowance, UINT64_MAX);
    invalid(&p, &d, ready + 1, allowance, ready);
    bad = d; bad.slot_ns = UINT64_MAX; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.period_ns = UINT64_MAX; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.period_ns = 0; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.deadline_ns++; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.release_ns = slot - period + 1; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.release_ns = slot - 2 * period - 1; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.lead_ns++; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.cost_ns = 0; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.cost_ns = UINT64_MAX; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.allowance_ns = 0; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.allowance_ns = UINT64_MAX; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.margin_ns = 0; invalid(&p, &bad, work, allowance, ready);
    bad = d; bad.margin_ns++; invalid(&p, &bad, work, allowance, ready);
    predict_observation o = predict_pacer_observe(NULL, &d, work, allowance, ready);
    assert(!o.valid && !predict_pacer_choose(NULL, slot, period).slot_ns);
    predict_decision later = choose(&p, slot + period, period);
    finish(&p, &later, work, allowance);
    invalid(&p, &d, work, allowance, ready);
    invalid(&p, &later, work, allowance, later.release_ns + work);
    before = p;
    bad = predict_pacer_choose(&p, slot, period);
    assert(!bad.slot_ns && !bad.release_ns);
    same_state(&p, &before);
    bad = predict_pacer_choose(&p, later.slot_ns, period);
    assert(!bad.slot_ns && !bad.release_ns);
    same_state(&p, &before);

    predict_pacer_reset(&p);
    d = choose(&p, (uint64_t)INT64_MAX, period);
    assert(d.release_ns == (uint64_t)INT64_MAX - 2 * period);
    finish(&p, &d, period, allowance);
    predict_pacer_reset(&p);
    d = choose(&p, 3, 1);
    assert(d.release_ns == 1 && d.lead_ns == 1 && d.deadline_ns == 2);
    finish(&p, &d, 1, 1);
    /* Large valid periods also keep saturated demand and rate products safe. */
    predict_pacer_reset(&p);
    uint64_t huge_period = (uint64_t)INT64_MAX / 2;
    d = choose(&p, (uint64_t)INT64_MAX, huge_period);
    assert(d.release_ns == 1 && d.lead_ns == huge_period);
    finish(&p, &d, huge_period, 1);
}

static void calibrated_phase_not_physical_miss(void) {
    const uint64_t period = 2 * refresh60, allowance = 1000000, work = 45000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, work, allowance, 300);
    assert(!p.phase_count && !p.display_count);
    for (unsigned i = 0; i < 600; i++) {
        int result = present(&p, &slot, period, 10000000 + (i % 5) * 1000000);
        if (i + 1 < PREDICT_PACE_WARMUP) {
            assert(result == -1 && !p.display_count && !p.display_misses);
        } else {
            assert(!result && p.display_count == p.phase_count && !p.display_misses);
            assert(p.display_phase_ns == 10000000);
        }
        assert(p.cost_ns == work && p.allowance_ns == allowance && !p.display_protection_ns);
    }
    assert(p.phase_count == 300 && p.display_count == 300);
    predict_pacer_reset(&p);
    train(&p, &slot, period, work, allowance, 300);
    for (unsigned i = 0; i < 10; i++) {
        int result = present(&p, &slot, period, -12000000);
        assert(result == (i + 1 < PREDICT_PACE_WARMUP ? -1 : 0));
    }
    assert(p.display_phase_ns == -12000000 && !p.display_misses && p.allowance_ns == allowance);
}

static void physical_tails_and_permitted_decay(unsigned ppm) {
    const uint64_t period = 2 * refresh60, allowance = 1000000, work = 45000000;
    const int64_t phase = 12000000;
    predict_pacer p = {0};
    uint64_t slot = base;
    predict_pacer_set_budget(&p, ppm);
    train(&p, &slot, period, work, allowance, 300);
    for (unsigned i = 0; i < 300; i++) present(&p, &slot, period, phase);
    unsigned permitted = (unsigned)((uint64_t)300 * ppm / PREDICT_PACE_PPM);
    uint64_t target = refresh60 - refresh60 / 2;
    for (unsigned i = 0; i <= permitted; i++) {
        assert(present(&p, &slot, period, phase + (int64_t)refresh60) == 1);
        assert(p.display_misses == i + 1 && p.cost_ns == work);
        if (i < permitted) assert(p.allowance_ns == allowance && !p.display_protection_ns);
        else assert(p.display_protection_ns == target && p.allowance_ns == allowance + target);
    }
    /* Repeated above-budget tails set one target, not a per-miss increment. */
    for (unsigned i = 0; i < 10; i++) {
        assert(present(&p, &slot, period, phase + (int64_t)refresh60) == 1);
        assert(p.display_protection_ns == target && p.allowance_ns == allowance + target);
    }
    int decayed_on_miss = 0;
    for (unsigned i = 0; i < 800; i++) {
        uint64_t before = p.display_protection_ns;
        int permitted_tail = ppm == 20000 && i % 50 == 49;
        int result = present(&p, &slot, period, phase + (permitted_tail ? (int64_t)refresh60 : 0));
        assert(result == permitted_tail);
        assert(p.display_protection_ns <= before && p.cost_ns == work);
        if (permitted_tail && p.display_protection_ns < before) decayed_on_miss = 1;
    }
    assert(!p.display_protection_ns && p.allowance_ns == allowance);
    assert(p.display_misses == (ppm == 20000 ? 6 : 0));
    if (ppm == 20000) assert(decayed_on_miss);
}

static void single_refresh_tail_with_callback_phase(void) {
    const uint64_t period = refresh60, allowance = 500000;
    predict_pacer p = {0};
    uint64_t slot = base;
    train(&p, &slot, period, period / 2, allowance, 300);
    for (unsigned i = 0; i < 300; i++) present(&p, &slot, period, 12000000);
    for (unsigned i = 0; i < 7; i++) {
        /* display-S exceeds P, but display-baseline is only one refresh late. */
        assert(present(&p, &slot, period, 12000000 + (int64_t)refresh60) == 1);
        assert(p.display_misses == i + 1);
    }
    assert(p.display_protection_ns == period / 2);
    assert(p.allowance_ns == allowance + period / 2 && p.cost_ns == period / 2);
}

static void physical_invalid_and_epoch_reset(void) {
    const uint64_t period = 2 * refresh60;
    predict_pacer p = {0};
    uint64_t slot = base;
    assert(predict_pacer_presented(&p, slot, period, refresh60, slot) == -1);
    train(&p, &slot, period, 45000000, 1000000, 300);
    uint64_t first = p.first_slot_ns;
    for (unsigned i = 0; i < 8; i++) present(&p, &slot, period, 12000000);
    predict_pacer before = p;
    assert(predict_pacer_presented(NULL, slot, period, refresh60, slot) == -1);
    assert(predict_pacer_presented(&p, first - 1, period, refresh60, first - 1) == -1);
    assert(predict_pacer_presented(&p, slot - period, period, refresh60, slot) == -1);
    assert(predict_pacer_presented(&p, slot, period, 0, slot) == -1);
    assert(predict_pacer_presented(&p, slot, period, period + 1, slot) == -1);
    assert(predict_pacer_presented(&p, slot, period, refresh60 + 1, slot) == -1);
    assert(predict_pacer_presented(&p, slot, refresh60, refresh60, slot) == -1);
    assert(predict_pacer_presented(&p, slot, period, refresh60, 0) == -1);
    assert(predict_pacer_presented(&p, slot, period, refresh60, UINT64_MAX) == -1);
    assert(predict_pacer_presented(&p, UINT64_MAX, period, refresh60, slot) == -1);
    assert(predict_pacer_presented(&p, slot, period, refresh60, slot + period + refresh60 + 1) == -1);
    assert(predict_pacer_presented(&p, slot, period, refresh60, slot - period - refresh60 - 1) == -1);
    assert(predict_pacer_presented(&p, slot, period, refresh60,
        slot + 12000000 - period - refresh60 / 2 - 1) == -1);
    same_state(&p, &before);
    predict_pacer_set_budget(&p, 0);
    predict_pacer_reset(&p);
    assert(!p.phase_count && !p.display_count && !p.display_misses && !p.display_phase_ns);
    assert(!p.last_display_slot_ns && !p.display_refresh_ns && !p.miss_budget_ppm);
    train(&p, &slot, period, 45000000, 1000000, 300);
    for (unsigned i = 0; i < 8; i++) present(&p, &slot, period, -5000000);
    assert(p.display_phase_ns == -5000000 && !p.display_misses);
}

int main(void) {
    budget_configuration();
    warmup_does_not_bypass_rate_gate();
    sampled_demand_not_full_lead();
    nearest_rank_and_expiry();
    budget_changes_on_feedback();
    permitted_misses_do_not_stop_decay();
    complete_readiness_and_wait_exclusion();
    allowance_quantile_and_recovery();
    independent_histories();
    calibrated_phase_not_physical_miss();
    single_refresh_tail_with_callback_phase();
    physical_invalid_and_epoch_reset();
    const unsigned budgets[] = {0, 10000, 20000, 50000};
    for (unsigned i = 0; i < sizeof budgets / sizeof *budgets; i++) {
        fast_raise_requires_budget(budgets[i]);
        physical_tails_and_permitted_decay(budgets[i]);
    }
    for (unsigned interval = 1; interval <= 3; interval++) {
        warmup_and_fractional(interval);
        fixed_slot_sequence(interval);
    }
    stalls_and_mode_resets();
    invalid_and_overflow();
    return 0;
}
