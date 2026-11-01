/* MIT. Causal adaptive slot policy; caller serializes every operation.
 * Only scalar reservations live here. Cancelling/forgetting a reservation NEVER
 * completes GPU work, releases a source/output, or retracts a platform apply. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define FGS_WARMUP_INTERVALS 12u
#define FGS_MAX_INTERVAL_NS UINT64_C(40000000)
#define FGS_RESET_GAP_NS UINT64_C(250000000)
#define FGS_MAX_REQUESTS 8u
#define FGS_PENDING_CAP 16u
#define FGS_HISTORY_CAP 64u

typedef enum {
    FGS_OK, FGS_INVALID, FGS_CLOCK, FGS_OVERFLOW, FGS_CAPACITY, FGS_STALE,
    FGS_DUPLICATE, FGS_NOT_FOUND, FGS_NOT_READY, FGS_WAIT, FGS_LATE,
    FGS_REAL_COLLISION
} fgs_status;
typedef enum {
    FGS_MODE_WARMUP, FGS_MODE_ACTIVE, FGS_MODE_PASSTHROUGH, FGS_MODE_FLOOR,
    FGS_MODE_SCENE, FGS_MODE_GAP, FGS_MODE_CLOCK
} fgs_mode;
typedef enum {
    FGS_CANCEL_REAL, FGS_CANCEL_SCENE, FGS_CANCEL_GAP, FGS_CANCEL_CLOCK,
    FGS_CANCEL_RESET, FGS_CANCEL_END, FGS_CANCEL_LATE, FGS_CANCEL_OWNER
} fgs_cancel_reason;

typedef struct {
    uint64_t window_epoch, generation, ordinal, target_ns;
} fgs_slot;
typedef struct {
    fgs_slot slot;
    unsigned generated_conflicts;
} fgs_real_plan;
typedef struct {
    uint64_t token, frame, source_epoch;
    fgs_slot real_slot, slot;
    uint64_t publication_ns, ewma_ns, predicted_next_ordinal, deadline_ns;
    uint64_t ready_ns;
    unsigned ready;
} fgs_request;
typedef struct {
    fgs_request request;
    fgs_cancel_reason reason;
} fgs_cancel;
typedef struct {
    fgs_request requests[FGS_MAX_REQUESTS];
    fgs_cancel cancelled[FGS_PENDING_CAP];
    unsigned request_count, cancel_count;
    fgs_mode mode;
    fgs_status request_status;
    unsigned deadline_missed, target_missed;
} fgs_batch;
typedef struct {
    fgs_request request;
    unsigned deadline_missed, target_missed, not_ready;
} fgs_gen_commit;

enum { FGS_SLOT_EMPTY, FGS_SLOT_REAL, FGS_SLOT_GENERATED };
typedef struct {
    uint64_t ordinal;
    unsigned kind;
} fgs_occupied;
typedef struct {
    uint64_t window_epoch, generation, base_ns, period_ns, margin_ns;
    uint64_t next_token, last_publication_ns, last_source_epoch, ewma_ns;
    uint64_t high_ordinal, sealed_before;
    fgs_slot last_real;
    fgs_request pending[FGS_PENDING_CAP];
    fgs_occupied occupied[FGS_HISTORY_CAP];
    unsigned configured, have_publication, have_history, have_real;
    unsigned samples, pending_count;
    fgs_mode mode;
} fgs_state;

static inline int fgs_add_ns(uint64_t a, uint64_t b, uint64_t *out) {
    if (a>UINT64_MAX-b) return 0;
    *out=a+b;
    return 1;
}
static inline int fgs_slot_target(const fgs_state *s, uint64_t ordinal,
        uint64_t *target) {
    if (!s->configured || !s->period_ns ||
            ordinal>(UINT64_MAX-s->base_ns)/s->period_ns) return 0;
    *target=s->base_ns+ordinal*s->period_ns;
    return 1;
}
static inline int fgs_first_ordinal(const fgs_state *s, uint64_t time_ns,
        uint64_t *ordinal) {
    if (!s->configured || !s->period_ns) return 0;
    if (time_ns<=s->base_ns) { *ordinal=0; return 1; }
    uint64_t distance=time_ns-s->base_ns;
    uint64_t n=distance/s->period_ns;
    if (distance%s->period_ns) {
        if (n==UINT64_MAX) return 0;
        n++;
    }
    uint64_t target;
    if (!fgs_slot_target(s,n,&target)) return 0;
    *ordinal=n;
    return 1;
}
static inline fgs_slot fgs_make_slot(const fgs_state *s, uint64_t ordinal,
        uint64_t target) {
    return (fgs_slot){s->window_epoch,s->generation,ordinal,target};
}
static inline fgs_status fgs_validate_slot(const fgs_state *s, fgs_slot slot) {
    if (!s->configured) return FGS_CLOCK;
    if (slot.window_epoch!=s->window_epoch || slot.generation!=s->generation)
        return FGS_STALE;
    uint64_t target;
    if (!fgs_slot_target(s,slot.ordinal,&target)) return FGS_OVERFLOW;
    if (target!=slot.target_ns) return FGS_INVALID;
    if (slot.ordinal<s->sealed_before) return FGS_STALE;
    return FGS_OK;
}
static inline unsigned fgs_slot_kind(const fgs_state *s, uint64_t ordinal) {
    if (ordinal<s->sealed_before) return FGS_SLOT_EMPTY;
    const fgs_occupied *p=&s->occupied[ordinal%FGS_HISTORY_CAP];
    return p->kind && p->ordinal==ordinal ? p->kind : FGS_SLOT_EMPTY;
}
static inline int fgs_pending_index(const fgs_state *s, uint64_t token) {
    if (!token) return -1;
    for (unsigned i=0;i<FGS_PENDING_CAP;i++)
        if (s->pending[i].token==token) return (int)i;
    return -1;
}
static inline void fgs_remove_pending(fgs_state *s, unsigned index) {
    s->pending[index]=(fgs_request){0};
    s->pending_count--;
}
static inline void fgs_batch_cancel(fgs_state *s, unsigned index,
        fgs_cancel_reason reason, fgs_batch *out) {
    /* A batch can enumerate the entire fixed pending array, without truncation. */
    out->cancelled[out->cancel_count++]=(fgs_cancel){s->pending[index],reason};
    fgs_remove_pending(s,index);
}
/* Age metadata only once actual time makes earlier slots ineligible. A burst
 * of future reservations must hit capacity, never silently seal FREE holes.
 * The actually applied slot is retained even after a post-apply timing miss. */
