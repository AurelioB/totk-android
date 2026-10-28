/* Always-on AHB presentation diagnostics. No pacing, clocks, waits or allocation.
 * Deferred fences can resolve out of order: join only adjacent transaction IDs,
 * never callback order, and never bridge missing timestamps or generations. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum {
    VK_PRESENT_CADENCE_HISTORY = 32, VK_PRESENT_CADENCE_BINS = 6,
    VK_PRESENT_CADENCE_SAMPLES = 300, VK_PRESENT_CADENCE_TAILS = 4
};
typedef struct {
    uint64_t id, generation, desired_ns, refresh_ns, display_ns, apply_ns;
    uint32_t interval;
} vk_present_cadence_sample;
typedef struct {
    uint64_t first_id, second_id, desired_delta_ns, display_delta_ns;
    unsigned before_interval; /* zero means unavailable, not proof of a transition */
} vk_present_cadence_tail;
typedef struct { uint64_t p50_ns, p95_ns, p99_ns; } vk_present_cadence_percentile;
typedef struct {
    uint64_t results, signaled, missing, pairs, missing_pairs, boundaries, history_lost;
    uint64_t repeated, reversed, before_desired, late_refresh, worst_late_ns;
    uint64_t apply_late, worst_apply_late_ns, interval1, interval2, other, legacy;
    uint64_t sum_ns, min_ns, max_ns, bins[VK_PRESENT_CADENCE_BINS], interval2_bins[VK_PRESENT_CADENCE_BINS];
    uint64_t samples[VK_PRESENT_CADENCE_SAMPLES], sample_lost;
    unsigned sample_count, tail_count;
    vk_present_cadence_tail tails[VK_PRESENT_CADENCE_TAILS];
} vk_present_cadence_counts;
typedef struct {
    vk_present_cadence_sample history[VK_PRESENT_CADENCE_HISTORY];
    vk_present_cadence_counts counts;
} vk_present_cadence;

static inline unsigned vk_present_cadence_bin(uint64_t dt) {
    return dt < 12500000 ? 0 : dt < 20900000 ? 1 : dt < 29000000 ? 2 :
        dt < 37500000 ? 3 : dt < 45800000 ? 4 : 5;
}
/* Sort only when reporting, after the per-result hot path has finished. Exact
 * nearest-rank percentiles of up to 300 retained adjacent-ID intervals. */
static inline vk_present_cadence_percentile vk_present_cadence_percentiles(vk_present_cadence_counts *c) {
    unsigned n = c->sample_count;
    if (!n) return (vk_present_cadence_percentile){0};
    for (unsigned i = 1; i < n; i++) {
        uint64_t v = c->samples[i]; unsigned j = i;
        while (j && c->samples[j - 1] > v) { c->samples[j] = c->samples[j - 1]; j--; }
        c->samples[j] = v;
    }
    return (vk_present_cadence_percentile){c->samples[(n * 50 + 99) / 100 - 1],
        c->samples[(n * 95 + 99) / 100 - 1], c->samples[(n * 99 + 99) / 100 - 1]};
}
static inline void vk_present_cadence_pair(vk_present_cadence *state,
        const vk_present_cadence_sample *a, const vk_present_cadence_sample *b) {
    vk_present_cadence_counts *c = &state->counts;
    if (a->generation != b->generation || a->refresh_ns != b->refresh_ns) { c->boundaries++; return; }
    if (!a->display_ns || !b->display_ns) { c->missing_pairs++; return; }
    if (b->display_ns < a->display_ns) { c->reversed++; return; }
    uint64_t dt = b->display_ns - a->display_ns;
    if (!dt) c->repeated++;
    c->pairs++; c->sum_ns += dt;
    if (c->sample_count < VK_PRESENT_CADENCE_SAMPLES) c->samples[c->sample_count++] = dt;
    else c->sample_lost++;
    if (c->pairs == 1 || dt < c->min_ns) c->min_ns = dt;
    if (dt > c->max_ns) c->max_ns = dt;
    unsigned bin = vk_present_cadence_bin(dt);
    c->bins[bin]++;
    if (a->interval == 2 && b->interval == 2) {
        c->interval2_bins[bin]++;
        if (bin != 3 && c->tail_count < VK_PRESENT_CADENCE_TAILS) {
            const vk_present_cadence_sample *before = a->id > 1
                ? &state->history[(a->id - 1) % VK_PRESENT_CADENCE_HISTORY] : NULL;
            unsigned interval = before && before->id == a->id - 1 &&
                before->generation == a->generation && before->refresh_ns == a->refresh_ns ? before->interval : 0;
            c->tails[c->tail_count++] = (vk_present_cadence_tail){.first_id = a->id, .second_id = b->id,
                .desired_delta_ns = b->desired_ns >= a->desired_ns ? b->desired_ns - a->desired_ns : 0,
                .display_delta_ns = dt, .before_interval = interval};
        }
    }
}
static inline void vk_present_cadence_result(vk_present_cadence *c, vk_present_cadence_sample sample) {
    if (!sample.id) return;
    vk_present_cadence_counts *n = &c->counts;
    n->results++;
    n->interval1 += sample.interval == 1; n->interval2 += sample.interval == 2;
    n->other += sample.interval > 2; n->legacy += !sample.interval;
    if (sample.desired_ns && sample.apply_ns > sample.desired_ns) {
        uint64_t late = sample.apply_ns - sample.desired_ns;
        n->apply_late++;
        if (late > n->worst_apply_late_ns) n->worst_apply_late_ns = late;
    }
    if (sample.display_ns) {
        n->signaled++;
        if (sample.refresh_ns && sample.desired_ns) {
            if (sample.display_ns < sample.desired_ns) n->before_desired++;
            else {
                uint64_t late = sample.display_ns - sample.desired_ns;
                n->late_refresh += late >= sample.refresh_ns;
                if (late > n->worst_late_ns) n->worst_late_ns = late;
            }
        }
    } else n->missing++;
    vk_present_cadence_sample *slot = &c->history[sample.id % VK_PRESENT_CADENCE_HISTORY];
    if (slot->id >= sample.id) { n->history_lost++; return; }
    if (sample.id > 1) {
        const vk_present_cadence_sample *previous = &c->history[(sample.id - 1) % VK_PRESENT_CADENCE_HISTORY];
        if (previous->id == sample.id - 1) vk_present_cadence_pair(c, previous, &sample);
    }
    if (sample.id < UINT64_MAX) {
        const vk_present_cadence_sample *next = &c->history[(sample.id + 1) % VK_PRESENT_CADENCE_HISTORY];
        if (next->id == sample.id + 1) vk_present_cadence_pair(c, &sample, next);
    }
    *slot = sample;
}
