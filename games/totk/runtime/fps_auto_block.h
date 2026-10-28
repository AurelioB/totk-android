/* Original nested-wait accounting; LASRecomp-Android-Plus's MIT pacing
 * design inspired separating blocked wall time from production demand.
 * https://github.com/Nycolazs/LASRecomp-Android-Plus (cca791a3).
 * Caller owns one counter per host thread. No clocks, allocation or locks. */
#pragma once
#include <stdint.h>
#include <limits.h>
typedef struct { uint64_t total_ns, start_ns; unsigned depth; } fps_auto_block_clock;
static inline int fps_auto_block_begin(fps_auto_block_clock *p, uint64_t now) {
    if (p->depth == UINT_MAX) return 0;
    if (!p->depth++) p->start_ns = now;
    return 1;
}
static inline void fps_auto_block_end(fps_auto_block_clock *p, uint64_t now) {
    if (!p->depth || --p->depth) return;
    if (now >= p->start_ns) {
        uint64_t elapsed = now - p->start_ns;
        p->total_ns = elapsed > UINT64_MAX - p->total_ns ? UINT64_MAX : p->total_ns + elapsed;
    }
}
