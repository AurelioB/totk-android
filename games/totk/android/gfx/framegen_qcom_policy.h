#ifndef TOTK_FRAMEGEN_QCOM_POLICY_H
#define TOTK_FRAMEGEN_QCOM_POLICY_H
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include "framegen_budget.h"
/* Selection stays independent of capability refusal: adaptive never changes to
 * the camera engine when this backend is unavailable. */
static inline int fg_qcom_adaptive(const char *mode) {
    return mode && !strcmp(mode,"adaptive");
}
static inline int fg_qcom_requested(const char *engine,const char *mode) {
    return engine && !strcmp(engine,"qcom") && mode &&
        (!strcmp(mode,"locked") || fg_qcom_adaptive(mode));
}
/* Frozen Thor qualification identity: model/device from the recorded Thor
 * preflight, Vulkan renderer from its retained driver log. GPU family alone
 * is not admission: Odin AFME faults are not recoverable fallback. */
static inline int fg_qcom_adaptive_device(const char *model,const char *device,
        uint32_t vendor,const char *gpu) {
    return model && !strcmp(model,"AYN Thor") && device && !strcmp(device,"kalama") &&
        vendor==0x5143 && gpu && !strcmp(gpu,"Turnip Adreno (TM) 740");
}
/* Capability/import/fence/corruption refusal is a process-lifetime latch.
 * Cooldown expiry and drained-resource rearm never clear it. Production and
 * host tests use these same atomic admission transitions. */
static inline int fg_qcom_refused(const _Atomic int *refused) {
    return atomic_load_explicit(refused,memory_order_acquire);
}
static inline int fg_qcom_refuse(_Atomic int *refused) {
    return !atomic_exchange_explicit(refused,1,memory_order_acq_rel);
}
static inline int fg_qcom_admitted(const _Atomic int *refused,int warmed,
        int suspended,int cooling,unsigned quarantined) {
    return !fg_qcom_refused(refused) && warmed && !suspended && !cooling && !quarantined;
}
static inline int fg_qcom_rearm(const _Atomic int *refused,_Atomic int *suspended,
        int warmed,int cooling,unsigned quarantined,unsigned pending) {
    if (!fg_qcom_admitted(refused,warmed,0,cooling,quarantined) || pending) return 0;
    return atomic_exchange_explicit(suspended,0,memory_order_acq_rel)!=0;
}
/* The worker's actual initialization entry runs through this boundary. Host
 * tests inject that entry, so rejected devices must not enter EGL context
 * creation or AFME symbol discovery at all, not merely fail can_generate. */
typedef int (*fg_qcom_initialize_fn)(void *);
static inline int fg_qcom_initialize(const _Atomic int *refused,int adaptive,
        const char *model,const char *device,uint32_t vendor,const char *gpu,
        fg_qcom_initialize_fn initialize,void *context) {
    if (fg_qcom_refused(refused) ||
        (adaptive && !fg_qcom_adaptive_device(model,device,vendor,gpu))) return 0;
    return initialize && initialize(context);
}
/* The production GL callback and host fixture share this exact dispatch. No
 * clamping, conversion or phase-keyed resource allocation belongs here. */
typedef void (*fg_qcom_extrapolate_fn)(unsigned,unsigned,unsigned,float);
static inline int fg_qcom_phase_valid(float phase) {
    return isfinite(phase) && phase>=0.0f && phase<=1.0f;
}
static inline int fg_qcom_extrapolate(fg_qcom_extrapolate_fn extrapolate,
        unsigned previous,unsigned current,unsigned output,float phase) {
    if (!extrapolate || !fg_qcom_phase_valid(phase)) return 0;
    extrapolate(previous,current,output,phase);
    return 1;
}
/* Fixed-slot ownership shared by production and deterministic host tests.
 * Native-fence completion retires GL's input reads/output write; the covering
 * Vulkan ticket independently retires its output reads and semaphore use.
 * Failed post-dispatch handoffs retain both leases permanently. */
