/* Standalone deterministic policy tests. Synthetic timestamps only: no
 * Android, GPU, clocks, waits or allocation. The owner runs this host test. */
#include "fps_auto.h"
#include "acquire_clock.h"
#include "vk_present_slots.h"
#include "../../runtime/fps_auto_runtime.h"
#include <assert.h>
#include <stddef.h>

static const uint64_t base = 1000000000ull, refresh = 16666667ull;

static void bounds(const fps_auto_policy *p) {
    assert(p->interval == 1 || p->interval == 2);
    assert(p->desired == 1 || p->desired == 2);
    assert(p->count <= FPS_AUTO_HISTORY_CAPACITY && p->misses <= p->count);
    assert(p->head < FPS_AUTO_HISTORY_CAPACITY);
    assert(p->drop_streak <= FPS_AUTO_DROP_WINDOWS);
    assert(p->last_drop_misses <= p->last_drop_count);
    assert(p->last_drop_count <= FPS_AUTO_HISTORY_CAPACITY);
    assert(p->return_backoff == 1 || p->return_backoff == 2 ||
        p->return_backoff == 4 || p->return_backoff == FPS_AUTO_RETURN_BACKOFF_MAX);
    assert(p->have_return <= 1);
    assert(p->last_drop_coverage_ns < FPS_AUTO_DROP_WINDOW_NS);
    assert(p->last_game_count == p->last_drop_count);
    assert(p->last_game_coverage_ns == p->last_drop_coverage_ns);
    assert(p->game_count <= FPS_AUTO_HISTORY_CAPACITY);
    assert(p->game_slow_count <= p->game_count);
    assert(!p->game_count || p->game_last_ns - p->game_first_ns < FPS_AUTO_DROP_WINDOW_NS);
    assert(p->coverage_ns <= (p->interval == 1
        ? FPS_AUTO_DROP_WINDOW_NS : FPS_AUTO_RECOVER_WINDOW_NS));
}
static fps_auto_observation observe(fps_auto_policy *p, uint64_t now,
        uint64_t ready, uint64_t game) {
    unsigned interval = p->interval, backoff = p->return_backoff;
    uint64_t last_return = p->last_return_ns;
    fps_auto_observation r = fps_auto_observe(p, now, ready, game);
    assert(p->interval == interval); /* Observation never changes pacing. */
    assert(p->return_backoff == backoff && p->last_return_ns == last_return);
    bounds(p);
    return r;
}
static void valid(fps_auto_policy *p, uint64_t now, uint64_t ready, uint64_t game) {
    assert(observe(p, now, ready, game).valid);
}
static unsigned apply(fps_auto_policy *p, uint64_t now) {
    unsigned interval = fps_auto_apply(p, now);
    assert(interval == p->interval);
    bounds(p);
    return interval;
}
static void present(fps_auto_policy *p, uint64_t now) {
    unsigned interval = p->interval, backoff = p->return_backoff;
    uint64_t last_return = p->last_return_ns;
    fps_auto_game_present(p, now);
    assert(p->interval == interval); /* HLE present never applies pacing. */
    assert(p->return_backoff == backoff && p->last_return_ns == last_return);
    bounds(p);
}
static void game_valid(fps_auto_policy *p, uint64_t now, uint64_t ready, uint64_t game) {
    present(p, now);
    valid(p, now, ready, game);
}
static uint64_t request_drop(fps_auto_policy *p, uint64_t start) {
    assert(p->interval == 1);
    const uint64_t step = 20000000ull; /* Actual 50 fps; no busy/readiness misses. */
    for (unsigned i = 1; i <= 600; i++) {
        uint64_t now = start + i * step;
        game_valid(p, now, refresh, refresh);
        if (p->desired == 2) {
            assert(now - start >= FPS_AUTO_DROP_WINDOWS * FPS_AUTO_DROP_WINDOW_NS);
            assert(p->drop_streak == FPS_AUTO_DROP_WINDOWS);
            assert(p->last_drop_count >= FPS_AUTO_DROP_MIN_SAMPLES);
            assert(p->last_drop_coverage_ns >= FPS_AUTO_DROP_COVERAGE_NS);
            assert(p->drop_last_miss_ns - p->drop_first_miss_ns >= FPS_AUTO_DROP_MISS_SPAN_NS);
            assert(p->last_game_fps_milli == 50000 && !p->misses);
            assert(p->reason == FPS_AUTO_REASON_DROP && p->interval == 1);
            return now;
        }
        assert(apply(p, now) == 1);
    }
    assert(0 && "two independent sustained-overload windows must request interval 2");
    return 0;
}
static uint64_t request30(fps_auto_policy *p, uint64_t start) {
    fps_auto_reset(p, start, refresh);
    return request_drop(p, start);
}
static uint64_t enter30(fps_auto_policy *p, uint64_t start) {
    uint64_t now = request30(p, start), generation = p->generation;
    assert(apply(p, now) == 2);
    assert(!p->count && !p->misses && !p->coverage_ns && !p->drop_streak);
    assert(!p->game_count && !p->have_game_present && !p->last_game_count);
    assert(p->generation == generation + 1 && p->last_change_ns == now);
    return now;
}
static uint64_t return60(fps_auto_policy *p, uint64_t start) {
    assert(p->interval == 2);
    uint64_t hold_ns = fps_auto_recovery_wait_ns(p, start);
    for (unsigned i = 1; i <= 2000; i++) {
        uint64_t now = start + i * 2 * refresh;
        game_valid(p, now, refresh, 2 * refresh);
        if (now - start < hold_ns) assert(p->desired == 2);
        if (p->desired == 1) {
            assert(!fps_auto_recovery_wait_ns(p, now));
            assert(apply(p, now) == 1);
            assert(p->have_return && p->last_return_ns == now);
            return now;
        }
        assert(apply(p, now) == 2);
    }
    assert(0 && "healthy feedback must recover after the bounded hold");
    return 0;
}