static inline uint64_t fgs_commit_floor(const fgs_state *s, fgs_slot slot,
        uint64_t publication_ns) {
    uint64_t time, floor;
    if (!fgs_add_ns(publication_ns,s->margin_ns,&time) ||
            !fgs_first_ordinal(s,time,&floor)) floor=slot.ordinal;
    if (floor>slot.ordinal) floor=slot.ordinal;
    return floor>s->sealed_before ? floor : s->sealed_before;
}
static inline int fgs_storage_available(const fgs_state *s, uint64_t ordinal,
        uint64_t floor) {
    const fgs_occupied *p=&s->occupied[ordinal%FGS_HISTORY_CAP];
    return !p->kind || p->ordinal==ordinal || p->ordinal<floor;
}
static inline void fgs_record_slot(fgs_state *s, fgs_slot slot, unsigned kind,
        uint64_t floor) {
    s->sealed_before=floor;
    if (!s->have_history || slot.ordinal>s->high_ordinal) {
        s->high_ordinal=slot.ordinal;
        s->have_history=1;
    }
    /* This finite metadata is wholly unrelated to external resource ownership. */
    s->occupied[slot.ordinal%FGS_HISTORY_CAP]=(fgs_occupied){slot.ordinal,kind};
}
static inline void fgs_clear_predictor(fgs_state *s) {
    s->have_publication=0;
    s->samples=0;
    s->ewma_ns=0;
}

/* Fresh storage only. For a live state use reset/end and consume cancellations.
 * The lattice is immutable until reset, except the opt-in fgs_reanchor phase
 * correction below, which also bumps generation: generation+ordinal, never a
 * rounded nanosecond equality, is identity. */
