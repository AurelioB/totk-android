/* Opt-in Android 60/30 admission policy. Pure, allocation-free and serialized
 * by its owner; timestamps and durations are monotonic nanoseconds. This does
 * not change guest window state, pace frames, or alter GPU/resource lifetimes.
 * The owner resets on refresh/guest-mode changes and rejects old-epoch feedback.
 * Only apply, at acquire, may change the effective interval after reset. */
#pragma once
#include <stdint.h>

enum {
    FPS_AUTO_HISTORY_CAPACITY = 1024, /* >= eight seconds at 60 Hz */
    FPS_AUTO_DROP_MIN_SAMPLES = 120, /* sustained 45 fps supplies ~225 */
    FPS_AUTO_DROP_WINDOWS = 2,
    FPS_AUTO_DROP_FPS = 55,
    FPS_AUTO_RECOVER_MIN_SAMPLES = 180,
    FPS_AUTO_RECOVER_MISS_PERCENT = 5,
    FPS_AUTO_RETURN_BACKOFF_MAX = 8
};
#define FPS_AUTO_DROP_WINDOW_NS 5000000000ull
#define FPS_AUTO_DROP_COVERAGE_NS 4750000000ull
#define FPS_AUTO_DROP_MISS_SPAN_NS 5000000000ull
#define FPS_AUTO_RECOVER_WINDOW_NS 8000000000ull
#define FPS_AUTO_RECOVER_COVERAGE_NS 7500000000ull
/* Failed-return holdback idea: LASRecomp-Android-Plus (Peiikai, MIT).
 * Independently implemented; no game-specific offsets or pacing code reused.
 * https://github.com/Nycolazs/LASRecomp-Android-Plus/blob/cca791a3e7218e1dac95e7d4a11b8f97dba3ea51/runtime/nvn/pace.c */
#define FPS_AUTO_FAILED_RETURN_NS 20000000000ull
#define FPS_AUTO_HEALTHY_RETURN_NS 60000000000ull
#define FPS_AUTO_DWELL_NS 5000000000ull
#define FPS_AUTO_STALL_NS 1000000000ull
#define FPS_AUTO_MARGIN_NS 1000000ull
#define FPS_AUTO_RECOVER_MARGIN_NS 2000000ull

typedef enum {
    FPS_AUTO_REASON_RESET,
    FPS_AUTO_REASON_DROP,
    FPS_AUTO_REASON_RECOVER,
    FPS_AUTO_REASON_INVALID,
    FPS_AUTO_REASON_STALL,
    FPS_AUTO_REASON_CLOCK,
    FPS_AUTO_REASON_MISSING,
    FPS_AUTO_REASON_CAPACITY,
    FPS_AUTO_REASON_INHIBITED
} fps_auto_reason;

typedef struct {
    struct {
        uint64_t now_ns;
        unsigned char miss;
    } samples[FPS_AUTO_HISTORY_CAPACITY];
    uint64_t refresh_ns, generation, last_change_ns, coverage_ns;
    uint64_t last_observe_ns, last_apply_ns;
    uint64_t last_return_ns, recovery_start_ns, healthy_since_ns;
    unsigned return_backoff, have_return;
    unsigned interval, desired, count, misses, head, have_observe;
    /* Raw HLE presents own downshift evidence, independently of delayed READY
     * feedback and its CPU/busy-time diagnostics. Completed-window statistics
     * survive the next window's first sample for acquire-side decision logs. */
    uint64_t last_game_present_ns, game_first_ns, game_last_ns;
    uint64_t game_first_slow_ns, game_last_slow_ns;
    unsigned have_game_present, game_count, game_slow_count;
    uint64_t last_game_fps_milli, last_game_coverage_ns;
    unsigned last_game_count;
    /* Existing transition-log fields describe that same raw window. "miss"
     * here means a raw present interval strictly slower than 55 fps. */
    uint64_t last_drop_coverage_ns;
    uint64_t drop_first_miss_ns, drop_last_miss_ns;
    uint64_t drop_window_first_slow_ns;
    unsigned drop_streak, last_drop_count, last_drop_misses;
    fps_auto_reason reason;
} fps_auto_policy;

typedef struct {
    unsigned valid, miss, reset;
} fps_auto_observation;

