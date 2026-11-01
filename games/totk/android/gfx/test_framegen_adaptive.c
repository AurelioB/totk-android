/* Pure production helpers: no GPU, Vulkan/Android runtime, sleeps or clocks.
 * Queue ownership below is an explicit event model, not a device fence claim.
 * make -C games/totk/android framegenadaptivetest TARGET=linux CC=cc OUT=<scratch>
 * <scratch>/framegenadaptivetest */
#include "framegen_adaptive.h"
#include "framegen_adaptive_plan.h"
#include "framegen_adaptive_pace.h"
#include "framegen_budget_external.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr,"framegen adaptive: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)
static const uint64_t second=UINT64_C(1000000000), hz60=UINT64_C(16666667), hz120=UINT64_C(8333333);
static int near(double a,double b) { return fabs(a-b)<1e-7; }
static fg_adaptive_stamp stamp(uint64_t sim,uint64_t anchor,uint64_t epoch,int eligible) {
    return (fg_adaptive_stamp){sim,anchor,epoch,eligible};
}
static fg_adaptive_policy pair(uint64_t refresh,uint64_t dt) {
    fg_adaptive_policy p={0};
    CHECK(fg_adaptive_observe(&p,stamp(second,second,7,1),refresh)==FG_ADAPTIVE_WARMUP,
        "first eligible real only warms history");
    CHECK(fg_adaptive_observe(&p,stamp(second+dt,second+dt,7,1),refresh)==FG_ADAPTIVE_ACTIVE,
        "second sub-refresh-rate real supplies a measurable pair");
    return p;
}
static void phases(void) {
    const uint64_t refreshes[]={hz60,hz120};
    const uint64_t dt=UINT64_C(24000000);
    const uint64_t offsets[]={0,1,3000000,7200000,12000000,18000000,dt-1,dt};
    const double expected[]={0,1.0/24000000,.125,.3,.5,.75,1.0-1.0/24000000,1};
    for (unsigned r=0;r<2;r++) {
        fg_adaptive_policy p=pair(refreshes[r],dt);
        for (int interpolation=0;interpolation<=1;interpolation++) for (unsigned i=0;i<8;i++) {
            float phase=NAN;
            CHECK(fg_adaptive_phase(&p,p.current.anchor_ns+offsets[i],interpolation,&phase),
                "both no-delay prediction and delayed bracket accept in-horizon targets");
            CHECK(isfinite(phase) && near(phase,expected[i]),
                "requested fractional phase follows measured simulation interval, not a fixed midpoint");
            if (!i || i==7)
                CHECK(phase==(i?1.0f:0.0f),"endpoint phase is exact for retained-real endpoint selection");
        }
        float phase=NAN;
        CHECK(!fg_adaptive_phase(&p,p.current.anchor_ns-1,0,&phase) && isnan(phase),
            "target before current real is refused without inventing phase");
        CHECK(!fg_adaptive_phase(&p,p.current.anchor_ns+dt+1,0,&phase) && isnan(phase),
            "prediction beyond one simulation interval is refused rather than clamped to wrong time");
        CHECK(!fg_adaptive_phase(&p,p.current.anchor_ns+dt+1,1,&phase),
            "delayed interpolation cannot extrapolate beyond its available real bracket");
        CHECK(!fg_adaptive_phase(&p,p.current.anchor_ns,0,NULL),"null output is invalid");
        p.state=FG_ADAPTIVE_SCENE;
        CHECK(!fg_adaptive_phase(&p,p.current.anchor_ns+dt/2,0,&phase),
            "invalid/suspended policy never returns a phase even for an in-range target");
    }
    /* This API has integer timestamps, not caller-supplied float phase inputs.
     * NaN is an output sentinel above; backend float-input validation lives in
     * the separate framegen budget/QCOM callback fixture. */
}
static void irregular_and_passthrough(void) {
    const uint64_t intervals[]={20000000,31000000,18000000,39000000,24000000};
    fg_adaptive_policy p={0};
    uint64_t sim=second,anchor=2*second;
    CHECK(fg_adaptive_observe(&p,stamp(sim,anchor,2,1),hz60)==FG_ADAPTIVE_WARMUP,"irregular initial source");
    for (unsigned i=0;i<5;i++) {
        fg_adaptive_stamp previous=p.current;
        sim+=intervals[i]; anchor+=intervals[4-i];
        CHECK(fg_adaptive_observe(&p,stamp(sim,anchor,2,1),hz60)==FG_ADAPTIVE_ACTIVE,
            "irregular real intervals remain measured rather than quantized to real30");
        CHECK(p.previous.sim_ns==previous.sim_ns && p.previous.anchor_ns==previous.anchor_ns,
            "the two exact eligible stamps are retained without changing their anchors");
        float phase=NAN;
        CHECK(fg_adaptive_phase(&p,anchor+intervals[4-i]/4,0,&phase) && near(phase,.25),
            "game-clock secant maps original guest distance into the consumed simulation interval");
    }
    for (unsigned r=0;r<2;r++) {
        uint64_t refresh=r?hz120:hz60;
        p=(fg_adaptive_policy){0}; sim=anchor=second;
        fg_adaptive_observe(&p,stamp(sim,anchor,3,1),refresh);
        sim+=refresh; anchor+=refresh;
        CHECK(fg_adaptive_observe(&p,stamp(sim,anchor,3,1),refresh)==FG_ADAPTIVE_PASSTHROUGH,
            "at physical refresh passes through real pixels");
        float phase=NAN;
        CHECK(!fg_adaptive_phase(&p,anchor+refresh/2,0,&phase),"pass-through cannot generate artificial gaps");
        sim+=refresh/2; anchor+=refresh/2;
        CHECK(fg_adaptive_observe(&p,stamp(sim,anchor,3,1),refresh)==FG_ADAPTIVE_PASSTHROUGH,
            "above physical refresh remains real-only");
        sim+=24000000; anchor+=24000000;
        CHECK(fg_adaptive_observe(&p,stamp(sim,anchor,3,1),refresh)==FG_ADAPTIVE_ACTIVE,
            "a later slower measured real pair can resume prediction without a fixed30 toggle");
    }
    p=pair(hz60,24000000);
    fg_adaptive_stamp current=p.current;
    current.sim_ns+=FG_ADAPTIVE_MAX_INTERVAL_NS; current.anchor_ns+=FG_ADAPTIVE_MAX_INTERVAL_NS;
    CHECK(fg_adaptive_observe(&p,current,hz60)==FG_ADAPTIVE_ACTIVE,"exact25fps floor boundary remains eligible");
}
static void display_anchor_mapping(void) {
    const uint64_t refreshes[]={hz60,hz120};
    for (unsigned r=0;r<2;r++) {
        uint64_t refresh=refreshes[r],dt=25000000;
        fg_adaptive_policy p=pair(refresh,dt);
        fg_adaptive_stamp real=p.current;
        uint64_t display=real.anchor_ns+6*refresh;
        uint64_t target=display+dt/2;
        float phase=NAN;
        CHECK(p.display_anchor_ns==real.anchor_ns,"default policy keeps the guest-present anchor until display mapping");
        CHECK(!fg_adaptive_phase(&p,target,0,&phase),
            "unmapped guest anchor cannot silently pretend long render/presentation latency is simulation progress");
        p.display_anchor_ns=display; /* Production's immutable real-vsync reservation. */
        for (int interpolation=0;interpolation<=1;interpolation++)
            CHECK(fg_adaptive_phase(&p,target,interpolation,&phase) && near(phase,.5),
                "phase starts at the real picture's display slot, not earlier render or guest completion");
        CHECK(p.current.anchor_ns==real.anchor_ns && p.current.sim_ns==real.sim_ns,
            "mapping display phase does not rewrite immutable source simulation/guest timestamp authority");
        uint64_t early_render=real.anchor_ns+1000000,late_render=display-1;
        uint64_t early_slot=fg_adaptive_first_slot(display,refresh,early_render);
        uint64_t late_slot=fg_adaptive_first_slot(display,refresh,late_render);
        CHECK(early_slot==late_slot && early_slot==display+refresh,
            "variable render readiness before the same fixed slot does not rephase target selection");
        CHECK(fg_adaptive_phase(&p,early_slot,0,&phase) && near(phase,(double)refresh/dt),
            "requested fixed-slot phase uses simulation delta, not render completion delay");
        /* Display-slot quantization can separate real publications by >40ms
         * even though both guest time and published simulation dt are healthy. */
        fg_adaptive_stamp next=stamp(real.sim_ns+31000000,real.anchor_ns+20000000,real.epoch,1);
        CHECK(fg_adaptive_observe(&p,next,refresh)==FG_ADAPTIVE_ACTIVE,
            "floor readiness uses original irregular guest anchors, never mapped display-slot separation");
        uint64_t next_display=display+6*refresh;
        CHECK(next_display-display>FG_ADAPTIVE_MAX_INTERVAL_NS,"fixture spans display gap beyond the real25fps floor");
        p.display_anchor_ns=next_display;
        CHECK(fg_adaptive_phase(&p,next_display+7750000,0,&phase) && near(phase,.3875),
            "smoothed31ms simulation/20ms guest secant maps7.75ms display advance to.3875 game interval");
        CHECK(p.previous.anchor_ns==real.anchor_ns && p.current.anchor_ns==next.anchor_ns,
            "both original guest anchors remain available for stale/floor validation after mapping");
    }
    fg_adaptive_policy normal={0},half={0};
    fg_adaptive_observe(&normal,stamp(second,second,71,1),hz60);
    fg_adaptive_observe(&half,stamp(second,second,71,1),hz60);
    CHECK(fg_adaptive_observe(&normal,stamp(second+24000000,second+24000000,71,1),hz60)==FG_ADAPTIVE_ACTIVE,
        "normal24ms real images remain below display rate");
    CHECK(fg_adaptive_observe(&half,stamp(second+12000000,second+24000000,71,1),hz60)==FG_ADAPTIVE_ACTIVE,
        "half-step12ms simulation still has24ms real-image arrival interval");
    float full_phase,half_phase;
    CHECK(fg_adaptive_phase(&normal,second+30000000,0,&full_phase) &&
        fg_adaptive_phase(&half,second+30000000,0,&half_phase) &&
        full_phase==.25f && half_phase==.25f,
        "half-step halves target simulation advance but preserves relative pair phase");
}
static void new_pair_after(fg_adaptive_policy *p,uint64_t sim,uint64_t anchor,uint64_t epoch,uint64_t refresh) {
    CHECK(fg_adaptive_observe(p,stamp(sim,anchor,epoch,1),refresh)==FG_ADAPTIVE_WARMUP,
        "suspension discards temporal evidence; first fresh source only warms up");
    CHECK(fg_adaptive_observe(p,stamp(sim+24000000,anchor+24000000,epoch,1),refresh)==FG_ADAPTIVE_ACTIVE,
        "second fresh eligible source resumes the pair");
}
static void discontinuities(void) {
    fg_adaptive_policy p=pair(hz60,24000000);
    fg_adaptive_stamp f=p.current;
    f.sim_ns+=24000000; f.anchor_ns+=24000000; f.epoch++;
    CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_WARMUP,"epoch change cannot pair old queued/source stamps");
    CHECK(!p.previous.sim_ns,"different epochs leave no previous temporal source");
    f.sim_ns+=24000000; f.anchor_ns+=24000000;
    CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_ACTIVE,"second source in new epoch resumes normally");
    for (unsigned reason=0;reason<3;reason++) {
        /* Runtime represents pause/menu/camera cut as ineligible stamp/new epoch;
         * policy consumes that evidence, never guesses scene classification. */
        f=p.current; f.sim_ns+=24000000; f.anchor_ns+=24000000; f.epoch++; f.eligible=0;
        CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_SCENE,"pause/menu/cut eligibility suspends generation");
        new_pair_after(&p,f.sim_ns+24000000,f.anchor_ns+24000000,f.epoch,hz60);
    }
    f=p.current; f.sim_ns+=40000001; f.anchor_ns+=24000000;
    CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_FLOOR,"simulation interval below25fps suspends");
    new_pair_after(&p,f.sim_ns+24000000,f.anchor_ns+24000000,f.epoch,hz60);
    f=p.current; f.sim_ns+=24000000; f.anchor_ns+=40000001;
    CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_FLOOR,"host real gap also invalidates smoothed healthy dt");
    new_pair_after(&p,f.sim_ns+24000000,f.anchor_ns+24000000,f.epoch,hz60);
    for (unsigned reverse=0;reverse<3;reverse++) {
        f=p.current;
        if (!reverse) f.sim_ns--;
        else if (reverse==1) f.anchor_ns--;
        CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_STALE,"reversed/duplicate stamps are stale");
        new_pair_after(&p,f.sim_ns+48000000,f.anchor_ns+48000000,f.epoch,hz60);
    }
    const uint64_t bad_clocks[]={0,7999999,10000000,11111111,18000001,UINT64_MAX};
    for (unsigned i=0;i<sizeof bad_clocks/sizeof *bad_clocks;i++) {
        f=p.current; f.sim_ns+=24000000; f.anchor_ns+=24000000;
        CHECK(fg_adaptive_observe(&p,f,bad_clocks[i])==FG_ADAPTIVE_CLOCK,"only supported60/120Hz clocks may generate");
        new_pair_after(&p,f.sim_ns+24000000,f.anchor_ns+24000000,f.epoch,hz60);
    }
    f=p.current; f.sim_ns=0;
    CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_SCENE,"absent simulation phase is not a real source");
    new_pair_after(&p,second,second,f.epoch+1,hz60);
    f=p.current; f.anchor_ns=0;
    CHECK(fg_adaptive_observe(&p,f,hz60)==FG_ADAPTIVE_SCENE,"absent monotonic anchor is not a real source");
    new_pair_after(&p,second,second,f.epoch+1,hz60);
}
static void slots(void) {
    const uint64_t refreshes[]={hz60,hz120};
    for (unsigned r=0;r<2;r++) {
        uint64_t refresh=refreshes[r],real=second;
        CHECK(fg_adaptive_first_slot(real,refresh,real-1)==real+refresh,"first generated slot follows real publication");
        CHECK(fg_adaptive_first_slot(real,refresh,real+refresh)==real+2*refresh,"a slot at now is already missed");
        uint64_t next=fg_adaptive_first_slot(real,refresh,real+3*refresh+17);
        CHECK(next==real+4*refresh && (next-real)%refresh==0,"late work skips to future fixed slot without midpoint/rephase");
        CHECK(fg_adaptive_first_slot(real,refresh,real+refresh+1)==real+2*refresh,
            "queued generation does not advance a real reservation's lattice");
        CHECK(!fg_adaptive_slot_publish(next,next,next-1,1),"same slot cannot publish twice");
        CHECK(!fg_adaptive_slot_publish(next-refresh,next,next-1,0),"late real cannot reverse immutable publication order");
        CHECK(fg_adaptive_slot_publish(next,next-refresh,next-1,1),"future generated slot publishes in increasing order");
        CHECK(!fg_adaptive_slot_publish(next,next-refresh,next,1),"generation at/after deadline leaves slot untouched");
        CHECK(fg_adaptive_slot_publish(next,next-refresh,next+1,0),"ready real need not wait for a generated deadline");
    }
    CHECK(!fg_adaptive_first_slot(0,hz60,second) && !fg_adaptive_first_slot(second,0,second),"absent lattice is refused");
    CHECK(!fg_adaptive_first_slot(UINT64_MAX-10,11,0),"initial slot addition cannot wrap");
    CHECK(!fg_adaptive_first_slot(1,1,UINT64_MAX),"catch-up increment cannot wrap");
    CHECK(!fg_adaptive_first_slot(UINT64_MAX-100,50,UINT64_MAX),"near-limit catch-up overflows closed");
    CHECK(fg_adaptive_first_slot(UINT64_MAX-100,50,UINT64_MAX-51)==UINT64_MAX-50,
        "representable near-limit future slot remains exact");
    CHECK(!fg_adaptive_slot_publish(0,0,0,0),"zero is not a published slot");
}
/* Publication and source/fence ownership are intentionally different state.
 * This model drives production arbitration helpers while completion events
 * explicitly supply each covering fence. It does not exercise a real GPU. */
