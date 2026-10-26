/* Standalone pure timestamp fixture. No Android, GPU, real clocks or waits. */
#include "display_clock.h"
#include "vk_present_slots.h"
#include "acquire_clock.h"
#include "predict_pace.h"
#include <assert.h>
#include <stdio.h>

static const uint64_t nominal = 16666666, observed = 16693000;
static display_clock_result feed(display_period_clock *p, uint64_t *frame,
        uint64_t period, unsigned callbacks, unsigned step) {
    display_clock_result r = {0};
    for (unsigned i = 0; i < callbacks; i++) {
        *frame += period * step;
        r = display_clock_observe(p, *frame, nominal);
    }
    return r;
}
static void warm_and_freeze(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_result r = display_clock_observe(&p, frame, nominal);
    assert(r.changed && !r.calibrated && r.refresh_ns == nominal && !r.previous_frame_ns);
    feed(&p, &frame, observed, 255, 1);
    assert(!p.calibrated && p.refresh_ns == nominal);
    uint64_t previous = frame;
    r = feed(&p, &frame, observed, 1, 1);
    assert(r.changed && r.calibrated && r.refresh_ns == observed && r.previous_frame_ns == previous);
    assert(r.measured_ns == observed && r.valid_blocks == DISPLAY_CLOCK_BLOCKS);
    assert(r.window_ticks == 256 && r.window_ns == 256 * observed);
    r = feed(&p, &frame, observed + 900, 1000, 1);
    assert(!r.changed && p.refresh_ns == observed); /* Never reset each callback/window. */
    assert(p.measured_ns == observed); /* No validation until the minute boundary. */
}
static void initial_precision_is_not_discarded(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    display_clock_result r = feed(&p, &frame, nominal + 1, 256, 1);
    assert(r.calibrated && r.changed && p.refresh_ns == nominal + 1 && r.measured_ns == nominal + 1);
}
static void skipped_callbacks_and_jitter(void) {
    display_period_clock p = {0}; uint64_t ideal = 1000000000;
    display_clock_observe(&p, ideal, nominal);
    unsigned promotions = 0;
    for (unsigned i = 0; i < 400; i++) {
        ideal += observed * (i % 7 == 0 ? 3 : i % 3 == 0 ? 2 : 1);
        uint64_t stamp = i % 2 ? ideal + 80000 : ideal - 80000;
        display_clock_result r = display_clock_observe(&p, stamp, nominal);
        promotions += r.changed;
        if (r.changed) assert(r.previous_frame_ns < stamp);
    }
    assert(promotions == 1 && p.calibrated && display_clock_distance(p.refresh_ns, observed) <= 500 && !p.rejected);
    assert(p.window_ticks >= 256 && p.valid_blocks >= 6);
}
static void fence_intervals_do_not_halve_the_display_rate(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    display_clock_result r = feed(&p, &frame, observed, 128, 2);
    assert(r.calibrated && r.changed && r.refresh_ns == observed && r.window_ticks == 256);
    assert(r.window_ns == 256 * observed);
    /* A separate callback observer never inherits this fence's phase/history. */
    display_period_clock callback = {0};
    uint64_t callback_frame = frame + 9000000;
    display_clock_observe(&callback, callback_frame, nominal);
    feed(&callback, &callback_frame, nominal, 260, 1);
    assert(callback.refresh_ns == nominal && p.refresh_ns == observed);
}
static void invalid_stamps_and_pause(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    feed(&p, &frame, observed, 120, 1);
    uint64_t previous = p.previous_frame_ns;
    display_clock_observe(&p, frame, nominal);
    display_clock_observe(&p, frame - 1, nominal);
    display_clock_observe(&p, 0, nominal);
    assert(p.previous_frame_ns == previous && p.rejected == 3);
    frame += observed * (DISPLAY_CLOCK_MAX_SKIP + 1);
    display_clock_result r = display_clock_observe(&p, frame, nominal);
    assert(!r.changed && !p.blocks && !p.block_ticks && p.rejected == 4);
    feed(&p, &frame, observed, 255, 1);
    assert(!p.calibrated);
    r = feed(&p, &frame, observed, 1, 1);
    assert(r.changed && p.refresh_ns == observed);
    uint64_t refresh = p.refresh_ns;
    display_clock_observe(&p, frame + observed, 0);
    display_clock_observe(&p, frame + observed, UINT64_MAX);
    assert(p.refresh_ns == refresh && p.previous_frame_ns == frame);
}
static void revalidate_without_churn(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    feed(&p, &frame, observed, 256, 1);
    frame = p.next_validation_ns;
    display_clock_observe(&p, frame, nominal); /* Long pause is not a period sample. */
    display_clock_result r = feed(&p, &frame, observed + 1200, 256, 1);
    assert(!r.changed && p.refresh_ns == observed && !p.candidate_windows);
    assert(p.measured_ns == observed + 1200);
    frame = p.next_validation_ns;
    display_clock_observe(&p, frame, nominal);
    const uint64_t new_period = observed + 50000;
    r = feed(&p, &frame, new_period, 256, 1);
    assert(!r.changed && p.refresh_ns == observed && p.candidate_windows == 1);
    r = feed(&p, &frame, new_period, 255, 1);
    assert(!r.changed && p.refresh_ns == observed);
    r = feed(&p, &frame, new_period, 1, 1);
    assert(r.changed && p.refresh_ns == new_period && !p.candidate_windows);
}
static void authoritative_mode_reset_and_immutable_targets(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    vk_present_slots slots = {0};
    vk_present_slot queued = vk_present_slots_reserve(&slots, 1, frame, nominal, 2, frame);
    vk_present_slot retained = queued;
    display_clock_result r = feed(&p, &frame, observed, 256, 1);
    vk_present_slot next = vk_present_slots_reserve(&slots, 1, r.previous_frame_ns, r.refresh_ns, 2, frame);
    assert(next.desired_ns >= queued.desired_ns + 2 * observed);
    assert(queued.desired_ns == retained.desired_ns && queued.refresh_ns == retained.refresh_ns);
    r = display_clock_observe(&p, frame + 8333333, 8333333);
    assert(r.changed && !r.calibrated && r.refresh_ns == 8333333 && r.previous_frame_ns == frame);
    assert(!p.blocks && !p.candidate_windows && !p.measured_ns);
}
static void reject_unannounced_whole_modes_and_unstable_blocks(void) {
    display_period_clock p = {0}; uint64_t frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    feed(&p, &frame, nominal + nominal / 25, 256, 1);
    assert(!p.calibrated && p.refresh_ns == nominal); /* 4% needs a nominal-mode callback. */
    p = (display_period_clock){0}; frame = 1000000000;
    display_clock_observe(&p, frame, nominal);
    for (unsigned block = 0; block < 8; block++)
        feed(&p, &frame, observed + (block % 2 ? 100000 : 0), 32, 1);
    assert(!p.calibrated && p.valid_blocks == 4 && p.refresh_ns == nominal);
}
static uint64_t physical_present(uint64_t target, uint64_t anchor) {
    uint64_t delta = target - anchor;
    return anchor + (delta / observed + (delta % observed != 0)) * observed;
}
static void measured_period_removes_nominal_beat(void) {
    const uint64_t anchor = 1000000000;
    vk_present_slots old_slots = {0}, measured_slots = {0};
    acquire_clock acquire = {0};
    predict_pacer predictor = {0};
    uint64_t old_previous = 0, measured_previous = 0;
    unsigned old_short = 0;
    for (unsigned i = 0; i < 600; i++) {
        uint64_t callback = anchor + i * 2 * observed;
        vk_present_slot old = vk_present_slots_reserve(&old_slots, 1, callback, nominal, 2, callback);
        vk_present_slot measured = vk_present_slots_reserve(&measured_slots, 1, callback, observed, 2, callback);
        assert(!old.skipped && !measured.skipped);
        uint64_t old_display = physical_present(old.desired_ns, anchor);
        uint64_t measured_display = physical_present(measured.desired_ns, anchor);
        if (i) {
            old_short += old_display - old_previous == observed;
            assert(measured_display - measured_previous == 2 * observed);
        }
        old_previous = old_display; measured_previous = measured_display;
        acquire_clock_release release = acquire_clock_reserve(&acquire, 1, callback, observed, 2, callback);
        assert(release.reached && release.target_ns == callback);
        assert(release.reanchor == (i ? 0 : ACQUIRE_CLOCK_START));
        assert(acquire_clock_slack_ready(&acquire, callback, 1, 1));
        predict_decision decision = predict_pacer_choose(&predictor, measured.desired_ns, 2 * observed);
        assert(decision.slot_ns == measured.desired_ns && decision.period_ns == 2 * observed);
        assert(decision.release_ns == decision.slot_ns - 2 * decision.period_ns);
    }
    assert(old_short); /* Nominal 60Hz against 59.905Hz loses a physical tick. */
}
int main(void) {
    warm_and_freeze(); initial_precision_is_not_discarded(); skipped_callbacks_and_jitter();
    fence_intervals_do_not_halve_the_display_rate(); invalid_stamps_and_pause(); revalidate_without_churn();
    authoritative_mode_reset_and_immutable_targets(); reject_unannounced_whole_modes_and_unstable_blocks();
    measured_period_removes_nominal_beat();
    puts("display clock: robust measured periods, skipped callbacks/fences, frozen validation and immutable queued targets passed");
    return 0;
}
