/* Host-only production-policy tests and timestamp-row replay. No Android/GPU,
 * sleeping, system clocks, or resource retirement in the scheduler.
 * cc -std=c11 -O2 -Wall -Wextra -Werror test_framegen_adaptive_slots.c -lm -o <test>
 * <test>
 * <test> --replay [period_ns margin_ns readiness_ns [start_ns end_ns]] < rows
 * Rows: publication_ns target_ns source_frame sequence [eligible=1]. Sorted by
 * publication, then sequence; REAL wins exact-time ties. First target fixes the
 * lattice. JSONL output is occupancy OPPORTUNITIES, not retained game phases or
 * a compositor/GPU prediction. Optional bounds filter the final summary only. */
#include "framegen_adaptive_slots.h"
#include "framegen_adaptive.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr,"adaptive slots: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)
static const uint64_t second=UINT64_C(1000000000);
static const uint64_t hz60=UINT64_C(16666666), hz120=UINT64_C(8333333);
static const uint64_t margin=UINT64_C(2000000);

typedef struct {
    uint64_t publication_ns, target_ns, frame, sequence;
    int eligible;
} real_row;
typedef struct {
    uint64_t real_slots, generated_slots, started_jobs, early_cancels, late_jobs;
    uint64_t other_cancels, real_displacements, deadline_misses, target_misses;
} replay_stats;
typedef struct {
    uint64_t now_ns, frame, slot;
    unsigned generated;
} trace_event;
typedef struct {
    trace_event events[2048];
    unsigned count;
} replay_trace;
typedef struct {
    fgs_state state;
    replay_stats all, bounded;
    uint64_t cost_ns, start_ns, end_ns;
    replay_trace *trace;
    int emit;
} replay_runner;

static int in_bounds(const replay_runner *r, uint64_t time) {
    return time>=r->start_ns && time<r->end_ns;
}
static void trace_commit(replay_runner *r, uint64_t now, uint64_t frame,
        uint64_t slot, unsigned generated) {
    if (!r->trace) return;
    CHECK(r->trace->count<sizeof r->trace->events/sizeof r->trace->events[0],
        "bounded synthetic trace has room");
    r->trace->events[r->trace->count++]=(trace_event){now,frame,slot,generated};
}
static void runner_init(replay_runner *r, uint64_t base, uint64_t period,
        uint64_t latch_margin, uint64_t cost, int emit, uint64_t start, uint64_t end,
        replay_trace *trace) {
    *r=(replay_runner){.cost_ns=cost,.start_ns=start,.end_ns=end,.trace=trace,.emit=emit};
    if (trace) trace->count=0;
    CHECK(fgs_init(&r->state,1,base,period,latch_margin)==FGS_OK,"replay clock initializes");
}
static void runner_cancel(replay_runner *r, fgs_cancel cancelled, uint64_t now) {
    const fgs_request *q=&cancelled.request;
    int bounded=in_bounds(r,q->publication_ns);
    const char *event;
    if (cancelled.reason==FGS_CANCEL_REAL) {
        event="early-real-cancel";
        r->all.early_cancels++;
        if (bounded) r->bounded.early_cancels++;
    } else if (cancelled.reason==FGS_CANCEL_LATE) {
        event="generation-late";
        r->all.late_jobs++;
        if (bounded) r->bounded.late_jobs++;
    } else {
        event="prediction-reset-cancel";
        r->all.other_cancels++;
        if (bounded) r->bounded.other_cancels++;
    }
    if (r->emit) printf("{\"event\":\"%s\",\"now_ns\":%" PRIu64
        ",\"frame\":%" PRIu64 ",\"slot\":%" PRIu64 ",\"token\":%" PRIu64
        ",\"publication_ns\":%" PRIu64 ",\"reason\":%u}\n",
        event,now,q->frame,q->slot.ordinal,q->token,q->publication_ns,(unsigned)cancelled.reason);
}
static void runner_real(replay_runner *r, real_row row) {
    fgs_real_plan plan;
    CHECK(fgs_peek_real(&r->state,row.publication_ns,&plan)==FGS_OK,"replay REAL first-free plan");
    fgs_batch batch;
    CHECK(fgs_commit_real(&r->state,plan.slot,row.publication_ns,row.frame,1,row.eligible,&batch)==FGS_OK,
        "replay executes production REAL commit");
    CHECK(batch.request_status==FGS_OK,"replay does not silently lose bounded requests");
    r->all.real_slots++;
    r->all.real_displacements+=plan.generated_conflicts;
    r->all.deadline_misses+=batch.deadline_missed;
    r->all.target_misses+=batch.target_missed;
    if (in_bounds(r,plan.slot.target_ns)) {
        r->bounded.real_slots++;
        r->bounded.real_displacements+=plan.generated_conflicts;
        r->bounded.deadline_misses+=batch.deadline_missed;
        r->bounded.target_misses+=batch.target_missed;
    }
    trace_commit(r,row.publication_ns,row.frame,plan.slot.ordinal,0);
    if (r->emit) printf("{\"event\":\"real-slot\",\"now_ns\":%" PRIu64
        ",\"frame\":%" PRIu64 ",\"sequence\":%" PRIu64 ",\"slot\":%" PRIu64
        ",\"target_ns\":%" PRIu64 ",\"generated_conflicts\":%u,\"deadline_missed\":%u}\n",
        row.publication_ns,row.frame,row.sequence,plan.slot.ordinal,plan.slot.target_ns,
        plan.generated_conflicts,batch.deadline_missed);
    for (unsigned i=0;i<batch.cancel_count;i++) runner_cancel(r,batch.cancelled[i],row.publication_ns);
    for (unsigned i=0;i<batch.request_count;i++) {
        fgs_request q=batch.requests[i];
        CHECK(q.real_slot.ordinal<q.slot.ordinal && q.slot.ordinal<q.predicted_next_ordinal,
            "production request is strictly inside its causal predicted interval");
        r->all.started_jobs++;
        if (in_bounds(r,q.publication_ns)) r->bounded.started_jobs++;
        uint64_t ready;
        int overflow=!fgs_add_ns(q.publication_ns,r->cost_ns,&ready);
        if (overflow) ready=UINT64_MAX;
        if (r->emit) printf("{\"event\":\"generated-request\",\"now_ns\":%" PRIu64
            ",\"frame\":%" PRIu64 ",\"slot\":%" PRIu64 ",\"token\":%" PRIu64
            ",\"target_ns\":%" PRIu64 ",\"real_slot\":%" PRIu64
            ",\"real_target_ns\":%" PRIu64 ",\"publication_ns\":%" PRIu64
            ",\"ema_ns\":%" PRIu64 ",\"predicted_next_slot\":%" PRIu64
            ",\"ready_ns\":%" PRIu64 ",\"commit_ns\":%" PRIu64
            ",\"phase_status\":\"UNKNOWN_RETAINED_GAME_ANCHORS\"}\n",
            row.publication_ns,q.frame,q.slot.ordinal,q.token,q.slot.target_ns,
            q.real_slot.ordinal,q.real_slot.target_ns,q.publication_ns,q.ewma_ns,
            q.predicted_next_ordinal,ready,q.deadline_ns);
        fgs_cancel cancelled;
        fgs_status status=overflow ? fgs_cancel_gen(&r->state,q.token,FGS_CANCEL_LATE,&cancelled) :
            fgs_mark_ready(&r->state,q.token,ready,&cancelled);
        if (status==FGS_LATE || overflow) runner_cancel(r,cancelled,row.publication_ns);
        else CHECK(status==FGS_OK,"replay marks production reservation ready");
    }
}
/* Only the event pump is a fixture: prediction, reservation, collision,
 * cancellation, readiness and commit all execute the real header above. */