static inline fgs_status fgs_init(fgs_state *s, uint64_t window_epoch,
        uint64_t base_ns, uint64_t period_ns, uint64_t margin_ns) {
    if (!s) return FGS_INVALID;
    *s=(fgs_state){.window_epoch=window_epoch,.generation=1,.base_ns=base_ns,
        .period_ns=period_ns,.margin_ns=margin_ns,.mode=FGS_MODE_WARMUP};
    if (!period_ns || margin_ns>=period_ns) {
        s->mode=FGS_MODE_CLOCK;
        return FGS_CLOCK;
    }
    s->configured=1;
    return FGS_OK;
}
static inline fgs_status fgs_reset(fgs_state *s, uint64_t window_epoch,
        uint64_t base_ns, uint64_t period_ns, uint64_t margin_ns, fgs_batch *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_batch){0};
    for (unsigned i=0;i<FGS_PENDING_CAP;i++)
        if (s->pending[i].token) fgs_batch_cancel(s,i,FGS_CANCEL_RESET,out);
    uint64_t generation=s->generation, token=s->next_token;
    *s=(fgs_state){.window_epoch=window_epoch,.generation=generation,.base_ns=base_ns,
        .period_ns=period_ns,.margin_ns=margin_ns,.next_token=token,
        .mode=FGS_MODE_CLOCK};
    out->mode=FGS_MODE_CLOCK;
    if (generation==UINT64_MAX) return FGS_OVERFLOW;
    s->generation=generation+1;
    if (!period_ns || margin_ns>=period_ns) return FGS_CLOCK;
    s->configured=1;
    s->mode=out->mode=FGS_MODE_WARMUP;
    return FGS_OK;
}
static inline fgs_status fgs_end(fgs_state *s, fgs_batch *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_batch){.mode=FGS_MODE_CLOCK};
    for (unsigned i=0;i<FGS_PENDING_CAP;i++)
        if (s->pending[i].token) fgs_batch_cancel(s,i,FGS_CANCEL_END,out);
    s->configured=0;
    fgs_clear_predictor(s);
    s->mode=FGS_MODE_CLOCK;
    /* Keep generation at exhaustion rather than wrapping into an old identity. */
    if (s->generation==UINT64_MAX) return FGS_OVERFLOW;
    s->generation++;
    return FGS_OK;
}
/* Signed phase of one API vsync timestamp against its NEAREST lattice ordinal:
 * error=vsync-target. An exact half-period tie selects the earlier ordinal. */
static inline int fgs_phase_error(const fgs_state *s, uint64_t vsync_ns,
        uint64_t *ordinal, int64_t *error_ns) {
    if (!s || !ordinal || !error_ns || !s->configured || !s->period_ns ||
            s->period_ns>(uint64_t)INT64_MAX) return 0;
    uint64_t n=0, target;
    if (vsync_ns<s->base_ns) {
        if (s->base_ns-vsync_ns>s->period_ns/2) return 0;
    } else {
        uint64_t distance=vsync_ns-s->base_ns;
        n=distance/s->period_ns;
        if (distance%s->period_ns>s->period_ns/2) {
            if (n==UINT64_MAX) return 0;
            n++;
        }
    }
    if (!fgs_slot_target(s,n,&target)) return 0;
    *ordinal=n;
    *error_ns=vsync_ns>=target ? (int64_t)(vsync_ns-target) : -(int64_t)(target-vsync_ns);
    return 1;
}
typedef struct {
    uint64_t old_generation, generation, old_base_ns, base_ns, period_ns;
    uint64_t anchor_ordinal, old_sealed_before, sealed_before;
    int64_t old_lag_ns; /* T(anchor ordinal)-vsync before the re-anchor */
    unsigned live, moved;
} fgs_reanchor_result;
/* Display-vsync-preserving translation. The platform test is desired<expected
 * with desired=target-1, so a committed old_target is displayed on the first
 * physical vsync >= old_target. The new lattice estimates vsync k as
 * V'(k)=T'(k)-lag_ns; display(n) is V'(n+1) for lag>0 (T just after its vsync)
 * or V'(n) for lag<=0. Pick the new ordinal with the SAME display vsync; refuse
 * when that is not representable or not within one ordinal of old_ordinal. */
