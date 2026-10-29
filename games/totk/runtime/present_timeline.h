/* API-provided Android frame timelines, copied before callback data expires.
 * The producer owns selector state; snapshots and selected pairs are values.
 * No clocks, Android calls, waits or allocation. Times are CLOCK_MONOTONIC ns. */
#pragma once
#include <stdint.h>

enum { RUNTIME_PRESENT_TIMELINE_NONE, RUNTIME_PRESENT_TIMELINE_REAL, RUNTIME_PRESENT_TIMELINE_GENERATED };
/* The native Choreographer source is the default internal display. Never attach
 * its IDs to a different logical display or an unknown window attachment. */
static inline int runtime_present_timeline_display_allowed(int source_display, int window_display) {
    return source_display >= 0 && source_display == window_display;
}
/* Own GPU completion remains mandatory in the caller. A timeline real has
 * already survived the one-real-record hold and owns immutable N pixels; it
 * must not additionally wait for N+1 GPU completion. A generated ticket still
 * requires all source work. NONE preserves the exact legacy forward-wait rule. */
static inline int runtime_present_pair_wait_required(unsigned role, int generated, int has_source) {
    if (!has_source) return 0;
    if (!role) return !generated;
    return role != RUNTIME_PRESENT_TIMELINE_REAL || generated;
}

enum { RUNTIME_PRESENT_TIMELINE_CAPACITY = 16 };
typedef struct {
    uint64_t expected_ns, deadline_ns;
    int64_t vsync_id;
} runtime_present_timeline_entry;
typedef struct {
    uint64_t frame_ns, sampled_ns, generation, refresh_ns;
    unsigned count, preferred;
    runtime_present_timeline_entry entries[RUNTIME_PRESENT_TIMELINE_CAPACITY];
} runtime_present_timelines;
typedef struct {
    uint64_t window_generation, clock_generation, last_real_ns, last_child_ns;
} runtime_present_timeline_state;
typedef struct {
    runtime_present_timeline_entry real, child;
    uint64_t refresh_ns;
} runtime_present_timeline_pair;

/* Reserve the earliest adjacent real/child entries at or after the API's
 * preferred timeline, at least lead (1/2) physical refreshes beyond now.
 * Expected times, deadlines and IDs are never synthesized or advanced: IDs
 * need only be nonnegative and unique, not contiguous or numerically ordered.
 * Both deadlines must remain open; missed candidates can be skipped, never
 * combined with a nonadjacent child. Validate the entire snapshot first.
 *
 * A repeated snapshot or recalibrated clock may serve another pair only beyond
 * the last child. Optional distinct-refresh admission also requires half a
 * refresh of separation: a newly calibrated prediction a few microseconds
 * later is not a new physical refresh. Only window replacement resets that
 * floor: the caller drains the old surface separately. Reset and reservation commit together
 * on success; every failure leaves state and output untouched. Callers must
 * not invoke this policy when disabled or runtime-unavailable. */