typedef struct {
    unsigned previous,current;
    int pending,leased,quarantined,prepared;
} fg_qcom_job;
static inline int fg_qcom_job_available(const fg_qcom_job *job) {
    return !job->pending && !job->leased && !job->quarantined;
}
static inline int fg_qcom_job_begin(fg_qcom_job *job,unsigned previous,unsigned current) {
    if (!fg_qcom_job_available(job)) return 0;
    *job=(fg_qcom_job){.previous=previous,.current=current,.pending=1,.leased=1};
    return 1;
}
/* Preparation protects the exact pair without starting a GL timeout/query.
 * Completion callbacks cannot retire a template that was never dispatched. */
static inline int fg_qcom_job_prepare(fg_qcom_job *job,unsigned previous,unsigned current) {
    if (!fg_qcom_job_available(job) || previous==current) return 0;
    *job=(fg_qcom_job){.previous=previous,.current=current,.leased=1,.prepared=1};
    return 1;
}
static inline int fg_qcom_job_dispatch(fg_qcom_job *job) {
    if (!job->prepared || job->pending || job->quarantined) return 0;
    job->prepared=0; job->pending=1;
    return 1;
}
static inline int fg_qcom_job_cancel(fg_qcom_job *job) {
    if (!job->prepared || job->pending || job->quarantined) return 0;
    *job=(fg_qcom_job){0};
    return 1;
}
static inline int fg_qcom_job_reads_source(const fg_qcom_job *job,unsigned source) {
    return (job->prepared || job->pending || job->quarantined) &&
        (job->previous==source || job->current==source);
}
static inline void fg_qcom_job_gl_complete(fg_qcom_job *job) {
    if (!job->quarantined && !job->prepared) job->pending=0;
}
static inline void fg_qcom_job_vk_complete(fg_qcom_job *job) {
    if (!job->quarantined && !job->prepared) job->leased=0;
}
static inline void fg_qcom_job_quarantine(fg_qcom_job *job) {
    job->quarantined=1;
}
/* Only the recorder can reserve generation. If the handoff stops BEFORE GL,
 * undo exactly that reservation, never a newer reservation, feedback update or
 * sealed measurement. Post-dispatch cancellation must not use this rollback. */
typedef struct {
    uint64_t frame,reserved,last_reserved,last_probe,reservations,probes;
} fg_qcom_reservation;
static inline fg_qcom_reservation fg_qcom_reservation_save(const fg_budget_policy *p,uint64_t frame) {
    return (fg_qcom_reservation){frame,p->reserved_frames[frame%FG_BUDGET_FRAMES],
        p->last_reserved,p->last_probe,p->reservations,p->probes};
}
static inline int fg_qcom_reservation_cancel(fg_budget_policy *p,const fg_qcom_reservation *saved) {
    if (p->last_reserved!=saved->frame || p->reservations!=saved->reservations+1 ||
        p->reserved_frames[saved->frame%FG_BUDGET_FRAMES]!=saved->frame) return 0;
    p->reserved_frames[saved->frame%FG_BUDGET_FRAMES]=saved->reserved;
    p->last_reserved=saved->last_reserved; p->last_probe=saved->last_probe;
    p->reservations=saved->reservations; p->probes=saved->probes;
    return 1;
}
/* Prepared publication is PREDICTIVE, not a claim that the still-open charging
 * interval's REAL work is complete. Predict REAL as max(latest two complete
 * valid intervals), at most four ticks old, then add ALL measured incremental
 * work already charged here. A missing child ticket/GL result is never zero.
 * Ordinary accounting alone owns actual costs. This bounded audit only copies
 * finalized aggregates, including canceled/refused work, and pauses the next
 * eight charge intervals after an overshoot or unavailable actual measurement. */
