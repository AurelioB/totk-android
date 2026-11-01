/* MIT. Original, allocation-free adaptive output-count budget.
 * Guest-present intervals determine demand/credit, never generated image phase.
 * Simulation timestamps and fixed-slot phase remain owned by framegen_adaptive.h.
 */
#pragma once
#include "framegen_adaptive.h"

enum {
    FG_ADAPTIVE_PLAN_MIN_SAMPLES=12,
    FG_ADAPTIVE_PLAN_CAPACITY=4,
    FG_ADAPTIVE_PLAN_DOWN_READS=3
};
#define FG_ADAPTIVE_PLAN_UP_NS UINT64_C(500000000)
typedef struct {
    uint64_t last_present_ns,refresh_ns,up_since_ns;
    /* Credit is display time in integer nanoseconds. Keeping only its
     * fractional-refresh remainder prevents missed/warmup work becoming a
     * later burst, and avoids floating accumulation ever exceeding budget. */
    uint64_t credit_ns;
    double interval_ns; /* EMA is demand evidence only, not budget authority. */
    unsigned samples,ceiling,up_floor,down_reads;
} fg_adaptive_plan;

static inline void fg_adaptive_plan_reset(fg_adaptive_plan *p) {
    *p=(fg_adaptive_plan){0};
}
/* Caller reports every real guest present exactly once, using its immutable
 * monotonic guest anchor. ACTIVE is 1. Every other state (including an epoch
 * warmup/reset) discards all count evidence; no old credit crosses epochs.
 * Return value is a maximum number of attempts, not a promise of ready pixels
 * or permission to manufacture a display slot. Unused grants are forfeited. */
static inline unsigned fg_adaptive_plan_outputs(fg_adaptive_plan *p,
        uint64_t guest_present_ns,uint64_t refresh_ns,unsigned state) {
    if (state!=FG_ADAPTIVE_ACTIVE || !guest_present_ns ||
        !((refresh_ns>=UINT64_C(8000000) && refresh_ns<=UINT64_C(8650000)) ||
          (refresh_ns>=UINT64_C(16000000) && refresh_ns<=UINT64_C(17300000)))) {
        fg_adaptive_plan_reset(p);
        return 0;
    }
    if (!p->last_present_ns || p->refresh_ns!=refresh_ns) {
        *p=(fg_adaptive_plan){.last_present_ns=guest_present_ns,.refresh_ns=refresh_ns};
        return 0;
    }
    if (guest_present_ns<=p->last_present_ns) {
        fg_adaptive_plan_reset(p);
        return 0;
    }
    const uint64_t previous=p->last_present_ns;
    const uint64_t interval=guest_present_ns-previous;
    if (interval>FG_ADAPTIVE_MAX_INTERVAL_NS) {
        fg_adaptive_plan_reset(p);
        return 0;
    }
    if (interval<=refresh_ns) {
        /* At/above refresh has no uncovered slot demand. This real may anchor
         * a later fresh interval, but contributes no sample or saved credit. */
        *p=(fg_adaptive_plan){.last_present_ns=guest_present_ns,.refresh_ns=refresh_ns};
        return 0;
    }
    p->last_present_ns=guest_present_ns;
    p->interval_ns=p->samples ? p->interval_ns+0.08*((double)interval-p->interval_ns) :
        (double)interval;
    if (p->samples<FG_ADAPTIVE_PLAN_MIN_SAMPLES) p->samples++;
    const double ratio=p->interval_ns/(double)refresh_ns;
    unsigned demand=ratio>FG_ADAPTIVE_PLAN_CAPACITY ? FG_ADAPTIVE_PLAN_CAPACITY :
        (unsigned)ceil(ratio)-1;
    if (demand>p->ceiling) {
        p->down_reads=0;
        if (!p->up_since_ns) { p->up_since_ns=previous; p->up_floor=demand; }
        else if (demand<p->up_floor) p->up_floor=demand;
        if (p->samples>=FG_ADAPTIVE_PLAN_MIN_SAMPLES &&
            guest_present_ns-p->up_since_ns>=FG_ADAPTIVE_PLAN_UP_NS) {
            p->ceiling=p->up_floor;
            p->up_since_ns=0; p->up_floor=0;
        }
    } else {
        p->up_since_ns=0; p->up_floor=0;
        if (demand<p->ceiling) {
            if (++p->down_reads>=FG_ADAPTIVE_PLAN_DOWN_READS) {
                p->ceiling=demand; p->down_reads=0;
            }
        } else p->down_reads=0;
    }
    /* Exact fractional count budget: credit += interval/refresh - 1 real.
     * Valid interval <=40ms and remainder <refresh make this sum bounded,
     * independent of process lifetime. No EMA estimate can earn GPU work. */
    const uint64_t credit=p->credit_ns+(interval-refresh_ns);
    unsigned count=(unsigned)(credit/refresh_ns);
    p->credit_ns=credit%refresh_ns;
    if (p->samples<FG_ADAPTIVE_PLAN_MIN_SAMPLES) return 0;
    if (count>p->ceiling) count=p->ceiling;
    /* ceil(interval/refresh)-1 without float rounding or addition overflow. */
    const unsigned horizon=(unsigned)((interval-1)/refresh_ns);
    if (count>horizon) count=horizon;
    if (count>FG_ADAPTIVE_PLAN_CAPACITY) count=FG_ADAPTIVE_PLAN_CAPACITY;
    return count;
}
