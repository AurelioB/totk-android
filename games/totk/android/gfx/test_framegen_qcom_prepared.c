/* Host-only prepared/pending ownership contract; no EGL/Vulkan dependency. */
#include "framegen_qcom_policy.h"
#include "framegen_budget_external.h"
#include <assert.h>
#include <stdio.h>

static unsigned calls;
static float observed_phase;
static void extrapolate(unsigned previous,unsigned current,unsigned output,float phase) {
    assert(previous==1 && current==2 && output==7);
    calls++;
    observed_phase=phase;
}

static void prepared_cancel(void) {
    fg_qcom_job job={0};
    assert(fg_qcom_job_prepare(&job,1,2));
    assert(job.prepared && job.leased && !job.pending && !job.quarantined);
    assert(fg_qcom_job_reads_source(&job,1) && fg_qcom_job_reads_source(&job,2));
    assert(!fg_qcom_job_reads_source(&job,0) && !fg_qcom_job_available(&job));
    assert(!fg_qcom_job_prepare(&job,2,3) && !fg_qcom_job_begin(&job,2,3));
    /* A prepared template is not a timed GL job, even if unrelated completion
     * callbacks, a missed scheduler deadline, or shutdown precede dispatch. */
    fg_qcom_job_gl_complete(&job);
    fg_qcom_job_vk_complete(&job);
    assert(job.prepared && job.leased && !job.pending);
    assert(fg_qcom_job_cancel(&job));
    assert(fg_qcom_job_available(&job));
    assert(!fg_qcom_job_reads_source(&job,1));
    assert(!fg_qcom_job_cancel(&job) && !fg_qcom_job_dispatch(&job));
    assert(calls==0);
}

static void submitted_cancel(int gl_first) {
    fg_qcom_job job={0};
    assert(fg_qcom_job_prepare(&job,1,2));
    assert(fg_qcom_job_dispatch(&job));
    assert(!job.prepared && job.pending && job.leased);
    assert(!fg_qcom_job_dispatch(&job) && !fg_qcom_job_cancel(&job));
    assert(fg_qcom_extrapolate(extrapolate,1,2,7,0.375f));
    assert(observed_phase==0.375f);
    /* Late/disabled/surface-lost output cannot release the in-flight graph.
     * GL and the covering FIFO ticket remain independent retirement owners. */
    if (gl_first) fg_qcom_job_gl_complete(&job);
    else fg_qcom_job_vk_complete(&job);
    assert(!fg_qcom_job_available(&job) && !fg_qcom_job_cancel(&job));
    assert(fg_qcom_job_reads_source(&job,1)==!gl_first);
    if (gl_first) fg_qcom_job_vk_complete(&job);
    else fg_qcom_job_gl_complete(&job);
    assert(fg_qcom_job_available(&job));
    fg_qcom_job_vk_complete(&job);
    fg_qcom_job_gl_complete(&job);
    assert(fg_qcom_job_available(&job));
}

static void failed_bridge_shutdown(void) {
    fg_qcom_job job={0};
    assert(fg_qcom_job_prepare(&job,1,2) && fg_qcom_job_dispatch(&job));
    fg_qcom_job_quarantine(&job);
    assert(!fg_qcom_job_cancel(&job));
    fg_qcom_job_gl_complete(&job);
    fg_qcom_job_vk_complete(&job);
    assert(job.quarantined && job.leased && job.pending);
    assert(fg_qcom_job_reads_source(&job,1) && fg_qcom_job_reads_source(&job,2));
    assert(!fg_qcom_job_available(&job) && !fg_qcom_job_prepare(&job,2,3));
}

static void original_policy(void) {
    fg_qcom_job job={0};
    assert(fg_qcom_job_begin(&job,1,2));
    assert(job.pending && job.leased && !job.prepared);
    fg_qcom_job_gl_complete(&job);
    assert(!fg_qcom_job_reads_source(&job,1) && !fg_qcom_job_available(&job));
    fg_qcom_job_vk_complete(&job);
    assert(fg_qcom_job_available(&job));
    assert(!fg_qcom_extrapolate(extrapolate,1,2,7,NAN));
    assert(!fg_qcom_extrapolate(extrapolate,1,2,7,1.01f));
    assert(fg_qcom_adaptive_device("AYN Thor","kalama",0x5143,"Turnip Adreno (TM) 740"));
    assert(!fg_qcom_adaptive_device("Odin 3","sun",0x5143,"Turnip Adreno (TM) 830"));
}