static inline int fgs_reanchor_translate(const fgs_state *next, int64_t lag_ns,
        uint64_t old_ordinal, uint64_t old_target, uint64_t *ordinal) {
    uint64_t time, display, n;
    if (lag_ns>=0) {
        if (!fgs_add_ns(old_target,(uint64_t)lag_ns,&time)) return 0;
    } else time=old_target>(uint64_t)-lag_ns ? old_target-(uint64_t)-lag_ns : 0;
    /* first k with T'(k)>=old_target+lag, i.e. V'(k)>=old_target */
    if (!fgs_first_ordinal(next,time,&display)) return 0;
    if (lag_ns>0) {
        if (!display) return 0;
        n=display-1;
    } else n=display;
    if (n+1<old_ordinal || n>old_ordinal+1) return 0;
    *ordinal=n;
    return 1;
}
/* Opt-in same-window/same-clock PHASE correction (TOTK_FG_ADAPTIVE_LATTICE=1).
 * Unlike reset it keeps the cadence predictor (EWMA, samples, last actual
 * publication/source epoch, mode): those are CLOCK_MONOTONIC publication
 * facts, independent of lattice phase; predicted ordinals are recomputed from
 * the lattice at each REAL commit. Window, clock generation, period and scene
 * changes still use reset, so warmup still guards every regime change.
 *
 * Refused with no state change while ANY GEN request is pending (FGS_WAIT),
 * or on invalid/overflowing arithmetic. Identity: generation is bumped, so
 * every earlier slot value becomes STALE; no live identity's target changes.
 * The ordinal nearest vsync_ns keeps its number and is re-targeted to
 * vsync_ns+lag_ns (signed, |lag|<period/2). Every still-live committed
 * REAL/GEN is translated to the new ordinal that displays on the SAME physical
 * vsync (see fgs_reanchor_translate), and sealed_before moves past the latest
 * one, so a later REAL can neither land on an irrevocably applied GEN's
 * display vsync nor fill a hole before it. The live old-numbering occupancy
 * entries are cleared: every translated slot is below the new sealed floor. */
static inline fgs_status fgs_reanchor(fgs_state *s, uint64_t vsync_ns,
        int64_t lag_ns, uint64_t period_ns, fgs_reanchor_result *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_reanchor_result){0};
    if (!s->configured) return FGS_CLOCK;
    if (s->pending_count) return FGS_WAIT;
    if (!period_ns || s->margin_ns>=period_ns || period_ns>(uint64_t)INT64_MAX) return FGS_CLOCK;
    if (lag_ns>=(int64_t)(period_ns/2) || lag_ns<=-(int64_t)(period_ns/2)) return FGS_INVALID;
    if (s->generation==UINT64_MAX) return FGS_OVERFLOW;
    uint64_t anchor_ordinal, anchor;
    int64_t phase;
    if (!fgs_phase_error(s,vsync_ns,&anchor_ordinal,&phase)) return FGS_OVERFLOW;
    if (lag_ns>=0) {
        if (!fgs_add_ns(vsync_ns,(uint64_t)lag_ns,&anchor)) return FGS_OVERFLOW;
    } else {
        if (vsync_ns<(uint64_t)-lag_ns) return FGS_OVERFLOW;
        anchor=vsync_ns-(uint64_t)-lag_ns;
    }
    if (anchor_ordinal>anchor/period_ns) return FGS_OVERFLOW;
    fgs_state next=*s;
    next.base_ns=anchor-anchor_ordinal*period_ns;
    next.period_ns=period_ns;
    next.generation=s->generation+1;
    uint64_t target, high=0;
    unsigned live=0, moved=0;
    for (unsigned i=0;i<FGS_HISTORY_CAP;i++) {
        const fgs_occupied *p=&s->occupied[i];
        if (!p->kind || p->ordinal<s->sealed_before) continue;
        uint64_t translated;
        if (!fgs_slot_target(s,p->ordinal,&target) ||
                !fgs_reanchor_translate(&next,lag_ns,p->ordinal,target,&translated) ||
                !fgs_slot_target(&next,translated,&target)) return FGS_OVERFLOW;
        if (!live || translated>high) high=translated;
        /* Old-numbering metadata; the translated slot is below the new floor. */
        next.occupied[i]=(fgs_occupied){0};
        live++;
        moved+=translated!=p->ordinal;
    }
    if (s->have_real) {
        uint64_t translated;
        if (!fgs_reanchor_translate(&next,lag_ns,s->last_real.ordinal,
                s->last_real.target_ns,&translated) ||
                !fgs_slot_target(&next,translated,&target)) return FGS_OVERFLOW;
        next.last_real=fgs_make_slot(&next,translated,target);
    }
    if (live) {
        if (high==UINT64_MAX || !fgs_slot_target(&next,high+1,&target)) return FGS_OVERFLOW;
        if (high+1>next.sealed_before) next.sealed_before=high+1;
    }
    *out=(fgs_reanchor_result){.old_generation=s->generation,.generation=next.generation,
        .old_base_ns=s->base_ns,.base_ns=next.base_ns,.period_ns=period_ns,
        .anchor_ordinal=anchor_ordinal,.old_sealed_before=s->sealed_before,
        .sealed_before=next.sealed_before,.old_lag_ns=-phase,.live=live,.moved=moved};
    *s=next;
    return FGS_OK;
}

