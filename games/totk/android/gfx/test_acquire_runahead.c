/* Deterministic fixed-slot producer/presenter: no GPU, wall clock or sleeps.
 * Uses the production admission, reservation and credit policies. */
#include "acquire_clock.h"
#include "acquire_slack.h"
#include <assert.h>
#include <stdio.h>

enum { FRAMES = 1300 };
static const uint64_t base = 1000000000, refresh = 16666667;
typedef struct {
    uint64_t now, post[FRAMES], slot[FRAMES], input[FRAMES];
    unsigned queued, retired, pending, max_pending, repeats, refilled;
} simulation;
static uint64_t callback(uint64_t now) { return base + (now - base) / refresh * refresh; }
static void advance(simulation *s, uint64_t now) {
    s->now = now;
    while (s->retired < s->queued && s->post[s->retired] <= now) {
        assert(runtime_present_credit_update(&s->pending, -1));
        s->retired++;
    }
}
static simulation run(unsigned lead, int overrun) {
    const uint64_t period = 2 * refresh;
    simulation s = {.now = base};
    acquire_clock acquire = {0};
    vk_present_slots present = {0};
    /* Five consecutive +8..13ms production overruns drain a one-frame bank;
     * eight early frames between clusters can refill a two-frame bank. */
    const uint64_t work[] = {24000000,24000000,24000000,24000000,24000000,24000000,24000000,24000000,
                             42000000,45000000,44000000,46000000,42000000};
    for (unsigned f = 0; f < FRAMES; f++) {
        uint64_t arrival = s.now;
        acquire_clock_release r = acquire_clock_reserve(&acquire, 1, callback(s.now), refresh, 2, arrival);
        assert(r.mode == ACQUIRE_CLOCK_ACTIVE);
        while (!acquire_clock_slack_ready(&acquire, callback(s.now), s.pending, lead)) {
            advance(&s, callback(s.now) + refresh);
            r = acquire_clock_observe(&acquire, callback(s.now), refresh, arrival);
            assert(r.mode == ACQUIRE_CLOCK_ACTIVE);
        }
        s.input[f] = s.now; /* model input read immediately after admission */
        uint64_t work_ns = overrun && f == 100 ? 5 * period : work[f % 13];
        advance(&s, s.now + work_ns);
        vk_present_slot slot = vk_present_slots_reserve(&present, 1, callback(s.now), refresh, 2, s.now);
        assert(slot.mode == VK_PRESENT_SLOTS_ACTIVE);
        if (!overrun && lead == 2) {
            assert(!slot.skipped);
            if (f) assert(slot.desired_ns == s.slot[0] + f * period);
        }
        s.slot[f] = slot.desired_ns;
        uint64_t post = slot.desired_ns > s.now ? slot.desired_ns : s.now;
        uint64_t rounded = callback(post);
        if (rounded < post) rounded += refresh;
        /* FIFO, at most one picture per physical refresh. Never replace an
         * older late picture by a newer one or move a slot deliberately early. */
        if (f && rounded <= s.post[f - 1]) rounded = s.post[f - 1] + refresh;
        s.post[f] = rounded;
        assert(s.post[f] >= slot.desired_ns && s.post[f] >= s.input[f]);
        if (f) {
            assert(s.slot[f] > s.slot[f - 1] && s.post[f] > s.post[f - 1]);
            s.repeats += s.post[f] - s.post[f - 1] > period;
        }
        assert(s.post[f] - s.input[f] <= (lead + 1) * period + refresh + (overrun ? 5 * period : 0));
        assert(runtime_present_credit_update(&s.pending, 1)); s.queued++;
        assert(s.pending <= 1 + lead);
        if (s.pending > s.max_pending) s.max_pending = s.pending;
        if (f > 100 && f % 13 == 7 && s.pending == lead + 1) s.refilled++;
        advance(&s, s.now);
    }
    advance(&s, s.post[FRAMES - 1]);
    assert(!s.pending && s.retired == FRAMES);
    return s;
}
static void missing_surface(void) {
    const uint64_t period = 2 * refresh;
    for (unsigned lead = 0; lead <= 2; lead++) {
        acquire_clock clock = {0}; uint64_t now = base; unsigned immediate = 0;
        for (unsigned f = 0; f < 2400; f++) {
            acquire_clock_reserve(&clock, 1, now, refresh, 2, now);
            int waited = 0;
            while (!acquire_clock_slack_ready(&clock, now, 0, lead)) {
                now += refresh; waited = 1;
                acquire_clock_observe(&clock, now, refresh, now);
            }
            immediate = waited ? 1 : immediate + 1;
            assert(immediate <= lead + 1);
            assert(clock.target_ns == base + f * period);
            assert(f <= (now - base) / period + lead);
            assert(!acquire_clock_slack_ready(&clock, now, lead + 1, lead));
        }
        /* No large bank after suspension, including callback staleness. */
        uint64_t arrival = now + 1000 * period;
        acquire_clock_reserve(&clock, 1, now, refresh, 2, arrival);
        assert(!acquire_clock_slack_ready(&clock, now, 0, lead));
        acquire_clock_observe(&clock, arrival, refresh, arrival);
        assert(acquire_clock_slack_ready(&clock, arrival, 0, lead));
    }
    /* Overflow-safe two-period subtraction and malformed-lead clamp. */
    acquire_clock huge = {.target_ns = UINT64_MAX, .refresh_ns = UINT64_MAX / 2, .interval = 1};
    assert(acquire_clock_slack_ready(&huge, 1, 2, 2));
    assert(!acquire_clock_slack_ready(&huge, 1, 3, 99));
}
static void configuration(void) {
    assert(acquire_slack_parse("2") == ACQUIRE_SLACK_TWO);
    assert(vk_present_buffer_count(NULL, "1") == 3);
    assert(vk_present_buffer_count(NULL, "2") == 4);
    assert(vk_present_buffer_count("4", "2") == 4);
    assert(vk_present_buffer_count("100", "2") == 4);
    assert(!acquire_slack_two_refusal("1", NULL, NULL));
    assert(!acquire_slack_two_refusal("1", "4", "0"));
    assert(acquire_slack_two_refusal("0", "4", "0"));
    assert(acquire_slack_two_refusal(NULL, "4", "0"));
    assert(acquire_slack_two_refusal("1", "3", "0"));
    assert(acquire_slack_two_refusal("1", "2", "0"));
    assert(acquire_slack_two_refusal("1", "", "0"));
    assert(acquire_slack_two_refusal("1", "4", "interp"));
    assert(acquire_slack_two_refusal("1", "4", "1"));
    assert(vk_present_buffer_count("3", "2") == 3); /* explicit override is never raised */
}
int main(void) {
    configuration(); missing_surface();
    simulation one = run(1, 0), two = run(2, 0), hitch = run(2, 1);
    assert(one.repeats > 0 && !two.repeats && hitch.repeats > 0);
    assert(one.max_pending == 2 && two.max_pending == 3 && two.refilled > 50);
    /* Fully refilled early stretches spend the extra bank as latency. Lead1
     * can reanchor after its visible stalls; allow one refresh of phase error. */
    for (unsigned f = 20; f < FRAMES; f += 13) {
        uint64_t a = one.post[f] - one.input[f], b = two.post[f] - two.input[f];
        assert(b >= a + refresh && b <= a + 3 * refresh);
    }
    printf("acquire runahead: clustered late repeats lead1=%u lead2=%u; >2-period overrun repeats=%u; pending max=%u/%u refill=%u; ordered, bounded latency and configuration refusal passed\n",
           one.repeats, two.repeats, hitch.repeats, one.max_pending, two.max_pending, two.refilled);
    return 0;
}