enum { OWN_GL=1,OWN_COPY=2,OWN_POST=4,OWN_ALL=7 };
typedef struct {
    uint64_t slot,epoch;
    unsigned covering,completed,source_refs;
    int queued;
} candidate_model;
static int model_publish(candidate_model *c,uint64_t *last,uint64_t now) {
    if (!c->queued || !fg_adaptive_slot_publish(c->slot,*last,now,1)) return 0;
    c->queued=0; *last=c->slot; return 1;
}
static void model_complete(candidate_model *c,unsigned fence) {
    c->completed|=fence;
    if (!c->queued && (c->completed&c->covering)==c->covering) c->source_refs=0;
}
static void real_wins_and_retirement(void) {
    uint64_t slot=second+hz60,epoch=9;
    CHECK(fg_adaptive_real_wins(slot,epoch,slot,epoch,slot-1),"ready real wins same epoch/target");
    CHECK(fg_adaptive_real_wins(slot,epoch,slot,epoch,slot),"real ready exactly at slot wins");
    CHECK(!fg_adaptive_real_wins(slot,epoch,slot,epoch,slot+1),"real arriving after target cannot retroactively suppress publication");
    CHECK(!fg_adaptive_real_wins(slot,epoch,slot,epoch+1,slot-1),"different epochs cannot alias source/queue identity");
    CHECK(!fg_adaptive_real_wins(slot,epoch,slot+hz60,epoch,slot-1),"different fixed slots are independent");
    CHECK(!fg_adaptive_real_wins(slot,epoch,slot,epoch,0),"missing readiness is not proof a real won");
    CHECK(fg_adaptive_supersedes(slot+hz60,100,epoch,slot,101,epoch,slot-1),
        "earlier ready real preempts dequeued future prediction without reordering slots");
    CHECK(fg_adaptive_supersedes(slot,100,epoch,slot,101,epoch,slot-1),
        "newer real replaces an older pair at equal slot");
    CHECK(!fg_adaptive_supersedes(slot,101,epoch,slot,100,epoch,slot-1),
        "older real cannot suppress newest pair");
    CHECK(!fg_adaptive_supersedes(slot,100,epoch,slot+hz60,101,epoch,slot-1),
        "future real cannot retroactively suppress earlier target");
    CHECK(!fg_adaptive_supersedes(slot,100,epoch,slot,101,epoch+1,slot-1),
        "cross-epoch real invalidation remains explicit, never source aliasing");
    const unsigned orders[][3]={{OWN_GL,OWN_COPY,OWN_POST},{OWN_POST,OWN_GL,OWN_COPY},{OWN_COPY,OWN_POST,OWN_GL}};
    for (unsigned i=0;i<3;i++) {
        candidate_model c={slot,epoch,OWN_ALL,0,2,1};
        uint64_t last=slot-hz60;
        unsigned publications=0;
        model_complete(&c,orders[i][0]); /* One covering GPU job completed before race. */
        CHECK(c.source_refs==2 && c.queued,"partial completion retains queued source ownership");
        if (fg_adaptive_real_wins(c.slot,c.epoch,slot,epoch,slot-1)) c.queued=0;
        CHECK(!c.queued && c.source_refs==2 && c.covering==OWN_ALL,
            "suppressing generated presentation never frees source references or covering fences");
        if (fg_adaptive_slot_publish(slot,last,slot,0)) { last=slot; publications++; }
        publications+=(unsigned)model_publish(&c,&last,slot-1);
        CHECK(publications==1 && last==slot,"real/generation race has one publication and monotonic slot order");
        model_complete(&c,orders[i][1]);
        CHECK(c.source_refs==2,"second fence still cannot retire a source covered by third job");
        model_complete(&c,orders[i][2]);
        CHECK(!c.source_refs,"all covering completions retire suppressed work without early source reuse");
        c=(candidate_model){slot+hz60,epoch,OWN_ALL,0,2,1};
        CHECK(model_publish(&c,&last,slot+1) && last==slot+hz60,"next future candidate keeps immutable ordering");
        CHECK(c.source_refs==2,"publication also does not release outstanding GPU/source ownership");
        model_complete(&c,OWN_ALL);
        CHECK(!c.source_refs,"published work retires only after its covering completions");
    }
}
static void budget_warm(fg_budget_policy *p) {
    fg_budget_sample(p,1,20,1); fg_budget_seal(p,1,1);
    fg_budget_sample(p,2,20,1); fg_budget_seal(p,2,2);
}
static void budget_interval(fg_budget_policy *p,fg_budget_external *e,uint64_t frame) {
    uint64_t real=p->last_ticket+1,synthetic=real+1;
    fg_budget_external_sample(p,e,real,20,1);
    fg_budget_external_sample(p,e,synthetic,.5,1);
    fg_budget_synthetic(p,synthetic,synthetic);
    fg_budget_external_seal(p,e,frame,synthetic);
}
static void budget_groups(void) {
    fg_budget_policy p={0}; fg_budget_external e={0}; fg_budget_external_group g={0};
    budget_warm(&p);
    fg_budget_external_group_begin(&e,&g,3);
    fg_budget_external_group_end(&p,&e,&g,3,2,1);
    CHECK(!g.pending && near(g.ms,2) && isnan(e.cost[3].ms),
        "first phase completion before next begin cannot finalize an unsealed group");
    budget_interval(&p,&e,3);
    CHECK(p.frames[3].status==FG_BUDGET_PENDING,"real interval seal alone cannot make unfinished external phases free");
    fg_budget_external_group_begin(&e,&g,3);
    CHECK(g.pending==1 && near(g.ms,2),"second phase begin preserves already completed phase cost");
    fg_budget_external_group_end(&p,&e,&g,3,3,1);
    CHECK(fg_budget_completed(&p,3).status==FG_BUDGET_MISSING,"all known phases completed still requires explicit group seal");
    fg_budget_external_group_seal(&p,&e,&g,3);
    fg_budget_decision d=fg_budget_completed(&p,3);
    CHECK(d.status==FG_BUDGET_ACCEPT && near(d.real_ms,20) && near(d.incremental_ms,5.5) && near(d.total_ms,25.5),
        "all GL phases aggregate exactly once alongside disjoint Vulkan incremental work");
    fg_budget_external_group_end(&p,&e,&g,3,100,1);
    fg_budget_external_group_seal(&p,&e,&g,3);
    CHECK(near(fg_budget_completed(&p,3).total_ms,25.5),"duplicate completion/seal after finish cannot rewrite cost");

    fg_budget_external_group_begin(&e,&g,4);
    fg_budget_external_group_begin(&e,&g,4);
    fg_budget_external_group_begin(&e,&g,4);
    fg_budget_external_group_seal(&p,&e,&g,4);
    budget_interval(&p,&e,4);
    fg_budget_external_group_end(&p,&e,&g,99,1,1);
    CHECK(g.pending==3,"wrong frame completion cannot decrement active phase ownership");
    fg_budget_external_group_end(&p,&e,&g,4,1,1);
    fg_budget_external_group_end(&p,&e,&g,4,2,1);
    CHECK(fg_budget_completed(&p,4).status==FG_BUDGET_MISSING && g.pending==1,
        "sealed group waits for every outstanding phase, not first/most completed jobs");
    fg_budget_external_group_end(&p,&e,&g,4,7,1);
    d=fg_budget_completed(&p,4);
    CHECK(d.status==FG_BUDGET_OVERLOAD && near(d.incremental_ms,10.5) && near(d.total_ms,30.5),
        "multiple phase sum enforces total GPU ceiling instead of certifying each phase separately");

    /* Both GL phases finish/seal before any Vulkan sample or interval seal. */
    fg_budget_external_group_begin(&e,&g,5);
    fg_budget_external_group_begin(&e,&g,5);
    fg_budget_external_group_end(&p,&e,&g,5,.25,1);
    fg_budget_external_group_end(&p,&e,&g,5,.75,1);
    fg_budget_external_group_seal(&p,&e,&g,5);
    CHECK(fg_budget_completed(&p,5).status==FG_BUDGET_MISSING,"GL completion is not Vulkan interval authority");
    budget_interval(&p,&e,5);
    CHECK(near(fg_budget_completed(&p,5).total_ms,21.5),"early external aggregate is charged when Vulkan interval later seals");
}
static void budget_missing_and_invalid(void) {
    const double costs[]={NAN,INFINITY,-1,1};
    for (unsigned i=0;i<4;i++) {
        fg_budget_policy p={0}; fg_budget_external e={0}; fg_budget_external_group g={0}; budget_warm(&p);
        fg_budget_external_group_begin(&e,&g,3);
        fg_budget_external_group_begin(&e,&g,3);
        fg_budget_external_group_end(&p,&e,&g,3,1,1);
        fg_budget_external_group_end(&p,&e,&g,3,costs[i],i!=3);
        fg_budget_external_group_seal(&p,&e,&g,3);
        budget_interval(&p,&e,3);
        CHECK(p.frames[3].status==FG_BUDGET_UNAVAILABLE && fg_budget_completed(&p,3).status==FG_BUDGET_MISSING,
            "nonfinite/negative/missing external phase invalidates whole aggregate, never a measured subset");
    }
    fg_budget_policy p={0}; fg_budget_external e={0}; fg_budget_external_group missing={0},next={0}; budget_warm(&p);
    fg_budget_external_group_begin(&e,&missing,3);
    fg_budget_external_group_begin(&e,&missing,3);
    fg_budget_external_group_end(&p,&e,&missing,3,1,1);
    fg_budget_external_group_seal(&p,&e,&missing,3);
    budget_interval(&p,&e,3);
    CHECK(missing.pending==1 && p.frames[3].status==FG_BUDGET_PENDING,"missing phase remains pending, not zero cost");
    fg_budget_external_group_begin(&e,&next,4);
    fg_budget_external_group_end(&p,&e,&next,4,2,1);
    fg_budget_external_group_seal(&p,&e,&next,4);
    budget_interval(&p,&e,4);
    CHECK(fg_budget_completed(&p,4).status==FG_BUDGET_MISSING,"later completed aggregate cannot bypass older pending interval");
    fg_budget_external_group_end(&p,&e,&missing,3,0,0); /* Explicit query failure/timeout event. */
    CHECK(p.frames[3].status==FG_BUDGET_UNAVAILABLE && near(fg_budget_completed(&p,4).total_ms,22.5),
        "unavailable old query fails closed and allows independent fresh aggregate to recover");

    p=(fg_budget_policy){0}; e=(fg_budget_external){0}; next=(fg_budget_external_group){0}; budget_warm(&p);
    fg_budget_external_group_begin(&e,&next,3);
    fg_budget_external_group_end(&p,&e,&next,3,1,1);
    fg_budget_external_group_seal(&p,&e,&next,3);
    fg_budget_external_sample(&p,&e,3,20,1);
    fg_budget_external_sample(&p,&e,5,.5,1); /* Ticket4 is missing, not a valid measured subset. */
    fg_budget_synthetic(&p,5,5); fg_budget_external_seal(&p,&e,3,5);
    CHECK(p.frames[3].status==FG_BUDGET_UNAVAILABLE,"complete external aggregate cannot hide missing Vulkan timing");
    fg_budget_external_group_begin(&e,&next,4);
    fg_budget_external_group_end(&p,&e,&next,4,1,1);
    fg_budget_external_group_seal(&p,&e,&next,4);
    budget_interval(&p,&e,4);
    CHECK(near(fg_budget_completed(&p,4).total_ms,21.5),"new complete Vulkan/external bracket recovers after missing sample");

    p=(fg_budget_policy){0}; e=(fg_budget_external){0}; next=(fg_budget_external_group){0}; budget_warm(&p);
    fg_budget_external_group_begin(&e,&next,3); fg_budget_external_group_begin(&e,&next,3);
    fg_budget_external_group_end(&p,&e,&next,3,DBL_MAX,1);
    fg_budget_external_group_end(&p,&e,&next,3,DBL_MAX,1);
    fg_budget_external_group_seal(&p,&e,&next,3); budget_interval(&p,&e,3);
    CHECK(fg_budget_completed(&p,3).status==FG_BUDGET_MISSING,"finite phase costs whose sum overflows cannot certify work");
}
static void budget_ring_identity(void) {
    fg_budget_policy p={0}; fg_budget_external e={0}; fg_budget_external_group groups[FG_BUDGET_FRAMES]={0};
    budget_warm(&p);
    for (uint64_t frame=3;frame<100;frame++) {
        fg_budget_external_group *g=&groups[frame%FG_BUDGET_FRAMES];
        fg_budget_external_group_begin(&e,g,frame);
        fg_budget_external_group_end(&p,&e,g,frame,.25,1);
        fg_budget_external_group_begin(&e,g,frame);
        fg_budget_external_group_end(&p,&e,g,frame,.75,1);
        if (frame&1) fg_budget_external_group_seal(&p,&e,g,frame);
        budget_interval(&p,&e,frame);
        if (!(frame&1)) fg_budget_external_group_seal(&p,&e,g,frame);
        CHECK(near(fg_budget_completed(&p,frame).total_ms,21.5),"group identities/cost sums survive repeated bounded-ring reuse");
    }
    fg_budget_external_group_end(&p,&e,&groups[3],3,100,1);
    fg_budget_external_group_seal(&p,&e,&groups[3],3);
    CHECK(near(fg_budget_completed(&p,99).total_ms,21.5),"stale overwritten group cannot alias newest aggregate");
}
static uint64_t planner_rate(uint64_t refresh,unsigned fps,unsigned frames) {
    fg_adaptive_plan p={0};
    uint64_t start=second,previous=start,generated=0;
    CHECK(!fg_adaptive_plan_outputs(&p,start,refresh,FG_ADAPTIVE_ACTIVE),
        "planner first guest anchor cannot invent an interval or credit");
    for (unsigned i=1;i<=frames;i++) {
        uint64_t now=start+(uint64_t)i*second/fps,interval=now-previous;
        unsigned count=fg_adaptive_plan_outputs(&p,now,refresh,FG_ADAPTIVE_ACTIVE);
        generated+=count;
        CHECK(count<=FG_ADAPTIVE_PLAN_CAPACITY && count<=(interval-1)/refresh,
            "planned attempts never exceed fixed output capacity or this real's immediate future horizon");
        CHECK(generated<=(now-start)/refresh-i,
            "every prefix obeys elapsed physical display opportunities minus real frames");
        CHECK(p.credit_ns<refresh,"fractional display credit remains a bounded sub-refresh remainder");
        if (i<FG_ADAPTIVE_PLAN_MIN_SAMPLES || now-start<FG_ADAPTIVE_PLAN_UP_NS)
            CHECK(!count,"minimum samples and sustained half-second demand precede generation");
        previous=now;
    }
    uint64_t budget=(previous-start)/refresh-frames;
    CHECK(generated>0 && generated+40>=budget,
        "fractional planner fills long-run display budget without integer-ratio entry starvation");
    return generated;
}
static void planner_fractional_rates(void) {
    uint64_t generated=planner_rate(hz60,43,4300);
    CHECK(generated>1600 && generated<1701,
        "43-to60 remains fractional rather than an integer floor cap that disables all generation");
    const unsigned rates[]={55,57,59};
    for (unsigned i=0;i<3;i++) {
        generated=planner_rate(hz60,rates[i],rates[i]*100);
        CHECK(generated<(uint64_t)rates[i]*100/4,
            "55-to59 near-display short gaps earn only sparse attempts, never one forced child per real");
    }
    (void)planner_rate(hz120,43,4300);
    (void)planner_rate(hz120,60,6000);
    fg_adaptive_plan p={0};
    uint64_t now=second;
    fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE);
    for (unsigned i=1;i<=120;i++) {
        now+=2*hz60;
        unsigned count=fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE);
        if (i>32) CHECK(count==1 && p.ceiling==1,
            "stable30-to60 produces exactly one child after bounded rate/hysteresis warmup");
    }
    p=(fg_adaptive_plan){0}; now=second;
    fg_adaptive_plan_outputs(&p,now,hz120,FG_ADAPTIVE_ACTIVE);
    for (unsigned i=1;i<=120;i++) {
        now+=2*hz120;
        unsigned count=fg_adaptive_plan_outputs(&p,now,hz120,FG_ADAPTIVE_ACTIVE);
        if (i>64) CHECK(count==1,
            "120Hz with real60 earns one child per real, not a fixed60Hz policy");
    }
    p=(fg_adaptive_plan){0}; now=second;
    fg_adaptive_plan_outputs(&p,now,8000000,FG_ADAPTIVE_ACTIVE);
    for (unsigned i=1;i<=40;i++) {
        now+=FG_ADAPTIVE_MAX_INTERVAL_NS;
        unsigned count=fg_adaptive_plan_outputs(&p,now,8000000,FG_ADAPTIVE_ACTIVE);
        CHECK(count<=4,"25fps floor at shortest admitted refresh cannot exceed four bounded output jobs");
        if (i>20) CHECK(count==4 && p.ceiling==4,"exact floor endpoint retains four useful output credits");
    }
}
static void planner_hysteresis(void) {
    fg_adaptive_plan p={.last_present_ns=second,.refresh_ns=hz120,
        .interval_ns=3.0*hz120,.samples=FG_ADAPTIVE_PLAN_MIN_SAMPLES,.ceiling=1};
    uint64_t now=second;
    for (unsigned i=1;i<=21;i++) {
        now+=3*hz120;
        unsigned count=fg_adaptive_plan_outputs(&p,now,hz120,FG_ADAPTIVE_ACTIVE);
        if (now-second<FG_ADAPTIVE_PLAN_UP_NS)
            CHECK(p.ceiling==1 && count<=1,"higher demand cannot step up before a continuous half-second");
        else CHECK(p.ceiling==2 && count==2,"sustained half-second demand admits the new bounded ceiling");
    }
    p=(fg_adaptive_plan){.last_present_ns=second,.refresh_ns=hz120,
        .interval_ns=3.0*hz120,.samples=FG_ADAPTIVE_PLAN_MIN_SAMPLES,.ceiling=3};
    now=second;
    for (unsigned i=1;i<=3;i++) {
        now+=2*hz120;
        unsigned count=fg_adaptive_plan_outputs(&p,now,hz120,FG_ADAPTIVE_ACTIVE);
        CHECK(count<=1,"instant display credit/horizon clips work even while a larger ceiling is hysteretic");
        CHECK(p.ceiling==(i<3 ? 3u : 2u),
            "lower demand steps down after exactly three lower EMA reads, not another half-second");
        if (i==1) CHECK(fabs(p.interval_ns-2.92*hz120)<.001,
            "source interval EMA smooths planner demand without becoming phase or earned-credit authority");
    }
    p=(fg_adaptive_plan){.last_present_ns=second,.refresh_ns=hz120,
        .interval_ns=2.0*hz120,.samples=FG_ADAPTIVE_PLAN_MIN_SAMPLES,.ceiling=1};
    now=second;
    for (unsigned i=0;i<300;i++) {
        now+=(i&1 ? 5 : 3)*hz120/2;
        CHECK(fg_adaptive_plan_outputs(&p,now,hz120,FG_ADAPTIVE_ACTIVE)<=1 && p.ceiling==1,
            "rate chatter across an integer demand boundary never satisfies sustained step-up evidence");
    }
}
static fg_adaptive_plan planner_warm(void) {
    fg_adaptive_plan p={0};
    uint64_t now=second;
    fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE);
    for (unsigned i=0;i<40;i++) {
        now+=2*hz60;
        (void)fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE);
    }
    CHECK(p.samples==FG_ADAPTIVE_PLAN_MIN_SAMPLES && p.ceiling==1,
        "reset fixture begins with mature bounded planner evidence");
    p.credit_ns=hz60/2;
    return p;
}
static int planner_empty(const fg_adaptive_plan *p) {
    return !p->last_present_ns && !p->refresh_ns && !p->up_since_ns && !p->credit_ns &&
        !p->interval_ns && !p->samples && !p->ceiling && !p->up_floor && !p->down_reads;
}
static void planner_resets_and_phase(void) {
    for (unsigned state=0;state<=FG_ADAPTIVE_REASONS;state++) {
        if (state==FG_ADAPTIVE_ACTIVE) continue;
        fg_adaptive_plan p=planner_warm();
        CHECK(!fg_adaptive_plan_outputs(&p,p.last_present_ns+2*hz60,hz60,state) && planner_empty(&p),
            "pause/menu/cut/epoch warmup/pass-through/refusal clears all count evidence and fractional credit");
    }
    const uint64_t gaps[]={40000001,500000001,UINT64_MAX/2};
    for (unsigned i=0;i<3;i++) {
        fg_adaptive_plan p=planner_warm();
        uint64_t now=p.last_present_ns+gaps[i];
        CHECK(!fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE) && planner_empty(&p),
            "floor violation or long hitch clears samples, hysteresis and credit, never pays a catch-up burst");
        CHECK(!fg_adaptive_plan_outputs(&p,now+24000000,hz60,FG_ADAPTIVE_ACTIVE) &&
            !p.samples && !p.credit_ns,"first fresh guest after a hitch only anchors a new planner epoch");
    }
    const uint64_t clocks[]={0,7999999,10000000,11111111,18000001,UINT64_MAX};
    for (unsigned i=0;i<6;i++) {
        fg_adaptive_plan p=planner_warm();
        CHECK(!fg_adaptive_plan_outputs(&p,p.last_present_ns+24000000,clocks[i],FG_ADAPTIVE_ACTIVE) &&
            planner_empty(&p),"invalid/non60/non120 clock cannot retain source evidence or display credit");
    }
    for (unsigned kind=0;kind<3;kind++) {
        fg_adaptive_plan p=planner_warm();
        uint64_t now=kind==0 ? 0 : p.last_present_ns-(kind==2);
        CHECK(!fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE) && planner_empty(&p),
            "missing, duplicate and reversed guest anchors reset instead of earning elapsed output credit");
    }
    fg_adaptive_plan p=planner_warm();
    uint64_t now=p.last_present_ns+24000000;
    CHECK(!fg_adaptive_plan_outputs(&p,now,hz120,FG_ADAPTIVE_ACTIVE) &&
        p.last_present_ns==now && !p.samples && !p.credit_ns && !p.ceiling,
        "display-clock change reanchors without carrying old refresh-scaled credit");
    for (unsigned divisor=1;divisor<=2;divisor++) {
        p=planner_warm(); now=p.last_present_ns+hz60/divisor;
        CHECK(!fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE) &&
            p.last_present_ns==now && !p.samples && !p.credit_ns && !p.ceiling,
            "at/above display source rate resets immediately without waiting for three lower reads");
    }
    p=planner_warm();
    fg_adaptive_plan_reset(&p);
    CHECK(planner_empty(&p),"explicit epoch reset discards every planner field");
    fg_adaptive_policy simulation=pair(hz60,31000000);
    float before=NAN,after=NAN;
    CHECK(fg_adaptive_phase(&simulation,simulation.display_anchor_ns+9300000,0,&before),
        "simulation phase fixture begins at a measured .3 target");
    p=(fg_adaptive_plan){0}; now=second;
    fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE);
    uint64_t generated=0,elapsed=0;
    for (unsigned i=0;i<1000;i++) {
        uint64_t interval=i&1 ? 37000000 : 18000000;
        now+=interval; elapsed+=interval;
        generated+=fg_adaptive_plan_outputs(&p,now,hz60,FG_ADAPTIVE_ACTIVE);
        CHECK(generated<=elapsed/hz60-(i+1),
            "irregular actual guest intervals, not smoothed demand, enforce each cumulative display budget");
    }
    CHECK(fg_adaptive_phase(&simulation,simulation.display_anchor_ns+9300000,0,&after) &&
        before==after && near(after,.3),
        "EMA, count hysteresis and fractional credit never replace actual simulation-dt image phase");
}

