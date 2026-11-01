/* MIT. Adaptive publication arbitration, separate from GPU/compositor ownership.
 * Decisions never retire a buffer or advance committed display order. */
#pragma once
#include <stdint.h>
#include "framegen_slots.h"

typedef struct {
    uint64_t generation, last_slot, latest_real;
} fg_adaptive_present_state;
typedef struct {
    uint64_t slot, frame, epoch, generation;
    int generated;
} fg_adaptive_present_job;
typedef struct {
    uint64_t now, epoch, generation;
    int window, suppressed, ready_real, queued_real;
    uint64_t release; /* opt-in earlier GEN hold end; zero is exactly the r7 hold */
} fg_adaptive_present_context;
typedef enum {
    FG_ADAPTIVE_PRESENT_APPLY, FG_ADAPTIVE_PRESENT_WAIT,
    FG_ADAPTIVE_PRESENT_YIELD, FG_ADAPTIVE_PRESENT_SUPPRESSED,
    FG_ADAPTIVE_PRESENT_WINDOW, FG_ADAPTIVE_PRESENT_ORDER,
    FG_ADAPTIVE_PRESENT_EXPIRED
} fg_adaptive_present_result;

/* Absolute CLOCK_MONOTONIC deadline, deliberately BEFORE the target refresh.
 * A display-callback/slot-reached wait cannot implement this contract. */
static inline uint64_t fg_adaptive_present_deadline(uint64_t slot) {
    return slot>UINT64_C(2000000) ? slot-UINT64_C(2000000) : 0;
}
/* Opt-in latch release may only move the hold EARLIER; zero or a later value
 * leaves the r7 deadline above untouched. Never applies at/after the slot. */
static inline uint64_t fg_adaptive_present_hold(uint64_t slot, uint64_t release) {
    uint64_t deadline=fg_adaptive_present_deadline(slot);
    return release && release<deadline ? release : deadline;
}
/* QCOM predicts past the newest real image without locked30's source-slot
 * readiness requirement. Both GPU tickets must instead finish before THIS
 * fractional output's immutable refresh target. Never infer this contract
 * from a backend's legacy interpolation flag or a nominal real interval. */
static inline framegen_child_reason fg_adaptive_child_status(uint64_t source_ready,
        uint64_t output_ready,uint64_t child_slot,uint64_t now) {
    if (!source_ready || !output_ready) return FRAMEGEN_CHILD_SOURCE_MISSING;
    if (source_ready>=child_slot || output_ready>=child_slot)
        return FRAMEGEN_CHILD_SOURCE_LATE;
    if (now>=child_slot) return FRAMEGEN_CHILD_DEADLINE;
    return FRAMEGEN_CHILD_READY;
}
static inline fg_adaptive_present_result fg_adaptive_present_decide(
        const fg_adaptive_present_state *state, fg_adaptive_present_job job,
        fg_adaptive_present_context context) {
    if (!context.window || job.generation!=context.generation)
        return FG_ADAPTIVE_PRESENT_WINDOW;
    uint64_t last=state->generation==job.generation ? state->last_slot : 0;
    uint64_t real=state->generation==job.generation ? state->latest_real : 0;
    if (job.generated && (context.suppressed || context.epoch!=job.epoch ||
            context.ready_real || job.frame<real))
        return FG_ADAPTIVE_PRESENT_SUPPRESSED;
    if (!job.slot || job.slot<=last) return FG_ADAPTIVE_PRESENT_ORDER;
    if (job.generated && context.now>=job.slot) return FG_ADAPTIVE_PRESENT_EXPIRED;
    /* Return the still-owned candidate to the ordered queue instead of holding
     * an earlier real behind its deadline wait or transaction preparation. */
    if (job.generated && context.queued_real) return FG_ADAPTIVE_PRESENT_YIELD;
    if (job.generated && context.now<fg_adaptive_present_hold(job.slot,context.release))
        return FG_ADAPTIVE_PRESENT_WAIT;
    return FG_ADAPTIVE_PRESENT_APPLY;
}
/* Call only AFTER the serialized platform handoff, never on a decision or a
 * staged transaction. Cancellation therefore cannot consume a real's slot. */
static inline void fg_adaptive_present_committed(fg_adaptive_present_state *state,
        fg_adaptive_present_job job) {
    if (state->generation!=job.generation)
        *state=(fg_adaptive_present_state){.generation=job.generation};
    state->last_slot=job.slot;
    if (!job.generated && job.frame>state->latest_real) state->latest_real=job.frame;
}