/* Read-only: staging then abandoning a transaction cannot train cadence or
 * reserve a slot. Provisional GEN does not block REAL. Irrevocably committed
 * GEN does; generated_conflicts reports that residual, not a fake cancellation. */
static inline fgs_status fgs_peek_real(const fgs_state *s,
        uint64_t publication_ns, fgs_real_plan *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_real_plan){0};
    if (!s->configured) return FGS_CLOCK;
    uint64_t time, ordinal;
    if (!fgs_add_ns(publication_ns,s->margin_ns,&time) ||
            !fgs_first_ordinal(s,time,&ordinal)) return FGS_OVERFLOW;
    if (ordinal<s->sealed_before) ordinal=s->sealed_before;
    uint64_t floor=ordinal;
    for (unsigned i=0;i<=FGS_HISTORY_CAP;i++) {
        unsigned kind=fgs_slot_kind(s,ordinal);
        if (!kind) {
            uint64_t target;
            if (!fgs_slot_target(s,ordinal,&target)) return FGS_OVERFLOW;
            if (!fgs_storage_available(s,ordinal,floor)) return FGS_CAPACITY;
            out->slot=fgs_make_slot(s,ordinal,target);
            return FGS_OK;
        }
        if (kind==FGS_SLOT_GENERATED) out->generated_conflicts++;
        if (ordinal==UINT64_MAX) return FGS_OVERFLOW;
        ordinal++;
    }
    return FGS_CAPACITY;
}

/* AFTER actual REAL apply only, using the exact assigned target even if apply
 * overran its cutoff. Timing misses are diagnostics and never undo publication.
 * Prediction uses only actual past publications. It does NOT supply game phase. */