static void initialization(void) {
    _Static_assert(FPS_AUTO_HISTORY_CAPACITY >= 8 * 60, "eight seconds at 60 Hz");
    fps_auto_policy p; /* Reset must not read uninitialized storage. */
    fps_auto_reset(&p, base, refresh);
    assert(p.interval == 1 && p.desired == 1 && p.refresh_ns == refresh);
    assert(p.reason == FPS_AUTO_REASON_RESET && p.generation == 1);
    assert(!p.count && !p.misses && !p.coverage_ns && !p.have_observe);
    assert(!p.drop_streak && !p.last_drop_count && !p.last_drop_misses);
    assert(!p.last_drop_coverage_ns && !p.drop_first_miss_ns && !p.drop_last_miss_ns);
    assert(!p.game_count && !p.have_game_present && !p.last_game_present_ns);
    assert(!p.last_game_fps_milli && !p.last_game_count && !p.last_game_coverage_ns);
    assert(p.return_backoff == 1 && !p.have_return && !p.last_return_ns);
    assert(p.recovery_start_ns == base && p.healthy_since_ns == base);
    assert(!fps_auto_recovery_wait_ns(&p, base));
    assert(!fps_auto_recovery_wait_ns(NULL, UINT64_MAX));
    assert(p.last_change_ns == base && apply(&p, base) == 1);
    assert(apply(&p, base) == 1); /* Same-time repeated acquire is harmless. */
    fps_auto_reset(NULL, 0, 0);
    fps_auto_inhibit(NULL, 0);
    fps_auto_game_present(NULL, 0);
    assert(fps_auto_apply(NULL, 0) == 1);
    assert(!fps_auto_observe(NULL, 0, 0, 0).valid);
    fps_auto_policy empty = {0};
    assert(fps_auto_apply(&empty, 0) == 1);
    assert(!fps_auto_observe(&empty, 0, refresh, refresh).valid);
    fps_auto_game_present(&empty, 0);
    assert(!empty.have_game_present && !empty.game_count);
}

static void drop_strict_thresholds(void) {
    fps_auto_policy p;
    /* Raw rate, not readiness share, owns admission. Include the original
     * slightly-late 25% readiness false positive and all-late fast streams. */
    const unsigned rates[] = {60000, 57000, 59940, 60000, 55000, 54000, 45000};
    for (unsigned rate = 0; rate < sizeof rates / sizeof rates[0]; rate++) {
        fps_auto_reset(&p, base, refresh);
        unsigned dropped = 0;
        for (unsigned i = 0; i <= 1200; i++) {
            uint64_t now = base + (uint64_t)i * 1000000000000ull / rates[rate];
            unsigned miss = rate ? rates[rate] >= 55000 : i % 4 == 0;
            game_valid(&p, now, refresh + miss, refresh);
            if (rates[rate] >= 55000) {
                assert(p.desired == 1 && apply(&p, now) == 1);
            } else if (p.desired == 2) {
                assert(now - base >= 2 * FPS_AUTO_DROP_WINDOW_NS);
                assert(p.last_game_count >= FPS_AUTO_DROP_MIN_SAMPLES);
                assert(p.last_game_fps_milli >= rates[rate] - 1 &&
                    p.last_game_fps_milli <= rates[rate] + 1);
                assert(apply(&p, now) == 2);
                dropped = 1;
                break;
            } else assert(apply(&p, now) == 1);
        }
        assert(dropped == (rates[rate] < 55000));
    }

    /* Exactly 55, without integer-period rounding: 264 intervals /4.8 s.
     * Boundary frames belong only to their new independent window. */
    fps_auto_reset(&p, base, refresh);
    for (unsigned window = 0; window < 2; window++) {
        for (unsigned i = 0; i <= 264; i++) {
            uint64_t now = base + window * FPS_AUTO_DROP_WINDOW_NS +
                (uint64_t)i * 1000000000ull / 55;
            game_valid(&p, now, refresh + 1, refresh);
            assert(p.desired == 1);
        }
    }
    game_valid(&p, base + 2 * FPS_AUTO_DROP_WINDOW_NS, refresh + 1, refresh);
    assert(p.last_game_count == 265 && p.last_game_coverage_ns == 4800000000ull);
    assert(p.last_game_fps_milli == 55000 && !p.drop_streak && p.desired == 1);

    /* READY-only samples, even when all late, are never a surrogate raw stream. */
    fps_auto_reset(&p, base, refresh);
    for (unsigned i = 0; i <= 720; i++) {
        valid(&p, base + i * refresh, refresh + 1, refresh);
        assert(p.desired == 1);
    }
    assert(!p.game_count && !p.last_game_count && !p.drop_streak);
    fps_auto_observation r;
    fps_auto_reset(&p, base, refresh);
    r = observe(&p, base, refresh, refresh + FPS_AUTO_MARGIN_NS);
    assert(r.valid && !r.miss); /* Busy/readiness boundaries remain diagnostics. */
    r = observe(&p, base + 1, refresh, refresh + FPS_AUTO_MARGIN_NS + 1);
    assert(r.valid && r.miss && p.desired == 1);
    r = observe(&p, base + 2, refresh + 1, refresh);
    assert(r.valid && r.miss && p.desired == 1);
}

static void drop_coverage_samples_and_dwell(void) {
    fps_auto_policy p;
    fps_auto_reset(&p, base, refresh);
    for (unsigned i = 0; i < FPS_AUTO_DROP_MIN_SAMPLES - 1; i++) present(&p,
        base + i * FPS_AUTO_DROP_COVERAGE_NS / (FPS_AUTO_DROP_MIN_SAMPLES - 2));
    present(&p, base + FPS_AUTO_DROP_WINDOW_NS);
    assert(p.last_game_count == FPS_AUTO_DROP_MIN_SAMPLES - 1);
    assert(p.last_game_coverage_ns == FPS_AUTO_DROP_COVERAGE_NS);
    assert(!p.drop_streak && p.desired == 1); /* Coverage alone is insufficient. */

    fps_auto_reset(&p, base, refresh);
    for (unsigned i = 0; i < FPS_AUTO_DROP_MIN_SAMPLES; i++) present(&p,
        base + i * (FPS_AUTO_DROP_COVERAGE_NS - 1) / (FPS_AUTO_DROP_MIN_SAMPLES - 1));
    present(&p, base + FPS_AUTO_DROP_WINDOW_NS);
    assert(p.last_game_count == FPS_AUTO_DROP_MIN_SAMPLES);
    assert(p.last_game_coverage_ns == FPS_AUTO_DROP_COVERAGE_NS - 1);
    assert(!p.drop_streak && p.desired == 1); /* One nanosecond short. */

    fps_auto_reset(&p, base, refresh);
    for (unsigned window = 0; window < 2; window++) {
        for (unsigned i = 0; i < FPS_AUTO_DROP_MIN_SAMPLES; i++) present(&p,
            base + window * FPS_AUTO_DROP_WINDOW_NS +
                i * FPS_AUTO_DROP_COVERAGE_NS / (FPS_AUTO_DROP_MIN_SAMPLES - 1));
        if (!window) assert(!p.drop_streak && p.desired == 1);
    }
    assert(p.drop_streak == 1 && p.game_count == FPS_AUTO_DROP_MIN_SAMPLES && p.desired == 1);
    uint64_t now = base + 2 * FPS_AUTO_DROP_WINDOW_NS;
    present(&p, now - 1);
    assert(p.desired == 1); /* Two full five-second windows have not elapsed. */
    present(&p, now);
    assert(p.desired == 2 && p.interval == 1 && !p.count); /* No READY samples needed. */
    assert(p.last_game_count == FPS_AUTO_DROP_MIN_SAMPLES + 1 && p.game_count == 1);

    /* A later READY timestamp may precede the newest present, and neither it
     * nor fresh fast readiness may clobber the raw pending downshift. */
    valid(&p, now - refresh, refresh, refresh);
    assert(p.desired == 2 && p.reason == FPS_AUTO_REASON_DROP);
    present(&p, now + 1);
    valid(&p, now, refresh, refresh);
    assert(p.desired == 2 && p.reason == FPS_AUTO_REASON_DROP);
    /* Exercise acquire dwell independently using that justified decision. */
    p.last_change_ns = now - FPS_AUTO_DWELL_NS + 1;
    assert(apply(&p, now) == 1 && p.desired == 2);
    assert(apply(&p, now + 1) == 2);
}