typedef struct {
    fg_budget_policy p;
    fg_budget_external external;
    fg_budget_external_group group;
    fg_qcom_prepared_policy audit;
} prepared_budget;
static void real_interval(prepared_budget *b,double ms) {
    uint64_t ticket=b->p.last_ticket+1,frame=b->p.last_frame+1;
    fg_budget_external_sample(&b->p,&b->external,ticket,ms,1);
    fg_budget_external_seal(&b->p,&b->external,frame,ticket);
}
static void budget_init(prepared_budget *b,double source_ms) {
    *b=(prepared_budget){0};
    real_interval(b,1); /* boundary, not a partial REAL */
    real_interval(b,source_ms);
    assert(b->p.frames[2].status==FG_BUDGET_VALID);
}
static void child(prepared_budget *b,uint64_t first,double vk0,double vk1,double gl) {
    uint64_t frame=b->p.last_frame+1;
    fg_budget_external_group_begin(&b->external,&b->group,frame);
    fg_budget_synthetic(&b->p,first,first+1);
    assert(fg_qcom_prepared_charge(&b->audit,frame,first,first+1));
    fg_budget_external_sample(&b->p,&b->external,first,vk0,1);
    fg_budget_external_sample(&b->p,&b->external,first+1,vk1,1);
    fg_budget_external_group_end(&b->p,&b->external,&b->group,frame,gl,1);
}
static fg_budget_decision completed(const prepared_budget *b,uint64_t source,
        uint64_t charge,uint64_t first,double gl) {
    return fg_qcom_prepared_completed(&b->p,&b->audit,source,charge,first,first+1,gl,
        b->group.frame,b->group.ms,b->group.pending,b->group.valid);
}
static void seal_charge(prepared_budget *b,double real_ms) {
    uint64_t frame=b->group.frame,ticket=b->audit.interval[frame%FG_BUDGET_FRAMES].last+1;
    fg_budget_external_group_seal(&b->p,&b->external,&b->group,frame);
    fg_budget_external_seal(&b->p,&b->external,frame,ticket);
    fg_budget_external_sample(&b->p,&b->external,ticket,real_ms,1);
}
static void differing_source_and_charge(void) {
    prepared_budget b;
    budget_init(&b,5);
    real_interval(&b,29); /* latest complete REAL, not source frame 2 */
    child(&b,4,1,2,4);
    fg_budget_policy before=b.p;
    fg_budget_decision d=completed(&b,2,4,4,4);
    assert(d.status==FG_BUDGET_OVERLOAD && d.probe && d.real_ms==29 &&
        d.incremental_ms==7 && d.total_ms==36 && d.requested_frame==4);
    assert(!memcmp(&b.p,&before,sizeof before)); /* never rewrites old source or history */
    seal_charge(&b,29);
    before=b.p;
    d=completed(&b,2,4,4,4);
    assert(d.status==FG_BUDGET_OVERLOAD && !d.probe && d.real_ms==29 && d.total_ms==36);
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,4,5) && b.audit.overshoots==1);
    assert(!memcmp(&b.p,&before,sizeof before));
    /* An expensive old source is provenance, NOT an actual/predicted charge.
     * Two newer completed intervals provide the explicit current predictor. */
    budget_init(&b,29);
    real_interval(&b,5); real_interval(&b,6);
    child(&b,5,1,2,4);
    d=completed(&b,2,5,5,4);
    assert(d.status==FG_BUDGET_ACCEPT && d.probe && d.real_ms==6 && d.total_ms==13);
    assert(d.real_samples==2 && d.newest_real_frame==4);
    seal_charge(&b,8);
    d=completed(&b,2,5,5,4);
    assert(!d.probe && d.real_ms==8 && d.incremental_ms==7 && d.total_ms==15);
}
static void missing_completion_evidence(void) {
    prepared_budget b;
    budget_init(&b,5); child(&b,3,1,2,4);
    fg_budget_policy before=b.p;
    fg_qcom_prepared_policy audit=b.audit;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_ACCEPT); /* current REAL unknown: prediction */
    assert(completed(&b,2,3,3,NAN).status==FG_BUDGET_MISSING);
    assert(completed(&b,2,3,3,-1).status==FG_BUDGET_MISSING);
    assert(completed(&b,1,3,3,4).status==FG_BUDGET_MISSING); /* boundary is not source evidence */
    assert(completed(&b,2,4,3,4).status==FG_BUDGET_MISSING); /* wrong charge identity */
    assert(completed(&b,2,3,2,4).status==FG_BUDGET_MISSING); /* REAL must never become synthetic */
    assert(completed(&b,2,3,4,4).status==FG_BUDGET_MISSING); /* uncovered child ticket */
    b.group.pending=1;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING); /* incomplete GL group */
    b.group.pending=0; b.group.valid=0;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING);
    b.group.valid=1; b.group.ms=NAN;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING);
    b.group.ms=4; b.group.frame=4;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING);
    b.group.frame=3;
    b.p.slots[4].measured=0;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING); /* missing covering timer */
    b.p.slots[4].measured=-1;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING); /* invalid covering timer */
    b.p.slots[4].measured=1; b.p.slots[4].synthetic=0;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING);
    b.p=before; b.p.frames[2].status=FG_BUDGET_PENDING;
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING); /* partial source REAL */
    b.p=before;
    for (unsigned replay=0;replay<4;replay++)
        assert(completed(&b,2,3,3,4).total_ms==12);
    assert(!memcmp(&b.p,&before,sizeof before) && !memcmp(&b.audit,&audit,sizeof audit));
    /* Seal without a covering fence: no predictor may disguise this known
     * unavailable charge as a zero-cost completed current interval. */
    fg_budget_external_group_seal(&b.p,&b.external,&b.group,3);
    fg_budget_external_seal(&b.p,&b.external,3,0);
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING);
    before=b.p;
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,3,4));
    assert(b.audit.missing==1 && !b.audit.overshoots && b.audit.cooldown_until==12);
    assert(!memcmp(&b.p,&before,sizeof before));
    /* No complete REAL within the existing four-tick freshness horizon. */
    budget_init(&b,5);
    for (unsigned i=0;i<5;i++) {
        uint64_t frame=b.p.last_frame+1,ticket=b.p.last_ticket+1;
        fg_budget_external_seal(&b.p,&b.external,frame,ticket);
    }
    child(&b,8,1,2,4);
    assert(completed(&b,2,8,8,4).status==FG_BUDGET_MISSING);
}
static void overshoot_backoff_recovery(void) {
    prepared_budget b;
    budget_init(&b,5); child(&b,3,1,2,4);
    fg_budget_decision d=completed(&b,2,3,3,4);
    assert(d.status==FG_BUDGET_ACCEPT && d.probe && d.total_ms==12);
    fg_qcom_prepared_decision(&b.audit,3,&d,1);
    assert(!fg_qcom_prepared_audit(&b.p,&b.audit,3,4)); /* open REAL: actual UNKNOWN */
    /* REAL 29 + child 7 is an admitted predictive miss, NOT a strict-actual
     * acceptance. Immutable ordinary accounting exposes 36 and starts K=8. */
    seal_charge(&b,29);
    fg_budget_policy before=b.p;
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,3,4));
    assert(b.audit.audits==1 && b.audit.overshoots==1 && !b.audit.missing);
    assert(b.audit.interval[3].actual.real_ms==29 && b.audit.interval[3].actual.incremental_ms==7);
    assert(b.audit.interval[3].predicted.total_ms==12 && b.audit.cooldown_until==12);
    assert(!memcmp(&b.p,&before,sizeof before));
    fg_qcom_prepared_policy audit=b.audit;
    for (unsigned i=0;i<4;i++) {
        assert(!fg_qcom_prepared_audit(&b.p,&b.audit,3,20));
        assert(completed(&b,2,3,3,4).status==FG_BUDGET_OVERLOAD);
    }
    assert(!memcmp(&b.audit,&audit,sizeof audit)); /* replay cannot extend cooldown */
    for (uint64_t frame=4;frame<12;frame++) {
        assert(fg_qcom_prepared_cooling(&b.audit,frame));
        real_interval(&b,6);
        if (frame==5) {
            prepared_budget blocked=b;
            uint64_t ticket=blocked.p.last_ticket+1;
            child(&blocked,ticket,1,2,4);
            fg_budget_decision refused=completed(&blocked,5,6,ticket,4);
            assert(refused.total_ms==13 && refused.status==FG_BUDGET_OVERLOAD &&
                refused.reason==FG_BUDGET_REASON_FEEDBACK && refused.retry_frame==12);
        }
    }
    assert(!fg_qcom_prepared_cooling(&b.audit,12));
    uint64_t first=b.p.last_ticket+1;
    child(&b,first,1,2,4);
    d=completed(&b,11,12,first,4);
    assert(d.status==FG_BUDGET_ACCEPT && d.probe && d.real_ms==6 && d.total_ms==13);
    seal_charge(&b,6);
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,12,13));
    assert(b.audit.audits==2 && b.audit.overshoots==1 && !b.audit.missing);
}
static void multiple_charged_sources(void) {
    prepared_budget b;
    budget_init(&b,5); real_interval(&b,6);
    child(&b,4,1,2,4);
    fg_budget_decision first=completed(&b,2,4,4,4);
    assert(first.status==FG_BUDGET_ACCEPT && first.total_ms==13);
    fg_qcom_prepared_decision(&b.audit,4,&first,1);
    child(&b,6,1,1,16); /* scalar policy permits a second actual charge, runtime does not */
    fg_budget_decision second=completed(&b,3,4,6,16);
    assert(second.status==FG_BUDGET_OVERLOAD && second.real_ms==6 &&
        second.incremental_ms==25 && second.total_ms==31); /* NOT isolated second child's 24 */
    fg_qcom_prepared_decision(&b.audit,4,&second,0);
    fg_qcom_prepared_policy audit=b.audit;
    assert(!fg_qcom_prepared_charge(&b.audit,4,4,5) && !fg_qcom_prepared_charge(&b.audit,4,6,7));
    assert(!fg_qcom_prepared_charge(&b.audit,4,5,8)); /* overlapping replay is not new GPU work */
    assert(!memcmp(&b.audit,&audit,sizeof audit));
    fg_budget_policy before=b.p;
    assert(completed(&b,2,4,4,4).incremental_ms==25);
    assert(completed(&b,3,4,6,16).incremental_ms==25);
    assert(!memcmp(&b.p,&before,sizeof before));
    seal_charge(&b,8);
    assert(b.p.frames[4].real_ms==8 && b.p.frames[4].incremental_ms==25);
    before=b.p;
    /* Refused/canceled second child's work is still present in actual 33. */
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,4,5));
    assert(b.audit.interval[4].children==2 && b.audit.interval[4].actual.incremental_ms==25);
    assert(b.audit.overshoots==1 && b.audit.decisions==2 && b.audit.publications==1);
    fg_budget_external_group_end(&b.p,&b.external,&b.group,4,16,1); /* duplicate GL is inert */
    fg_budget_external_sample(&b.p,&b.external,7,99,1); /* duplicate FIFO timer is inert */
    assert(!memcmp(&b.p,&before,sizeof before));
    assert(!b.p.slots[2].synthetic && !b.p.slots[3].synthetic && !b.p.slots[8].synthetic);
}
static void pending_actual_and_ceiling(void) {
    prepared_budget b;
    budget_init(&b,23); child(&b,3,1,2,4);
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_ACCEPT); /* unchanged inclusive 30 */
    fg_budget_external_group_seal(&b.p,&b.external,&b.group,3);
    fg_budget_external_seal(&b.p,&b.external,3,5); /* REAL fence/timer not yet complete */
    assert(b.p.frames[3].status==FG_BUDGET_PENDING);
    assert(!fg_qcom_prepared_audit(&b.p,&b.audit,3,4));
    fg_budget_decision d=completed(&b,2,3,3,4);
    assert(d.status==FG_BUDGET_ACCEPT && d.probe && d.total_ms==30); /* explicit prediction only */
    fg_budget_external_sample(&b.p,&b.external,5,24,1);
    d=completed(&b,2,3,3,4);
    assert(d.status==FG_BUDGET_OVERLOAD && !d.probe && d.total_ms==31);
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,3,10)); /* delayed audit pauses current next 8 */
    assert(b.audit.overshoots==1 && b.audit.cooldown_until==18);
    budget_init(&b,24); child(&b,3,1,2,4);
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_OVERLOAD);
    /* The GL timer is also part of actual finalization, even after REAL retires. */
    b.group.pending=1;
    b.external.cost[3].ms=NAN;
    seal_charge(&b,5);
    assert(b.p.frames[3].status==FG_BUDGET_PENDING);
    assert(!fg_qcom_prepared_audit(&b.p,&b.audit,3,4));
    assert(completed(&b,2,3,3,4).status==FG_BUDGET_MISSING);
    fg_budget_external_group_end(&b.p,&b.external,&b.group,3,0,0);
    assert(b.p.frames[3].status==FG_BUDGET_UNAVAILABLE);
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,3,4) && b.audit.missing==1);
}
static void unpublished_actual_and_retention(void) {
    prepared_budget b;
    budget_init(&b,5); child(&b,3,1,2,4);
    assert(fg_qcom_prepared_pending(&b.audit));
    assert(!fg_qcom_prepared_capacity(&b.audit,3+FG_BUDGET_FRAMES));
    assert(!fg_qcom_prepared_charge(&b.audit,3+FG_BUDGET_FRAMES,6,7));
    /* No acceptance/decision callback at all: cancellation cannot erase cost. */
    seal_charge(&b,29);
    assert(fg_qcom_prepared_audit(&b.p,&b.audit,3,4));
    assert(!b.audit.decisions && !b.audit.publications && b.audit.overshoots==1);
    assert(!fg_qcom_prepared_pending(&b.audit));
    assert(fg_qcom_prepared_capacity(&b.audit,3+FG_BUDGET_FRAMES));
    fg_qcom_prepared_interval immutable=b.audit.interval[3];
    for (unsigned i=0;i<FG_BUDGET_FRAMES;i++) real_interval(&b,6);
    assert(!memcmp(&b.audit.interval[3],&immutable,sizeof immutable));
    uint64_t first=b.p.last_ticket+1,charge=b.p.last_frame+1;
    child(&b,first,1,2,4);
    assert(completed(&b,2,charge,first,4).status==FG_BUDGET_MISSING); /* overwritten provenance */
}