static inline fgs_status fgs_commit_real(fgs_state *s, fgs_slot assigned,
        uint64_t actual_publication_ns, uint64_t frame, uint64_t source_epoch,
        int eligible, fgs_batch *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_batch){0};
    fgs_status valid=fgs_validate_slot(s,assigned);
    if (valid!=FGS_OK) return valid;
    if (fgs_slot_kind(s,assigned.ordinal)) return FGS_DUPLICATE;
    uint64_t floor=fgs_commit_floor(s,assigned,actual_publication_ns);
    if (!fgs_storage_available(s,assigned.ordinal,floor)) return FGS_CAPACITY;
    uint64_t cutoff=assigned.target_ns>=s->margin_ns ?
        assigned.target_ns-s->margin_ns : 0;
    out->deadline_missed=actual_publication_ns>cutoff;
    out->target_missed=actual_publication_ns>=assigned.target_ns;
    int scene=!eligible || (s->have_publication && source_epoch!=s->last_source_epoch);
    int clock=s->have_publication && actual_publication_ns<=s->last_publication_ns;
    int gap=s->have_publication && actual_publication_ns>s->last_publication_ns &&
        actual_publication_ns-s->last_publication_ns>FGS_RESET_GAP_NS;
    fgs_record_slot(s,assigned,FGS_SLOT_REAL,floor);
    for (unsigned i=0;i<FGS_PENDING_CAP;i++) {
        if (!s->pending[i].token) continue;
        if (scene || clock || gap || s->pending[i].slot.ordinal<s->sealed_before ||
                s->pending[i].slot.ordinal>=assigned.ordinal) {
            fgs_cancel_reason reason=scene ? FGS_CANCEL_SCENE :
                clock ? FGS_CANCEL_CLOCK : gap ? FGS_CANCEL_GAP :
                s->pending[i].slot.ordinal<s->sealed_before ? FGS_CANCEL_CLOCK : FGS_CANCEL_REAL;
            fgs_batch_cancel(s,i,reason,out);
        }
    }
    if (scene || clock || gap) fgs_clear_predictor(s);
    if (!scene && !clock && !gap && s->have_publication) {
        uint64_t interval=actual_publication_ns-s->last_publication_ns;
        /* Both EMA and interval are bounded by RESET_GAP_NS; still check the
         * arithmetic explicitly, including token/ordinal/time paths below. */
        if (!s->samples) s->ewma_ns=interval;
        else if (s->ewma_ns>(UINT64_MAX-interval)/7) {
            fgs_clear_predictor(s);
            clock=1;
        } else s->ewma_ns=(7*s->ewma_ns+interval)/8;
        if (!clock && s->samples<FGS_WARMUP_INTERVALS) s->samples++;
    }
    s->last_publication_ns=actual_publication_ns;
    s->last_source_epoch=source_epoch;
    s->have_publication=1;
    s->last_real=assigned;
    s->have_real=1;
    s->mode=scene ? FGS_MODE_SCENE : clock ? FGS_MODE_CLOCK : gap ? FGS_MODE_GAP :
        s->samples<FGS_WARMUP_INTERVALS ? FGS_MODE_WARMUP :
        s->ewma_ns<=s->period_ns ? FGS_MODE_PASSTHROUGH :
        s->ewma_ns>FGS_MAX_INTERVAL_NS ? FGS_MODE_FLOOR : FGS_MODE_ACTIVE;
    out->mode=s->mode;
    if (s->mode!=FGS_MODE_ACTIVE) return FGS_OK;
    uint64_t predicted, time;
    if (!fgs_add_ns(actual_publication_ns,s->ewma_ns,&time) ||
            !fgs_add_ns(time,s->margin_ns,&time) ||
            !fgs_first_ordinal(s,time,&predicted)) {
        out->request_status=FGS_OVERFLOW;
        return FGS_OK;
    }
    if (predicted<=assigned.ordinal || predicted-assigned.ordinal<=1) return FGS_OK;
    if (predicted-assigned.ordinal-1>FGS_MAX_REQUESTS) {
        out->request_status=FGS_CAPACITY;
        return FGS_OK;
    }
    for (uint64_t ordinal=assigned.ordinal+1;ordinal<predicted;ordinal++) {
        if (fgs_slot_kind(s,ordinal)) continue;
        int owned=0, free_index=-1;
        for (unsigned i=0;i<FGS_PENDING_CAP;i++) {
            if (s->pending[i].token && s->pending[i].slot.ordinal==ordinal) owned=1;
            if (!s->pending[i].token && free_index<0) free_index=(int)i;
        }
        if (owned) continue;
        if (free_index<0) { out->request_status=FGS_CAPACITY; break; }
        uint64_t target;
        if (!fgs_slot_target(s,ordinal,&target) || s->next_token==UINT64_MAX) {
            out->request_status=FGS_OVERFLOW;
            break;
        }
        fgs_request request={.token=++s->next_token,.frame=frame,
            .source_epoch=source_epoch,.real_slot=assigned,
            .slot=fgs_make_slot(s,ordinal,target),.publication_ns=actual_publication_ns,
            .ewma_ns=s->ewma_ns,.predicted_next_ordinal=predicted,
            .deadline_ns=target-s->margin_ns};
        s->pending[free_index]=request;
        s->pending_count++;
        out->requests[out->request_count++]=request;
    }
    return FGS_OK;
}

static inline fgs_status fgs_cancel_gen(fgs_state *s, uint64_t token,
        fgs_cancel_reason reason, fgs_cancel *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_cancel){0};
    int index=fgs_pending_index(s,token);
    if (index<0) return FGS_NOT_FOUND;
    *out=(fgs_cancel){s->pending[index],reason};
    fgs_remove_pending(s,(unsigned)index);
    return FGS_OK;
}
/* Readiness is an actual completion timestamp (or the replay's declared cost),
 * not ownership retirement. Late completion cancels only this publication. */
static inline fgs_status fgs_mark_ready(fgs_state *s, uint64_t token,
        uint64_t ready_ns, fgs_cancel *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_cancel){0};
    int index=fgs_pending_index(s,token);
    if (index<0) return FGS_NOT_FOUND;
    fgs_request *request=&s->pending[index];
    if (ready_ns<request->publication_ns) return FGS_INVALID;
    if (request->ready) return ready_ns==request->ready_ns ? FGS_OK : FGS_INVALID;
    if (ready_ns>request->deadline_ns) {
        fgs_cancel_gen(s,token,FGS_CANCEL_LATE,out);
        return FGS_LATE;
    }
    request->ready=1;
    request->ready_ns=ready_ns;
    return FGS_OK;
}
/* Pure pre-apply admission. Check immediately before apply under the same lock.
 * A known newer ready REAL takes this and every later provisional GEN slot.
 * Model replay calls at deadline exactly, processing REAL first on ties. Real
 * devices may apply between cutoff and target; commit reports that residual. */