/* Clearing metadata is O(1); unused ring entries are never read. No per-frame
 * array initialization, copying, sorting, or whole-history scanning is needed. */
static inline void fps_auto_clear_samples(fps_auto_policy *p) {
    p->count = p->misses = p->head = 0;
    p->coverage_ns = 0;
}
static inline void fps_auto_clear_game_window(fps_auto_policy *p) {
    p->game_count = p->game_slow_count = 0;
    p->game_first_ns = p->game_last_ns = 0;
    p->game_first_slow_ns = p->game_last_slow_ns = 0;
}
static inline void fps_auto_clear_history(fps_auto_policy *p) {
    fps_auto_clear_samples(p);
    fps_auto_clear_game_window(p);
    p->have_game_present = 0;
    p->last_game_present_ns = 0;
    p->last_game_fps_milli = p->last_game_coverage_ns = 0;
    p->last_game_count = 0;
    p->drop_streak = p->last_drop_count = p->last_drop_misses = 0;
    p->last_drop_coverage_ns = 0;
    p->drop_first_miss_ns = p->drop_last_miss_ns = 0;
    p->drop_window_first_slow_ns = 0;
}
static inline int fps_auto_refresh_valid(uint64_t refresh_ns) {
    return refresh_ns > FPS_AUTO_MARGIN_NS && refresh_ns <= UINT64_MAX / 3;
}
static inline void fps_auto_reset(fps_auto_policy *p, uint64_t now_ns, uint64_t refresh_ns) {
    if (!p) return;
    fps_auto_clear_history(p);
    p->refresh_ns = refresh_ns;
    p->generation = 1;
    p->last_change_ns = p->last_apply_ns = now_ns;
    p->last_observe_ns = 0;
    p->last_return_ns = 0;
    p->recovery_start_ns = p->healthy_since_ns = now_ns;
    p->return_backoff = 1;
    p->have_return = 0;
    p->interval = p->desired = 1;
    p->have_observe = 0;
    p->reason = FPS_AUTO_REASON_RESET;
}

/* Remaining minimum 30-fps hold, independent of the rolling healthy window.
 * Subtract elapsed time rather than forming a deadline that could overflow. */
static inline uint64_t fps_auto_recovery_wait_ns(const fps_auto_policy *p, uint64_t now_ns) {
    if (!p || p->interval != 2) return 0;
    uint64_t hold_ns = FPS_AUTO_RECOVER_WINDOW_NS * p->return_backoff;
    if (now_ns < p->recovery_start_ns) return hold_ns;
    uint64_t elapsed_ns = now_ns - p->recovery_start_ns;
    return elapsed_ns < hold_ns ? hold_ns - elapsed_ns : 0;
}

/* Discontinuities invalidate pending decisions as well as evidence, restart
 * dwell, and retain the current effective interval. Explicit reset initializes
 * interval 1. The frontend owns its epoch: generation changes here are useful
 * for logging/invalidation but reset itself always initializes generation 1. */
static inline void fps_auto_discard(fps_auto_policy *p, uint64_t now_ns, fps_auto_reason reason) {
    fps_auto_clear_history(p);
    p->desired = p->interval;
    p->last_change_ns = now_ns;
    p->healthy_since_ns = now_ns;
    if (reason == FPS_AUTO_REASON_CLOCK) {
        p->have_return = 0; /* The previous return's clock epoch is incomparable. */
        p->recovery_start_ns = now_ns;
    }
    p->generation++;
    p->reason = reason;
}

/* Loading/transition feedback cannot start or complete an eligible window.
 * Repeated inhibition restarts dwell without changing the effective interval
 * or treating a cancelled decision as a failed return. Backoff survives it.
 * The owner rejects old-epoch completions before observing again. */
static inline void fps_auto_inhibit(fps_auto_policy *p, uint64_t now_ns) {
    if (!p) return;
    if (now_ns < p->last_apply_ns ||
        (p->have_game_present && now_ns < p->last_game_present_ns) ||
        (p->have_observe && now_ns < p->last_observe_ns)) {
        p->have_return = 0;
        p->recovery_start_ns = now_ns;
    }
    fps_auto_discard(p, now_ns, FPS_AUTO_REASON_INHIBITED);
    p->last_observe_ns = 0;
    p->last_apply_ns = now_ns;
    p->have_observe = 0;
}