static void independent_drop_windows_and_spikes(void) {
    fps_auto_policy p;
    const uint64_t second = 1000000000ull;
    const uint64_t spike_starts[] = {0, 4 * second, 9 * second, 14 * second};
    for (unsigned spike = 0; spike < sizeof spike_starts / sizeof spike_starts[0]; spike++) {
        fps_auto_reset(&p, base, refresh);
        unsigned saw_pair = 0;
        for (uint64_t now = base; now <= base + 22 * second;) {
            game_valid(&p, now, refresh + 1, refresh);
            if (p.drop_streak == 2) {
                saw_pair = 1;
                assert(p.drop_last_miss_ns - p.drop_first_miss_ns < FPS_AUTO_DROP_MISS_SPAN_NS);
            }
            assert(p.desired == 1 && apply(&p, now) == 1);
            uint64_t elapsed = now - base;
            unsigned slow = elapsed >= spike_starts[spike] &&
                elapsed < spike_starts[spike] + 2 * second;
            now += slow ? 2 * refresh : refresh;
        }
        if (spike) assert(saw_pair); /* Both rate windows fail; the span guard rejects them. */
    }

    /* One entire bad window followed by healthy cadence cannot form a pair. */
    fps_auto_reset(&p, base, refresh);
    unsigned saw_one = 0;
    for (uint64_t now = base; now <= base + 16 * second;) {
        game_valid(&p, now, refresh + 1, refresh);
        if (p.drop_streak == 1) saw_one = 1;
        assert(p.desired == 1 && apply(&p, now) == 1);
        now += now - base < FPS_AUTO_DROP_WINDOW_NS ? 2 * refresh : refresh;
    }
    assert(saw_one && !p.drop_streak);

    /* Slow-event span is inclusive at exactly five seconds, independent of
     * both rate and coverage. One valid 1-s gap in each window makes its
     * actual rate <55 while leaving just one slow event per window. */
    for (unsigned short_span = 0; short_span < 2; short_span++) {
        fps_auto_reset(&p, base, refresh);
        for (unsigned window = 0; window < 2; window++) {
            uint64_t start = base + window * FPS_AUTO_DROP_WINDOW_NS;
            present(&p, start);
            for (unsigned i = 1; i <= 62; i++) present(&p, start + i * 16000000ull);
            present(&p, start + second);
            uint64_t slow_end = start + 2 * second - (window ? short_span : 0);
            present(&p, slow_end);
            for (unsigned i = 1; i <= 187; i++) present(&p, slow_end + i * 16000000ull);
        }
        present(&p, base + 2 * FPS_AUTO_DROP_WINDOW_NS);
        assert(p.drop_streak == 2 && p.last_drop_misses == 1);
        assert(p.drop_last_miss_ns - p.drop_first_miss_ns ==
            FPS_AUTO_DROP_MISS_SPAN_NS - short_span);
        assert(p.desired == (short_span ? 1u : 2u));
    }
}

static void inhibition_restarts_eligibility(void) {
    fps_auto_policy p;
    const uint64_t step = 25000000ull;
    fps_auto_reset(&p, base, refresh);
    for (unsigned i = 0; i <= 200; i++) game_valid(&p, base + i * step, refresh + 1, refresh);
    assert(p.drop_streak == 1 && p.game_count == 1);
    uint64_t now = base + FPS_AUTO_DROP_WINDOW_NS, generation = p.generation;
    fps_auto_inhibit(&p, now);
    assert(p.interval == 1 && p.desired == 1 && !p.count && !p.misses && !p.coverage_ns);
    assert(!p.game_count && !p.have_game_present && !p.last_game_count && !p.last_game_fps_milli);
    assert(!p.drop_streak && !p.last_drop_count && !p.last_drop_misses && !p.last_drop_coverage_ns);
    assert(!p.have_observe && p.last_apply_ns == now && p.last_change_ns == now);
    assert(p.generation == generation + 1 && p.reason == FPS_AUTO_REASON_INHIBITED);
    for (unsigned i = 1; i <= 401; i++) {
        game_valid(&p, now + i * step, refresh + 1, refresh);
        if (i <= 400) assert(p.desired == 1);
    }
    assert(p.desired == 2); /* Ten new seconds, never first-window carry-over. */
    fps_auto_inhibit(&p, now + 402 * step);
    assert(p.interval == 1 && p.desired == 1 && !p.count && !p.drop_streak && !p.game_count);
    assert(apply(&p, now + 402 * step) == 1);

    now = enter30(&p, base);
    for (unsigned i = 1; i <= 240; i++) game_valid(&p,
        now + i * 2 * refresh, refresh, 2 * refresh);
    assert(p.desired == 1);
    now += 240 * 2 * refresh;
    fps_auto_inhibit(&p, now);
    assert(p.interval == 2 && p.desired == 2 && !p.count && !p.have_observe);
    assert(!p.game_count && !p.have_game_present);
    assert(apply(&p, now) == 2); /* Loading must not commit pending recovery. */
    fps_auto_inhibit(&p, now + FPS_AUTO_STALL_NS);
    assert(p.last_change_ns == now + FPS_AUTO_STALL_NS && p.interval == 2);
}

static void recover_margin_and_paced_game_interval(void) {
    fps_auto_policy p;
    uint64_t transition = enter30(&p, base), start = transition + 500000000ull;
    for (unsigned i = 0; i < 250; i++) {
        fps_auto_observation r = observe(&p, start + i * 30000000ull,
            refresh + FPS_AUTO_RECOVER_MARGIN_NS, 2 * refresh);
        assert(r.valid && !r.miss && p.desired == 2);
    }
    assert(p.coverage_ns == FPS_AUTO_RECOVER_COVERAGE_NS - 30000000ull);
    uint64_t now = start + FPS_AUTO_RECOVER_COVERAGE_NS;
    valid(&p, now, refresh + FPS_AUTO_RECOVER_MARGIN_NS, 2 * refresh);
    assert(p.count == 251 && !p.misses && p.desired == 1 && p.interval == 2);
    assert(p.reason == FPS_AUTO_REASON_RECOVER);
    uint64_t generation = p.generation;
    assert(apply(&p, now) == 1 && !p.count && !p.coverage_ns);
    assert(p.generation == generation + 1 && p.last_change_ns == now);

    transition = enter30(&p, base);
    fps_auto_observation r = observe(&p, transition + 2 * refresh,
        refresh + FPS_AUTO_RECOVER_MARGIN_NS + 1, 2 * refresh);
    assert(r.valid && r.miss && p.count == 1); /* One nanosecond over tolerance. */
    for (unsigned i = 2; i <= 400; i++) {
        r = observe(&p, transition + i * 2 * refresh, 2 * refresh, 2 * refresh);
        assert(r.valid && r.miss && p.desired == 2);
    }
    assert(p.misses == p.count); /* Fitting 33 ms is insufficient for 60 Hz. */
}

