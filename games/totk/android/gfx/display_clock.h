/* Android frame-timestamp period calibration, not callback-arrival or fence
 * timing. Pure, bounded and allocation-free; the caller serializes this state.
 * Nominal mode changes reset immediately. A measured period is frozen between
 * validations, so nanosecond noise cannot reset acquire/predict/queued slots. */
#pragma once
#include <stdint.h>

enum {
    DISPLAY_CLOCK_BLOCKS = 8,
    DISPLAY_CLOCK_BLOCK_TICKS = 32,
    DISPLAY_CLOCK_MAX_SKIP = 8
};
#define DISPLAY_CLOCK_VALIDATE_NS UINT64_C(60000000000)

typedef struct {
    uint64_t nominal_ns, refresh_ns, previous_frame_ns;
    uint64_t block_ns, block_ticks, elapsed_ns[DISPLAY_CLOCK_BLOCKS], ticks[DISPLAY_CLOCK_BLOCKS];
    uint64_t next_validation_ns, candidate_ns, measured_ns, window_ns, window_ticks, rejected;
    unsigned blocks, calibrated, candidate_windows, valid_blocks;
} display_period_clock;
typedef struct {
    uint64_t refresh_ns, previous_frame_ns, measured_ns, window_ns, window_ticks, rejected;
    unsigned changed, calibrated, valid_blocks;
} display_clock_result;

static inline uint64_t display_clock_distance(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}
static inline void display_clock_clear_window(display_period_clock *p) {
    p->blocks = 0; p->block_ns = p->block_ticks = 0;
}
static inline display_clock_result display_clock_snapshot(const display_period_clock *p,
        uint64_t previous_frame_ns, unsigned changed) {
    return (display_clock_result){.refresh_ns = p->refresh_ns, .previous_frame_ns = previous_frame_ns,
        .measured_ns = p->measured_ns, .window_ns = p->window_ns, .window_ticks = p->window_ticks,
        .rejected = p->rejected, .changed = changed, .calibrated = p->calibrated,
        .valid_blocks = p->valid_blocks};
}
/* Feed ONLY Android's monotonically increasing physical frame timestamp and
 * the refresh-callback nominal period. Repeated/backward stamps are ignored.
 * Integer skipped callbacks (up to eight physical ticks) retain their tick
 * count; larger gaps/discontinuities restart the measurement, never invent a
 * slower display. Eight >=32-tick blocks give >=4.2s at 60Hz. A median consensus
 * discards up to two noisy blocks; accepted endpoints are then averaged over
 * their complete spans, not averaged callback reciprocals.
 *
 * changed means refresh_ns actually changed. On that event the caller may
 * anchor FUTURE reservations to previous_frame_ns, preserving every already
 * queued target. This helper neither extrapolates phase nor edits reservations.
 * After initial calibration, revalidate once/minute. A material change requires
 * two consecutive stable windows; tiny drift never churns the effective mode. */
static inline display_clock_result display_clock_observe(display_period_clock *p,
        uint64_t frame_ns, uint64_t nominal_refresh_ns) {
    uint64_t previous = p->previous_frame_ns;
    if (nominal_refresh_ns < 1000000 || nominal_refresh_ns > 1000000000)
        return display_clock_snapshot(p, previous, 0);
    unsigned changed = 0;
    if (p->nominal_ns != nominal_refresh_ns) {
        changed = p->refresh_ns != nominal_refresh_ns;
        *p = (display_period_clock){.nominal_ns = nominal_refresh_ns, .refresh_ns = nominal_refresh_ns};
        if (frame_ns) p->previous_frame_ns = frame_ns;
        return display_clock_snapshot(p, previous, changed);
    }
    if (!frame_ns || frame_ns <= previous) {
        p->rejected++;
        return display_clock_snapshot(p, previous, 0);
    }
    p->previous_frame_ns = frame_ns;
    if (!previous || frame_ns < p->next_validation_ns)
        return display_clock_snapshot(p, previous, 0);
    uint64_t dt = frame_ns - previous, reference = p->refresh_ns;
    uint64_t ticks = dt / reference + (dt % reference >= reference / 2 + reference % 2);
    if (!ticks || ticks > DISPLAY_CLOCK_MAX_SKIP ||
        display_clock_distance(dt, ticks * reference) > reference / 8) {
        p->rejected++;
        display_clock_clear_window(p); p->candidate_windows = 0;
        return display_clock_snapshot(p, previous, 0);
    }
    p->block_ns += dt; p->block_ticks += ticks;
    if (p->block_ticks < DISPLAY_CLOCK_BLOCK_TICKS)
        return display_clock_snapshot(p, previous, 0);
    p->elapsed_ns[p->blocks] = p->block_ns; p->ticks[p->blocks++] = p->block_ticks;
    p->block_ns = p->block_ticks = 0;
    if (p->blocks < DISPLAY_CLOCK_BLOCKS)
        return display_clock_snapshot(p, previous, 0);
    uint64_t periods[DISPLAY_CLOCK_BLOCKS];
    for (unsigned i = 0; i < DISPLAY_CLOCK_BLOCKS; i++) periods[i] = p->elapsed_ns[i] / p->ticks[i];
    for (unsigned i = 1; i < DISPLAY_CLOCK_BLOCKS; i++) {
        uint64_t v = periods[i]; unsigned j = i;
        while (j && periods[j - 1] > v) { periods[j] = periods[j - 1]; j--; }
        periods[j] = v;
    }
    uint64_t median = periods[DISPLAY_CLOCK_BLOCKS / 2];
    uint64_t tolerance = reference / 1000;
    if (tolerance < 2000) tolerance = 2000;
    uint64_t elapsed = 0, physical_ticks = 0;
    unsigned good = 0;
    for (unsigned i = 0; i < DISPLAY_CLOCK_BLOCKS; i++) {
        uint64_t period = p->elapsed_ns[i] / p->ticks[i];
        if (display_clock_distance(period, median) > tolerance) continue;
        elapsed += p->elapsed_ns[i]; physical_ticks += p->ticks[i]; good++;
    }
    display_clock_clear_window(p);
    p->valid_blocks = good; p->window_ns = elapsed; p->window_ticks = physical_ticks;
    if (good < DISPLAY_CLOCK_BLOCKS - 2) { p->candidate_windows = 0; return display_clock_snapshot(p, previous, 0); }
    uint64_t measured = elapsed / physical_ticks;
    p->measured_ns = measured;
    /* Calibration corrects small nominal rounding/rate errors, not an
     * unannounced whole refresh mode. Require its authoritative callback. */
    if (display_clock_distance(measured, nominal_refresh_ns) > nominal_refresh_ns / 50) {
        p->candidate_windows = 0; return display_clock_snapshot(p, previous, 0);
    }
    uint64_t material = reference / 1000;
    if (material < 5000) material = 5000;
    if (!p->calibrated) {
        p->calibrated = 1;
        if (measured != reference) {
            p->refresh_ns = measured; changed = 1;
        }
    } else if (display_clock_distance(measured, reference) >= material) {
        if (!p->candidate_windows || display_clock_distance(measured, p->candidate_ns) > 2000) {
            p->candidate_ns = measured; p->candidate_windows = 1;
            return display_clock_snapshot(p, previous, 0);
        }
        p->refresh_ns = measured; changed = 1;
    }
    p->candidate_windows = 0;
    p->next_validation_ns = frame_ns > UINT64_MAX - DISPLAY_CLOCK_VALIDATE_NS
        ? UINT64_MAX : frame_ns + DISPLAY_CLOCK_VALIDATE_NS;
    return display_clock_snapshot(p, previous, changed);
}