static void pace_deadline_edges(void) {
    const uint64_t minimum=FG_ADAPTIVE_PACE_MIN_INTERVAL_NS;
    CHECK(fg_adaptive_pace_deadline(0,second,hz120)==second &&
        !fg_adaptive_pace_deadline(0,0,0),
        "first actual release starts at now without inventing prior pacing history");
    CHECK(2*hz120<minimum && fg_adaptive_pace_deadline(second,second+1,hz120)==second+minimum,
        "rounded120Hz physical intervals never raise real simulation rate above60");
    CHECK(fg_adaptive_pace_deadline(second,second+1000000,hz60)==second+minimum &&
        fg_adaptive_pace_deadline(second,second+1000000,8000000)==second+minimum &&
        fg_adaptive_pace_deadline(second,second+1000000,8650000)==second+minimum,
        "60Hz and both near120Hz clock bands share the strict60-real maximum");
    CHECK(fg_adaptive_pace_deadline(second,second+1,17300000)==second+17300000,
        "a physical display slower than60Hz remains the tighter real-frame cap");
    CHECK(fg_adaptive_pace_deadline(second,second+1,0)==second+minimum,
        "temporarily missing display clock does not remove the independent60-real cap");
    CHECK(fg_adaptive_pace_deadline(second,second+250000000,hz120)==second+250000000,
        "a completed GPU/texture stall releases at current readiness without paying an obsolete deadline");
    CHECK(fg_adaptive_pace_deadline(second,second-1,hz120)==second+minimum,
        "an earlier sampled now cannot bypass the last actual release interval");
    CHECK(fg_adaptive_pace_deadline(UINT64_MAX-10,UINT64_MAX-1,hz120)==UINT64_MAX &&
        fg_adaptive_pace_deadline(UINT64_MAX,1,hz60)==UINT64_MAX &&
        fg_adaptive_pace_deadline(1,1,UINT64_MAX)==UINT64_MAX,
        "release-plus-interval saturation never wraps into an immediately reusable deadline");
    CHECK(fg_adaptive_pace_deadline(UINT64_MAX-minimum,UINT64_MAX-1,hz120)==UINT64_MAX,
        "the exactly representable final minimum interval retains its endpoint");
}
/* Explicit consumer event model: all texture/GPU work has completed at ready;
 * the host sleep may overshoot; last_actual is committed only AFTER that
 * sleep. This exercises the production helper, not a real HLE/sleep/GPU path. */