static inline fgs_status fgs_admit_gen(const fgs_state *s, uint64_t token,
        uint64_t now_ns, const fgs_slot *known_real, fgs_request *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_request){0};
    if (!s->configured) return FGS_CLOCK;
    int index=fgs_pending_index(s,token);
    if (index<0) return FGS_NOT_FOUND;
    const fgs_request *request=&s->pending[index];
    fgs_status valid=fgs_validate_slot(s,request->slot);
    if (valid!=FGS_OK) return valid;
    if (known_real) {
        valid=fgs_validate_slot(s,*known_real);
        if (valid!=FGS_OK) return valid;
        if (known_real->ordinal<=request->slot.ordinal) return FGS_REAL_COLLISION;
    }
    if (s->have_real && s->last_real.ordinal>request->real_slot.ordinal &&
            s->last_real.ordinal<=request->slot.ordinal) return FGS_REAL_COLLISION;
    if (fgs_slot_kind(s,request->slot.ordinal)) return FGS_DUPLICATE;
    if (!request->ready) return FGS_NOT_READY;
    if (now_ns<request->deadline_ns) return FGS_WAIT;
    if (now_ns>request->slot.target_ns ||
            (now_ns==request->slot.target_ns && s->margin_ns)) return FGS_LATE;
    if (!fgs_storage_available(s,request->slot.ordinal,
            fgs_commit_floor(s,request->slot,now_ns))) return FGS_CAPACITY;
    *out=*request;
    return FGS_OK;
}
/* Opt-in early release (TOTK_FG_ADAPTIVE_EARLY_APPLY=1 only). Exactly fgs_admit_gen
 * except that a READY request may pass its WAIT once now>=release_ns, where
 * release_ns is strictly before the request deadline. Every validity, ready,
 * real-collision, duplicate and storage check is still evaluated now; the
 * readiness cutoff (fgs_mark_ready vs deadline_ns) is unchanged. */
static inline fgs_status fgs_admit_gen_release(const fgs_state *s, uint64_t token,
        uint64_t now_ns, const fgs_slot *known_real, uint64_t release_ns,
        fgs_request *out) {
    fgs_status status=fgs_admit_gen(s,token,now_ns,known_real,out);
    if (status!=FGS_WAIT || !release_ns || now_ns<release_ns) return status;
    const fgs_request *request=&s->pending[fgs_pending_index(s,token)];
    if (release_ns>=request->deadline_ns) return FGS_WAIT;
    if (!fgs_storage_available(s,request->slot.ordinal,
            fgs_commit_floor(s,request->slot,now_ns))) return FGS_CAPACITY;
    *out=*request;
    return FGS_OK;
}
/* AFTER actual GEN apply only. Timing/ready misses cannot retract an apply;
 * record the assigned slot and return diagnostics, never a fake late cancel.
 * The caller must have admitted it and serialized apply+commit with REAL/reset. */
static inline fgs_status fgs_commit_gen(fgs_state *s, uint64_t token,
        uint64_t actual_publication_ns, fgs_gen_commit *out) {
    if (!s || !out) return FGS_INVALID;
    *out=(fgs_gen_commit){0};
    int index=fgs_pending_index(s,token);
    if (index<0) return FGS_NOT_FOUND;
    fgs_request request=s->pending[index];
    fgs_status valid=fgs_validate_slot(s,request.slot);
    if (valid!=FGS_OK) return valid;
    if (fgs_slot_kind(s,request.slot.ordinal)) return FGS_DUPLICATE;
    uint64_t floor=fgs_commit_floor(s,request.slot,actual_publication_ns);
    if (!fgs_storage_available(s,request.slot.ordinal,floor)) return FGS_CAPACITY;
    *out=(fgs_gen_commit){.request=request,
        .deadline_missed=actual_publication_ns>request.deadline_ns,
        .target_missed=actual_publication_ns>=request.slot.target_ns,
        .not_ready=!request.ready};
    fgs_record_slot(s,request.slot,FGS_SLOT_GENERATED,floor);
    fgs_remove_pending(s,(unsigned)index);
    return FGS_OK;
}
