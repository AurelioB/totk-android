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