static inline int runtime_present_timeline_choose(runtime_present_timeline_state *state,
        const runtime_present_timelines *snapshot, uint64_t now_ns,
        unsigned lead_refreshes, uint64_t window_generation, int distinct_refreshes,
        runtime_present_timeline_pair *pair) {
    const uint64_t limit = (uint64_t)INT64_MAX;
    if (!state || !snapshot || !pair || lead_refreshes < 1 || lead_refreshes > 2 ||
        snapshot->count < 2 || snapshot->count > RUNTIME_PRESENT_TIMELINE_CAPACITY ||
        snapshot->preferred >= snapshot->count || !snapshot->refresh_ns ||
        snapshot->refresh_ns > limit / 2 || !snapshot->frame_ns ||
        snapshot->frame_ns > snapshot->sampled_ns || snapshot->sampled_ns > now_ns ||
        now_ns > limit) return 0;
    uint64_t max_age = 2 * snapshot->refresh_ns;
    if (now_ns - snapshot->frame_ns > max_age) return 0;
    uint64_t lead_ns = lead_refreshes * snapshot->refresh_ns;
    if (now_ns > limit - lead_ns) return 0;
    uint64_t minimum_ns = now_ns + lead_ns;
    for (unsigned i = 0; i < snapshot->count; i++) {
        const runtime_present_timeline_entry *entry = &snapshot->entries[i];
        if (!entry->expected_ns || entry->expected_ns > limit || !entry->deadline_ns ||
            entry->deadline_ns > entry->expected_ns || entry->vsync_id < 0) return 0;
        if (i && (entry->expected_ns <= snapshot->entries[i - 1].expected_ns ||
            entry->deadline_ns <= snapshot->entries[i - 1].deadline_ns)) return 0;
        for (unsigned j = 0; j < i; j++)
            if (entry->vsync_id == snapshot->entries[j].vsync_id) return 0;
    }
    uint64_t floor_ns = state->window_generation == window_generation ?
        state->last_child_ns : 0;
    uint64_t half_refresh = snapshot->refresh_ns / 2 + snapshot->refresh_ns % 2;
    for (unsigned i = snapshot->preferred; i + 1 < snapshot->count; i++) {
        const runtime_present_timeline_entry *real = &snapshot->entries[i];
        const runtime_present_timeline_entry *child = &snapshot->entries[i + 1];
        if (real->expected_ns < minimum_ns || real->expected_ns <= floor_ns ||
            (distinct_refreshes && floor_ns && real->expected_ns - floor_ns < half_refresh) ||
            real->deadline_ns <= now_ns || child->deadline_ns <= now_ns) continue;
        *pair = (runtime_present_timeline_pair){.real = *real, .child = *child,
            .refresh_ns = snapshot->refresh_ns};
        *state = (runtime_present_timeline_state){.window_generation = window_generation,
            .clock_generation = snapshot->generation, .last_real_ns = real->expected_ns,
            .last_child_ns = child->expected_ns};
        return 1;
    }
    return 0;
}

/* Admitted-only fallback when API candidates are temporarily unavailable.
 * A fresh legacy callback anchors a finite adjacent pair on its own lattice,
 * strictly beyond the prior child's time even after recalibration. These are
 * NOT API timeline entries: both IDs are invalid (-1), and deadline=expected
 * is only the legacy submission gate, never a platform-provided deadline.
 * Keep the last observed API clock generation; fallback has no API epoch.
 * As above, window replacement alone resets the floor and failure is inert. */
static inline int runtime_present_timeline_reanchor(runtime_present_timeline_state *state,
        uint64_t callback_ns, uint64_t refresh_ns, uint64_t now_ns,
        unsigned lead_refreshes, uint64_t window_generation,
        runtime_present_timeline_pair *pair) {
    const uint64_t limit = (uint64_t)INT64_MAX;
    if (!state || !pair || lead_refreshes < 1 || lead_refreshes > 2 ||
        !callback_ns || !refresh_ns || refresh_ns > limit / 2 ||
        callback_ns > now_ns || now_ns > limit ||
        now_ns - callback_ns > 2 * refresh_ns) return 0;
    uint64_t lead_ns = lead_refreshes * refresh_ns;
    if (now_ns > limit - lead_ns) return 0;
    uint64_t minimum_ns = now_ns + lead_ns;
    if (state->window_generation == window_generation) {
        uint64_t floor_ns = state->last_child_ns;
        if (floor_ns >= minimum_ns) {
            if (floor_ns >= limit) return 0;
            minimum_ns = floor_ns + 1;
        }
    }
    uint64_t delta = minimum_ns - callback_ns;
    uint64_t steps = delta / refresh_ns + (delta % refresh_ns != 0);
    if (callback_ns > limit - refresh_ns ||
        steps > (limit - refresh_ns - callback_ns) / refresh_ns) return 0;
    uint64_t real_ns = callback_ns + steps * refresh_ns, child_ns = real_ns + refresh_ns;
    *pair = (runtime_present_timeline_pair){
        .real = {.expected_ns = real_ns, .deadline_ns = real_ns, .vsync_id = -1},
        .child = {.expected_ns = child_ns, .deadline_ns = child_ns, .vsync_id = -1},
        .refresh_ns = refresh_ns};
    state->window_generation = window_generation;
    state->last_real_ns = real_ns;
    state->last_child_ns = child_ns;
    return 1;
}