static void recovery_tail_budget(void) {
    fps_auto_policy p;
    uint64_t transition = enter30(&p, base), start = transition + 40000000ull;
    for (unsigned i = 0; i < 200; i++) {
        unsigned tail = i < 10;
        fps_auto_observation r = observe(&p, start + i * 40000000ull,
            refresh + FPS_AUTO_RECOVER_MARGIN_NS + tail, 2 * refresh);
        assert(r.valid && r.miss == tail && !r.reset);
        if (i < 199) assert(p.desired == 2);
    }
    assert(p.count == 200 && p.misses == 10 && p.desired == 1); /* Exactly 95% fits. */
    valid(&p, start + FPS_AUTO_RECOVER_WINDOW_NS,
        refresh + FPS_AUTO_RECOVER_MARGIN_NS + 1, 2 * refresh);
    assert(p.count == 201 && p.misses == 11 && p.desired == 2); /* >5% retracts recovery. */
    assert(p.coverage_ns == FPS_AUTO_RECOVER_WINDOW_NS); /* Inclusive eight-second window. */
    uint64_t generation = p.generation;
    for (unsigned i = 201; i < 800; i++) valid(&p, start + i * 40000000ull,
        refresh + FPS_AUTO_RECOVER_MARGIN_NS, 2 * refresh);
    assert(p.desired == 1 && !p.misses && p.head != 0);
    assert(p.count == 201 && p.coverage_ns == FPS_AUTO_RECOVER_WINDOW_NS);
    assert(p.generation == generation); /* Expiry is not an epoch reset. */

    transition = enter30(&p, base);
    generation = p.generation;
    uint64_t now = transition;
    for (unsigned i = 1; i <= 260; i++) {
        unsigned tail = i == 10;
        uint64_t gap = tail ? 200000000ull : 2 * refresh;
        now += gap;
        fps_auto_observation r = observe(&p, now,
            refresh + FPS_AUTO_RECOVER_MARGIN_NS + tail, gap);
        assert(r.valid && !r.reset && r.miss == tail && p.generation == generation);
        if (p.desired == 1) {
            assert(p.misses == 1 && p.count >= FPS_AUTO_RECOVER_MIN_SAMPLES);
            assert(apply(&p, now) == 1);
            return; /* One 200 ms READY gap consumes a tail sample, not all evidence. */
        }
    }
    assert(0 && "an isolated longer valid frame must not prevent recovery");
}

