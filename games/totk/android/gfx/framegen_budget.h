#ifndef TOTK_FRAMEGEN_BUDGET_H
#define TOTK_FRAMEGEN_BUDGET_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* The real tick is 33.333 ms. Leave 3.333 ms outside the measured GPU budget.
 * No clock calibration, host completion latency or averaged FPS enters this
 * policy. Each sample is a completed, exact Vulkan submission ticket. */
#define FG_BUDGET_TARGET_MS 30.0
#define FG_BUDGET_TICK_MS (1000.0 / 30.0)

enum { FG_BUDGET_SLOTS = 256, FG_BUDGET_FRAMES = 32,
       FG_BUDGET_WINDOW = 32, FG_BUDGET_MAX_AGE = 4,
       FG_BUDGET_PROBE_INTERVAL = 30, FG_BUDGET_FEEDBACK_MIN = 4,
       FG_BUDGET_FEEDBACK_MAX = 120 };
enum { FG_BUDGET_PENDING, FG_BUDGET_VALID, FG_BUDGET_UNAVAILABLE };
enum { FG_BUDGET_ACCEPT, FG_BUDGET_MISSING, FG_BUDGET_OVERLOAD };
enum { FG_BUDGET_REASON_NONE, FG_BUDGET_REASON_MISSING,
       FG_BUDGET_REASON_ESTIMATE, FG_BUDGET_REASON_REAL,
       FG_BUDGET_REASON_PROBE_RATE, FG_BUDGET_REASON_FEEDBACK,
       FG_BUDGET_REASON_RESERVED, FG_BUDGET_REASON_STALE_ESTIMATE };

typedef struct {
    uint64_t ticket;
    double ms;
    int measured, synthetic;
} fg_budget_slot;
typedef struct {
    uint64_t frame, first, last;
    double real_ms, incremental_ms;
    int status, synthetic;
} fg_budget_frame;
typedef struct {
    fg_budget_slot slots[FG_BUDGET_SLOTS];
    fg_budget_frame frames[FG_BUDGET_FRAMES];
    uint64_t last_sample, last_ticket, last_frame, finalize;
    uint64_t reserved_frames[FG_BUDGET_FRAMES], feedback_frames[FG_BUDGET_FRAMES];
    uint64_t last_reserved, last_probe, reservations, probes;
    uint64_t cooldown_until, feedback_misses;
    unsigned feedback_backoff;
    int boundary, invalid_interval, feedback_gpu_only;
} fg_budget_policy;
typedef struct {
    unsigned status, reason, probe, real_samples, incremental_samples;
    unsigned stale_incremental; /* only fg_budget_admit_stale sets this */
    uint64_t requested_frame, frame, first, last;
    uint64_t incremental_frame, incremental_first, incremental_last;
    uint64_t newest_real_frame, newest_incremental_frame, retry_frame;
    double real_ms, incremental_ms, total_ms, margin_ms;
} fg_budget_decision;

static inline fg_budget_slot *fg_budget_slot_at(fg_budget_policy *p, uint64_t ticket) {
    fg_budget_slot *s=&p->slots[ticket % FG_BUDGET_SLOTS];
    if (s->ticket != ticket) *s=(fg_budget_slot){.ticket=ticket};
    return s;
}

static inline void fg_budget_finalize(fg_budget_policy *p) {
    while (p->finalize && p->finalize <= p->last_frame) {
        fg_budget_frame *f=&p->frames[p->finalize % FG_BUDGET_FRAMES];
        if (f->frame != p->finalize) { p->finalize++; continue; }
        if (f->status == FG_BUDGET_PENDING) {
            if (f->last > p->last_sample) break;
            f->status=FG_BUDGET_VALID;
            for (uint64_t ticket=f->first;; ticket++) {
                const fg_budget_slot *s=&p->slots[ticket % FG_BUDGET_SLOTS];
                if (s->ticket != ticket || s->measured != 1) {
                    f->status=FG_BUDGET_UNAVAILABLE; break;
                }
                if (s->synthetic) { f->incremental_ms+=s->ms; f->synthetic=1; }
                else f->real_ms+=s->ms;
                if (ticket == f->last) break;
            }
            if (!isfinite(f->real_ms) || !isfinite(f->incremental_ms) || f->real_ms <= 0)
                f->status=FG_BUDGET_UNAVAILABLE;
        }
        p->finalize++;
    }
}

/* Completion order is queue order. Duplicated/late callbacks cannot replace a
 * ticket's value. Classification may precede or follow its GPU sample, but must
 * precede seal(), so a frame is never finalized with ambiguous ownership. */
static inline void fg_budget_sample(fg_budget_policy *p, uint64_t ticket, double ms, int valid) {
    if (!ticket || ticket <= p->last_sample) return;
    fg_budget_slot *s=fg_budget_slot_at(p,ticket);
    s->ms=ms;
    s->measured=valid && isfinite(ms) && ms >= 0 ? 1 : -1;
    p->last_sample=ticket;
    fg_budget_finalize(p);
}