enum { FG_QCOM_PREPARED_BACKOFF=8 };
typedef struct {
    uint64_t frame,first,last;
    unsigned children;
    int audited;
    fg_budget_decision predicted;
    fg_budget_frame actual;
} fg_qcom_prepared_interval;
typedef struct {
    fg_qcom_prepared_interval interval[FG_BUDGET_FRAMES];
    uint64_t cooldown_until,audits,overshoots,missing,decisions,publications;
    unsigned pending;
} fg_qcom_prepared_policy;
static inline int fg_qcom_prepared_cooling(const fg_qcom_prepared_policy *a,uint64_t frame) {
    return a->cooldown_until &&
        (frame<a->cooldown_until || a->cooldown_until==UINT64_MAX);
}
static inline int fg_qcom_prepared_capacity(const fg_qcom_prepared_policy *a,uint64_t frame) {
    const fg_qcom_prepared_interval *c=&a->interval[frame%FG_BUDGET_FRAMES];
    return frame && (!c->frame || (c->frame==frame && !c->audited) ||
        (c->frame<frame && c->audited));
}
/* Record each actual optional ticket range at submission, not at acceptance.
 * FIFO order makes duplicate/replayed/overlapping ranges inert. No GPU cost is
 * accumulated here, so this cannot double-charge ordinary external accounting. */
static inline int fg_qcom_prepared_charge(fg_qcom_prepared_policy *a,
        uint64_t frame,uint64_t first,uint64_t last) {
    if (!frame || !first || last<first || last-first>=FG_BUDGET_SLOTS) return 0;
    fg_qcom_prepared_interval *c=&a->interval[frame%FG_BUDGET_FRAMES];
    if (c->frame!=frame) {
        if (c->frame && (!c->audited || c->frame>frame)) return 0;
        *c=(fg_qcom_prepared_interval){.frame=frame,.first=first,
            .predicted={.status=FG_BUDGET_MISSING,.real_ms=NAN,.incremental_ms=NAN,.total_ms=NAN}};
        a->pending++;
    }
    if (c->audited || first<=c->last) return 0;
    c->last=last; c->children++;
    return 1;
}
/* Returns one only for a newly immutable actual snapshot. Call after every
 * sample, external completion and real seal under the accounting mutex. */
static inline int fg_qcom_prepared_audit(const fg_budget_policy *p,
        fg_qcom_prepared_policy *a,unsigned index,uint64_t now) {
    fg_qcom_prepared_interval *c=&a->interval[index];
    if (!c->frame || c->audited || p->last_frame<c->frame) return 0;
    const fg_budget_frame *f=&p->frames[c->frame%FG_BUDGET_FRAMES];
    if (f->frame==c->frame && f->status==FG_BUDGET_PENDING) return 0;
    if (f->frame!=c->frame && p->last_frame-c->frame<FG_BUDGET_FRAMES) return 0;
    c->actual=f->frame==c->frame ? *f :
        (fg_budget_frame){.frame=c->frame,.status=FG_BUDGET_UNAVAILABLE};
    int valid=c->actual.status==FG_BUDGET_VALID && c->actual.synthetic &&
        c->first>=c->actual.first && c->last<=c->actual.last &&
        isfinite(c->actual.real_ms) && c->actual.real_ms>0 &&
        isfinite(c->actual.incremental_ms) && c->actual.incremental_ms>=0 &&
        isfinite(c->actual.real_ms+c->actual.incremental_ms);
    int overshoot=valid && c->actual.real_ms+c->actual.incremental_ms>FG_BUDGET_TARGET_MS;
    c->audited=1; a->pending--; a->audits++; a->missing+=!valid; a->overshoots+=overshoot;
    if (!valid || overshoot) {
        uint64_t until=fg_budget_after(now,FG_QCOM_PREPARED_BACKOFF);
        if (until>a->cooldown_until) a->cooldown_until=until;
    }
    return 1;
}
static inline int fg_qcom_prepared_pending(const fg_qcom_prepared_policy *a) {
    return a->pending!=0;
}
/* external_frame/ms/pending/valid describe the existing charging-interval GL
 * group, not this child's source. external_ms independently authenticates this
 * child's native-fence/timer harvest. Neither is added to finalized history. */
