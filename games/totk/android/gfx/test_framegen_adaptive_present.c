/* Deterministic tests of the PRODUCTION publication-state helper used by the
 * presenter's monotonic deadline wait and final SurfaceTransaction guard.
 * No sleeps, GPU, Android runtime, callback timing or modeled GPU completion.
 * Actual resource retirement is covered separately by vkreplaypintest. */
#define _POSIX_C_SOURCE 200809L
#include "framegen_adaptive_present.h"
#include "framegen_adaptive.h"
#include "framegen_job_trace.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c,message) do { checks++; if (!(c)) { \
    fprintf(stderr,"adaptive presenter: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)
static const uint64_t second=UINT64_C(1000000000);
static fg_adaptive_present_job generated(uint64_t slot) {
    return (fg_adaptive_present_job){.slot=slot,.frame=10,.epoch=3,.generation=7,.generated=1};
}
static fg_adaptive_present_context context(uint64_t now) {
    return (fg_adaptive_present_context){.now=now,.epoch=3,.generation=7,.window=1};
}
static fg_adaptive_present_result decide_unchanged(fg_adaptive_present_state *state,
        fg_adaptive_present_job job,fg_adaptive_present_context c) {
    fg_adaptive_present_state before=*state;
    fg_adaptive_present_result result=fg_adaptive_present_decide(state,job,c);
    CHECK(!memcmp(state,&before,sizeof before),"waiting, rejection and eligibility decisions never consume a publication slot");
    return result;
}
static void early_deadlines(void) {
    const uint64_t refreshes[]={UINT64_C(16666667),UINT64_C(8333333)};
    for (unsigned i=0;i<2;i++) {
        fg_adaptive_present_state state={0};
        fg_adaptive_present_job job=generated(second+refreshes[i]);
        uint64_t deadline=fg_adaptive_present_deadline(job.slot);
        CHECK(deadline==job.slot-UINT64_C(2000000),"commit deadline is an absolute time two ms before the target");
        CHECK(deadline>second && deadline<job.slot,"both 60Hz and120Hz wake before, not at, their target refresh");
        CHECK(decide_unchanged(&state,job,context(second))==FG_ADAPTIVE_PRESENT_WAIT,"comfortably early candidate waits for its pre-slot deadline");
        CHECK(decide_unchanged(&state,job,context(deadline-1))==FG_ADAPTIVE_PRESENT_WAIT,"spurious and early queue wakes do not hand off before the commit deadline");
        CHECK(decide_unchanged(&state,job,context(deadline))==FG_ADAPTIVE_PRESENT_APPLY,"deadline wake admits the generated candidate instead of systematically expiring it");
        CHECK(decide_unchanged(&state,job,context(job.slot-1))==FG_ADAPTIVE_PRESENT_APPLY,"last pre-refresh nanosecond is still eligible without a display callback");
        CHECK(decide_unchanged(&state,job,context(job.slot))==FG_ADAPTIVE_PRESENT_EXPIRED,"a genuine target-refresh wake is too late for generated publication");
    }
}
static void real_during_wait_and_drain(void) {
    fg_adaptive_present_state state={0};
    fg_adaptive_present_job job=generated(second+UINT64_C(16666667));
    uint64_t deadline=fg_adaptive_present_deadline(job.slot);
    CHECK(decide_unchanged(&state,job,context(second))==FG_ADAPTIVE_PRESENT_WAIT,"candidate begins waiting with no replacement real");
    fg_adaptive_present_context wake=context(second+UINT64_C(1000000));
    wake.queued_real=1;
    CHECK(decide_unchanged(&state,job,wake)==FG_ADAPTIVE_PRESENT_YIELD,"earlier newly queued real returns the still-owned child to arbitration, even before its fence is ready");
    wake.ready_real=1;
    CHECK(decide_unchanged(&state,job,wake)==FG_ADAPTIVE_PRESENT_SUPPRESSED,"a ready newer real arriving during the wait cancels the generated candidate immediately");
    CHECK(decide_unchanged(&state,job,context(deadline))==FG_ADAPTIVE_PRESENT_APPLY,"initial eligibility before a blocking transaction drain does not commit anything");
    fg_adaptive_present_context after_drain=context(deadline+UINT64_C(100000));
    after_drain.ready_real=after_drain.queued_real=1;
    CHECK(decide_unchanged(&state,job,after_drain)==FG_ADAPTIVE_PRESENT_SUPPRESSED,"real-ready is checked again after the blocking drain and immediately before apply");
    CHECK(!state.last_slot && !state.latest_real,"canceled staged candidate does not consume the replacement real's slot or frame");
    fg_adaptive_present_job real=job; real.generated=0; real.frame++;
    CHECK(decide_unchanged(&state,real,after_drain)==FG_ADAPTIVE_PRESENT_APPLY,"replacement real for exactly the same target remains publishable");
    fg_adaptive_present_committed(&state,real);
    CHECK(state.last_slot==real.slot && state.latest_real==real.frame,"only actual real handoff advances committed slot and latest-real identity");
    CHECK(decide_unchanged(&state,job,context(deadline+UINT64_C(200000)))==FG_ADAPTIVE_PRESENT_SUPPRESSED,"old pair cannot publish after the newer real's actual handoff");
}
static void epoch_and_window_changes(void) {
    fg_adaptive_present_state state={.generation=7,.last_slot=second-1,.latest_real=8};
    fg_adaptive_present_job job=generated(second+UINT64_C(8333333));
    fg_adaptive_present_context c=context(second);
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_WAIT,"epoch/window case starts with an early candidate");
    c.epoch++;
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_SUPPRESSED,"scene epoch change while waiting cancels the candidate");
    c=context(fg_adaptive_present_deadline(job.slot));
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_APPLY,"eligible before transaction drain");
    c.epoch++;
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_SUPPRESSED,"epoch change during transaction drain cancels at the final guard");
    c=context(fg_adaptive_present_deadline(job.slot)); c.suppressed=1;
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_SUPPRESSED,"producer/common-cancel suppression stays effective through final apply guard");
    c=context(fg_adaptive_present_deadline(job.slot)); c.window=0;
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_WINDOW,"window detach wakes and cancels a waiting or prepared candidate");
    c.window=1; c.generation++;
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_WINDOW,"replacement window generation cannot receive a staged old-window candidate");
    CHECK(state.generation==7 && state.last_slot==second-1 && state.latest_real==8,"refused new-generation preparation cannot reset committed history");
    job.generation=c.generation; c.epoch=job.epoch;
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_APPLY,"a candidate for the live replacement generation can be handed off");
    fg_adaptive_present_committed(&state,job);
    CHECK(state.generation==job.generation && state.last_slot==job.slot && !state.latest_real,"actual generation handoff alone resets the prior generation's publication history");
}
static void order_and_retirement_separation(void) {
    fg_adaptive_present_state state={0};
    fg_adaptive_present_job job=generated(second+UINT64_C(16666667));
    fg_adaptive_present_context c=context(fg_adaptive_present_deadline(job.slot));
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_APPLY,"candidate eligibility does not itself publish");
    CHECK(!state.last_slot,"preparation/drain/failed platform handoff leaves no committed watermark");
    fg_adaptive_present_committed(&state,job);
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_ORDER,"a committed slot cannot be handed off twice");
    fg_adaptive_present_job older=job; older.slot--;
    CHECK(decide_unchanged(&state,older,c)==FG_ADAPTIVE_PRESENT_ORDER,"older targets cannot reorder committed output");
    fg_adaptive_present_job next=job; next.slot+=UINT64_C(8333333);
    c.now=fg_adaptive_present_deadline(next.slot);
    CHECK(decide_unchanged(&state,next,c)==FG_ADAPTIVE_PRESENT_APPLY,"strictly increasing generated slot remains eligible");
    fg_adaptive_present_committed(&state,next);
    fg_adaptive_present_job real=next; real.generated=0; real.frame=20; real.slot+=UINT64_C(8333333);
    c.now=real.slot+UINT64_C(1000000);
    CHECK(decide_unchanged(&state,real,c)==FG_ADAPTIVE_PRESENT_APPLY,"late real handoff is retained, unlike expired prediction");
    fg_adaptive_present_committed(&state,real);
    job.slot=real.slot+UINT64_C(8333333); c.now=fg_adaptive_present_deadline(job.slot);
    CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_SUPPRESSED,"later slot does not revive an older pair after a fresh real");
    /* Arbitration state has no fence/owner/busy flag. Actual delayed-GPU
     * retirement is exercised by the production APIs in vkreplaypintest. */
    c.suppressed=1;
    for (unsigned i=0;i<8;i++)
        CHECK(decide_unchanged(&state,job,c)==FG_ADAPTIVE_PRESENT_SUPPRESSED,
            "repeated cancellation decisions leave already committed order unchanged");
}
static void fractional_gpu_deadlines(void) {
    const uint64_t refreshes[]={UINT64_C(16666667),UINT64_C(8333333)};
    const uint64_t intervals[]={UINT64_C(24000000),UINT64_C(25000000),UINT64_C(27000000)};
    for (unsigned r=0;r<2;r++) for (unsigned i=0;i<3;i++) {
        fg_adaptive_policy p={0};
        fg_adaptive_observe(&p,(fg_adaptive_stamp){second,second,1,1},refreshes[r]);
        CHECK(fg_adaptive_observe(&p,(fg_adaptive_stamp){second+intervals[i],
            second+intervals[i],1,1},refreshes[r])==FG_ADAPTIVE_ACTIVE,"irregular approximately40fps source is active");
        const uint64_t real=p.display_anchor_ns;
        for (uint64_t slot=real+refreshes[r];slot-real<=intervals[i];slot+=refreshes[r]) {
            float phase=0;
            CHECK(fg_adaptive_phase(&p,slot,0,&phase) && phase>0 && phase<=1,
                "deadline belongs to an actual fractional game-time slot");
            uint64_t source=real+UINT64_C(1000000),output=real+UINT64_C(4000000);
            uint64_t now=fg_adaptive_present_deadline(slot);
            CHECK(!framegen_child_ready(source,real,slot,now),"legacy locked source-slot contract would reject this late real");
            CHECK(fg_adaptive_child_status(source,output,slot,now)==FRAMEGEN_CHILD_READY,
                "adaptive accepts source after real slot when both tickets and presenter beat child target");
            CHECK(fg_adaptive_child_status(slot,output,slot,now)==FRAMEGEN_CHILD_SOURCE_LATE,
                "source completing exactly at child target remains too late");
            CHECK(fg_adaptive_child_status(source,slot,slot,now)==FRAMEGEN_CHILD_SOURCE_LATE,
                "output completing exactly at child target remains too late");
            CHECK(fg_adaptive_child_status(source,output,slot,slot)==FRAMEGEN_CHILD_DEADLINE,
                "presenter at target remains expired; no deadline threshold relaxed");
            CHECK(fg_adaptive_child_status(0,output,slot,now)==FRAMEGEN_CHILD_SOURCE_MISSING &&
                fg_adaptive_child_status(source,0,slot,now)==FRAMEGEN_CHILD_SOURCE_MISSING,
                "missing ticket publication never becomes ready");
        }
    }
}
static void job_trace_authority(void) {
    fg_job_trace t={.frame=7,.submit=second,.pickup=second+1,.source_ready=second+2,
        .output_ready=second+3,.target=second+10,.decision=second+9};
    CHECK(!strcmp(fg_job_trace_class(t),"before-target"),"early observations are not a miss");
    t.decision=t.target;
    CHECK(!strcmp(fg_job_trace_class(t),"presenter-drain-or-deschedule-late"),"both ticket publications predate target: presenter lateness proven");
    t.output_ready=t.target;
    CHECK(!strcmp(fg_job_trace_class(t),"fence-or-cpu-publication-late-gpu-cause-unknown"),"late CPU publication is not mislabeled hardware GPU lateness");
    t.source_ready=0;
    CHECK(!strcmp(fg_job_trace_class(t),"ready-unavailable"),"missing timestamp remains unknown");
    char line[640];
    int n=fg_job_trace_format(line,sizeof line,t,"adaptive","deadline");
    CHECK(n>0 && (size_t)n<sizeof line && strstr(line,"submit-request-ns=") &&
        strstr(line,"pickup-ns=") && strstr(line,"target-ns=") &&
        strstr(line,"not-hardware-signal"),"production formatter retains all clock authority labels");
}
static void slot_job_trace_format(void) {
    static const struct { fg_slot_cancel_reason reason; const char *name; } reasons[]={
        {FG_SLOT_CANCEL_NONE,"none"},
        {FG_SLOT_CANCEL_MODE,"mode"},
        {FG_SLOT_CANCEL_OWNER,"owner"},
        {FG_SLOT_CANCEL_WINDOW,"window"},
        {FG_SLOT_CANCEL_EPOCH,"epoch"},
        {FG_SLOT_CANCEL_SNAPSHOT_PRESSURE,"snapshot-pressure"},
        {FG_SLOT_CANCEL_EMPTY,"empty"},
        {FG_SLOT_CANCEL_NO_HOLE,"no-hole"},
        {FG_SLOT_CANCEL_POLICY,"policy"},
        {FG_SLOT_CANCEL_DEADLINE,"deadline"},
        {FG_SLOT_CANCEL_CLOCK,"clock"},
        {FG_SLOT_CANCEL_PHASE,"phase"},
        {FG_SLOT_CANCEL_DISPATCH_REFUSED,"dispatch-refused"}
    };
    char line[512];
    for (unsigned i=0;i<sizeof reasons/sizeof reasons[0];i++) {
        CHECK(!strcmp(fg_slot_cancel_reason_name(reasons[i].reason),reasons[i].name),
            "every slot cancellation enum has its stable reason name");
        fg_slot_job_trace t={
            .frame=11,.token=22,.target=33,.boundary=44,
            .reason=(unsigned)reasons[i].reason,.generation_state=i,
            .backend_reason=100+i,.pressure_observations=200+i,
            .pressure_cancellations=300+i,.pressure_appends=400+i
        };
        int n=fg_slot_job_trace_format(line,sizeof line,t);
        char expected[64];
        CHECK(n>0 && (size_t)n<sizeof line && !strncmp(line,"[adaptive-slot-job] ",20),
            "slot formatter emits a complete adaptive-slot-job record");
        CHECK(strstr(line,"frame=11 ") && strstr(line,"token=22 ") &&
            strstr(line,"target=33 ") && strstr(line,"boundary=44 "),
            "slot formatter retains frame, token, target, and boundary");
        snprintf(expected,sizeof expected,"reason=%s ",reasons[i].name);
        CHECK(strstr(line,expected)!=NULL,"slot formatter uses every exact reason name");
        snprintf(expected,sizeof expected,"generation-state=%u ",i);
        CHECK(strstr(line,expected)!=NULL,"slot formatter preserves numeric generation state");
        snprintf(expected,sizeof expected,"backend-reason=%u ",100+i);
        CHECK(strstr(line,expected)!=NULL,"slot formatter preserves backend reason");
        snprintf(expected,sizeof expected,"pressure-observations-lifetime-cpu=%u ",200+i);
        CHECK(strstr(line,expected)!=NULL,"observation counter is labelled lifetime CPU data");
        snprintf(expected,sizeof expected,"pressure-cancellations-lifetime-cpu=%u ",300+i);
        CHECK(strstr(line,expected)!=NULL,"cancellation counter is labelled lifetime CPU data");
        snprintf(expected,sizeof expected,"pressure-appends-lifetime-cpu=%u",400+i);
        CHECK(strstr(line,expected)!=NULL,"append counter is labelled lifetime CPU data");
    }
    CHECK(!strcmp(fg_slot_cancel_reason_name((fg_slot_cancel_reason)UINT32_MAX),"unknown"),
        "unknown cancellation enum has a deterministic name");
    fg_slot_job_trace unknown={.reason=UINT32_MAX};
    int n=fg_slot_job_trace_format(line,sizeof line,unknown);
    CHECK(n>0 && strstr(line,"reason=unknown "),
        "formatter preserves unknown cancellation enum safely");

    fg_slot_job_trace t={.frame=1,.reason=FG_SLOT_CANCEL_PHASE};
    char full[512],short_line[12];
    int full_n=fg_slot_job_trace_format(full,sizeof full,t);
    int short_n=fg_slot_job_trace_format(short_line,sizeof short_line,t);
    CHECK(full_n>=(int)sizeof short_line && short_n==full_n,
        "truncated formatter reports the complete required length");
    CHECK(short_line[sizeof short_line-1]=='\0' &&
        !strncmp(short_line,full,sizeof short_line-1),
        "truncated formatter remains NUL-terminated and preserves its prefix");
    CHECK(fg_slot_job_trace_format(NULL,0,t)==full_n,
        "zero-capacity formatter reports required length without writing");
}
static uint64_t benchmark_clock(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static void trace_benchmark(void) {
    enum { count=200000 };
    FILE *sink=fopen("/dev/null","w");
    CHECK(sink!=NULL,"host benchmark log sink opens");
    char line[640];
    fg_job_trace t={7,second,second+1,second+2,second+3,second+10,second+11};
    unsigned long bytes=0;
    uint64_t begin=benchmark_clock();
    for (unsigned i=0;i<count;i++) {
        t.frame=i;
        int n=fg_job_trace_format(line,sizeof line,t,"adaptive","deadline");
        bytes+=(unsigned)n;
        fprintf(sink,"%s\n",line);
    }
    fflush(sink);
    uint64_t elapsed=benchmark_clock()-begin;
    fclose(sink);
    printf("job trace HOST formatter+stdio-/dev/null: jobs=%u bytes=%lu elapsed-ns=%llu ns/job=%.3f; NOT Android-logcat/device overhead\n",
        count,bytes,(unsigned long long)elapsed,(double)elapsed/count);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--trace-benchmark")) { trace_benchmark(); return 0; }
    early_deadlines(); real_during_wait_and_drain(); epoch_and_window_changes(); order_and_retirement_separation();
    fractional_gpu_deadlines(); job_trace_authority(); slot_job_trace_format();
    printf("adaptive presenter: %u checks passed (absolute pre-slot deadlines, wait/drain cancellation, committed-only order)\n",checks);
    return 0;
}
