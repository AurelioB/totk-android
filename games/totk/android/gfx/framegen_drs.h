#ifndef TOTK_FRAMEGEN_DRS_H
#define TOTK_FRAMEGEN_DRS_H
#include <float.h>
#include "framegen_budget.h"

/* Real-frame target only: no invented GPU sample and no change to admission.
 * Sparse overload probes refresh incremental cost about once/second. Keep an
 * authenticated estimate for at most three seconds, not just the four-tick
 * admission freshness window, or rejected pairs could never drive DRS down. */
#define FG_DRS_SECOND UINT64_C(1000000000)
#define FG_DRS_STALE (3 * FG_DRS_SECOND)
#define FG_DRS_NATIVE FLT_MAX

typedef struct {
    uint64_t update_ns, sample_ns, sample_frame;
    double incremental_ms, budget_ms;
    int active;
} fg_drs_policy;

/* At most one tightening/recovery publication per wall-clock second. Safety
 * restoration is immediate and does not erase that rate limit. Native DRS
 * retains its own level counters and hysteresis in addition to this deadband. */
static inline double fg_drs_update(fg_drs_policy *p, uint64_t now, int enabled,
        uint64_t sample_frame, double incremental_ms) {
    if (enabled && sample_frame && sample_frame != p->sample_frame &&
        isfinite(incremental_ms) && incremental_ms >= 0) {
        p->sample_frame=sample_frame; p->sample_ns=now;
        if (!p->active) p->incremental_ms=incremental_ms;
        else p->incremental_ms+=.25*(incremental_ms-p->incremental_ms);
    }
    if (!enabled || !p->sample_ns || now < p->sample_ns || now-p->sample_ns > FG_DRS_STALE) {
        p->active=0; p->sample_ns=p->sample_frame=0;
        p->budget_ms=FG_DRS_NATIVE;
        return FG_DRS_NATIVE;
    }
    if (p->update_ns && (now < p->update_ns || now-p->update_ns < FG_DRS_SECOND))
        return p->active ? p->budget_ms : FG_DRS_NATIVE;
    p->update_ns=now;
    /* Half a millisecond of headroom inside the existing 30-ms total ceiling.
     * The floor limits this experimental controller's quality pressure, not
     * the generated-frame admission test: >6-ms incremental can still drop. */
    double target=fmax(24.0,fmin(30.0,FG_BUDGET_TARGET_MS-p->incremental_ms-.5));
    if (!p->active) p->budget_ms=target;
    else if (target < p->budget_ms-.25) p->budget_ms=fmax(target,p->budget_ms-1.0);
    else if (target > p->budget_ms+.5) p->budget_ms=fmin(target,p->budget_ms+.25);
    p->active=1;
    return p->budget_ms;
}
#endif
