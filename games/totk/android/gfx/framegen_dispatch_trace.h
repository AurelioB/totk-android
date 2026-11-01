/* MIT. Diagnostic-only dispatch observations. Never admission authority. */
#pragma once
#include <stdint.h>
#include "framegen_budget.h"

typedef struct {
    int enabled;
    uint64_t source,charge,deadline;
    unsigned output,previous,current;
    float phase;
    int budget_known,budget_status,budget_reason,probe,cooling,capacity;
    int qcom_warmed,qcom_suspended,qcom_cooling,qcom_quarantined,qcom_admitted;
    uint64_t budget_frame,reserved_frame,cooldown_until;
    /* charge nominates an interval, NOT proof that any work was submitted.
     * Tickets are copied only after existing assignments; charge_status is
     * 0 not attempted, 1 accepted, 2 refused by the existing range charge. */
    uint64_t first_ticket,ticket;
    int range_observed,charge_status;
} fg_dispatch_trace;
/* Recording-owner scope, valid only during the existing prepared dispatch.
 * QCOM copies it into its already-owned output; no policy query is permitted.
 * TLS prevents an unrelated completion thread from inheriting this scope. */
extern _Thread_local fg_dispatch_trace *vk_fg_dispatch_trace_current;
void fg_dispatch_trace_event(const fg_dispatch_trace *trace,const char *event,
    const char *stage,const char *reason);
/* Each original predicate is evaluated EXACTLY once, in its original &&/||
 * position. No trace-on probe may replace, repeat or bypass that predicate. */
#define FG_DISPATCH_GUARD(trace,stage,reason,expression) \
    ((expression) ? (fg_dispatch_trace_event((trace),"guard",(stage),(reason)),1) : 0)
#define FG_DISPATCH_FACT(trace,field,expression) \
    ((trace) && (trace)->enabled ? ((trace)->field=(expression)) : (expression))
static inline void fg_dispatch_trace_tickets(fg_dispatch_trace *trace,
        uint64_t first,uint64_t ticket) {
    if (!trace || !trace->enabled) return;
    trace->first_ticket=first; trace->ticket=ticket;
    trace->range_observed=first && ticket>=first;
}
static inline const char *fg_dispatch_charge_status(int status) {
    return status==1 ? "accepted" : status==2 ? "refused" : "not-attempted";
}
static inline const char *fg_dispatch_budget_reason(int reason) {
    switch (reason) {
    case FG_BUDGET_REASON_NONE: return "none";
    case FG_BUDGET_REASON_MISSING: return "missing";
    case FG_BUDGET_REASON_ESTIMATE: return "estimate";
    case FG_BUDGET_REASON_REAL: return "real";
    case FG_BUDGET_REASON_PROBE_RATE: return "probe-rate";
    case FG_BUDGET_REASON_FEEDBACK: return "feedback";
    case FG_BUDGET_REASON_RESERVED: return "reserved";
    case FG_BUDGET_REASON_STALE_ESTIMATE: return "stale-estimate";
    default: return "unqueried";
    }
}