/* Only tickets created after the real presenter submission belong here. Their
 * entire slot bracket (generation, post and presentation copy) is incremental.
 * Real brackets retain source copies, replay, authority and pair dilation, so
 * none of those costs is added a second time in adaptive mode. */
static inline void fg_budget_synthetic(fg_budget_policy *p, uint64_t first, uint64_t last) {
    if (!first || first <= p->last_ticket || last < first || last-first >= FG_BUDGET_SLOTS) {
        p->invalid_interval=1; return;
    }
    for (uint64_t ticket=first;; ticket++) {
        fg_budget_slot_at(p,ticket)->synthetic=1;
        if (ticket == last) break;
    }
}

/* The first endpoint establishes a boundary, not a partial "real frame". An
 * empty, backwards, oversized or incomplete interval is explicitly unavailable.
 * Caller frame IDs are consecutive real presents, never synthetic presents. */
static inline void fg_budget_seal(fg_budget_policy *p, uint64_t frame, uint64_t ticket) {
    if (!frame || frame <= p->last_frame) return;
    fg_budget_frame *f=&p->frames[frame % FG_BUDGET_FRAMES];
    *f=(fg_budget_frame){.frame=frame,.first=p->last_ticket+1,.last=ticket};
    if (!p->boundary || frame != p->last_frame+1 || ticket <= p->last_ticket ||
        ticket-p->last_ticket > FG_BUDGET_SLOTS || p->invalid_interval)
        f->status=FG_BUDGET_UNAVAILABLE;
    if (!p->finalize || frame-p->finalize >= FG_BUDGET_FRAMES) p->finalize=frame;
    p->last_frame=frame; p->last_ticket=ticket; p->boundary=ticket != 0;
    p->invalid_interval=0;
    fg_budget_finalize(p);
}

static inline fg_budget_decision fg_budget_decide(const fg_budget_frame *f, double incremental) {
    fg_budget_decision d={.status=FG_BUDGET_MISSING,.reason=FG_BUDGET_REASON_MISSING};
    if (!f || f->status != FG_BUDGET_VALID || !isfinite(incremental) || incremental < 0) return d;
    d.requested_frame=d.frame=d.newest_real_frame=f->frame; d.first=f->first; d.last=f->last;
    d.real_samples=1; d.incremental_samples=f->synthetic ? 1 : 0;
    if (f->synthetic) {
        d.incremental_frame=d.newest_incremental_frame=f->frame;
        d.incremental_first=f->first; d.incremental_last=f->last;
    }
    d.real_ms=f->real_ms; d.incremental_ms=incremental;
    d.total_ms=d.real_ms+d.incremental_ms;
    d.margin_ms=FG_BUDGET_TICK_MS-d.total_ms;
    d.status=isfinite(d.total_ms) && d.total_ms <= FG_BUDGET_TARGET_MS ?
        FG_BUDGET_ACCEPT : FG_BUDGET_OVERLOAD;
    d.reason=d.status == FG_BUDGET_ACCEPT ? FG_BUDGET_REASON_NONE : FG_BUDGET_REASON_ESTIMATE;
    return d;
}

static inline uint64_t fg_budget_after(uint64_t frame, unsigned ticks) {
    return frame > UINT64_MAX-ticks ? UINT64_MAX : frame+ticks;
}

/* Independent nearest-rank p90s over at most 32 completed real intervals. The
 * newest completed real interval must be valid and at most four ticks old;
 * missing data never falls back to an older measured subset. Historical
 * incremental samples inform p90 only while at least one is equally fresh.
 * Without that freshness, one explicitly unestimated probe per 30 real ticks
 * can refresh the estimate. Its eventual exact completion still must fit 30 ms.
 * Queries never consume a probe: reserve() is called only at GPU work start. */
