/* MIT. Two independent opt-in policies for the adaptive slot scheduler
 * (TOTK_FG_ADAPTIVE_SLOTS=1 only): TOTK_FG_ADAPTIVE_LATTICE=1 re-anchors the
 * slot lattice phase (fga_latch_phase_check + fgs_reanchor) and nothing else;
 * TOTK_FG_ADAPTIVE_EARLY_APPLY=1 releases READY GENs at the matched SF
 * deadline (fga_latch_release_for) and nothing else. Either works alone or
 * with the other. Pure: no clocks, Android calls, waits, locks or allocation.
 * Callers pass only API FrameTimeline snapshots that
 * runtime_android_adaptive_slot_timelines already proved fresh and that match
 * the scheduler clock. A disabled selector returns exactly the r7 behaviour.
 *
 * Encoding contract (present_clock.h): a slot target T is submitted as
 * desired=T-1 and SurfaceFlinger's desired<expected test is strict, so the
 * vsync that can first latch it is the earliest API entry with expected>T-1. */
#pragma once
#include <stdint.h>
#include "framegen_adaptive_slots.h"
#include "framegen_adaptive_present.h"
#include "../../runtime/present_timeline.h"

/* Lattice lag: T - E, where E is the API vsync at or before T (the nearest
 * one to T). Measured r7: the GEN is present when T sits just AFTER a vsync E
 * (r7 reset anchors T=E; desired T-1 in (E,E+P] displays at E+P), and real
 * actual-desired falls from ~16.5ms (present) to <~14ms (absent) as T drifts
 * later. The lattice period (r7: 16,660,581ns) is LONGER than the measured SF
 * vsync (~16,655,9xxns), so T drifts ~4.6us/refresh (~0.28ms/s) LATER and the
 * lag grows. LATTICE=1 re-targets lag to +500us whenever it leaves
 * [100us,1500us]: the low edge keeps T clear of E's sign change (where the
 * strict test would flip the display vsync), the high edge keeps the r7 GEN
 * apply (T-2ms..T-1.4ms) >= ~16.5ms before its display vsync E+P (r7's
 * present regime needed >= ~13.7ms). With the measured drift this is one
 * generation bump per ~3.6s of steady play. */
#define FGA_LATCH_LAG_NS INT64_C(500000)
#define FGA_LATCH_LAG_MIN_NS INT64_C(100000)
#define FGA_LATCH_LAG_MAX_NS INT64_C(1500000)
/* Early release precedes the latch entry's SF deadline by this guard. r7's
 * measured release->apply-return median is ~0.6ms (hold T-2ms, apply ~T-1.4ms);
 * the guard leaves ~0.9ms for presenter tail latency and transaction transit. */
#define FGA_LATCH_DEADLINE_GUARD_NS UINT64_C(1500000)

/* Both selectors use TOTK_FG_ADAPTIVE_SLOTS' exact spelling: absent/"0" off,
 * "1" on, anything else invalid (-1), which callers treat as off and log once. */
static inline int fga_latch_request(const char *value) {
    if (!value || (value[0]=='0' && !value[1])) return 0;
    return value[0]=='1' && !value[1] ? 1 : -1;
}

typedef struct {
    runtime_present_timeline_entry entry;
    int64_t offset_ns; /* entry.expected_ns - target_ns */
    unsigned index, found;
} fga_latch_match;

static inline int64_t fga_latch_delta(uint64_t a, uint64_t b) {
    if (a>=b) return a-b>(uint64_t)INT64_MAX ? INT64_MAX : (int64_t)(a-b);
    return b-a>(uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)(b-a);
}
/* Nearest API vsync within half a refresh: the phase diagnostic for T. */
static inline fga_latch_match fga_latch_nearest(const runtime_present_timelines *snapshot,
        uint64_t target_ns) {
    fga_latch_match best={0};
    if (!snapshot || !target_ns || snapshot->count>RUNTIME_PRESENT_TIMELINE_CAPACITY)
        return best;
    uint64_t best_distance=0;
    for (unsigned i=0;i<snapshot->count;i++) {
        const runtime_present_timeline_entry *e=&snapshot->entries[i];
        uint64_t distance=e->expected_ns>=target_ns ? e->expected_ns-target_ns :
            target_ns-e->expected_ns;
        if (distance>snapshot->refresh_ns/2) continue;
        if (!best.found || distance<best_distance) {
            best=(fga_latch_match){*e,fga_latch_delta(e->expected_ns,target_ns),i,1};
            best_distance=distance;
        }
    }
    return best;
}
/* The vsync SurfaceFlinger can first latch desired_ns on: the earliest entry
 * with expected>desired, accepted only when it is T's own or the next refresh
 * (expected-T < refresh). Anything else is "none", never an invented vsync. */
