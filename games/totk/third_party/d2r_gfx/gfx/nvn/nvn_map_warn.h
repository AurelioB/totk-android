/* Process-map warning policy only; no mapping/protection or tracker decisions. */
#pragma once
#include <stdint.h>

typedef struct { unsigned log_percent, notice_percent, clear_percent; } nvn_map_warn_thresholds;
typedef struct { int notice, logged_early, logged_notice, logged_unknown; } nvn_map_warn_state;
enum { NVN_MAP_WARN_LOG_EARLY = 1, NVN_MAP_WARN_LOG_NOTICE = 2, NVN_MAP_WARN_LOG_UNKNOWN = 4 };
typedef struct { nvn_map_warn_state state; unsigned logs; } nvn_map_warn_result;

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