/* Called once at each eligible current-epoch raw HLE present, before any
 * subtraction of pacing/blocked time and independently of GPU completion.
 * Only this stream may request 60->30. Two independent half-open five-second
 * windows each need >=4.75 s usable coverage, enough raw frames, and actual
 * (N-1)/coverage strictly below 55 fps. Boundary frames seed only the next
 * window. Slow interval events across the pair must span >=5 s, rejecting a
 * single two-second spike even when it straddles the window boundary.
 *
 * Timestamps, READY feedback and acquires have independent order watermarks.
 * A present never applies a mode change. Invalid clocks/gaps cancel evidence;
 * the owner additionally resets/inhibits and rejects old-epoch presents. */
static inline void fps_auto_game_present(fps_auto_policy *p, uint64_t now_ns) {
    if (!p || (p->interval != 1 && p->interval != 2)) return;
    unsigned had_present = p->have_game_present;
    uint64_t previous_ns = p->last_game_present_ns;
    if (had_present && now_ns <= previous_ns) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_CLOCK);
        return;
    }
    if (had_present && now_ns - previous_ns > FPS_AUTO_STALL_NS) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_STALL);
        return;
    }
    if (!fps_auto_refresh_valid(p->refresh_ns)) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_INVALID);
        return;
    }
    p->last_game_present_ns = now_ns;
    p->have_game_present = 1;
    if (p->interval != 1) return; /* Paced 30-fps presents cannot prove recovery. */
    if (p->game_count && now_ns - p->game_first_ns >= FPS_AUTO_DROP_WINDOW_NS) {
        uint64_t coverage_ns = p->game_last_ns - p->game_first_ns;
        p->last_game_count = p->last_drop_count = p->game_count;
        p->last_game_coverage_ns = p->last_drop_coverage_ns = coverage_ns;
        p->last_drop_misses = p->game_slow_count;
        /* Window duration <5 s and count <=1024 bound both multiplications,
         * even at UINT64_MAX timestamps. Compare the exact rate, not its
         * rounded milli-fps diagnostic: exact 55 fps must not downshift. */
        uint64_t intervals = p->game_count - 1;
        p->last_game_fps_milli = coverage_ns ? intervals * 1000000000000ull / coverage_ns : 0;
        if (p->game_count >= FPS_AUTO_DROP_MIN_SAMPLES &&
            coverage_ns >= FPS_AUTO_DROP_COVERAGE_NS &&
            intervals * 1000000000ull < FPS_AUTO_DROP_FPS * coverage_ns) {
            p->drop_first_miss_ns = p->drop_streak
                ? p->drop_window_first_slow_ns : p->game_first_slow_ns;
            p->drop_window_first_slow_ns = p->game_first_slow_ns;
            p->drop_last_miss_ns = p->game_last_slow_ns;
            if (p->drop_streak < FPS_AUTO_DROP_WINDOWS) p->drop_streak++;
        } else {
            p->drop_streak = 0;
            p->drop_first_miss_ns = p->drop_last_miss_ns = 0;
            p->drop_window_first_slow_ns = 0;
        }
        fps_auto_clear_game_window(p);
        p->desired = 1;
        if (p->drop_streak >= FPS_AUTO_DROP_WINDOWS &&
            p->drop_last_miss_ns - p->drop_first_miss_ns >= FPS_AUTO_DROP_MISS_SPAN_NS &&
            now_ns >= p->last_change_ns && now_ns - p->last_change_ns >= FPS_AUTO_DWELL_NS) {
            p->desired = 2;
            p->reason = FPS_AUTO_REASON_DROP;
        }
    }
    if (p->game_count == FPS_AUTO_HISTORY_CAPACITY) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_CAPACITY);
        p->last_game_present_ns = now_ns;
        p->have_game_present = 1;
        had_present = 0;
    }
    if (!p->game_count) p->game_first_ns = now_ns;
    p->game_last_ns = now_ns;
    p->game_count++;
    if (had_present && now_ns - previous_ns > 1000000000ull / FPS_AUTO_DROP_FPS) {
        if (!p->game_slow_count) p->game_first_slow_ns = now_ns;
        p->game_last_slow_ns = now_ns;
        p->game_slow_count++;
    }
}