static void recovery_minimum_samples_and_sparse_feedback(void) {
    fps_auto_policy p;
    uint64_t transition = enter30(&p, base), start = transition + 500000000ull;
    for (unsigned i = 0; i < 179; i++) valid(&p,
        start + i * FPS_AUTO_RECOVER_COVERAGE_NS / 178,
        refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(p.count == 179 && p.coverage_ns == FPS_AUTO_RECOVER_COVERAGE_NS && p.desired == 2);
    valid(&p, start + FPS_AUTO_RECOVER_COVERAGE_NS + 1,
        refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(p.count == 180 && p.desired == 1);

    transition = enter30(&p, base);
    start = transition + 2 * refresh;
    for (unsigned i = 0; i < 400; i++) {
        valid(&p, start + i * 3 * refresh, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
        assert(p.desired == 2); /* Continuous but too few frames in eight seconds. */
    }
    assert(p.count < FPS_AUTO_RECOVER_MIN_SAMPLES && p.coverage_ns >= FPS_AUTO_RECOVER_COVERAGE_NS);
    uint64_t now = start + 399 * 3 * refresh, generation = p.generation;
    now += 3 * refresh + 1;
    fps_auto_observation r = observe(&p, now, refresh, 2 * refresh);
    assert(r.valid && !r.reset && p.count < FPS_AUTO_RECOVER_MIN_SAMPLES);
    assert(p.generation == generation && p.desired == 2);
    for (unsigned i = 1; i <= 200; i++) {
        now += 100000000ull;
        r = observe(&p, now, refresh, 2 * refresh);
        assert(r.valid && !r.reset && p.desired == 2);
    }
    assert(p.count <= 81 && p.generation == generation); /* Sparse feedback cannot recover. */
}

static void no_flapping_after_recovery(void) {
    fps_auto_policy p;
    uint64_t transition = enter30(&p, base), now = transition;
    for (unsigned i = 0; i < 260 && p.desired == 2; i++) {
        now += 2 * refresh;
        valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    }
    assert(p.desired == 1 && apply(&p, now) == 1);
    transition = now;
    now = request_drop(&p, transition);
    assert(now - transition >= 2 * FPS_AUTO_DROP_WINDOW_NS);
    assert(now - transition >= FPS_AUTO_DWELL_NS);
    assert(apply(&p, now) == 2);
}

static void recovery_hold_commit_boundary(void) {
    fps_auto_policy p;
    uint64_t dropped = enter30(&p, base);
    for (unsigned i = 0; i <= 300; i++) valid(&p,
        dropped + i * 25000000ull, refresh, 2 * refresh);
    assert(p.coverage_ns == FPS_AUTO_RECOVER_COVERAGE_NS && p.count == 301);
    assert(p.desired == 2 && fps_auto_recovery_wait_ns(&p, p.last_observe_ns) == 500000000ull);
    /* Exercise the commit guard independently of the observation-side guard. */
    p.desired = 1;
    assert(apply(&p, p.last_observe_ns) == 2);
    uint64_t now = dropped + FPS_AUTO_RECOVER_WINDOW_NS - 1;
    valid(&p, now, refresh, 2 * refresh);
    assert(p.desired == 2 && fps_auto_recovery_wait_ns(&p, now) == 1);
    p.desired = 1;
    assert(apply(&p, now) == 2 && p.return_backoff == 1);
    valid(&p, now + 1, refresh, 2 * refresh);
    assert(p.desired == 1 && apply(&p, now + 1) == 1);
    assert(p.last_return_ns == dropped + FPS_AUTO_RECOVER_WINDOW_NS && p.have_return);
}

static void failed_return_backoff_and_cap(void) {
    fps_auto_policy p;
    uint64_t now = enter30(&p, base);
    assert(p.return_backoff == 1 && !p.have_return && !p.last_return_ns);
    assert(fps_auto_recovery_wait_ns(&p, now) == FPS_AUTO_RECOVER_WINDOW_NS);
    assert(fps_auto_recovery_wait_ns(&p, now + FPS_AUTO_RECOVER_WINDOW_NS - 1) == 1);
    assert(!fps_auto_recovery_wait_ns(&p, now + FPS_AUTO_RECOVER_WINDOW_NS));
    uint64_t returned = return60(&p, now);
    const unsigned expected[] = {2, 4, 8, 8};
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        unsigned previous_backoff = p.return_backoff;
        now = request_drop(&p, returned);
        assert(now - returned < FPS_AUTO_FAILED_RETURN_NS);
        assert(p.return_backoff == previous_backoff && p.have_return);
        assert(p.last_return_ns == returned); /* A request is not a failed return. */
        assert(apply(&p, now) == 2 && p.return_backoff == expected[i]);
        assert(!p.have_return && p.last_return_ns == returned);
        uint64_t hold_ns = FPS_AUTO_RECOVER_WINDOW_NS * expected[i];
        assert(fps_auto_recovery_wait_ns(&p, now) == hold_ns);
        assert(fps_auto_recovery_wait_ns(&p, now - 1) == hold_ns);
        assert(fps_auto_recovery_wait_ns(&p, now + hold_ns - 1) == 1);
        assert(!fps_auto_recovery_wait_ns(&p, now + hold_ns));
        assert(apply(&p, now) == 2 && p.return_backoff == expected[i]);
        returned = return60(&p, now);
        assert(returned - now >= hold_ns && p.return_backoff == expected[i]);
        assert(!fps_auto_recovery_wait_ns(&p, returned));
    }
}

static void failed_return_commit_boundary(void) {
    fps_auto_policy p;
    for (unsigned before = 0; before <= 1; before++) {
        uint64_t now = enter30(&p, base), returned = return60(&p, now);
        now = request_drop(&p, returned);
        uint64_t target = returned + FPS_AUTO_FAILED_RETURN_NS - before;
        const uint64_t step = 20000000ull;
        while (target - now > step) {
            now += step;
            game_valid(&p, now, refresh, refresh);
            assert(p.desired == 2 && p.interval == 1 && p.return_backoff == 1);
        }
        game_valid(&p, target, refresh, refresh);
        assert(p.desired == 2 && p.return_backoff == 1);
        assert(apply(&p, target) == 2);
        assert(p.return_backoff == (before ? 2u : 1u));
        /* The request was early in both cases; only actual acquire time counts. */
    }
}

static void healthy_minute_resets_backoff(void) {
    fps_auto_policy p;
    uint64_t now = enter30(&p, base);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t returned = return60(&p, now);
        now = request_drop(&p, returned);
        assert(apply(&p, now) == 2 && p.return_backoff == (2u << i));
    }
    uint64_t returned = return60(&p, now);
    const uint64_t step = refresh;
    for (unsigned i = 1; i < 3600; i++) {
        now = returned + i * step;
        game_valid(&p, now, refresh, refresh);
        assert(apply(&p, now) == 1 && p.return_backoff == 8);
    }
    now = returned + FPS_AUTO_HEALTHY_RETURN_NS - 1;
    game_valid(&p, now, refresh, refresh);
    assert(apply(&p, now) == 1 && p.return_backoff == 8);
    game_valid(&p, now + 1, refresh, refresh);
    assert(p.return_backoff == 8); /* Even debt reset commits only at acquire. */
    assert(apply(&p, now + 1) == 1 && p.return_backoff == 1);
    assert(p.last_return_ns == returned && p.have_return);
}

static void inhibition_does_not_fail_returns(void) {
    fps_auto_policy p;
    uint64_t now = enter30(&p, base), returned = return60(&p, now);
    now = request_drop(&p, returned);
    assert(apply(&p, now) == 2 && p.return_backoff == 2);
    uint64_t dropped = now;
    for (unsigned i = 1; i <= 480; i++) {
        now = dropped + i * 2 * refresh;
        valid(&p, now, refresh, 2 * refresh);
    }
    assert(p.desired == 1 && p.interval == 2 && p.return_backoff == 2);
    fps_auto_inhibit(&p, now);
    assert(p.desired == 2 && p.interval == 2 && p.return_backoff == 2);
    assert(p.recovery_start_ns == dropped && !fps_auto_recovery_wait_ns(&p, now));
    assert(p.last_return_ns == returned && !p.have_return);
    assert(apply(&p, now) == 2); /* Cancelling recovery creates no return or failure. */
    returned = return60(&p, now);
    assert(p.return_backoff == 2);
    now = request_drop(&p, returned);
    fps_auto_inhibit(&p, now);
    assert(p.interval == 1 && p.desired == 1 && p.return_backoff == 2);
    assert(p.last_return_ns == returned && p.have_return);
    assert(apply(&p, now) == 1 && p.return_backoff == 2);
    /* Loading and idle wall time cannot masquerade as a healthy minute. */
    now += FPS_AUTO_HEALTHY_RETURN_NS + 1;
    fps_auto_inhibit(&p, now);
    assert(apply(&p, now) == 1 && p.return_backoff == 2);
    now += FPS_AUTO_HEALTHY_RETURN_NS + 1;
    valid(&p, now, refresh, refresh);
    assert(apply(&p, now) == 1 && p.return_backoff == 2);
    fps_auto_reset(&p, now, refresh);
    assert(p.return_backoff == 1 && !p.have_return && !p.last_return_ns);
}

static void backoff_clock_and_overflow_boundaries(void) {
    fps_auto_policy p;
    uint64_t now = enter30(&p, UINT64_MAX - FPS_AUTO_HEALTHY_RETURN_NS);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t returned = return60(&p, now);
        now = request_drop(&p, returned);
        assert(apply(&p, now) == 2 && p.return_backoff == (2u << i));
    }
    uint64_t dropped = now, hold_ns = 4 * FPS_AUTO_RECOVER_WINDOW_NS;
    while (UINT64_MAX - now > 2 * refresh) {
        now += 2 * refresh;
        valid(&p, now, refresh, 2 * refresh);
        assert(p.desired == 2 && apply(&p, now) == 2);
    }
    valid(&p, UINT64_MAX, refresh, 2 * refresh);
    assert(fps_auto_recovery_wait_ns(&p, UINT64_MAX) ==
        hold_ns - (UINT64_MAX - dropped));
    assert(p.desired == 2 && apply(&p, UINT64_MAX) == 2);
    assert(apply(&p, 0) == 2 && p.reason == FPS_AUTO_REASON_CLOCK);
    assert(p.return_backoff == 4 && p.recovery_start_ns == 0 && !p.have_return);
    assert(fps_auto_recovery_wait_ns(&p, 0) == hold_ns);
    now = return60(&p, 0);
    uint64_t returned = p.last_return_ns;
    assert(apply(&p, now - 1) == 1 && p.reason == FPS_AUTO_REASON_CLOCK);
    assert(p.return_backoff == 4 && !p.have_return && p.last_return_ns == returned);
    now = request_drop(&p, now - 1);
    assert(now - returned < FPS_AUTO_FAILED_RETURN_NS);
    assert(apply(&p, now) == 2 && p.return_backoff == 4);
    fps_auto_inhibit(&p, 0); /* Inhibition must also handle a reversed clock. */
    assert(p.return_backoff == 4 && p.recovery_start_ns == 0 && !p.have_return);
    assert(fps_auto_recovery_wait_ns(&p, 0) == hold_ns);
    assert(apply(&p, 0) == 2 && p.reason == FPS_AUTO_REASON_INHIBITED);
    now = return60(&p, 0);
    fps_auto_inhibit(&p, now - 1);
    assert(p.return_backoff == 4 && !p.have_return && p.last_return_ns == now);
    assert(apply(&p, now - 1) == 1 && p.return_backoff == 4);
    now = request_drop(&p, now - 1);
    assert(apply(&p, now) == 2 && p.return_backoff == 4);
}

