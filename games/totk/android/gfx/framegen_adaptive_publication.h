/* MIT. Scalar publication bridge/audit. Caller holds the presenter mutex.
 * No decision here retires GPU/compositor storage or retracts an apply. */
#pragma once
#include "framegen_adaptive_slots.h"
#include "../../runtime/present_clock.h"

#define FGS_PUBLICATION_HISTORY 64u
#define FGS_PUBLICATION_CONTEXTS 2u

typedef struct {
    uint64_t frame, source_epoch, window_epoch;
    uint64_t ready_ns, first_attempt_ns;
    unsigned ready_time_known;
} fgs_real_observation;
typedef struct {
    unsigned index;
    uint64_t sequence, generation;
} fgs_transaction_id;
typedef struct {
    fgs_transaction_id id;
    fgs_slot slot;
    uint64_t source_frame, source_epoch, apply_ns;
} fgs_applied_generation;
typedef struct {
    fgs_applied_generation generated[FGS_PUBLICATION_HISTORY];
    unsigned next;
} fgs_publication_history;
typedef struct {
    fgs_applied_generation generated[FGS_PUBLICATION_CONTEXTS];
    unsigned count;
    uint64_t start_ns;
} fgs_capacity_observation;
typedef enum {
    FGS_REAL_NOT_LATE,
    FGS_REAL_READY_AFTER_GEN_APPLY,
    FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED,
    FGS_REAL_READY_TIME_UNKNOWN
} fgs_late_real_kind;

static inline int fgs_transaction_equal(fgs_transaction_id a, fgs_transaction_id b) {
    return a.index==b.index && a.sequence==b.sequence && a.generation==b.generation;
}
static inline void fgs_publication_record(fgs_publication_history *history,
        fgs_applied_generation generation) {
    history->generated[history->next]=generation;
    history->next=(history->next+1)%FGS_PUBLICATION_HISTORY;
}
static inline const fgs_applied_generation *fgs_publication_find(
        const fgs_publication_history *history, fgs_transaction_id id) {
    if (!id.sequence) return NULL;
    for (unsigned i=0;i<FGS_PUBLICATION_HISTORY;i++) {
        const fgs_applied_generation *g=&history->generated[i];
        if (g->id.sequence && fgs_transaction_equal(g->id,id)) return g;
    }
    return NULL;
}
/* Deliberately no legacy callback target argument: READY newer reals must be
 * replanned on the SAME admitted API-clock lattice as the child. Planning is
 * read-only; caller updates queue metadata/requeues before any platform apply. */
static inline int fgs_publication_real_plan(const fgs_state *state,
        const fgs_request *child, fgs_real_observation real, uint64_t now_ns,
        fgs_real_plan *plan) {
    if (!state || !child || !plan || !real.ready_ns || real.ready_ns>now_ns ||
            real.frame<=child->frame || real.source_epoch!=child->source_epoch ||
            real.window_epoch!=child->slot.window_epoch ||
            fgs_validate_slot(state,child->slot)!=FGS_OK) return 0;
    if (fgs_peek_real(state,now_ns,plan)!=FGS_OK) return 0;
    uint64_t desired_ns;
    return runtime_present_adaptive_slot_desired(plan->slot.target_ns,state->period_ns,&desired_ns);
}
/* A queue re-anchor remains provisional. Stable REAL-before-GEN ordering at
 * equality makes YIELD actually service the real instead of spinning on GEN. */
static inline int fgs_publication_queue_before(uint64_t left_ns, int left_generated,
        uint64_t right_ns, int right_generated) {
    return left_ns<right_ns || (left_ns==right_ns && !left_generated && right_generated);
}
/* Potential residual, NOT an observed SF displacement. Ready is the backend's
 * CPU fence-publication timestamp, never a claimed calibrated GPU end time. */
static inline fgs_late_real_kind fgs_publication_late_real(
        const fgs_applied_generation *g, fgs_real_observation real) {
    if (!g || !g->id.sequence || !g->apply_ns ||
            g->slot.window_epoch!=real.window_epoch || g->source_epoch!=real.source_epoch ||
            real.frame<=g->source_frame)
        return FGS_REAL_NOT_LATE;
    if (!real.ready_time_known || !real.ready_ns) return FGS_REAL_READY_TIME_UNKNOWN;
    if (real.ready_ns>=g->slot.target_ns) return FGS_REAL_NOT_LATE;
    if (real.ready_ns>=g->apply_ns) return FGS_REAL_READY_AFTER_GEN_APPLY;
    if (real.first_attempt_ns>=g->apply_ns)
        return FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED;
    return FGS_REAL_NOT_LATE;
}
/* IDs are copied from the ACTUAL live retirement queue at wait entry, not
 * inferred from target overlap. A previously retired GEN cannot taint an
 * all-REAL capacity wait merely because it remains in the history ring. */
static inline void fgs_publication_capacity_begin(fgs_capacity_observation *out,
        const fgs_publication_history *history, const fgs_transaction_id *live,
        unsigned count, uint64_t start_ns) {
    *out=(fgs_capacity_observation){.start_ns=start_ns};
    if (count>FGS_PUBLICATION_CONTEXTS) return;
    for (unsigned i=0;i<count;i++) {
        const fgs_applied_generation *g=fgs_publication_find(history,live[i]);
        if (!g) continue;
        out->generated[out->count++]=*g;
    }
}
static inline int fgs_publication_capacity_spans_target(
        const fgs_capacity_observation *wait, unsigned index, uint64_t end_ns) {
    return wait && index<wait->count && wait->start_ns && end_ns>=wait->start_ns &&
        wait->start_ns<=wait->generated[index].slot.target_ns &&
        wait->generated[index].slot.target_ns<=end_ns;
}