/* Observe exactly once per completed immutable current-epoch frame. readiness
 * joins post-gate release-to-independent-final-ticket-ready (less matching
 * guest-kernel blocked time) with CPU production plus present-to-ready, taking
 * the larger span before measured copy preparation and steady allowance.
 * Required GPU/resource waits and the independent GPU tail belong in it;
 * intentional acquisition/slot/FIFO residence and guest-kernel idle do not.
 * game_interval is CPU production stripped of intentional pacing/blocked time.
 * Its misses and readiness misses at interval 1 are diagnostic only: neither
 * their share nor delayed READY sample counts may request a downshift.
 *
 * Recovery uses an eight-second rolling window: >=95% of readiness samples
 * must fit physical refresh +2 ms, with 7.5 s coverage and 180 samples. Valid
 * slow frames consume that 5% tail budget; an isolated READY gap over 3R does
 * not discard it. Recovery also requires 8/16/32/64 seconds at interval 2,
 * depending on acquire-applied failed returns; healthy evidence alone cannot
 * shorten that hold. Missing feedback cannot replace the sample/coverage
 * minimum. Zero/malformed data, stalls (>1 s), and duplicate/reversed READY
 * times clear all evidence and pending changes. Exactly 1 s is not a stall.
 * The 1024-entry bound never silently truncates an unexpired window: overflow
 * starts a new evidence run. The caller must reset when refresh/mode changes
 * and reject prior-epoch frames. READY time can precede the newest present or
 * acquire; comparison is only against the previous READY watermark. */
static inline fps_auto_observation fps_auto_observe(fps_auto_policy *p,
        uint64_t now_ns, uint64_t readiness_ns, uint64_t game_interval_ns) {
    fps_auto_observation r = {0};
    if (!p || (p->interval != 1 && p->interval != 2)) return r;
    unsigned had_observe = p->have_observe;
    uint64_t previous_ns = p->last_observe_ns;
    p->last_observe_ns = now_ns;
    p->have_observe = 1;
    if (had_observe && now_ns <= previous_ns) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_CLOCK);
        r.reset = 1;
        return r;
    }
    if ((had_observe && now_ns - previous_ns > FPS_AUTO_STALL_NS) ||
        readiness_ns > FPS_AUTO_STALL_NS || game_interval_ns > FPS_AUTO_STALL_NS) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_STALL);
        r.reset = 1;
        return r;
    }
    if (!fps_auto_refresh_valid(p->refresh_ns) || !readiness_ns || !game_interval_ns) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_INVALID);
        r.reset = 1;
        return r;
    }
    if (p->interval == 1 && !had_observe && now_ns >= p->healthy_since_ns &&
        now_ns - p->healthy_since_ns > FPS_AUTO_STALL_NS)
        p->healthy_since_ns = now_ns; /* Unobserved wall time is not a healthy run. */
    if (p->interval == 1 && p->count &&
        now_ns - p->samples[p->head].now_ns >= FPS_AUTO_DROP_WINDOW_NS) {
        fps_auto_clear_samples(p);
    }
    if (p->interval == 2) {
        while (p->count &&
            now_ns - p->samples[p->head].now_ns > FPS_AUTO_RECOVER_WINDOW_NS) {
            p->misses -= p->samples[p->head].miss;
            p->head = (p->head + 1) % FPS_AUTO_HISTORY_CAPACITY;
            p->count--;
        }
    }
    if (p->count == FPS_AUTO_HISTORY_CAPACITY) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_CAPACITY);
        r.reset = 1;
    }
    r.valid = 1;
    r.miss = p->interval == 1
        ? readiness_ns > p->refresh_ns || (game_interval_ns > p->refresh_ns &&
            game_interval_ns - p->refresh_ns > FPS_AUTO_MARGIN_NS)
        : readiness_ns > p->refresh_ns + FPS_AUTO_RECOVER_MARGIN_NS;
    unsigned next = (p->head + p->count) % FPS_AUTO_HISTORY_CAPACITY;
    p->samples[next].now_ns = now_ns;
    p->samples[next].miss = (unsigned char)r.miss;
    p->count++;
    p->misses += r.miss;
    p->coverage_ns = now_ns - p->samples[p->head].now_ns;
    if (p->interval == 2) {
        p->desired = 2; /* Fresh recovery evidence may retract a pending return. */
        if (now_ns >= p->last_change_ns && now_ns - p->last_change_ns >= FPS_AUTO_DWELL_NS &&
            !fps_auto_recovery_wait_ns(p, now_ns) &&
            p->count >= FPS_AUTO_RECOVER_MIN_SAMPLES &&
            p->coverage_ns >= FPS_AUTO_RECOVER_COVERAGE_NS &&
            (uint64_t)p->misses * 100 <= (uint64_t)p->count * FPS_AUTO_RECOVER_MISS_PERCENT) {
            p->desired = 1;
            p->reason = FPS_AUTO_REASON_RECOVER;
        }
    }
    return r;
}