static void pace_actual_release_sequences(void) {
    const uint64_t refreshes[]={hz60,hz120,8000000,8650000,16000000,17300000};
    for (unsigned lead=0;lead<=4;lead++) {
        uint64_t last_actual=0,ready=second;
        fg_adaptive_plan temporal={0};
        unsigned epoch=1;
        uint64_t first=fg_adaptive_pace_deadline(last_actual,ready,refreshes[0]);
        CHECK(first==ready,"empty startup pacing clock permits one first actual release");
        last_actual=first;
        for (unsigned frame=1;frame<=600;frame++) {
            uint64_t previous=last_actual;
            uint64_t refresh=refreshes[(frame/29+lead)%6];
            int stalled=frame%47==0;
            ready=previous+(stalled ? 220000000 : 1000000);
            if (frame%31==0) {
                epoch++;
                fg_adaptive_plan_reset(&temporal);
                CHECK(last_actual==previous && epoch>1,
                    "temporal epoch/reset cannot clear the independently retained actual-release clock");
            }
            uint64_t deadline=fg_adaptive_pace_deadline(last_actual,ready,refresh);
            CHECK(deadline>=ready && last_actual==previous,
                "pacing after all waits never changes release authority before sleeping");
            if (stalled) CHECK(deadline==ready,"stall recovery has no stale target or catch-up delay");
            else CHECK(deadline==previous+(refresh>FG_ADAPTIVE_PACE_MIN_INTERVAL_NS ?
                    refresh : FG_ADAPTIVE_PACE_MIN_INTERVAL_NS),
                "tiny1ms GPU/texture work and available buffer lead still obey a fresh full cap interval");
            uint64_t actual=deadline+(frame%13==0 ? 7000000 : 0); /* Host sleep overshoot. */
            CHECK(actual-previous>=FG_ADAPTIVE_PACE_MIN_INTERVAL_NS && actual-previous>=refresh,
                "every consecutive actual release is capped at60 and never exceeds physical display rate");
            last_actual=actual; /* Consumer commits time observed AFTER sleeping. */
            CHECK((uint64_t)frame*second<=60*(last_actual-first),
                "long-run real simulation release count cannot exceed60 despite headroom, resets or clock switches");
            uint64_t next=fg_adaptive_pace_deadline(last_actual,last_actual+1000000,hz120);
            CHECK(next==last_actual+FG_ADAPTIVE_PACE_MIN_INTERVAL_NS,
                "the next frame starts from actual post-stall/post-oversleep release, never banked intended targets");
        }
    }
}

int main(void) {
    phases(); irregular_and_passthrough(); display_anchor_mapping(); discontinuities(); slots(); real_wins_and_retirement();
    budget_groups(); budget_missing_and_invalid(); budget_ring_identity();
    planner_fractional_rates(); planner_hysteresis(); planner_resets_and_phase();
    pace_deadline_edges(); pace_actual_release_sequences();
    printf("framegen adaptive production-policy goldens: %u checks\n",checks);
    return 0;
}
