/* Display callback policy: one credit per fresh physical-vsync callback, never a
 * bank of frames to replay after a stall. All callers serialize access. */
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t frame_ns, tick, last_tick;
} display_pacer;

static inline int display_pace_tick(display_pacer *p, uint64_t frame_ns) {
    if (!frame_ns || frame_ns <= p->frame_ns) return 0;
    p->frame_ns = frame_ns;
    p->tick++;
    return 1;
}

/* Number of future callbacks still needed. A zero/negative NVN interval is
 * intentionally capped at one refresh in display-paced mode. */
static inline unsigned display_pace_pending(const display_pacer *p, unsigned interval) {
    if (!interval) interval = 1;
    uint64_t elapsed = p->tick - p->last_tick;
    return elapsed >= interval ? 0 : interval - (unsigned)elapsed;
}

static inline int display_pace_take(display_pacer *p, unsigned interval) {
    if (display_pace_pending(p, interval)) return 0;
    p->last_tick = p->tick;
    return 1;
}

#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
/* Frontend bridge used by the NVN display-link adapters. */
int runtime_android_display_pace_active(void);
int runtime_android_display_hz(void);
int runtime_android_display_wait(unsigned interval);
#endif