/* Constant-time acquire-side commit. The return is always a fixed-slot-
 * compatible interval 1 or 2, including absent/uninitialized policy state.
 * No observation may change effective interval; a commit clears history before
 * a new epoch can collect feedback. More than one second without feedback
 * cancels stale evidence/decisions, even if acquires continue. At interval 1
 * this follows raw presents, not delayed READY; recovery follows READY.
 * Same-time repeated acquires are harmless; reversed acquire time resets it.
 * An actual 60->30 less than 20 s after the last applied return doubles the
 * next 30 hold, up to 8x. A continuous, observed 60 s at 60 resets that debt.
 * Requests, invalidation, and loading never count as applied failures. */
static inline unsigned fps_auto_apply(fps_auto_policy *p, uint64_t now_ns) {
    if (!p || (p->interval != 1 && p->interval != 2)) return 1;
    uint64_t previous_ns = p->last_apply_ns;
    p->last_apply_ns = now_ns;
    if (now_ns < previous_ns) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_CLOCK);
        p->have_observe = 0;
        return p->interval;
    }
    uint64_t activity_ns = previous_ns;
    if (p->have_observe && p->last_observe_ns > activity_ns) activity_ns = p->last_observe_ns;
    if (p->have_game_present && p->last_game_present_ns > activity_ns)
        activity_ns = p->last_game_present_ns;
    uint64_t feedback_ns = p->interval == 1 && p->have_game_present
        ? p->last_game_present_ns : p->last_observe_ns;
    unsigned have_feedback = p->interval == 1 && p->have_game_present ? 1 : p->have_observe;
    if ((now_ns > activity_ns && now_ns - activity_ns > FPS_AUTO_STALL_NS) ||
        (have_feedback && now_ns > feedback_ns && now_ns - feedback_ns > FPS_AUTO_STALL_NS)) {
        fps_auto_discard(p, now_ns, FPS_AUTO_REASON_STALL);
        p->have_observe = 0;
    }
    if (p->return_backoff > 1 && p->interval == 1 && p->have_observe &&
        now_ns >= p->last_observe_ns &&
        now_ns >= p->healthy_since_ns &&
        now_ns - p->healthy_since_ns >= FPS_AUTO_HEALTHY_RETURN_NS)
        p->return_backoff = 1;
    if (p->desired != p->interval && (p->desired == 1 || p->desired == 2) &&
        now_ns >= p->last_change_ns && now_ns - p->last_change_ns >= FPS_AUTO_DWELL_NS &&
        (p->desired != 1 || !fps_auto_recovery_wait_ns(p, now_ns))) {
        if (p->interval == 1) {
            if (p->have_return && now_ns >= p->last_return_ns &&
                now_ns - p->last_return_ns < FPS_AUTO_FAILED_RETURN_NS &&
                p->return_backoff < FPS_AUTO_RETURN_BACKOFF_MAX)
                p->return_backoff *= 2;
            p->have_return = 0;
            p->recovery_start_ns = now_ns;
        } else {
            p->last_return_ns = p->healthy_since_ns = now_ns;
            p->have_return = 1;
        }
        p->interval = p->desired;
        fps_auto_clear_history(p);
        p->last_change_ns = now_ns;
        p->have_observe = 0;
        p->generation++;
    }
    return p->interval;
}
