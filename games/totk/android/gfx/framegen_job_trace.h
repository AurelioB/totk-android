/* MIT. Opt-in job telemetry: CPU ticket-ready publication is an upper bound
 * after GPU completion and CPU posts, NOT a hardware GPU signal timestamp. */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t frame,submit,pickup,source_ready,output_ready,target,decision;
} fg_job_trace;
static inline const char *fg_job_trace_class(fg_job_trace t) {
    if (!t.source_ready || !t.output_ready) return "ready-unavailable";
    if (t.source_ready>=t.target || t.output_ready>=t.target)
        return "fence-or-cpu-publication-late-gpu-cause-unknown";
    return t.decision>=t.target ? "presenter-drain-or-deschedule-late" : "before-target";
}
static inline int fg_job_trace_format(char *line,size_t capacity,fg_job_trace t,
        const char *mode,const char *outcome) {
    return snprintf(line,capacity,"[framegen-job] mode=%s frame=%llu submit-request-ns=%llu pickup-ns=%llu source-ready-observed-ns=%llu output-ready-observed-ns=%llu target-ns=%llu decision-ns=%llu outcome=%s timing=%s; ready=CPU-publication-after-GPU-and-posts-not-hardware-signal",
        mode,(unsigned long long)t.frame,(unsigned long long)t.submit,
        (unsigned long long)t.pickup,(unsigned long long)t.source_ready,
        (unsigned long long)t.output_ready,(unsigned long long)t.target,
        (unsigned long long)t.decision,outcome,fg_job_trace_class(t));
}
typedef enum {
    FG_SLOT_CANCEL_NONE = 0,
    FG_SLOT_CANCEL_MODE,
    FG_SLOT_CANCEL_OWNER,
    FG_SLOT_CANCEL_WINDOW,
    FG_SLOT_CANCEL_EPOCH,
    FG_SLOT_CANCEL_SNAPSHOT_PRESSURE,
    FG_SLOT_CANCEL_EMPTY,
    FG_SLOT_CANCEL_NO_HOLE,
    FG_SLOT_CANCEL_POLICY,
    FG_SLOT_CANCEL_DEADLINE,
    FG_SLOT_CANCEL_CLOCK,
    FG_SLOT_CANCEL_PHASE,
    FG_SLOT_CANCEL_DISPATCH_REFUSED
} fg_slot_cancel_reason;
static inline const char *fg_slot_cancel_reason_name(fg_slot_cancel_reason reason) {
    switch (reason) {
    case FG_SLOT_CANCEL_NONE: return "none";
    case FG_SLOT_CANCEL_MODE: return "mode";
    case FG_SLOT_CANCEL_OWNER: return "owner";
    case FG_SLOT_CANCEL_WINDOW: return "window";
    case FG_SLOT_CANCEL_EPOCH: return "epoch";
    case FG_SLOT_CANCEL_SNAPSHOT_PRESSURE: return "snapshot-pressure";
    case FG_SLOT_CANCEL_EMPTY: return "empty";
    case FG_SLOT_CANCEL_NO_HOLE: return "no-hole";
    case FG_SLOT_CANCEL_POLICY: return "policy";
    case FG_SLOT_CANCEL_DEADLINE: return "deadline";
    case FG_SLOT_CANCEL_CLOCK: return "clock";
    case FG_SLOT_CANCEL_PHASE: return "phase";
    case FG_SLOT_CANCEL_DISPATCH_REFUSED: return "dispatch-refused";
    default: return "unknown";
    }
}
typedef struct {
    uint64_t frame,token,target,boundary;
    unsigned reason,generation_state,backend_reason;
    uint64_t pressure_observations,pressure_cancellations,pressure_appends;
} fg_slot_job_trace;
static inline int fg_slot_job_trace_format(char *line,size_t capacity,fg_slot_job_trace t) {
    return snprintf(line,capacity,"[adaptive-slot-job] frame=%llu token=%llu target=%llu boundary=%llu reason=%s generation-state=%u backend-reason=%u pressure-observations-lifetime-cpu=%llu pressure-cancellations-lifetime-cpu=%llu pressure-appends-lifetime-cpu=%llu",
        (unsigned long long)t.frame,(unsigned long long)t.token,
        (unsigned long long)t.target,(unsigned long long)t.boundary,
        fg_slot_cancel_reason_name((fg_slot_cancel_reason)t.reason),
        t.generation_state,t.backend_reason,
        (unsigned long long)t.pressure_observations,
        (unsigned long long)t.pressure_cancellations,
        (unsigned long long)t.pressure_appends);
}