static void runner_due(replay_runner *r, uint64_t until, int inclusive) {
    for (;;) {
        const fgs_request *next=NULL;
        for (unsigned i=0;i<FGS_PENDING_CAP;i++) {
            const fgs_request *q=&r->state.pending[i];
            if (!q->token || q->deadline_ns>until || (!inclusive && q->deadline_ns==until)) continue;
            if (!next || q->deadline_ns<next->deadline_ns ||
                    (q->deadline_ns==next->deadline_ns && q->token<next->token)) next=q;
        }
        if (!next) return;
        fgs_request q=*next, admitted;
        CHECK(fgs_admit_gen(&r->state,q.token,q.deadline_ns,NULL,&admitted)==FGS_OK,
            "replay executes production GEN admission at ideal cutoff");
        fgs_gen_commit committed;
        CHECK(fgs_commit_gen(&r->state,q.token,q.deadline_ns,&committed)==FGS_OK,
            "replay executes production GEN commit");
        r->all.generated_slots++;
        if (in_bounds(r,q.slot.target_ns)) r->bounded.generated_slots++;
        trace_commit(r,q.deadline_ns,q.frame,q.slot.ordinal,1);
        if (r->emit) printf("{\"event\":\"generated-commit\",\"now_ns\":%" PRIu64
            ",\"frame\":%" PRIu64 ",\"slot\":%" PRIu64 ",\"token\":%" PRIu64
            ",\"target_ns\":%" PRIu64 ",\"publication_ns\":%" PRIu64 "}\n",
            q.deadline_ns,q.frame,q.slot.ordinal,q.token,q.slot.target_ns,q.publication_ns);
    }
}
static replay_stats fixture_run(const real_row *rows, unsigned count,
        uint64_t period, uint64_t latch_margin, uint64_t cost, replay_trace *trace) {
    CHECK(count>0,"synthetic fixture has a first known lattice anchor");
    replay_runner r;
    runner_init(&r,rows[0].target_ns,period,latch_margin,cost,0,0,UINT64_MAX,trace);
    for (unsigned i=0;i<count;i++) {
        if (i) CHECK(rows[i].publication_ns>=rows[i-1].publication_ns,"fixture rows are chronological");
        runner_due(&r,rows[i].publication_ns,0);
        runner_real(&r,rows[i]);
    }
    runner_due(&r,UINT64_MAX,1);
    CHECK(!r.state.pending_count,"all fixture opportunities resolve without GPU ownership assumptions");
    CHECK(r.all.started_jobs==r.all.generated_slots+r.all.early_cancels+
        r.all.late_jobs+r.all.other_cancels,"all started opportunities have exactly one policy outcome");
    return r.all;
}