static inline fg_budget_decision fg_qcom_prepared_completed(const fg_budget_policy *p,
        const fg_qcom_prepared_policy *a,uint64_t source_frame,uint64_t charge_frame,
        uint64_t first,uint64_t last,double external_ms,uint64_t external_frame,
        double external_total,unsigned external_pending,int external_valid) {
    fg_budget_decision missing={.status=FG_BUDGET_MISSING,.reason=FG_BUDGET_REASON_MISSING,
        .requested_frame=charge_frame,.real_ms=NAN,.incremental_ms=NAN,.total_ms=NAN};
    if (!source_frame || charge_frame<=source_frame || !first || last<first ||
        last-first>=FG_BUDGET_SLOTS || !isfinite(external_ms) || external_ms<0 ||
        external_frame!=charge_frame || external_pending || !external_valid ||
        !isfinite(external_total) || external_total<external_ms) return missing;
    const fg_budget_frame *source=&p->frames[source_frame%FG_BUDGET_FRAMES];
    const fg_budget_frame *charge=&p->frames[charge_frame%FG_BUDGET_FRAMES];
    const fg_qcom_prepared_interval *c=&a->interval[charge_frame%FG_BUDGET_FRAMES];
    if (source->frame!=source_frame || source->status!=FG_BUDGET_VALID ||
        !isfinite(source->real_ms) || source->real_ms<=0 || source->last>=first ||
        c->frame!=charge_frame || first<c->first || last>c->last) return missing;
    uint64_t begin;
    if (charge->frame==charge_frame) {
        if (charge->status==FG_BUDGET_UNAVAILABLE ||
            first<charge->first || last>charge->last) return missing;
        begin=charge->first;
    } else {
        if (p->last_frame==UINT64_MAX || charge_frame!=p->last_frame+1 ||
            first<=p->last_ticket) return missing;
        begin=p->last_ticket+1;
    }
    if (c->last<begin || c->last-begin>=FG_BUDGET_SLOTS) return missing;
    double incremental=external_total;
    for (uint64_t ticket=begin;;ticket++) {
        const fg_budget_slot *s=&p->slots[ticket%FG_BUDGET_SLOTS];
        if (s->ticket!=ticket || (ticket>=first && ticket<=last && !s->synthetic)) return missing;
        if (s->synthetic) {
            if (s->measured!=1 || !isfinite(s->ms) || s->ms<0) return missing;
            incremental+=s->ms;
        }
        if (ticket==c->last) break;
    }
    fg_budget_decision d;
    if (charge->frame==charge_frame && charge->status==FG_BUDGET_VALID) {
        /* Once known, the immutable ACTUAL aggregate supersedes prediction.
         * It already contains every synthetic ticket and the GL group once. */
        d=fg_budget_completed(p,charge_frame);
    } else {
        const fg_budget_frame *prediction=NULL;
        unsigned samples=0;
        uint64_t newest=0;
        for (unsigned age=1;age<=FG_BUDGET_MAX_AGE && age<charge_frame;age++) {
            uint64_t id=charge_frame-age;
            const fg_budget_frame *f=&p->frames[id%FG_BUDGET_FRAMES];
            if (f->frame!=id || f->status==FG_BUDGET_PENDING) continue;
            if (f->status!=FG_BUDGET_VALID || !isfinite(f->real_ms) || f->real_ms<=0) {
                if (!samples) return missing;
                break;
            }
            if (!samples) newest=id;
            if (!prediction || f->real_ms>prediction->real_ms) prediction=f;
            if (++samples==2) break;
        }
        if (!prediction) return missing;
        d=fg_budget_decide(prediction,incremental);
        d.probe=1; /* explicitly predicted REAL, exact measured incremental */
        d.real_samples=samples; d.newest_real_frame=newest;
    }
    d.requested_frame=charge_frame;
    d.incremental_frame=d.newest_incremental_frame=charge_frame;
    d.incremental_first=begin; d.incremental_last=c->last; d.incremental_samples=1;
    if (fg_qcom_prepared_cooling(a,charge_frame)) {
        d.status=FG_BUDGET_OVERLOAD; d.reason=FG_BUDGET_REASON_FEEDBACK;
        d.retry_frame=a->cooldown_until;
    }
    return d;
}
static inline void fg_qcom_prepared_decision(fg_qcom_prepared_policy *a,
        uint64_t frame,const fg_budget_decision *d,int published) {
    fg_qcom_prepared_interval *c=&a->interval[frame%FG_BUDGET_FRAMES];
    a->decisions++; a->publications+=!!published;
    if (c->frame==frame && !c->audited && isfinite(d->total_ms) &&
        (!isfinite(c->predicted.total_ms) || d->total_ms>c->predicted.total_ms))
        c->predicted=*d;
}
/* Real scene ticks, not time since process launch. Timing failures during the
 * first ten seconds of real30 never escalate to a permanent refusal. Locked
 * QCOM and LSFG retain their existing five-miss refusal; adaptive timing
 * incidents only reject the output and suspend generation for bounded reprobing. */