static void failed_attempt_budget(void) {
    fg_budget_policy p={0};
    fg_budget_sample(&p,1,1,1); fg_budget_seal(&p,1,1);
    fg_budget_sample(&p,2,5,1); fg_budget_seal(&p,2,2);
    fg_budget_policy before; memcpy(&before,&p,sizeof before);
    fg_qcom_reservation saved=fg_qcom_reservation_save(&p,3);
    fg_budget_decision d=fg_budget_reserve(&p,3);
    assert(d.status==FG_BUDGET_ACCEPT && d.probe && p.probes==1 && p.reservations==1);
    assert(fg_qcom_reservation_cancel(&p,&saved));
    assert(!memcmp(&p,&before,sizeof p)); /* failed/no-GL attempt has no phantom probe/cost */
    assert(!fg_qcom_reservation_cancel(&p,&saved)); /* duplicate cancellation is inert */
    d=fg_budget_reserve(&p,3);
    assert(d.status==FG_BUDGET_ACCEPT && p.probes==1 && p.reservations==1);
    assert(fg_budget_reserve(&p,3).status==FG_BUDGET_OVERLOAD);
    /* Post-dispatch callers retain this reservation, including canceled GPU
     * work. A stale rollback cannot erase a subsequently accepted interval. */
    p.last_reserved=4; p.reservations=2; p.reserved_frames[4]=4;
    assert(!fg_qcom_reservation_cancel(&p,&saved) && p.last_reserved==4 && p.reservations==2);
}

int main(void) {
    prepared_cancel();
    submitted_cancel(0);
    submitted_cancel(1);
    failed_bridge_shutdown();
    original_policy();
    differing_source_and_charge();
    missing_completion_evidence();
    overshoot_backoff_recovery();
    multiple_charged_sources();
    pending_actual_and_ceiling();
    unpublished_actual_and_retention();
    failed_attempt_budget();
    assert(calls==2);
    puts("prepared QCOM ownership/predictive admission/immutable actual audit: passed (host scalar contract, no device claim)");
    return 0;
}