static inline fga_latch_match fga_latch_eligible(const runtime_present_timelines *snapshot,
        uint64_t desired_ns, uint64_t target_ns) {
    fga_latch_match none={0};
    if (!snapshot || !target_ns || snapshot->count>RUNTIME_PRESENT_TIMELINE_CAPACITY)
        return none;
    for (unsigned i=0;i<snapshot->count;i++) {
        const runtime_present_timeline_entry *e=&snapshot->entries[i];
        if (e->expected_ns<=desired_ns) continue;
        if (e->expected_ns<target_ns || e->expected_ns-target_ns>=snapshot->refresh_ns)
            return none;
        return (fga_latch_match){*e,fga_latch_delta(e->expected_ns,target_ns),i,1};
    }
    return none;
}

typedef enum {
    FGA_LATCH_R7_DISABLED, FGA_LATCH_R7_INVALID, FGA_LATCH_R7_NO_MATCH,
    FGA_LATCH_R7_LATE, FGA_LATCH_R7_NOT_EARLIER, FGA_LATCH_EARLY, FGA_LATCH_KINDS
} fga_latch_kind;
static inline const char *fga_latch_kind_name(fga_latch_kind kind) {
    static const char *const names[FGA_LATCH_KINDS]={"r7-disabled","r7-invalid",
        "r7-no-match","r7-late","r7-not-earlier","early"};
    return (unsigned)kind<FGA_LATCH_KINDS ? names[kind] : "unknown";
}
typedef struct {
    uint64_t release_ns; /* 0 = exactly the r7 hold */
    uint64_t r7_hold_ns; /* effective r7 release: max(T-2ms, request deadline) */
    fga_latch_kind kind;
    fga_latch_match latch;
} fga_latch_release;

/* r7's GEN release is gated by both fgs_admit_gen (deadline_ns=T-margin) and
 * fg_adaptive_present_decide (T-2ms); the effective hold is the later one. */
static inline uint64_t fga_latch_r7_hold(const fgs_state *s, uint64_t target_ns) {
    uint64_t present=fg_adaptive_present_deadline(target_ns);
    uint64_t admit=s && target_ns>=s->margin_ns ? target_ns-s->margin_ns : 0;
    return present>admit ? present : admit;
}
/* Early release for one READY child. Only ever EARLIER than r7, never after T:
 * release = max(latch.deadline_ns-GUARD, T-period-margin+1), used only when it
 * is strictly before the r7 hold and the matched SF deadline is still ahead.
 * The floor keeps first_ordinal(now+margin)>=slot.ordinal at apply, so no later
 * REAL can be planned on an earlier ordinal than an irrevocably applied GEN.
 * No/stale/foreign match, a passed deadline or a non-earlier result: r7. */
static inline fga_latch_release fga_latch_release_for(int enabled, const fgs_state *s,
        fgs_slot slot, uint64_t desired_ns, const runtime_present_timelines *snapshot,
        uint64_t now_ns) {
    fga_latch_release r={.r7_hold_ns=fga_latch_r7_hold(s,slot.target_ns),
        .kind=FGA_LATCH_R7_DISABLED};
    if (!enabled) return r;
    r.kind=FGA_LATCH_R7_INVALID;
    if (!s || !snapshot || fgs_validate_slot(s,slot)!=FGS_OK ||
            desired_ns>=slot.target_ns || snapshot->refresh_ns!=s->period_ns) return r;
    r.latch=fga_latch_eligible(snapshot,desired_ns,slot.target_ns);
    r.kind=FGA_LATCH_R7_NO_MATCH;
    if (!r.latch.found) return r;
    r.kind=FGA_LATCH_R7_LATE;
    if (now_ns>=r.latch.entry.deadline_ns) return r;
    uint64_t release=r.latch.entry.deadline_ns>FGA_LATCH_DEADLINE_GUARD_NS ?
        r.latch.entry.deadline_ns-FGA_LATCH_DEADLINE_GUARD_NS : 0;
    uint64_t span=s->period_ns+s->margin_ns;
    uint64_t floor=slot.target_ns>=span ? slot.target_ns-span+1 : 0;
    if (release<floor) release=floor;
    r.kind=FGA_LATCH_R7_NOT_EARLIER;
    if (!release || release>=r.r7_hold_ns) return r;
    r.kind=FGA_LATCH_EARLY;
    r.release_ns=release;
    return r;
}

/* Lattice lag check against the snapshot's preferred vsync (the same API entry
 * fgs_reset anchors on). lag_ns=T(nearest ordinal)-vsync, signed. */
typedef struct {
    uint64_t ordinal;
    int64_t lag_ns;
    unsigned valid, reanchor;
} fga_latch_phase;
static inline fga_latch_phase fga_latch_phase_check(const fgs_state *s, uint64_t vsync_ns) {
    fga_latch_phase p={0};
    int64_t error;
    if (!fgs_phase_error(s,vsync_ns,&p.ordinal,&error)) return p;
    p.valid=1;
    p.lag_ns=-error;
    p.reanchor=p.lag_ns<FGA_LATCH_LAG_MIN_NS || p.lag_ns>FGA_LATCH_LAG_MAX_NS;
    return p;
}
