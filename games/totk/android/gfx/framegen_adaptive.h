/* MIT. Simulation-time adaptive prediction and fixed-refresh slot decisions.
 * Render fences establish readiness, never the phase of the simulated image. */
#pragma once
#include <stdint.h>
#include <math.h>

enum { FG_ADAPTIVE_WARMUP, FG_ADAPTIVE_ACTIVE, FG_ADAPTIVE_PASSTHROUGH,
    FG_ADAPTIVE_SCENE, FG_ADAPTIVE_FLOOR, FG_ADAPTIVE_STALE,
    FG_ADAPTIVE_CLOCK, FG_ADAPTIVE_HORIZON, FG_ADAPTIVE_REASONS };
typedef struct {
    uint64_t sim_ns, anchor_ns, epoch;
    int eligible;
} fg_adaptive_stamp;
typedef struct {
    fg_adaptive_stamp previous, current;
    uint64_t refresh_ns, display_anchor_ns;
    unsigned state;
} fg_adaptive_policy;
/* At most one real interval ahead; intervals below25fps are never predicted. */
#define FG_ADAPTIVE_MAX_INTERVAL_NS UINT64_C(40000000)
static inline unsigned fg_adaptive_observe(fg_adaptive_policy *p,
        fg_adaptive_stamp stamp, uint64_t refresh) {
    fg_adaptive_stamp old=p->current;
    p->refresh_ns=refresh;
    p->current=stamp;
    p->display_anchor_ns=stamp.anchor_ns;
    p->previous=(fg_adaptive_stamp){0};
    if (!((refresh>=UINT64_C(8000000) && refresh<=UINT64_C(8650000)) ||
          (refresh>=UINT64_C(16000000) && refresh<=UINT64_C(17300000)))) {
        p->current.eligible=0;
        return p->state=FG_ADAPTIVE_CLOCK;
    }
    if (!stamp.eligible || !stamp.sim_ns || !stamp.anchor_ns) {
        p->current.eligible=0;
        return p->state=FG_ADAPTIVE_SCENE;
    }
    if (!old.eligible || old.epoch!=stamp.epoch || !old.sim_ns)
        return p->state=FG_ADAPTIVE_WARMUP;
    if (stamp.sim_ns<=old.sim_ns || stamp.anchor_ns<=old.anchor_ns) {
        p->current.eligible=0;
        return p->state=FG_ADAPTIVE_STALE;
    }
    uint64_t dt=stamp.sim_ns-old.sim_ns;
    if (dt>FG_ADAPTIVE_MAX_INTERVAL_NS || stamp.anchor_ns-old.anchor_ns>FG_ADAPTIVE_MAX_INTERVAL_NS) {
        p->current.eligible=0;
        return p->state=FG_ADAPTIVE_FLOOR;
    }
    p->previous=old;
    if (stamp.anchor_ns-old.anchor_ns<=refresh) return p->state=FG_ADAPTIVE_PASSTHROUGH;
    return p->state=FG_ADAPTIVE_ACTIVE;
}
/* QCOM has no interpolation delay. An out-of-horizon target is refused, not
 * silently assigned the clamped image's wrong time. Clamp absorbs float error. */
static inline int fg_adaptive_phase(const fg_adaptive_policy *p,
        uint64_t target, int interpolation, float *phase) {
    if (!phase || p->state!=FG_ADAPTIVE_ACTIVE || target<p->display_anchor_ns)
        return 0;
    uint64_t sim_dt=p->current.sim_ns-p->previous.sim_ns;
    uint64_t host_dt=p->current.anchor_ns-p->previous.anchor_ns;
    uint64_t distance=target-p->display_anchor_ns;
    if (!sim_dt || !host_dt || distance>host_dt) return 0;
    /* Local game-clock secant: target_sim=last_sim+distance*sim_dt/host_dt.
     * Dividing its simulated advance by sim_dt cancels algebraically. This
     * preserves the actual half-step selector/time scale, not a unit-rate
     * assumption and never a render-completion interval. */
    double t=(double)distance/(double)host_dt;
    /* One-real-interval delayed LSFG maps the same display distance into the
     * available previous/current bracket. It must never extrapolate. */
    if (interpolation && (t<0 || t>1)) return 0;
    if (!isfinite(t)) return 0;
    *phase=(float)fmax(0.0,fmin(1.0,t));
    return 1;
}
/* First future fixed slot on this exact real's lattice, strictly after its real
 * publication. Queued generations never advance the real reservation lattice. */
static inline uint64_t fg_adaptive_first_slot(uint64_t real_slot,
        uint64_t refresh, uint64_t now) {
    if (!real_slot || !refresh || real_slot>UINT64_MAX-refresh) return 0;
    uint64_t next=real_slot+refresh;
    if (next<=now) {
        uint64_t n=(now-next)/refresh+1;
        if (n>(UINT64_MAX-next)/refresh) return 0;
        next+=n*refresh;
    }
    return next;
}
/* Slot arbitration is independent of GPU retirement: suppression only cancels
 * publication. The caller must still wait/release every covering fence. */
static inline int fg_adaptive_real_wins(uint64_t candidate_slot,
        uint64_t candidate_epoch, uint64_t real_slot, uint64_t real_epoch,
        uint64_t ready_ns) {
    return candidate_epoch==real_epoch && candidate_slot==real_slot &&
        ready_ns && ready_ns<=candidate_slot;
}
/* A newer ready real scheduled no later than this target invalidates an older
 * pair's prediction too. This also preempts a dequeued future child when the
 * real arrives behind it; queued target order must not become display order. */
static inline int fg_adaptive_supersedes(uint64_t candidate_slot,
        uint64_t candidate_frame,uint64_t candidate_epoch,uint64_t real_slot,
        uint64_t real_frame,uint64_t real_epoch,uint64_t ready_ns) {
    return candidate_epoch==real_epoch && real_frame>candidate_frame &&
        real_slot<=candidate_slot && ready_ns && ready_ns<=candidate_slot;
}
static inline int fg_adaptive_slot_publish(uint64_t slot, uint64_t last,
        uint64_t now, int generated) {
    return slot && slot>last && (!generated || now<slot);
}