static fgs_batch commit_real(fgs_state *s, uint64_t now, uint64_t frame,
        uint64_t epoch, int eligible) {
    fgs_real_plan plan;
    fgs_state before=*s;
    CHECK(fgs_peek_real(s,now,&plan)==FGS_OK,"REAL peek succeeds");
    CHECK(!memcmp(&before,s,sizeof before),"REAL peek has no slot/cadence/token side effects");
    fgs_batch batch;
    CHECK(fgs_commit_real(s,plan.slot,now,frame,epoch,eligible,&batch)==FGS_OK,"actual REAL commit succeeds");
    return batch;
}
static void cancel_requests(fgs_state *s, const fgs_batch *batch) {
    for (unsigned i=0;i<batch->request_count;i++) {
        fgs_cancel cancelled;
        CHECK(fgs_cancel_gen(s,batch->requests[i].token,FGS_CANCEL_OWNER,&cancelled)==FGS_OK,
            "owner can decline a request without consuming its slot");
    }
}
static fgs_batch fresh_requests(fgs_state *s, uint64_t period, uint64_t interval) {
    CHECK(fgs_init(s,7,second+period,period,margin)==FGS_OK,"fresh request lattice");
    for (unsigned i=0;i<40;i++) {
        fgs_batch batch=commit_real(s,second+i*interval,i+1,91,1);
        CHECK(batch.request_status==FGS_OK,"normal fixture request arithmetic fits");
        if (i<FGS_WARMUP_INTERVALS) CHECK(!batch.request_count,"minimum twelve past intervals, not frames");
        if (batch.request_count) return batch;
    }
    CHECK(0,"eligible stable cadence produces an actual request");
    return (fgs_batch){0};
}
static void first_free_and_peek(void) {
    fgs_state s;
    CHECK(fgs_init(&s,3,second,hz60,margin)==FGS_OK,"clock initializes");
    fgs_state before=s;
    fgs_real_plan first, repeated;
    CHECK(fgs_peek_real(&s,second-margin,&first)==FGS_OK && first.slot.ordinal==0,
        "REAL exactly at cutoff takes base slot");
    CHECK(fgs_peek_real(&s,second-margin,&repeated)==FGS_OK && repeated.slot.ordinal==0,
        "abandoned staging does not reserve a slot");
    CHECK(!memcmp(&s,&before,sizeof s),"repeated peeks leave all state untouched");
    fgs_batch batch;
    CHECK(fgs_commit_real(&s,first.slot,second-margin+1,44,12,1,&batch)==FGS_OK,
        "post-apply cutoff overrun records the assigned slot");
    CHECK(batch.deadline_missed && !batch.target_missed,"late actual apply is diagnostic, not retargeted");
    CHECK(s.samples==0 && s.last_real.ordinal==0,"only actual apply starts cadence");
    CHECK(fgs_peek_real(&s,second-margin,&repeated)==FGS_OK && repeated.slot.ordinal==1,
        "REAL first-free skips committed REAL even in a burst");
    before=s;
    CHECK(fgs_commit_real(&s,first.slot,second,45,12,1,&batch)==FGS_DUPLICATE,
        "duplicate actual physical slot is refused");
    CHECK(!memcmp(&s,&before,sizeof s),"duplicate never trains publication EWMA");
    fgs_slot wrong=repeated.slot;
    wrong.target_ns++;
    CHECK(fgs_commit_real(&s,wrong,second,45,12,1,&batch)==FGS_INVALID,
        "rounded target cannot masquerade as the ordinal's exact lattice target");
    fgs_slot future=repeated.slot;
    for (unsigned i=0;i<FGS_HISTORY_CAP;i++) {
        batch=commit_real(&s,second,100+i,12,1);
        CHECK(!batch.request_count,"nonpositive publication intervals reset rather than generate");
    }
    CHECK(s.sealed_before<=future.ordinal,"future burst slots never age merely because newer ordinals exist");
    CHECK(fgs_peek_real(&s,second,&repeated)==FGS_CAPACITY,
        "future committed-slot capacity is explicit rather than silently discarding reservations");
    batch=commit_real(&s,second+200*hz60,1000,12,1);
    CHECK(s.high_ordinal>=200 && s.sealed_before>future.ordinal,"actual time advances the finite metadata window");
    CHECK(fgs_validate_slot(&s,first.slot)==FGS_STALE,"aged physical slots remain permanently sealed");
    CHECK(fgs_peek_real(&s,second-margin,&repeated)==FGS_OK && repeated.slot.ordinal>s.high_ordinal,
        "a backward clock cannot make an aged committed slot free again");
}
static void ewma_modes_and_resets(void) {
    fgs_state s;
    fgs_batch batch=fresh_requests(&s,hz60,UINT64_C(25000000));
    CHECK(s.samples==FGS_WARMUP_INTERVALS && s.ewma_ns==UINT64_C(25000000),
        "constant cadence measures twelve already observed intervals");
    cancel_requests(&s,&batch);
    uint64_t now=s.last_publication_ns+UINT64_C(33000000);
    batch=commit_real(&s,now,100,91,1);
    CHECK(s.ewma_ns==UINT64_C(26000000),"EWMA uses exact integer seven-eighths past plus one-eighth new");
    cancel_requests(&s,&batch);
    batch=commit_real(&s,now+FGS_RESET_GAP_NS,101,91,1);
    CHECK(s.samples==FGS_WARMUP_INTERVALS && batch.mode==FGS_MODE_FLOOR,
        "exact250ms is observed but floor-suspended, not a greater-than250ms reset");
    cancel_requests(&s,&batch);
    now=s.last_publication_ns+FGS_RESET_GAP_NS+1;
    batch=commit_real(&s,now,102,91,1);
    CHECK(batch.mode==FGS_MODE_GAP && !s.samples && !s.ewma_ns,"long gap clears publication evidence");
    batch=commit_real(&s,now,103,91,1);
    CHECK(batch.mode==FGS_MODE_CLOCK && !s.samples,"nonpositive publication interval resets");
    batch=commit_real(&s,now-1,104,91,1);
    CHECK(batch.mode==FGS_MODE_CLOCK && !s.samples,"reversed publication interval resets");
    batch=commit_real(&s,now+UINT64_C(25000000),105,92,1);
    CHECK(batch.mode==FGS_MODE_SCENE && !s.samples,"immutable source epoch change resets history");
    batch=commit_real(&s,now+UINT64_C(50000000),106,92,0);
    CHECK(batch.mode==FGS_MODE_SCENE && !s.samples,"ineligible scene suppresses synthesis");
    batch=commit_real(&s,now+UINT64_C(75000000),107,92,1);
    CHECK(batch.mode==FGS_MODE_WARMUP && s.samples==1,"recovery rewarms from eligible past publication");
    const uint64_t intervals[]={hz60,hz60-1,UINT64_C(40000000),UINT64_C(40000001)};
    const fgs_mode modes[]={FGS_MODE_PASSTHROUGH,FGS_MODE_PASSTHROUGH,FGS_MODE_ACTIVE,FGS_MODE_FLOOR};
    for (unsigned k=0;k<4;k++) {
        CHECK(fgs_init(&s,4,second+hz60,hz60,margin)==FGS_OK,"mode fixture reset");
        for (unsigned i=0;i<=FGS_WARMUP_INTERVALS;i++) {
            batch=commit_real(&s,second+i*intervals[k],i+1,1,1);
            cancel_requests(&s,&batch);
        }
        CHECK(batch.mode==modes[k],"display cadence passes through and exact25fps floor remains eligible");
    }
}
static void pending_discontinuities(void) {
    const fgs_cancel_reason reasons[]={FGS_CANCEL_SCENE,FGS_CANCEL_SCENE,FGS_CANCEL_GAP,FGS_CANCEL_CLOCK};
    const fgs_mode modes[]={FGS_MODE_SCENE,FGS_MODE_SCENE,FGS_MODE_GAP,FGS_MODE_CLOCK};
    for (unsigned change=0;change<4;change++) {
        fgs_state s;
        fgs_batch source=fresh_requests(&s,hz120,UINT64_C(40000000));
        uint64_t now=s.last_publication_ns;
        if (change<2) now++;
        if (change==2) now+=FGS_RESET_GAP_NS+1;
        fgs_batch batch=commit_real(&s,now,1000,change==1?92:91,change!=0);
        CHECK(batch.mode==modes[change] && !s.samples && !s.ewma_ns && !batch.request_count,
            "scene/epoch/gap/nonpositive discontinuity resets all publication prediction");
        CHECK(batch.cancel_count==source.request_count && !s.pending_count,
            "discontinuity returns every still-pending child, not just an exact collision");
        for (unsigned i=0;i<batch.cancel_count;i++) {
            CHECK(batch.cancelled[i].reason==reasons[change] &&
                batch.cancelled[i].request.token==source.requests[i].token,
                "discontinuity retains each immutable cancellation identity and reason");
        }
    }
}
static void real_ties_and_children(void) {
    const uint64_t periods[]={hz60,hz120};
    for (unsigned k=0;k<2;k++) {
        fgs_state s;
        fgs_batch source=fresh_requests(&s,periods[k],k?UINT64_C(40000000):UINT64_C(25000000));
        CHECK(source.request_count>=(k?3u:1u),"120Hz offers multiple immutable children within source horizon");
        fgs_request first=source.requests[0], admitted;
        fgs_cancel cancelled;
        CHECK(fgs_mark_ready(&s,first.token,first.publication_ns,&cancelled)==FGS_OK,"tie child ready");
        fgs_real_plan real;
        CHECK(fgs_peek_real(&s,first.deadline_ns,&real)==FGS_OK &&
            real.slot.ordinal==first.slot.ordinal,"provisional GEN is first-free to REAL at exact cutoff");
        CHECK(fgs_admit_gen(&s,first.token,first.deadline_ns,&real.slot,&admitted)==FGS_REAL_COLLISION,
            "known ready REAL wins immediate pre-apply admission");
        fgs_batch batch;
        CHECK(fgs_commit_real(&s,real.slot,first.deadline_ns,10000,91,1,&batch)==FGS_OK,
            "REAL at ideal cutoff is processed first");
        CHECK(batch.cancel_count==source.request_count,"new REAL cancels every older child at or after its slot");
        for (unsigned i=0;i<source.request_count;i++) {
            CHECK(batch.cancelled[i].reason==FGS_CANCEL_REAL,"tie cancellation enumerates scalar request only");
            CHECK(fgs_admit_gen(&s,source.requests[i].token,first.deadline_ns,NULL,&admitted)==FGS_NOT_FOUND,
                "cancelled child cannot publish");
        }
        CHECK(fgs_slot_kind(&s,real.slot.ordinal)==FGS_SLOT_REAL,"committed tie slot belongs only to REAL");
        cancel_requests(&s,&batch);
    }
}
static void readiness_and_irrevocable_apply(void) {
    fgs_state s;
    fgs_batch source=fresh_requests(&s,hz60,UINT64_C(25000000));
    fgs_request q=source.requests[0], admitted;
    fgs_cancel cancelled;
    CHECK(fgs_admit_gen(&s,q.token,q.deadline_ns,NULL,&admitted)==FGS_NOT_READY,"GPU-unready output cannot apply");
    CHECK(fgs_mark_ready(&s,q.token,q.publication_ns-1,&cancelled)==FGS_INVALID,"completion cannot precede request");
    CHECK(fgs_mark_ready(&s,q.token,q.deadline_ns+1,&cancelled)==FGS_LATE &&
        cancelled.request.token==q.token && cancelled.reason==FGS_CANCEL_LATE,
        "late completion returns one cancellation and never consumes its target");
    CHECK(fgs_slot_kind(&s,q.slot.ordinal)==FGS_SLOT_EMPTY,"late ready is not a phantom commit");
    CHECK(fgs_cancel_gen(&s,q.token,FGS_CANCEL_OWNER,&cancelled)==FGS_NOT_FOUND,"cancellation is one policy outcome");
    source=fresh_requests(&s,hz60,UINT64_C(25000000)); q=source.requests[0];
    CHECK(fgs_mark_ready(&s,q.token,q.deadline_ns,&cancelled)==FGS_OK,"completion at cutoff is admissible");
    CHECK(fgs_mark_ready(&s,q.token,q.deadline_ns,&cancelled)==FGS_OK,"same immutable ready notification is idempotent");
    CHECK(fgs_mark_ready(&s,q.token,q.deadline_ns-1,&cancelled)==FGS_INVALID,"readiness timestamp cannot be rewritten");
    fgs_state before=s;
    CHECK(fgs_admit_gen(&s,q.token,q.deadline_ns-1,NULL,&admitted)==FGS_WAIT,"pre-cutoff output waits");
    CHECK(fgs_admit_gen(&s,q.token,q.slot.target_ns,NULL,&admitted)==FGS_LATE,"missed display target is rejected before apply");
    CHECK(!memcmp(&s,&before,sizeof s),"admission decisions do not commit, cancel, train or free");
    fgs_slot wrong=q.slot; wrong.generation++;
    CHECK(fgs_admit_gen(&s,q.token,q.deadline_ns,&wrong,&admitted)==FGS_STALE,"stale known-real clock cannot admit a child");
    CHECK(fgs_admit_gen(&s,q.token,q.deadline_ns,NULL,&admitted)==FGS_OK,"ideal-cutoff child admits");
    fgs_gen_commit committed;
    CHECK(fgs_commit_gen(&s,q.token,q.slot.target_ns+1,&committed)==FGS_OK,
        "post-apply timing miss is recorded, never fabricated as retractable");
    CHECK(committed.deadline_missed && committed.target_missed && !committed.not_ready,
        "actual cutoff/target overrun diagnostics survive commit");
    CHECK(fgs_slot_kind(&s,q.slot.ordinal)==FGS_SLOT_GENERATED,"actual applied generation still owns slot");
    CHECK(fgs_commit_gen(&s,q.token,q.slot.target_ns+2,&committed)==FGS_NOT_FOUND,"GEN cannot commit twice");
    fgs_real_plan real;
    CHECK(fgs_peek_real(&s,q.deadline_ns,&real)==FGS_OK && real.generated_conflicts==1 &&
        real.slot.ordinal>q.slot.ordinal,"irrevocable GEN collision is counted and REAL takes genuinely free slot");
}
static void lifecycle_and_external_ownership(void) {
    /* External leases deliberately never enter policy storage. Cancels merely
     * identify work; only this fixture's explicit completed-work loop retires. */
    for (unsigned transition=0;transition<5;transition++) {
        fgs_state s;
        fgs_batch source=fresh_requests(&s,hz120,UINT64_C(40000000));
        struct { uint64_t token; unsigned live, gpu_complete; } owned[FGS_MAX_REQUESTS]={0};
        unsigned live=source.request_count, retired=0;
        for (unsigned i=0;i<source.request_count;i++)
            owned[i].token=source.requests[i].token,owned[i].live=1;
        fgs_slot old=source.requests[0].slot;
        uint64_t token=s.next_token;
        fgs_batch cancelled;
        fgs_status status;
        if (transition==1 || transition==4) status=fgs_end(&s,&cancelled); /* disable/shutdown */
        else status=fgs_reset(&s,transition==2?8:7,
            second+hz120+(transition==3?1:0),transition==0?hz60:hz120,margin,&cancelled);
        CHECK(status==FGS_OK,"refresh/disable/window/clock/shutdown transition completes policy reset");
        CHECK(cancelled.cancel_count==source.request_count && !s.pending_count,
            "lifecycle transition enumerates every pending cancellation without truncation");
        CHECK(live==source.request_count && !retired,"policy reset never retires externally owned storage");
        for (unsigned i=0;i<source.request_count;i++) {
            CHECK(cancelled.cancelled[i].request.token==owned[i].token && owned[i].live && !owned[i].gpu_complete,
                "pending cancellation retains immutable ownership token while GPU may still run");
            owned[i].gpu_complete=1;
            CHECK(owned[i].live && owned[i].gpu_complete,"owner waits for all covering work before retirement");
            owned[i].live=0; live--; retired++;
        }
        CHECK(!live && retired==source.request_count,"only explicit owner completion retires each lease once");
        fgs_request admitted;
        CHECK(fgs_admit_gen(&s,source.requests[0].token,old.target_ns,NULL,&admitted)!=FGS_OK,
            "old epoch/generation output cannot publish after reset");
        CHECK(fgs_validate_slot(&s,old)!=FGS_OK,"same-window one-nanosecond re-anchor changes generation identity");
        CHECK(s.next_token==token,"reset never silently reuses cancellation tokens");
    }
}
static void capacities_and_overflow(void) {
    fgs_state s;
    fgs_real_plan plan;
    fgs_batch batch;
    CHECK(fgs_init(&s,1,second,0,0)==FGS_CLOCK && fgs_peek_real(&s,second,&plan)==FGS_CLOCK,
        "missing clock remains real-planner unavailable, not a fabricated lattice");
    CHECK(fgs_init(&s,1,second,hz60,hz60)==FGS_CLOCK,"unbounded latch margin is refused");
    CHECK(fgs_init(&s,1,UINT64_MAX-5,10,0)==FGS_OK &&
        fgs_peek_real(&s,UINT64_MAX-4,&plan)==FGS_OVERFLOW,"slot multiplication/addition overflow is refused");
    CHECK(fgs_init(&s,1,0,10,2)==FGS_OK && fgs_peek_real(&s,UINT64_MAX,&plan)==FGS_OVERFLOW,
        "publication plus margin overflow is refused");
    s.generation=UINT64_MAX;
    CHECK(fgs_reset(&s,1,0,10,2,&batch)==FGS_OVERFLOW && !s.configured && s.generation==UINT64_MAX,
        "reset generation never wraps stale identities");
    CHECK(fgs_reset(&s,1,0,10,2,&batch)==FGS_OVERFLOW,"repeated exhaustion stays disabled");
    CHECK(fgs_init(&s,1,second,1,0)==FGS_OK,"tiny-period stress lattice initializes");
    for (unsigned i=0;i<=FGS_WARMUP_INTERVALS;i++) batch=commit_real(&s,second+i*UINT64_C(25000000),i+1,1,1);
    CHECK(batch.request_status==FGS_CAPACITY && !batch.request_count,"excessive predicted children are rejected before unbounded iteration");
    CHECK(fgs_init(&s,1,second+hz120,hz120,margin)==FGS_OK,"pending-capacity stress clock");
    unsigned expired=0;
    for (unsigned i=0;i<32;i++) {
        batch=commit_real(&s,second+i*UINT64_C(40000000),i+1,1,1);
        CHECK(batch.request_status==FGS_OK,"time aging leaves normal request capacity available");
        expired+=batch.cancel_count;
        CHECK(s.pending_count<=FGS_PENDING_CAP,"unserviced work never exceeds scalar pending capacity");
        for (unsigned a=0;a<FGS_PENDING_CAP;a++) for (unsigned b=a+1;b<FGS_PENDING_CAP;b++)
            CHECK(!s.pending[a].token || !s.pending[b].token ||
                s.pending[a].slot.ordinal!=s.pending[b].slot.ordinal,"pending targets are unique despite capacity pressure");
    }
    CHECK(expired>0,"expired unserviced reservations return explicit scalar cancellations");
    unsigned pending=s.pending_count;
    CHECK(fgs_end(&s,&batch)==FGS_OK && batch.cancel_count==pending,"capacity shutdown enumerates every live token");
    batch=fresh_requests(&s,hz60,UINT64_C(25000000));
    cancel_requests(&s,&batch);
    s.next_token=UINT64_MAX;
    int exhausted=0;
    for (unsigned i=0;i<4;i++) {
        batch=commit_real(&s,s.last_publication_ns+UINT64_C(25000000),100+i,91,1);
        if (batch.request_status==FGS_OVERFLOW) exhausted=1;
        CHECK(!batch.request_count,"token exhaustion cannot wrap into pending/old tokens");
    }
    CHECK(exhausted,"token exhaustion reports request overflow while preserving REAL commits");
    CHECK(fgs_init(&s,1,UINT64_MAX-UINT64_C(1000000000),hz60,margin)==FGS_OK,"high-time prediction fixture");
    uint64_t origin=UINT64_MAX-UINT64_C(325000000);
    for (unsigned i=0;i<=FGS_WARMUP_INTERVALS;i++) {
        batch=commit_real(&s,origin+i*UINT64_C(25000000),i+1,1,1);
        cancel_requests(&s,&batch);
    }
    CHECK(batch.request_status==FGS_OVERFLOW && s.have_real,"prediction overflow never erases actual REAL publication");
}
static void game_anchor_phase(void) {
    fgs_state s;
    fgs_batch source=fresh_requests(&s,hz120,UINT64_C(40000000));
    unsigned accepted=0, refused=0;
    for (unsigned half=0;half<2;half++) {
        fg_adaptive_policy p={0};
        uint64_t game_dt=UINT64_C(25000000), sim_dt=half?game_dt/2:game_dt;
        fg_adaptive_stamp previous={second,10*second,91,1};
        fg_adaptive_stamp current={second+sim_dt,10*second+game_dt,91,1};
        CHECK(fg_adaptive_observe(&p,previous,hz120)==FG_ADAPTIVE_WARMUP,"phase uses immutable previous game stamp");
        CHECK(fg_adaptive_observe(&p,current,hz120)==FG_ADAPTIVE_ACTIVE,"phase uses actual game-anchor interval");
        p.display_anchor_ns=source.requests[0].real_slot.target_ns;
        uint64_t offsets[]={0,1,game_dt/2,game_dt-1,game_dt};
        for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
            float phase=NAN;
            CHECK(fg_adaptive_phase(&p,p.display_anchor_ns+offsets[i],0,&phase) &&
                fabs((double)phase-(double)offsets[i]/game_dt)<1e-7,
                "actual helper covers zero/near-zero/half/near-one/one without publication dt");
        }
        for (unsigned i=0;i<source.request_count;i++) {
            fgs_request q=source.requests[i];
            uint64_t distance=q.slot.target_ns-q.real_slot.target_ns;
            float phase=NAN;
            int valid=fg_adaptive_phase(&p,q.slot.target_ns,0,&phase);
            if (distance<=game_dt) {
                accepted++;
                CHECK(valid && fabs((double)phase-(double)distance/game_dt)<1e-7,
                    "each child has its own phase measured from assigned REAL and game anchors");
                CHECK(fabs((double)phase-(double)distance/q.ewma_ns)>0.01,
                    "publication EWMA is demonstrably not the game-phase denominator");
                double target_sim=(double)current.sim_ns+(double)distance*sim_dt/game_dt;
                CHECK(fabs((target_sim-current.sim_ns)/sim_dt-phase)<1e-7,
                    "half-step simulation changes motion distance, not relative pair phase");
            } else {
                refused++;
                CHECK(!valid && isnan(phase),"child beyond actual one-game-interval horizon is refused, not clamped");
            }
        }
        CHECK(p.current.anchor_ns==current.anchor_ns && p.previous.anchor_ns==previous.anchor_ns,
            "display mapping never rewrites immutable game anchors");
        current.epoch++;
        current.anchor_ns+=game_dt; current.sim_ns+=sim_dt;
        CHECK(fg_adaptive_observe(&p,current,hz120)==FG_ADAPTIVE_WARMUP,"wrong source epoch invalidates game pair");
        float phase=NAN;
        CHECK(!fg_adaptive_phase(&p,p.display_anchor_ns,0,&phase),"wrong-epoch source cannot invent phase");
    }
    CHECK(accepted>=4 && refused>=2,"multi-child phase fixture covers both in-horizon and refused opportunities");
}
static void irregular_replay_and_causality(void) {
    real_row rows[240];
    for (unsigned i=0;i<240;i++) rows[i]=(real_row){second+i*UINT64_C(25000000),second+hz60,i+1,i+1,1};
    for (unsigned m=0;m<3;m++) for (unsigned cost=0;cost<3;cost++) {
        replay_stats stats=fixture_run(rows,240,hz60,m*UINT64_C(1000000),cost*UINT64_C(4000000),NULL);
        CHECK(stats.real_slots==240 && !stats.real_displacements && stats.generated_slots>0,
            "constant cadence preserves REAL and generates opportunities for all replay sensitivities");
    }
    replay_stats costly=fixture_run(rows,240,hz60,margin,UINT64_C(100000000),NULL);
    CHECK(!costly.generated_slots && costly.late_jobs==costly.started_jobs && costly.started_jobs>0,
        "expensive readiness produces explicit late jobs, not phantom filled slots");
    for (unsigned i=0;i<240;i++) rows[i].publication_ns=second+i*UINT64_C(50000000);
    replay_stats floor=fixture_run(rows,240,hz60,margin,UINT64_C(4000000),NULL);
    CHECK(!floor.started_jobs,"below25 real fps never starts synthesis");
    for (unsigned i=0;i<240;i++) {
        rows[i].publication_ns=second+i*UINT64_C(16666667);
        rows[i].target_ns=second+hz120;
    }
    replay_stats fast=fixture_run(rows,240,hz120,UINT64_C(1000000),UINT64_C(4000000),NULL);
    CHECK(fast.real_slots==240 && fast.generated_slots>0 && !fast.real_displacements,
        "120Hz display with at-most60Hz source uses the same production policy");
    const uint64_t intervals[]={UINT64_C(21000000),UINT64_C(27000000),UINT64_C(30000000)};
    for (unsigned i=0;i<240;i++) {
        rows[i].publication_ns=i?rows[i-1].publication_ns+intervals[i%3]:second;
        rows[i].target_ns=second+hz60;
        if (i==30) rows[i].publication_ns+=UINT64_C(300000000);
        if (i==50) rows[i].eligible=0;
        if (i==90 || i==91) rows[i].publication_ns=rows[i-1].publication_ns;
    }
    replay_trace full, prefix;
    replay_stats irregular=fixture_run(rows,240,hz60,margin,UINT64_C(4000000),&full);
    CHECK(irregular.real_slots==240 && irregular.generated_slots>0 && !irregular.real_displacements,
        "irregular cadence, bursts, gaps and scene suspension preserve REAL");
    const unsigned lengths[]={20,40,60,80,100,180};
    for (unsigned k=0;k<sizeof lengths/sizeof lengths[0];k++) {
        unsigned length=lengths[k];
        fixture_run(rows,length,hz60,margin,UINT64_C(4000000),&prefix);
        uint64_t cutoff=rows[length-1].publication_ns;
        unsigned a=0,b=0;
        while (a<full.count && full.events[a].now_ns<=cutoff) a++;
        while (b<prefix.count && prefix.events[b].now_ns<=cutoff) b++;
        CHECK(a==b,"future rows cannot change earlier production-policy event count");
        for (unsigned i=0;i<a;i++) CHECK(full.events[i].now_ns==prefix.events[i].now_ns &&
            full.events[i].frame==prefix.events[i].frame && full.events[i].slot==prefix.events[i].slot &&
            full.events[i].generated==prefix.events[i].generated,
            "future rows cannot change an earlier exact chronological REAL/GEN decision");
    }
}

