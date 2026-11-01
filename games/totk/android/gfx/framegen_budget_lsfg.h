/* MIT. Same-queue LSFG lives inside real tickets, never as an external cost. */
#ifndef TOTK_FRAMEGEN_BUDGET_LSFG_H
#define TOTK_FRAMEGEN_BUDGET_LSFG_H
#include "framegen_budget.h"
/* Camera admission adds independently ranked real/incremental p90s over 32
 * intervals. LSFG instead uses correlated complete engine+presentation samples
 * over the last eight ticks (nearest-rank p90 == their maximum). This avoids
 * adding unrelated heavy intervals, and expires old model/scene costs quickly.
 * The unchanged freshness/probe/reservation/cooldown rules still apply, as does
 * the exact 30ms completion cap. Newest real cost is an additional hard guard.
 * No retrospective policy can guarantee a future scene's execution duration. */
static inline fg_budget_decision fg_budget_lsfg_admit(const fg_budget_policy *p,uint64_t frame) {
    fg_budget_decision d=fg_budget_admit(p,frame);
    if (d.probe || (d.status!=FG_BUDGET_ACCEPT && d.reason!=FG_BUDGET_REASON_ESTIMATE)) return d;
    const fg_budget_frame *pair=NULL,*newest=NULL;
    for (unsigned age=1;age<=8 && age<frame;age++) {
        const fg_budget_frame *f=&p->frames[(frame-age)%FG_BUDGET_FRAMES];
        if (f->frame!=frame-age || f->status!=FG_BUDGET_VALID) continue;
        if (!newest) newest=f;
        if (f->synthetic && (!pair || f->real_ms+f->incremental_ms>pair->real_ms+pair->incremental_ms)) pair=f;
    }
    if (!pair || !newest) return d;
    d.frame=pair->frame; d.first=pair->first; d.last=pair->last;
    d.real_ms=fmax(pair->real_ms,newest->real_ms);
    d.incremental_ms=pair->incremental_ms;
    d.total_ms=d.real_ms+d.incremental_ms; d.margin_ms=FG_BUDGET_TICK_MS-d.total_ms;
    d.status=isfinite(d.total_ms) && d.total_ms<=FG_BUDGET_TARGET_MS ? FG_BUDGET_ACCEPT : FG_BUDGET_OVERLOAD;
    d.reason=d.status==FG_BUDGET_ACCEPT ? FG_BUDGET_REASON_NONE : FG_BUDGET_REASON_ESTIMATE;
    return d;
}
static inline fg_budget_decision fg_budget_lsfg_reserve(fg_budget_policy *p,uint64_t frame) {
    fg_budget_decision d=fg_budget_lsfg_admit(p,frame);
    if (d.status!=FG_BUDGET_ACCEPT) return d;
    p->reserved_frames[frame%FG_BUDGET_FRAMES]=frame;
    p->last_reserved=frame; p->reservations++;
    if (d.probe) { p->last_probe=frame; p->probes++; }
    return d;
}
#endif