static inline fg_budget_decision fg_budget_admit(const fg_budget_policy *p, uint64_t frame) {
    fg_budget_decision d={.status=FG_BUDGET_MISSING,.reason=FG_BUDGET_REASON_MISSING,
        .requested_frame=frame,.incremental_ms=NAN,.total_ms=NAN,.margin_ms=NAN};
    if (!frame) return d;
    const fg_budget_frame *newest=NULL;
    const fg_budget_frame *real[FG_BUDGET_WINDOW], *incremental[FG_BUDGET_WINDOW];
    for (unsigned age=1; age <= FG_BUDGET_WINDOW && age < frame; age++) {
        uint64_t id=frame-age;
        const fg_budget_frame *f=&p->frames[id % FG_BUDGET_FRAMES];
        if (f->frame != id || f->status == FG_BUDGET_PENDING) continue;
        if (!newest) {
            if (f->status != FG_BUDGET_VALID || age > FG_BUDGET_MAX_AGE) return d;
            newest=f; d.newest_real_frame=id;
        }
        if (f->status != FG_BUDGET_VALID) continue;
        unsigned at=d.real_samples++;
        while (at && real[at-1]->real_ms > f->real_ms) {
            real[at]=real[at-1]; at--;
        }
        real[at]=f;
        if (f->synthetic) {
            if (!d.newest_incremental_frame) d.newest_incremental_frame=id;
            at=d.incremental_samples++;
            while (at && incremental[at-1]->incremental_ms > f->incremental_ms) {
                incremental[at]=incremental[at-1]; at--;
            }
            incremental[at]=f;
        }
    }
    if (!newest) return d;
    const fg_budget_frame *r=real[(9*d.real_samples+9)/10-1];
    d.frame=r->frame; d.first=r->first; d.last=r->last; d.real_ms=r->real_ms;
    if (d.incremental_samples) {
        const fg_budget_frame *i=incremental[(9*d.incremental_samples+9)/10-1];
        d.incremental_frame=i->frame; d.incremental_first=i->first; d.incremental_last=i->last;
        if (frame-d.newest_incremental_frame <= FG_BUDGET_MAX_AGE) {
            d.incremental_ms=i->incremental_ms; d.total_ms=d.real_ms+d.incremental_ms;
            d.margin_ms=FG_BUDGET_TICK_MS-d.total_ms;
        }
    }
    d.probe=!isfinite(d.incremental_ms);
    d.status=FG_BUDGET_OVERLOAD;
    if (frame <= p->last_reserved) { d.reason=FG_BUDGET_REASON_RESERVED; return d; }
    if (p->cooldown_until && (frame < p->cooldown_until || p->cooldown_until == UINT64_MAX)) {
        d.reason=FG_BUDGET_REASON_FEEDBACK; d.retry_frame=p->cooldown_until; return d;
    }
    if (d.probe) {
        /* Do not speculate on a real interval that already spends the entire
         * target, including a newest real outlier not selected by p90. */
        if (d.real_ms >= FG_BUDGET_TARGET_MS || newest->real_ms >= FG_BUDGET_TARGET_MS) {
            d.reason=FG_BUDGET_REASON_REAL; return d;
        }
        if (p->last_probe && frame-p->last_probe < FG_BUDGET_PROBE_INTERVAL) {
            d.reason=FG_BUDGET_REASON_PROBE_RATE;
            d.retry_frame=fg_budget_after(p->last_probe,FG_BUDGET_PROBE_INTERVAL); return d;
        }
    } else if (!isfinite(d.total_ms) || d.total_ms > FG_BUDGET_TARGET_MS) {
        d.reason=FG_BUDGET_REASON_ESTIMATE; return d;
    }
    d.status=FG_BUDGET_ACCEPT; d.reason=FG_BUDGET_REASON_NONE;
    return d;
}

/* TOTK_FG_ADAPTIVE_BUDGET=1, adaptive slot scheduler only. r7 measured the
 * synthetic cost as staleness-invariant (stale-probe children 4.00 ms mean vs
 * fresh 3.98 ms), while the 4-tick freshness rule turned any short child gap
 * into an unestimated probe limited to one per 30 ticks (81% of refusals).
 * Instead of probing, a measured incremental p90 still inside the 32-interval
 * window is added to the same fresh real p90 and held to the SAME 30 ms ceiling.
 * Reservation, feedback, real-alone and missing refusals are unchanged; the
 * original probe path remains only when the window has no incremental sample. */
static inline fg_budget_decision fg_budget_admit_stale(const fg_budget_policy *p, uint64_t frame) {
    fg_budget_decision d=fg_budget_admit(p,frame);
    if (!d.probe || !d.incremental_samples || d.status == FG_BUDGET_MISSING ||
        d.reason == FG_BUDGET_REASON_RESERVED || d.reason == FG_BUDGET_REASON_FEEDBACK ||
        d.reason == FG_BUDGET_REASON_REAL) return d;
    const fg_budget_frame *i=&p->frames[d.incremental_frame % FG_BUDGET_FRAMES];
    if (i->frame != d.incremental_frame || i->status != FG_BUDGET_VALID || !i->synthetic ||
        !isfinite(i->incremental_ms) || i->incremental_ms < 0) return d;
    d.probe=0; d.stale_incremental=1; d.retry_frame=0;
    d.incremental_ms=i->incremental_ms; d.total_ms=d.real_ms+d.incremental_ms;
    d.margin_ms=FG_BUDGET_TICK_MS-d.total_ms;
    if (isfinite(d.total_ms) && d.total_ms <= FG_BUDGET_TARGET_MS) {
        d.status=FG_BUDGET_ACCEPT; d.reason=FG_BUDGET_REASON_NONE;
    } else {
        d.status=FG_BUDGET_OVERLOAD; d.reason=FG_BUDGET_REASON_STALE_ESTIMATE;
    }
    return d;
}