static int parse_values(const char *text, uint64_t *values, unsigned capacity) {
    unsigned count=0;
    while (*text) {
        while (isspace((unsigned char)*text)) text++;
        if (!*text || *text=='#') break;
        if (count==capacity || !isdigit((unsigned char)*text)) return -1;
        errno=0;
        char *end;
        unsigned long long value=strtoull(text,&end,10);
        if (errno || end==text || (*end && !isspace((unsigned char)*end) && *end!='#')) return -1;
#if ULLONG_MAX > UINT64_MAX
        if (value>UINT64_MAX) return -1;
#endif
        values[count++]=(uint64_t)value;
        text=end;
    }
    return (int)count;
}
static uint64_t argument(const char *text) {
    uint64_t value;
    CHECK(parse_values(text,&value,1)==1,"CLI argument is an unsigned nanosecond integer");
    return value;
}
static void print_summary(const replay_runner *r) {
    const replay_stats *s=&r->bounded;
    printf("{\"event\":\"summary\",\"base_ns\":%" PRIu64 ",\"period_ns\":%" PRIu64
        ",\"margin_ns\":%" PRIu64 ",\"readiness_ns\":%" PRIu64
        ",\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64
        ",\"real_slots\":%" PRIu64 ",\"generated_opportunity_slots\":%" PRIu64
        ",\"started_jobs\":%" PRIu64 ",\"real_displacements\":%" PRIu64
        ",\"started_job_outcomes\":{\"generated-commit\":%" PRIu64
        ",\"early-real-cancel\":%" PRIu64 ",\"generation-late\":%" PRIu64
        ",\"prediction-reset-cancel\":%" PRIu64 "},\"all_real_slots\":%" PRIu64
        ",\"all_generated_opportunity_slots\":%" PRIu64 ",\"all_started_jobs\":%" PRIu64
        ",\"phase_feasibility\":\"UNKNOWN per retained job; occupancy conditional on actual game phase\"}\n",
        r->state.base_ns,r->state.period_ns,r->state.margin_ns,r->cost_ns,r->start_ns,r->end_ns,
        s->real_slots,s->generated_slots,s->started_jobs,s->real_displacements,
        s->started_jobs-s->early_cancels-s->late_jobs-s->other_cancels,
        s->early_cancels,s->late_jobs,s->other_cancels,
        r->all.real_slots,r->all.generated_slots,r->all.started_jobs);
}
static int replay_cli(int argc, char **argv) {
    if (argc!=2 && argc!=5 && argc!=7) {
        fprintf(stderr,"usage: %s --replay [period_ns margin_ns readiness_ns [start_ns end_ns]] < rows\n",argv[0]);
        return 2;
    }
    uint64_t period=argc>=5?argument(argv[2]):hz60;
    uint64_t latch_margin=argc>=5?argument(argv[3]):margin;
    uint64_t cost=argc>=5?argument(argv[4]):UINT64_C(4000000);
    uint64_t start=argc==7?argument(argv[5]):0, end=argc==7?argument(argv[6]):UINT64_MAX;
    CHECK(period && latch_margin<period && start<end,"CLI clock and optional summary bounds are valid");
    replay_runner r;
    int initialized=0;
    uint64_t previous_time=0, previous_sequence=0, line_number=0;
    char line[256];
    while (fgets(line,sizeof line,stdin)) {
        line_number++;
        if (!strchr(line,'\n') && !feof(stdin)) {
            fprintf(stderr,"replay row %" PRIu64 " exceeds input line bound\n",line_number);
            return 2;
        }
        uint64_t fields[5];
        int count=parse_values(line,fields,5);
        if (!count) continue;
        if ((count!=4 && count!=5) || (count==5 && fields[4]>1)) {
            fprintf(stderr,"invalid replay row %" PRIu64 ": expected publication target frame sequence [eligible]\n",line_number);
            return 2;
        }
        real_row row={fields[0],fields[1],fields[2],fields[3],count==5?(int)fields[4]:1};
        if (initialized && (row.publication_ns<previous_time ||
                (row.publication_ns==previous_time && row.sequence<previous_sequence))) {
            fprintf(stderr,"replay row %" PRIu64 " is not sorted by publication then sequence\n",line_number);
            return 2;
        }
        if (!initialized) {
            runner_init(&r,row.target_ns,period,latch_margin,cost,1,start,end,NULL);
            initialized=1;
        }
        runner_due(&r,row.publication_ns,0);
        runner_real(&r,row);
        previous_time=row.publication_ns; previous_sequence=row.sequence;
    }
    if (ferror(stdin)) { fprintf(stderr,"replay input read failed\n"); return 2; }
    if (!initialized) runner_init(&r,0,period,latch_margin,cost,1,start,end,NULL);
    runner_due(&r,UINT64_MAX,1);
    print_summary(&r);
    return 0;
}
int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--replay")) return replay_cli(argc,argv);
    if (argc!=1) { fprintf(stderr,"usage: %s [--replay ...]\n",argv[0]); return 2; }
    first_free_and_peek();
    ewma_modes_and_resets();
    pending_discontinuities();
    real_ties_and_children();
    readiness_and_irrevocable_apply();
    lifecycle_and_external_ownership();
    capacities_and_overflow();
    game_anchor_phase();
    irregular_replay_and_causality();
    printf("framegen adaptive slots: %u checks\n",checks);
    return 0;
}
