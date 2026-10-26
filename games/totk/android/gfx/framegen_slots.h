/* A synthetic child never advances the real-frame display lattice. */
#pragma once
#include <stdint.h>

static inline uint64_t framegen_midpoint(uint64_t real_slot, uint64_t refresh,
                                         unsigned interval, uint64_t now) {
    /* This prototype promises only 30 -> 60 on a physical 60 Hz mode.
     * Refuse late source reservations and leave enough recording/handoff room. */
    if (interval != 2 || refresh < 16000000 || refresh > 17300000 ||
        !real_slot || now > real_slot || real_slot > INT64_MAX - refresh)
        return 0;
    return real_slot + refresh;
}

typedef enum {
    FRAMEGEN_CHILD_READY, FRAMEGEN_CHILD_SOURCE_MISSING, FRAMEGEN_CHILD_SOURCE_LATE,
    FRAMEGEN_CHILD_DEADLINE, FRAMEGEN_CHILD_COMPLETION, FRAMEGEN_CHILD_OUTPUT,
    FRAMEGEN_CHILD_WINDOW, FRAMEGEN_CHILD_POST, FRAMEGEN_CHILD_REASONS
} framegen_child_reason;

/* Interpolation has already paid one real tick of latency. Only the child's
 * own physical refresh deadline matters, never the parent's reserved-late
 * flag. Fence readiness is independent of presenter FIFO residence. */
static inline framegen_child_reason framegen_interp_child_status(uint64_t source_ready,
        uint64_t child_slot, uint64_t now) {
    if (!source_ready) return FRAMEGEN_CHILD_SOURCE_MISSING;
    if (source_ready >= child_slot) return FRAMEGEN_CHILD_SOURCE_LATE;
    if (now >= child_slot) return FRAMEGEN_CHILD_DEADLINE;
    return FRAMEGEN_CHILD_READY;
}

static inline int framegen_child_ready(uint64_t source_ready, uint64_t real_slot,
                                       uint64_t child_slot, uint64_t now) {
    return source_ready && source_ready <= real_slot && now < child_slot;
}