static inline fg_budget_decision fg_budget_reserve_decision(fg_budget_policy *p,
        uint64_t frame, fg_budget_decision d) {
    if (d.status != FG_BUDGET_ACCEPT) return d;
    p->reserved_frames[frame % FG_BUDGET_FRAMES]=frame;
    p->last_reserved=frame; p->reservations++;
    if (d.probe) { p->last_probe=frame; p->probes++; }
    return d;
}

/* Reserve once, immediately before real work starts, not at prediction or
 * capability queries. Rechecking here closes the query/start feedback race;
 * failed starts before this point do not consume scarce bootstrap probes. */
static inline fg_budget_decision fg_budget_reserve(fg_budget_policy *p, uint64_t frame) {
    return fg_budget_reserve_decision(p,frame,fg_budget_admit(p,frame));
}
/* A stale-estimate acceptance is not a probe and never consumes the probe rate. */
static inline fg_budget_decision fg_budget_reserve_stale(fg_budget_policy *p, uint64_t frame) {
    return fg_budget_reserve_decision(p,frame,fg_budget_admit_stale(p,frame));
}

/* Presenter feedback is a measured real-target miss, not missing/teardown
 * timing. A generation reserved in [real_frame-2, real_frame+1] can compete
 * with that real present; no nearby exact reservation means no penalty.
 * Delayed feedback starts its pause at the current real tick. Duplicate
 * callbacks cannot increase backoff. Cooldowns are bounded real ticks, never
 * a host sleep; on-time feedback cannot cancel an active pause. */
static inline int fg_budget_feedback(fg_budget_policy *p, uint64_t real_frame, int late) {
    if (!real_frame || p->feedback_frames[real_frame % FG_BUDGET_FRAMES] == real_frame) return 0;
    p->feedback_frames[real_frame % FG_BUDGET_FRAMES]=real_frame;
    uint64_t now=p->last_frame;
    if (now < p->last_reserved) now=p->last_reserved;
    if (now < real_frame) now=real_frame;
    if (!late) {
        if (p->feedback_backoff && now >= p->cooldown_until && p->cooldown_until != UINT64_MAX)
            p->feedback_backoff/=2;
        return 0;
    }
    /* Locked interpolation must not mistake CPU/pacing lateness for GPU
       contention and starve otherwise under-budget pairs. Only an exact,
       complete measured interval can justify this additional cooldown;
       admission and completed-output checks retain the total GPU ceiling. */
    if (p->feedback_gpu_only) {
        const fg_budget_frame *f=&p->frames[real_frame % FG_BUDGET_FRAMES];
        if (now-real_frame > FG_BUDGET_MAX_AGE ||
            f->frame != real_frame || f->status != FG_BUDGET_VALID ||
            !isfinite(f->real_ms) || !isfinite(f->incremental_ms) ||
            f->real_ms+f->incremental_ms <= FG_BUDGET_TARGET_MS) return 0;
    }
    uint64_t first=real_frame > 2 ? real_frame-2 : 1;
    uint64_t last=real_frame == UINT64_MAX ? UINT64_MAX : real_frame+1;
    int correlated=0;
    for (uint64_t id=first;; id++) {
        if (p->reserved_frames[id % FG_BUDGET_FRAMES] == id) correlated=1;
        if (id == last) break;
    }
    if (!correlated) return 0;
    p->feedback_backoff=!p->feedback_backoff ? FG_BUDGET_FEEDBACK_MIN :
        p->feedback_backoff > FG_BUDGET_FEEDBACK_MAX/2 ? FG_BUDGET_FEEDBACK_MAX : 2*p->feedback_backoff;
    uint64_t until=fg_budget_after(now,p->feedback_backoff+1);
    if (until > p->cooldown_until) p->cooldown_until=until;
    p->feedback_misses++;
    return 1;
}

static inline fg_budget_decision fg_budget_completed(const fg_budget_policy *p, uint64_t frame) {
    const fg_budget_frame *f=&p->frames[frame % FG_BUDGET_FRAMES];
    if (f->frame != frame || !f->synthetic)
        return (fg_budget_decision){.status=FG_BUDGET_MISSING,.reason=FG_BUDGET_REASON_MISSING,
            .requested_frame=frame};
    return fg_budget_decide(f,f->incremental_ms);
}

/* Explicit overrides retain the legacy strict incremental ceiling, independent
 * of adaptive warmup. Callers supply the existing source+generated query sum. */
static inline int fg_budget_fixed(double limit, double incremental) {
    return isfinite(limit) && limit > 0 && isfinite(incremental) &&
        incremental >= 0 && incremental <= limit;
}

#endif