static void invalid_missing_stall_and_clock(void) {
    fps_auto_policy p;
    uint64_t transition = enter30(&p, base), now = transition;
    for (unsigned i = 0; i < 200; i++) {
        now += 2 * refresh;
        valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    }
    uint64_t generation = p.generation;
    now += 2 * refresh;
    fps_auto_observation r = observe(&p, now, 0, 2 * refresh);
    assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_INVALID);
    assert(!p.count && p.desired == 2 && p.generation == generation + 1);
    assert(apply(&p, now) == 2);
    now += 2 * refresh;
    valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(p.count == 1);
    r = observe(&p, now + 2 * refresh, refresh - FPS_AUTO_MARGIN_NS, 0);
    assert(!r.valid && r.reset && !p.count && p.desired == 2);
    now += 4 * refresh;
    valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    r = observe(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_CLOCK && !p.count);
    r = observe(&p, now - 1, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_CLOCK);
    now += 2 * refresh;
    valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(apply(&p, now + 1000) == 2);
    valid(&p, now + 1, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(p.count == 2); /* READY can be older than the most recent acquire. */
    assert(apply(&p, now + 999) == 2 && p.reason == FPS_AUTO_REASON_CLOCK && !p.count);
    now += 2 * refresh;
    valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(apply(&p, now + 3 * refresh + 1) == 2);
    assert(p.count == 1); /* Acquire age beyond 3R is not a READY-to-READY gap. */
    assert(apply(&p, now + 3 * refresh + 2 + FPS_AUTO_STALL_NS) == 2);
    assert(p.reason == FPS_AUTO_REASON_STALL);

    fps_auto_reset(&p, base, refresh);
    valid(&p, base, refresh, refresh);
    valid(&p, base + FPS_AUTO_STALL_NS, FPS_AUTO_STALL_NS, FPS_AUTO_STALL_NS);
    assert(p.count == 2); /* Exactly one second is still a valid slow sample. */
    r = observe(&p, base + 2 * FPS_AUTO_STALL_NS + 1, refresh, refresh);
    assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_STALL && !p.count);
    now = base + 2 * FPS_AUTO_STALL_NS + 2;
    r = observe(&p, now, FPS_AUTO_STALL_NS + 1, refresh);
    assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_STALL);
    r = observe(&p, now + 1, refresh, FPS_AUTO_STALL_NS + 1);
    assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_STALL);
}

static void pending_pause_and_mode_reset(void) {
    fps_auto_policy p;
    uint64_t now = request30(&p, base);
    assert(p.desired == 2 && p.interval == 1);
    assert(apply(&p, now + FPS_AUTO_STALL_NS + 1) == 1);
    assert(p.desired == 1 && !p.count && p.reason == FPS_AUTO_REASON_STALL);
    now = enter30(&p, base);
    fps_auto_reset(&p, now, 11111111ull); /* Physical refresh change. */
    assert(p.interval == 1 && p.desired == 1 && !p.count && p.refresh_ns == 11111111ull);
    valid(&p, now + 11111111ull, 10000000ull, 11111111ull);
    assert(p.count == 1 && !p.misses);
    fps_auto_reset(&p, now + 2 * refresh, refresh); /* Guest/mode reset. */
    assert(p.interval == 1 && p.desired == 1 && !p.count && !p.have_observe);

    fps_auto_reset(&p, base, refresh);
    now = base + refresh;
    valid(&p, now, refresh, refresh);
    assert(apply(&p, now) == 1);
    for (unsigned i = 1; i <= 10; i++) assert(apply(&p, now + i * 100000000ull) == 1);
    assert(p.count == 1); /* Exactly one second of old feedback remains valid. */
    assert(apply(&p, now + FPS_AUTO_STALL_NS + 1) == 1);
    assert(!p.count && p.reason == FPS_AUTO_REASON_STALL); /* Acquires did not hide missing feedback. */
}

static void raw_missing_stall_and_clock(void) {
    fps_auto_policy p;
    uint64_t now = request30(&p, base), generation = p.generation;
    /* Missing raw presents cancel a pending downshift even when READY feedback
     * keeps arriving; missing READY does not cancel a live raw rate window. */
    for (unsigned i = 1; i <= 10; i++) {
        valid(&p, now + i * 100000000ull, refresh, refresh);
        assert(p.interval == 1 && p.desired == 2); /* No acquire applies it yet. */
    }
    valid(&p, now + FPS_AUTO_STALL_NS + 1, refresh, refresh);
    assert(apply(&p, now + FPS_AUTO_STALL_NS + 1) == 1);
    assert(!p.game_count && !p.drop_streak && p.desired == 1);
    assert(p.reason == FPS_AUTO_REASON_STALL && p.generation == generation + 1);

    fps_auto_reset(&p, base, refresh);
    for (unsigned i = 0; i <= 500; i++) {
        now = base + i * 20000000ull;
        present(&p, now); /* No READY feedback at all. */
        if (i < 500) assert(apply(&p, now) == 1);
    }
    assert(p.desired == 2 && !p.count && apply(&p, now) == 2);

    fps_auto_reset(&p, base, refresh);
    present(&p, base);
    present(&p, base + FPS_AUTO_STALL_NS);
    assert(p.game_count == 2); /* Exactly one second remains usable. */
    generation = p.generation;
    present(&p, base + 2 * FPS_AUTO_STALL_NS + 1);
    assert(!p.game_count && !p.have_game_present && p.reason == FPS_AUTO_REASON_STALL);
    assert(p.generation == generation + 1 && p.desired == 1);
    present(&p, base + 2 * FPS_AUTO_STALL_NS + 2);
    assert(p.game_count == 1);
    present(&p, base + 2 * FPS_AUTO_STALL_NS + 2);
    assert(!p.game_count && !p.have_game_present && p.reason == FPS_AUTO_REASON_CLOCK);
    present(&p, base + 2 * FPS_AUTO_STALL_NS + 3);
    present(&p, base + 2 * FPS_AUTO_STALL_NS + 2);
    assert(!p.game_count && p.reason == FPS_AUTO_REASON_CLOCK);

    now = request30(&p, base);
    fps_auto_clear_history(&p);
    assert(!p.game_count && !p.have_game_present && !p.last_game_count);
    assert(!p.last_game_fps_milli && !p.last_game_coverage_ns && !p.drop_streak);
    fps_auto_reset(&p, now, refresh);
    assert(p.desired == 1 && !p.game_count && !p.have_game_present);
}

