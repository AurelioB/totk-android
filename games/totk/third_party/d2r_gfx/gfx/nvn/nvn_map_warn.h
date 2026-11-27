/* Process-map warning policy only; no mapping/protection or tracker decisions. */
#pragma once
#include <stdint.h>

typedef struct { unsigned log_percent, notice_percent, clear_percent; } nvn_map_warn_thresholds;
typedef struct { int notice, logged_early, logged_notice, logged_unknown; } nvn_map_warn_state;
enum { NVN_MAP_WARN_LOG_EARLY = 1, NVN_MAP_WARN_LOG_NOTICE = 2, NVN_MAP_WARN_LOG_UNKNOWN = 4 };
typedef struct { nvn_map_warn_state state; unsigned logs; } nvn_map_warn_result;

/* TEST ONLY: real counts, changed thresholds, unchanged policy/state. Twelve samples show,
   twelve hold below the raised notice threshold, then a raised clear threshold removes it.
   A normal count between 3% and 99% never needs to approach the real limit. */
enum { NVN_MAP_WARN_TEST_PHASE_SAMPLES = 12 };
static inline nvn_map_warn_thresholds nvn_map_warn_test_thresholds(unsigned sample) {
    if (sample <= NVN_MAP_WARN_TEST_PHASE_SAMPLES) return (nvn_map_warn_thresholds){1, 3, 2};
    if (sample <= 2 * NVN_MAP_WARN_TEST_PHASE_SAMPLES) return (nvn_map_warn_thresholds){1, 100, 2};
    return (nvn_map_warn_thresholds){1, 100, 99};
}

/* ceil(limit * percent / 100), without overflowing even at UINT64_MAX. */
static inline uint64_t nvn_map_warn_boundary(uint64_t limit, unsigned percent) {
    return (limit / 100) * percent + ((limit % 100) * percent + 99) / 100;
}

enum {
    NVN_MAP_WARN_SLOW_MS = 60000, NVN_MAP_WARN_MEDIUM_MS = 20000, NVN_MAP_WARN_FAST_MS = 5000,
    NVN_MAP_WARN_JUMP_ROWS = 2048, NVN_MAP_WARN_LOAD_RESERVE_ROWS = 15000
};
typedef struct {
    uint64_t last_count, fast_until_ns;
    unsigned band; /* 0: slow, 1: medium, 2: fast */
    int known;
} nvn_map_warn_cadence;
static inline void nvn_map_warn_cadence_boost(nvn_map_warn_cadence *s, uint64_t now) {
    const uint64_t hold = UINT64_C(10000000000); /* two fast periods after activity or a large jump */
    s->fast_until_ns = now > UINT64_MAX - hold ? UINT64_MAX : now + hold;
}
/* Called only for a newly completed sample. A reused sample cannot renew a jump hold.
   Escalate at 40/60%; de-escalate below 37/55%. Also keep one observed-size load of
   headroom to the normal 88% notice, even on kernels with a smaller map limit. */
static inline uint64_t nvn_map_warn_cadence_sample(nvn_map_warn_cadence *s, uint64_t count,
                                                  uint64_t limit, uint64_t now) {
    uint64_t jump = s->known && count > s->last_count ? count - s->last_count : 0;
    if (!count || !limit) { s->known = 0; s->band = 2; return 0; }
    if (!s->known) s->band = count >= nvn_map_warn_boundary(limit, 60) ? 2 :
                           count >= nvn_map_warn_boundary(limit, 40) ? 1 : 0;
    else if (count >= nvn_map_warn_boundary(limit, 60)) s->band = 2;
    else if (s->band == 2 && count < nvn_map_warn_boundary(limit, 55)) s->band = 1;
    if (count >= nvn_map_warn_boundary(limit, 40) && s->band == 0) s->band = 1;
    else if (count < nvn_map_warn_boundary(limit, 37)) s->band = 0;
    uint64_t notice = nvn_map_warn_boundary(limit, 88);
    if (notice <= NVN_MAP_WARN_LOAD_RESERVE_ROWS || count >= notice - NVN_MAP_WARN_LOAD_RESERVE_ROWS)
        s->band = 2;
    if (jump > NVN_MAP_WARN_JUMP_ROWS) nvn_map_warn_cadence_boost(s, now);
    s->last_count = count; s->known = 1;
    return jump > NVN_MAP_WARN_JUMP_ROWS ? jump : 0;
}
static inline unsigned nvn_map_warn_cadence_period(const nvn_map_warn_cadence *s, uint64_t now) {
    if (!s->known || now < s->fast_until_ns || s->band == 2) return NVN_MAP_WARN_FAST_MS;
    return s->band == 1 ? NVN_MAP_WARN_MEDIUM_MS : NVN_MAP_WARN_SLOW_MS;
}

/* Zero count is unknown (a live process cannot have zero mappings). Unknown clears the
   notice, never reuses a stale count, and logs only once per process, like each threshold. */
static inline nvn_map_warn_result nvn_map_warn_step(nvn_map_warn_state previous, uint64_t count,
                                                   uint64_t limit, nvn_map_warn_thresholds thresholds) {
    nvn_map_warn_result out = {.state = previous};
    if (!count || !limit) {
        out.state.notice = 0;
        if (!previous.logged_unknown) out.logs |= NVN_MAP_WARN_LOG_UNKNOWN;
        out.state.logged_unknown = 1;
        return out;
    }
    if (count >= nvn_map_warn_boundary(limit, thresholds.log_percent)) {
        if (!previous.logged_early) out.logs |= NVN_MAP_WARN_LOG_EARLY;
        out.state.logged_early = 1;
    }
    if (count >= nvn_map_warn_boundary(limit, thresholds.notice_percent)) {
        out.state.notice = 1;
        if (!previous.logged_notice) out.logs |= NVN_MAP_WARN_LOG_NOTICE;
        out.state.logged_notice = 1;
    } else if (count < nvn_map_warn_boundary(limit, thresholds.clear_percent)) out.state.notice = 0;
    return out;
}