enum { FG_QCOM_WARM_TICKS=300, FG_QCOM_MISS_WINDOW=120, FG_QCOM_MISS_LIMIT=5 };
enum { FG_QCOM_DEADLINE, FG_QCOM_TIMING, FG_QCOM_SLOW, FG_QCOM_FAILURE_KINDS };
typedef struct {
    uint64_t tick, first_scene, successes, window, cooldown_until;
    unsigned misses, backoff;
    uint64_t failures[FG_QCOM_FAILURE_KINDS], startup_ignored, cooldown_skips;
    uint64_t timing_cooldowns,timing_reprobes,timing_recoveries,reprobe_frame;
    unsigned reprobe_slot;
    int reprobe_pending;
} fg_qcom_policy;
static inline void fg_qcom_tick(fg_qcom_policy *p,int scene) {
    p->tick++;
    if (scene) { if (!p->first_scene) p->first_scene=p->tick; p->successes++; }
    if (p->window && p->tick-p->window>=FG_QCOM_MISS_WINDOW) {
        p->window=p->tick; p->misses=0; p->backoff=0;
    }
    if (p->tick<p->cooldown_until) p->cooldown_skips++;
}
static inline int fg_qcom_failure(fg_qcom_policy *p,unsigned kind) {
    if (kind>=FG_QCOM_FAILURE_KINDS) return 0;
    p->failures[kind]++;
    int startup=!p->first_scene || p->successes<FG_QCOM_WARM_TICKS;
    if (startup) p->startup_ignored++;
    /* Delayed callbacks for one outstanding incident do not escalate. */
    if (p->cooldown_until>p->tick) return 0;
    if (!p->window || p->tick-p->window>=FG_QCOM_MISS_WINDOW) { p->window=p->tick; p->misses=0; }
    if (!startup) p->misses++;
    p->backoff=!p->backoff ? 4 : p->backoff>=16 ? 30 : p->backoff*2;
    p->cooldown_until=p->tick+p->backoff+1;
    return !startup && p->misses>=FG_QCOM_MISS_LIMIT;
}
static inline int fg_qcom_can_generate(const fg_qcom_policy *p) {
    return p->tick>=p->cooldown_until;
}
/* Adaptive changes only the timing disposition, not the existing incident
 * coalescing, 4/8/16/30 cooldown, warmup or strict GPU admission thresholds. */
static inline int fg_qcom_timing_failure(fg_qcom_policy *p,unsigned kind,int adaptive) {
    uint64_t until=p->cooldown_until;
    int permanent=fg_qcom_failure(p,kind);
    if (!adaptive) return permanent;
    if (p->cooldown_until!=until) {
        p->timing_cooldowns++;
        p->reprobe_pending=1;
        p->reprobe_frame=0;
    }
    return 0;
}
/* Reprobe counts an actual new GL dispatch, not merely an expired cooldown.
 * Recovery requires that exact frame/slot's valid, non-slow timing completion;
 * an older job or another child of the same real frame cannot claim recovery. */
static inline int fg_qcom_timing_reprobe(fg_qcom_policy *p,uint64_t frame,unsigned slot) {
    if (!p->reprobe_pending) return 0;
    p->reprobe_pending=0; p->reprobe_frame=frame; p->timing_reprobes++;
    p->reprobe_slot=slot;
    return 1;
}
static inline int fg_qcom_timing_recovered(fg_qcom_policy *p,uint64_t frame,unsigned slot) {
    if (!p->reprobe_frame || p->reprobe_frame!=frame || p->reprobe_slot!=slot) return 0;
    p->reprobe_frame=0; p->timing_recoveries++;
    return 1;
}
#endif