static void capacity_and_unsigned_bounds(void) {
    fps_auto_policy p;
    uint64_t now = enter30(&p, base), start = now + 1;
    for (unsigned i = 0; i < FPS_AUTO_HISTORY_CAPACITY; i++) valid(&p,
        start + i, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(p.count == FPS_AUTO_HISTORY_CAPACITY && p.desired == 2);
    uint64_t generation = p.generation;
    fps_auto_observation r = observe(&p, start + FPS_AUTO_HISTORY_CAPACITY,
        refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    assert(r.valid && r.reset && p.reason == FPS_AUTO_REASON_CAPACITY);
    assert(p.count == 1 && !p.misses && !p.coverage_ns && p.generation == generation + 1);
    assert(p.desired == 2); /* A full ring cannot stand in for elapsed coverage. */

    fps_auto_reset(&p, 0, refresh);
    valid(&p, 0, refresh, refresh);
    valid(&p, 1, refresh, refresh);
    assert(p.count == 2 && p.coverage_ns == 1);
    start = UINT64_MAX - 2 * FPS_AUTO_DROP_WINDOW_NS;
    fps_auto_reset(&p, start, refresh);
    for (unsigned i = 0; i <= 400; i++) game_valid(&p, start + i * 25000000ull,
        refresh + 1, refresh);
    assert(p.last_observe_ns == UINT64_MAX && p.desired == 2);
    assert(apply(&p, UINT64_MAX) == 2);
    assert(apply(&p, 0) == 2 && p.reason == FPS_AUTO_REASON_CLOCK && !p.count);
    r = observe(&p, 0, refresh, refresh);
    assert(r.valid && !r.reset && p.count == 1); /* Acquire reset began the new clock epoch. */

    fps_auto_reset(&p, 0, refresh);
    for (unsigned i = 0; i < FPS_AUTO_HISTORY_CAPACITY; i++) present(&p, i);
    assert(p.game_count == FPS_AUTO_HISTORY_CAPACITY && p.desired == 1);
    generation = p.generation;
    present(&p, FPS_AUTO_HISTORY_CAPACITY);
    assert(p.game_count == 1 && p.game_first_ns == FPS_AUTO_HISTORY_CAPACITY);
    assert(p.generation == generation + 1 && p.reason == FPS_AUTO_REASON_CAPACITY);
    assert(!p.last_game_count && !p.drop_streak && p.desired == 1);

    const uint64_t invalid_refresh[] = {0, FPS_AUTO_MARGIN_NS, UINT64_MAX};
    for (unsigned i = 0; i < sizeof invalid_refresh / sizeof invalid_refresh[0]; i++) {
        fps_auto_reset(&p, base, invalid_refresh[i]);
        r = observe(&p, base + 1, refresh, refresh);
        assert(!r.valid && r.reset && p.reason == FPS_AUTO_REASON_INVALID && !p.count);
        assert(apply(&p, base + 1) == 1);
    }
    fps_auto_reset(&p, base, UINT64_MAX / 3);
    r = observe(&p, base + 1, refresh, refresh);
    assert(r.valid && !r.miss); /* Large refresh arithmetic never overflows. */
}

static void production_readiness_span(void) {
    const uint64_t tail = 1000000ull, preparation = 250000ull, allowance = 250000ull;
    runtime_fps_auto_frame frame = {
        .id = 1, .generation = 1, .release_ns = base + 5000000ull,
        .present_ns = base + 10000000ull, .game_interval_ns = 2 * refresh,
        .pacing_ns = 20000000ull, .interval = 2
    };
    uint64_t ready = frame.present_ns + tail;
    uint64_t fast_span = runtime_fps_auto_ready_span(&frame, ready, preparation, allowance);
    assert(fast_span == 2 * refresh - 20000000ull + tail + preparation + allowance);
    assert(fast_span <= refresh - FPS_AUTO_MARGIN_NS);
    assert(runtime_fps_auto_ready_span(&frame, ready, 0, allowance) == fast_span - preparation);
    frame.pacing_ns = 5000000ull;
    uint64_t slow_span = runtime_fps_auto_ready_span(&frame, ready, preparation, allowance);
    assert(slow_span == 2 * refresh - 5000000ull + tail + preparation + allowance);
    assert(slow_span > refresh); /* Same 33 ms game cadence, insufficient intentional pacing. */
    frame.pacing_ns = 20000000ull;
    frame.release_ns = frame.present_ns - 20000000ull;
    /* When post-gate resource/CPU work dominates, take that span, not the sum
     * of overlapping spans, and never add the GPU tail a second time. */
    assert(runtime_fps_auto_ready_span(&frame, ready, preparation, allowance) ==
        20000000ull + tail + preparation + allowance);
    frame.release_ns = base + 5000000ull;

    assert(!runtime_fps_auto_ready_span(NULL, ready, preparation, allowance));
    assert(!runtime_fps_auto_ready_span(&frame, 0, preparation, allowance));
    assert(!runtime_fps_auto_ready_span(&frame, frame.present_ns - 1, preparation, allowance));
    assert(!runtime_fps_auto_ready_span(&frame, ready, preparation, 0));
    runtime_fps_auto_frame malformed = frame;
    malformed.release_ns = 0;
    assert(!runtime_fps_auto_ready_span(&malformed, ready, preparation, allowance));
    malformed = frame;
    malformed.release_ns = malformed.present_ns + 1;
    assert(!runtime_fps_auto_ready_span(&malformed, ready, preparation, allowance));
    malformed = frame;
    malformed.game_interval_ns = 0;
    assert(!runtime_fps_auto_ready_span(&malformed, ready, preparation, allowance));
    malformed = frame;
    malformed.pacing_ns = malformed.game_interval_ns + 1;
    assert(!runtime_fps_auto_ready_span(&malformed, ready, preparation, allowance));
    malformed = (runtime_fps_auto_frame){
        .release_ns = base, .present_ns = base, .game_interval_ns = 1, .pacing_ns = 1
    };
    assert(!runtime_fps_auto_ready_span(&malformed, base, 0, allowance)); /* Zero work span. */
    assert(runtime_fps_auto_ready_span(&malformed, base + 1, 0, allowance) == 1 + allowance);
    malformed = (runtime_fps_auto_frame){
        .release_ns = 1, .present_ns = 1, .game_interval_ns = UINT64_MAX
    };
    assert(!runtime_fps_auto_ready_span(&malformed, 2, 0, 1)); /* Joined-span overflow. */
    malformed = (runtime_fps_auto_frame){
        .release_ns = 1, .present_ns = 2, .game_interval_ns = 1
    };
    assert(!runtime_fps_auto_ready_span(&malformed, 3, UINT64_MAX - 1, 1));
    assert(!runtime_fps_auto_ready_span(&malformed, 3, 0, UINT64_MAX - 1));
    assert(runtime_fps_auto_ready_span(&malformed, 3, UINT64_MAX - 3, 1) == UINT64_MAX);

    fps_auto_policy fast, slow;
    uint64_t transition = enter30(&fast, base);
    assert(enter30(&slow, base) == transition);
    for (unsigned i = 1; i <= 240; i++) {
        ready = transition + i * 2 * refresh;
        frame.present_ns = ready - tail;
        frame.release_ns = frame.present_ns - 5000000ull;
        frame.pacing_ns = 20000000ull;
        fast_span = runtime_fps_auto_ready_span(&frame, ready, preparation, allowance);
        frame.pacing_ns = 5000000ull;
        slow_span = runtime_fps_auto_ready_span(&frame, ready, preparation, allowance);
        fps_auto_observation good = observe(&fast, ready, fast_span, frame.game_interval_ns);
        fps_auto_observation bad = observe(&slow, ready, slow_span, frame.game_interval_ns);
        assert(good.valid && !good.miss && bad.valid && bad.miss);
    }
    assert(fast.desired == 1 && slow.desired == 2);
    assert(apply(&fast, ready) == 1 && apply(&slow, ready) == 2);
    frame.pacing_ns = frame.game_interval_ns + 1;
    uint64_t invalid_span = runtime_fps_auto_ready_span(&frame, ready, preparation, allowance);
    fps_auto_observation invalid = observe(&slow, ready + 2 * refresh, invalid_span, frame.game_interval_ns);
    assert(!invalid.valid && invalid.reset && !slow.count && slow.desired == 2);
}

static void buffered_feedback_preserves_recovery(void) {
    fps_auto_policy p;
    uint64_t transition = enter30(&p, base), generation = p.generation;
    for (unsigned i = 1; i <= 260; i++) {
        uint64_t ready_ns = transition + i * 2 * refresh;
        valid(&p, ready_ns, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
        unsigned desired = p.desired;
        /* Each acquisition arrives four refreshes after independent GPU ready.
         * The next feedback is older than that acquire, but READY gaps are 2R.
         * Presentation buffering must not destroy the eight-second evidence. */
        unsigned interval = apply(&p, ready_ns + 4 * refresh);
        if (desired == 1) {
            assert(interval == 1 && p.generation == generation + 1);
            assert(!p.count && p.last_change_ns == ready_ns + 4 * refresh);
            return;
        }
        assert(interval == 2 && p.count == i && p.generation == generation);
    }
    assert(0 && "intentional queue residence must not block recovery");
}

static void fixed_slot_compatibility(void) {
    fps_auto_policy p;
    fps_auto_reset(&p, base, refresh);
    unsigned at60 = apply(&p, base);
    uint64_t now = enter30(&p, base);
    unsigned at30 = apply(&p, now);
    for (unsigned i = 0; i < 260 && p.desired == 2; i++) {
        now += 2 * refresh;
        valid(&p, now, refresh - FPS_AUTO_MARGIN_NS, 2 * refresh);
    }
    assert(p.desired == 1);
    unsigned recovered = apply(&p, now);
    assert(at60 == 1 && at30 == 2 && recovered == 1);

    /* Feed real policy outputs into BOTH real fixed-clock consumers. Their
     * timelines are adjacent acquisitions, not the long observation windows:
     * neither consumer may reset phase merely because the interval changed. */
    const unsigned intervals[] = {at60, at60, at30, at30, recovered, recovered};
    acquire_clock acquisition = {0};
    vk_present_slots presentation = {0};
    uint64_t previous_release = 0, previous_desired = 0;
    for (unsigned i = 0; i < sizeof intervals / sizeof intervals[0]; i++) {
        uint64_t period = refresh * intervals[i];
        uint64_t release = i ? previous_release + period : base;
        uint64_t callback = i ? previous_release : base;
        acquire_clock_release a = acquire_clock_reserve(&acquisition, 1,
            callback, refresh, intervals[i], release);
        assert(a.mode == ACQUIRE_CLOCK_ACTIVE && a.target_ns == release);
        assert(a.reanchor == (i ? 0 : ACQUIRE_CLOCK_START));
        if (i) assert(!a.reached && release > previous_release); /* No banked/burst release. */
        a = acquire_clock_observe(&acquisition, release, refresh, release);
        assert(a.reached && !a.reanchor && a.target_ns == release);
        a = acquire_clock_observe(&acquisition, release, refresh, release);
        assert(a.target_ns == release); /* Repeated polls never advance a frame. */
        vk_present_slot s = vk_present_slots_reserve(&presentation, 1,
            release, refresh, intervals[i], release);
        uint64_t desired = i ? previous_desired + period : base + period;
        assert(s.mode == VK_PRESENT_SLOTS_ACTIVE && !s.skipped);
        assert(s.interval == intervals[i] && s.refresh_ns == refresh);
        assert(s.desired_ns == desired && s.copy_open_ns == desired);
        assert((release - base) % refresh == 0 && (desired - base) % refresh == 0);
        if (i) assert(desired > previous_desired); /* Queued targets cannot be overtaken. */
        previous_release = release;
        previous_desired = desired;
    }
}

int main(void) {
    initialization();
    drop_strict_thresholds();
    drop_coverage_samples_and_dwell();
    independent_drop_windows_and_spikes();
    inhibition_restarts_eligibility();
    recover_margin_and_paced_game_interval();
    recovery_tail_budget();
    recovery_minimum_samples_and_sparse_feedback();
    no_flapping_after_recovery();
    recovery_hold_commit_boundary();
    failed_return_backoff_and_cap();
    failed_return_commit_boundary();
    healthy_minute_resets_backoff();
    inhibition_does_not_fail_returns();
    backoff_clock_and_overflow_boundaries();
    invalid_missing_stall_and_clock();
    pending_pause_and_mode_reset();
    raw_missing_stall_and_clock();
    capacity_and_unsigned_bounds();
    production_readiness_span();
    buffered_feedback_preserves_recovery();
    fixed_slot_compatibility();
    return 0;
}
