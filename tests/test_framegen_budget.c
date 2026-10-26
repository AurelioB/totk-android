/* Pure production policy: no Vulkan, Android, game data, clocks or device. */
#include "../games/totk/android/gfx/framegen_budget.h"
#include "../games/totk/android/gfx/framegen_budget_external.h"
#include "../games/totk/android/gfx/framegen_qcom_policy.h"
#include "../games/totk/android/gfx/framegen_budget_lsfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr,"framegen budget: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)

static void warm(fg_budget_policy *p, double real) {
    fg_budget_sample(p,1,real,1); fg_budget_seal(p,1,1);
    fg_budget_sample(p,2,real,1); fg_budget_seal(p,2,2);
}
static int near(double a, double b) { return fabs(a-b) < 1e-9; }

static void interval(fg_budget_policy *p, uint64_t frame, double real, double incremental) {
    uint64_t ticket=p->last_ticket+1;
    fg_budget_sample(p,ticket,real,1);
    if (incremental >= 0) {
        ticket++;
        fg_budget_sample(p,ticket,incremental,1);
        fg_budget_synthetic(p,ticket,ticket);
    }
    fg_budget_seal(p,frame,ticket);
}

static void accounting(void) {
    fg_budget_policy p={0};
    CHECK(fg_budget_admit(&p,1).status == FG_BUDGET_MISSING,"no measured real GPU means no admission");
    fg_budget_sample(&p,1,12,1); fg_budget_seal(&p,1,1);
    CHECK(fg_budget_admit(&p,2).status == FG_BUDGET_MISSING,"first endpoint is not a complete frame");
    fg_budget_sample(&p,2,5,1); fg_budget_sample(&p,3,7,1); fg_budget_seal(&p,2,3);
    fg_budget_decision d=fg_budget_admit(&p,3);
    CHECK(d.status == FG_BUDGET_ACCEPT && near(d.real_ms,12) && d.probe &&
          isnan(d.incremental_ms) && isnan(d.total_ms) && isnan(d.margin_ms),
          "fresh measured real permits an explicitly unestimated first probe");
    CHECK(d.real_samples == 1 && d.incremental_samples == 0 && d.newest_real_frame == 2,
          "bootstrap identifies its measured real and missing incremental evidence");
    CHECK(d.first == 2 && d.last == 3 && d.frame == 2,"sample retains exact full-frame ticket provenance");
    /* Source preservation, authority and compose already live in this 12 ms.
       Only the separately submitted synthetic ticket is added, exactly once. */
    fg_budget_sample(&p,4,12,1); fg_budget_sample(&p,5,6,1);
    fg_budget_synthetic(&p,5,5); /* sample is allowed to arrive before its label */
    fg_budget_seal(&p,3,5);
    d=fg_budget_completed(&p,3);
    CHECK(d.status == FG_BUDGET_ACCEPT && near(d.real_ms,12) && near(d.incremental_ms,6) && near(d.total_ms,18),
          "separate synthetic cost is not counted in the real GPU bracket");
    CHECK(near(d.margin_ms,1000.0/30.0-18),"margin reports distance to the real tick");
    CHECK(near(fg_budget_admit(&p,4).total_ms,18),"measured synthetic cost replaces probe reserve");
    fg_budget_synthetic(&p,7,8); /* labeling before completion is also valid */
    fg_budget_seal(&p,4,8);
    fg_budget_sample(&p,6,12,1); fg_budget_sample(&p,7,3,1);
    CHECK(fg_budget_completed(&p,4).status == FG_BUDGET_MISSING,"no output before its exact covering interval completes");
    fg_budget_sample(&p,8,3,1);
    d=fg_budget_completed(&p,4);
    CHECK(d.status == FG_BUDGET_ACCEPT && near(d.total_ms,18),"multi-slot synthetic ranges contribute their whole GPU cost");
    fg_budget_sample(&p,8,100,1);
    CHECK(near(fg_budget_completed(&p,4).total_ms,18),"late duplicate ticket cannot rewrite completed authority");
}

static void headroom(void) {
    fg_budget_policy p={0}; warm(&p,12);
    interval(&p,3,23,8);
    fg_budget_decision d=fg_budget_completed(&p,3);
    CHECK(d.status == FG_BUDGET_OVERLOAD && near(d.total_ms,31),"actual cost rejects a probe that exceeds target");
    d=fg_budget_admit(&p,4);
    CHECK(d.status == FG_BUDGET_OVERLOAD && !d.probe && near(d.total_ms,31),
          "fresh measured overbudget estimates reject fresh GPU work");
    interval(&p,4,12,9);
    d=fg_budget_admit(&p,5);
    CHECK(d.status == FG_BUDGET_OVERLOAD && near(d.real_ms,23) && near(d.incremental_ms,9),
          "small independent p90 samples retain their nearest-rank high costs");
    for (uint64_t frame=5; frame <= 8; frame++) interval(&p,frame,12,-1);
    d=fg_budget_admit(&p,9);
    CHECK(d.status == FG_BUDGET_ACCEPT && d.probe && isnan(d.incremental_ms),
          "stale incremental estimates permit measured-real recovery instead of a permanent overload gate");

    fg_budget_policy edge={0}; warm(&edge,20);
    interval(&edge,3,21,9);
    d=fg_budget_completed(&edge,3);
    CHECK(d.status == FG_BUDGET_ACCEPT && near(d.total_ms,30) &&
          near(d.margin_ms,1000.0/30.0-30),"actual 30 ms leaves the required 3.333 ms margin");
    d=fg_budget_admit(&edge,4);
    CHECK(d.status == FG_BUDGET_ACCEPT && !d.probe && near(d.total_ms,30),
          "exactly 30 ms measured pre-admission remains accepted");
    CHECK(fg_budget_fixed(4,4) && !fg_budget_fixed(4,4.00001) && !fg_budget_fixed(4,6),"explicit 4 ms stays strict");
    CHECK(fg_budget_fixed(10,9) && fg_budget_fixed(1000,40),"explicit override is independent of adaptive total");
    CHECK(!fg_budget_fixed(NAN,1) && !fg_budget_fixed(0,0) && !fg_budget_fixed(4,NAN) && !fg_budget_fixed(4,-1),
          "nonfinite or invalid override costs fail closed");
}

static void bootstrap(void) {
    fg_budget_policy p={0}; warm(&p,28.316);
    fg_budget_decision a=fg_budget_admit(&p,3), b=fg_budget_admit(&p,3);
    CHECK(a.status == FG_BUDGET_ACCEPT && b.status == FG_BUDGET_ACCEPT &&
          a.probe && b.probe && near(a.real_ms,28.316) && isnan(a.incremental_ms),
          "Thor 28.316 ms real bootstraps without an invented 10 ms deadlock gate");
    CHECK(!p.reservations && !p.probes && !p.last_probe,"prediction and capture queries consume no reservation");
    a=fg_budget_reserve(&p,3);
    CHECK(a.status == FG_BUDGET_ACCEPT && a.probe && p.reservations == 1 &&
          p.probes == 1 && p.last_probe == 3,"actual work start consumes exactly one probe");
    b=fg_budget_reserve(&p,3);
    CHECK(b.status == FG_BUDGET_OVERLOAD && b.reason == FG_BUDGET_REASON_RESERVED &&
          p.reservations == 1 && p.probes == 1,"duplicate work-start reservation cannot consume or run twice");
    CHECK(fg_budget_admit(&p,3).reason == FG_BUDGET_REASON_RESERVED,
          "a query after reservation cannot authorize duplicate work");
    for (uint64_t frame=3; frame < 32; frame++) {
        interval(&p,frame,28.316,-1);
        a=fg_budget_admit(&p,frame+1); b=fg_budget_admit(&p,frame+1);
        CHECK(a.status == FG_BUDGET_OVERLOAD && a.probe && b.reason == FG_BUDGET_REASON_PROBE_RATE &&
              a.retry_frame == 33,"unmeasured probes are rate limited in real ticks despite repeated queries");
    }
    interval(&p,32,28.316,-1);
    a=fg_budget_reserve(&p,33);
    CHECK(a.status == FG_BUDGET_ACCEPT && a.probe && p.probes == 2 && p.reservations == 2,
          "probe recovery occurs exactly 30 real ticks after the previous actual start");
    interval(&p,33,28.316,6);
    a=fg_budget_completed(&p,33);
    CHECK(a.status == FG_BUDGET_OVERLOAD && near(a.total_ms,34.316),
          "bootstrap never exempts its actual exact completed total from the 30 ms ceiling");
    a=fg_budget_admit(&p,34);
    CHECK(a.status == FG_BUDGET_OVERLOAD && !a.probe && near(a.incremental_ms,6),
          "a measured 6 ms incremental estimate replaces unknown bootstrap even when total is overloaded");

    fg_budget_policy full={0}; warm(&full,30);
    a=fg_budget_reserve(&full,3);
    CHECK(a.status == FG_BUDGET_OVERLOAD && a.reason == FG_BUDGET_REASON_REAL && !full.probes,
          "real GPU cost alone at 30 ms never permits an unestimated probe");
    interval(&full,3,31,-1);
    CHECK(fg_budget_admit(&full,4).reason == FG_BUDGET_REASON_REAL,
          "real GPU cost alone above target never permits an unestimated probe");

    fg_budget_policy measured={0}; warm(&measured,20);
    interval(&measured,3,20,6);
    a=fg_budget_admit(&measured,4);
    CHECK(a.status == FG_BUDGET_ACCEPT && !a.probe && near(a.total_ms,26) &&
          a.incremental_samples == 1 && a.incremental_frame == 3 &&
          a.newest_incremental_frame == 3 && a.incremental_first == 3 && a.incremental_last == 4,
          "a fresh measured 6 ms estimate has exact independent interval provenance");
    CHECK(fg_budget_reserve(&measured,4).status == FG_BUDGET_ACCEPT && !measured.probes,
          "normal measured admissions do not consume bootstrap probes");
    interval(&measured,4,20,6);
    CHECK(fg_budget_reserve(&measured,5).status == FG_BUDGET_ACCEPT && measured.reservations == 2,
          "measured work is not restricted by the 30-tick probe rate limit");
}

static void percentiles(void) {
    fg_budget_policy p={0};
    interval(&p,1,15,-1);
    for (uint64_t frame=2; frame <= 33; frame++)
        interval(&p,frame,frame == 10 ? 26 : 15,frame == 20 ? 18 : 6);
    fg_budget_decision d=fg_budget_admit(&p,34);
    CHECK(d.status == FG_BUDGET_ACCEPT && !d.probe && d.real_samples == 32 &&
          d.incremental_samples == 32 && near(d.real_ms,15) && near(d.incremental_ms,6) &&
          near(d.total_ms,21),"independent recent real and incremental p90s exclude isolated outliers");
    CHECK(fg_budget_completed(&p,10).status == FG_BUDGET_OVERLOAD &&
          fg_budget_completed(&p,20).status == FG_BUDGET_OVERLOAD,
          "p90 robustness does not average away exact completed-interval overloads");
    interval(&p,34,31,-1);
    for (uint64_t frame=35; frame <= 38; frame++) interval(&p,frame,15,-1);
    interval(&p,39,31,-1);
    d=fg_budget_admit(&p,40);
    CHECK(d.probe && near(d.real_ms,15) && d.status == FG_BUDGET_OVERLOAD &&
          d.reason == FG_BUDGET_REASON_REAL,"a newest real-alone overload still blocks a stale probe despite robust p90");

    fg_budget_policy rank={0}; interval(&rank,1,1,-1);
    for (uint64_t frame=2; frame <= 33; frame++) interval(&rank,frame,frame-1,33-frame);
    d=fg_budget_admit(&rank,34);
    CHECK(d.real_samples == 32 && d.incremental_samples == 32 &&
          near(d.real_ms,29) && near(d.incremental_ms,28),
          "nearest-rank p90 is rank ceil(0.9*32), not maximum, median or interpolated percentile");
    CHECK(d.frame == 30 && d.incremental_frame == 5 && d.first == 58 && d.last == 59 &&
          d.incremental_first == 8 && d.incremental_last == 9,
          "independent percentiles retain their distinct exact complete-ticket intervals");
    CHECK(d.status == FG_BUDGET_OVERLOAD,"sustained measured overload is not hidden by percentile estimation");
}

static void stale_incremental(void) {
    fg_budget_policy p={0}; warm(&p,22);
    CHECK(fg_budget_reserve(&p,3).status == FG_BUDGET_ACCEPT,"initial overload recovery probe starts");
    interval(&p,3,22,9);
    CHECK(fg_budget_admit(&p,4).status == FG_BUDGET_OVERLOAD,"fresh known 31 ms estimate blocks work");
    for (uint64_t frame=4; frame <= 6; frame++) interval(&p,frame,22,-1);
    fg_budget_decision d=fg_budget_admit(&p,7);
    CHECK(!d.probe && near(d.incremental_ms,9),"incremental evidence remains fresh through four real ticks");
    interval(&p,7,22,-1);
    d=fg_budget_admit(&p,8);
    CHECK(d.probe && d.reason == FG_BUDGET_REASON_PROBE_RATE && isnan(d.incremental_ms) &&
          d.incremental_samples == 1 && d.newest_incremental_frame == 3,
          "stale incremental evidence cannot masquerade as a fresh estimate or bypass probe rate limits");
    for (uint64_t frame=8; frame <= 32; frame++) interval(&p,frame,22,-1);
    d=fg_budget_reserve(&p,33);
    CHECK(d.status == FG_BUDGET_ACCEPT && d.probe,"known overbudget history eventually recovers via a stale incremental probe");
    interval(&p,33,22,6);
    CHECK(fg_budget_completed(&p,33).status == FG_BUDGET_ACCEPT,"a successful refresh still needs actual total under target");
    interval(&p,34,22,-1); interval(&p,35,22,-1);
    d=fg_budget_admit(&p,36);
    CHECK(d.status == FG_BUDGET_ACCEPT && !d.probe && near(d.incremental_ms,6) &&
          d.incremental_samples == 1,"expired overbudget history yields to the fresh successful incremental sample");
}

static void feedback(void) {
    fg_budget_policy p={0}; warm(&p,20); interval(&p,3,20,6);
    CHECK(!fg_budget_feedback(&p,1,1) && !p.feedback_misses && !p.cooldown_until,
          "base-game lateness without nearby generated work does not impose adaptive cooldown");
    CHECK(fg_budget_reserve(&p,4).status == FG_BUDGET_ACCEPT,"measured work reserves before presenter feedback");
    CHECK(fg_budget_feedback(&p,3,1) && p.feedback_backoff == 4 && p.cooldown_until == 9 &&
          p.feedback_misses == 1,"a correlated miss starts four real ticks of cooldown from current work");
    CHECK(!fg_budget_feedback(&p,3,1) && p.feedback_backoff == 4 && p.feedback_misses == 1,
          "duplicate real-present feedback never increases backoff");
    CHECK(!fg_budget_feedback(&p,4,0) && p.feedback_backoff == 4,
          "on-time feedback cannot cancel an active cooldown");
    for (uint64_t frame=4; frame <= 7; frame++) interval(&p,frame,20,6);
    fg_budget_decision d=fg_budget_admit(&p,8);
    CHECK(d.status == FG_BUDGET_OVERLOAD && d.reason == FG_BUDGET_REASON_FEEDBACK && d.retry_frame == 9,
          "fresh otherwise-underbudget work pauses until the real-tick deadline");
    interval(&p,8,20,6);
    CHECK(fg_budget_reserve(&p,9).status == FG_BUDGET_ACCEPT,"elapsed real ticks restore admission without any wall-clock sleep");
    CHECK(fg_budget_feedback(&p,8,1) && p.feedback_backoff == 8 && p.cooldown_until == 18,
          "a repeated correlated miss doubles bounded feedback backoff");
    for (uint64_t frame=9; frame <= 16; frame++) interval(&p,frame,20,6);
    CHECK(fg_budget_admit(&p,17).reason == FG_BUDGET_REASON_FEEDBACK,"repeated overload keeps its full longer pause");
    interval(&p,17,20,6);
    CHECK(fg_budget_admit(&p,18).status == FG_BUDGET_ACCEPT,"repeated-miss cooldown also recovers at its finite deadline");
    for (uint64_t frame=18; frame <= 21; frame++) {
        interval(&p,frame,20,6); fg_budget_feedback(&p,frame,0);
    }
    CHECK(!p.feedback_backoff && !fg_budget_feedback(&p,22,1) && p.feedback_misses == 2,
          "healthy real feedback relaxes old backoff and unrelated later misses do not restart it");

    for (int offset=-3; offset <= 2; offset++) {
        fg_budget_policy q={0}; warm(&q,20);
        uint64_t reserved=(uint64_t)(10+offset);
        for (uint64_t frame=3; frame < reserved; frame++) interval(&q,frame,20,6);
        CHECK(fg_budget_reserve(&q,reserved).status == FG_BUDGET_ACCEPT,"correlation fixture reserves exact generation frame");
        for (uint64_t frame=reserved; frame <= 12; frame++) interval(&q,frame,20,6);
        CHECK(fg_budget_feedback(&q,10,1) == (offset >= -2 && offset <= 1),
              "only exact reservations in real-frame minus two through plus one correlate");
    }

    fg_budget_policy bounded={0}; warm(&bounded,20); interval(&bounded,3,20,6);
    for (unsigned miss=0; miss < 8; miss++) {
        uint64_t frame=bounded.last_frame+1;
        CHECK(fg_budget_reserve(&bounded,frame).status == FG_BUDGET_ACCEPT,"bounded backoff fixture recovers before each reservation");
        CHECK(fg_budget_feedback(&bounded,frame,1) && bounded.feedback_backoff <= FG_BUDGET_FEEDBACK_MAX,
              "persistent correlated misses never exceed bounded real-tick backoff");
        uint64_t until=bounded.cooldown_until;
        while (bounded.last_frame+1 < until) interval(&bounded,bounded.last_frame+1,20,6);
    }
    CHECK(bounded.feedback_backoff == FG_BUDGET_FEEDBACK_MAX && bounded.feedback_misses == 8,
          "exponential cooldown saturates rather than growing without bound");

    fg_budget_policy overflow={0};
    interval(&overflow,UINT64_MAX-3,20,-1); interval(&overflow,UINT64_MAX-2,20,6);
    CHECK(fg_budget_reserve(&overflow,UINT64_MAX-1).status == FG_BUDGET_ACCEPT,
          "near-limit real-frame identities still reserve normally");
    CHECK(fg_budget_feedback(&overflow,UINT64_MAX,1) && overflow.cooldown_until == UINT64_MAX &&
          fg_budget_admit(&overflow,UINT64_MAX).reason == FG_BUDGET_REASON_FEEDBACK,
          "feedback correlation and cooldown saturate at frame-id limit without wrapping");
}

static void locked_feedback(void) {
    fg_budget_policy p={.feedback_gpu_only=1}; warm(&p,20); interval(&p,3,20,6);
    CHECK(fg_budget_reserve(&p,4).status == FG_BUDGET_ACCEPT,"locked under-budget pair reserves normally");
    CHECK(!fg_budget_feedback(&p,3,1) && !p.cooldown_until,
          "late CPU/pacing feedback with exact 26 ms GPU cost does not starve locked interpolation");
    CHECK(!fg_budget_feedback(&p,4,1) && !p.cooldown_until,
          "pending GPU evidence cannot prove overload");
    interval(&p,4,20,6);
    CHECK(fg_budget_reserve(&p,5).status == FG_BUDGET_ACCEPT,"healthy locked generation continues");
    interval(&p,5,25,6);
    CHECK(fg_budget_completed(&p,5).status == FG_BUDGET_OVERLOAD,
          "locked mode retains exact 30 ms completed-output ceiling");
    CHECK(fg_budget_feedback(&p,5,1) && p.feedback_misses == 1 && p.feedback_backoff == 4,
          "exact correlated measured GPU overload still pauses locked interpolation");
    CHECK(fg_budget_admit(&p,6).status == FG_BUDGET_OVERLOAD,
          "locked measured overload never bypasses fresh admission");
    CHECK(!fg_budget_feedback(&p,5,1) && p.feedback_misses == 1,
          "duplicate locked overload feedback remains idempotent");
    for (uint64_t frame=6;frame<=20;frame++) interval(&p,frame,20,6);
    CHECK(fg_budget_reserve(&p,21).status == FG_BUDGET_ACCEPT,
          "locked mode recovers after finite cooldown and fresh under-budget GPU samples");
    CHECK(!fg_budget_feedback(&p,20,0) && p.feedback_backoff == 2,
          "healthy post-cooldown real feedback relaxes locked backoff");
    fg_budget_policy stale={.feedback_gpu_only=1}; warm(&stale,20); interval(&stale,3,20,6);
    CHECK(fg_budget_reserve(&stale,4).status == FG_BUDGET_ACCEPT,"stale fixture reserves exact work");
    interval(&stale,4,25,6);
    for (uint64_t frame=5;frame<=9;frame++) interval(&stale,frame,20,6);
    CHECK(!fg_budget_feedback(&stale,4,1) && !stale.feedback_misses,
          "old same-frame overload outside fresh GPU horizon cannot pause current healthy work");
}

static void unavailable(void) {
    fg_budget_policy p={0}; warm(&p,12);
    CHECK(fg_budget_admit(&p,6).status == FG_BUDGET_ACCEPT,"a completed real sample is usable through four real ticks");
    CHECK(fg_budget_admit(&p,7).status == FG_BUDGET_MISSING,"older real samples cannot authorize fresh work");
    fg_budget_sample(&p,3,NAN,1); fg_budget_seal(&p,3,3);
    CHECK(fg_budget_admit(&p,4).status == FG_BUDGET_MISSING,"nonfinite newest real sample does not fall back to older data");
    fg_budget_sample(&p,4,12,1); fg_budget_seal(&p,4,4);
    CHECK(fg_budget_admit(&p,5).status == FG_BUDGET_ACCEPT,"fresh real frames recover without a generation-dependent warmup");
    fg_budget_sample(&p,5,12,0); fg_budget_seal(&p,5,5);
    CHECK(fg_budget_admit(&p,6).status == FG_BUDGET_MISSING,"unavailable timestamp query fails closed");
    fg_budget_sample(&p,7,12,1); fg_budget_seal(&p,6,7);
    CHECK(fg_budget_admit(&p,7).status == FG_BUDGET_MISSING,"missing ticket inside an interval is not a measured subset");
    fg_budget_sample(&p,8,-1,1); fg_budget_seal(&p,7,8);
    CHECK(fg_budget_admit(&p,8).status == FG_BUDGET_MISSING,"negative elapsed values fail closed");
    fg_budget_sample(&p,9,0,1); fg_budget_seal(&p,8,9);
    CHECK(fg_budget_admit(&p,9).status == FG_BUDGET_MISSING,"zero complete-frame cost is not real-frame authority");
    fg_budget_seal(&p,9,9);
    CHECK(fg_budget_admit(&p,10).status == FG_BUDGET_MISSING,"a frame without a new submission has no fresh timing");
    fg_budget_sample(&p,10,12,1); fg_budget_synthetic(&p,9,10); fg_budget_seal(&p,10,10);
    CHECK(fg_budget_admit(&p,11).status == FG_BUDGET_MISSING,"synthetic labels may not touch an already sealed frame");
    CHECK(fg_budget_completed(&p,2).status == FG_BUDGET_MISSING,"a real-only interval cannot certify a generated output");

    fg_budget_policy overwritten={0}; warm(&overwritten,12);
    fg_budget_seal(&overwritten,3,3); /* pending frame's sample is overwritten before it arrives */
    fg_budget_sample(&overwritten,3+FG_BUDGET_SLOTS,12,1);
    CHECK(fg_budget_admit(&overwritten,4).status == FG_BUDGET_MISSING,"ring ticket mismatch cannot alias an older output");
    fg_budget_seal(&overwritten,4,3+FG_BUDGET_SLOTS);
    CHECK(fg_budget_admit(&overwritten,5).status == FG_BUDGET_MISSING,"oversized/incomplete intervals stay unavailable");

    fg_budget_policy longrun={0}; warm(&longrun,12);
    for (uint64_t frame=3; frame <= 80; frame++) {
        uint64_t real=frame*2-3, synthetic=real+1;
        fg_budget_sample(&longrun,real,12,1); fg_budget_sample(&longrun,synthetic,6,1);
        fg_budget_synthetic(&longrun,synthetic,synthetic); fg_budget_seal(&longrun,frame,synthetic);
    }
    CHECK(fg_budget_completed(&longrun,3).status == FG_BUDGET_MISSING,"overwritten frame identity cannot certify an old output");
    fg_budget_decision d=fg_budget_admit(&longrun,81);
    CHECK(d.status == FG_BUDGET_ACCEPT && d.real_samples == 32 && d.incremental_samples == 32 &&
          near(d.total_ms,18),"bounded rings retain full p90 windows through ticket and frame wraps");
    CHECK(fg_budget_reserve(&longrun,81).status == FG_BUDGET_ACCEPT,"wrapped policy reserves fresh exact work");
    for (uint64_t frame=81; frame <= 150; frame++) {
        interval(&longrun,frame,12,6);
        CHECK(fg_budget_reserve(&longrun,frame+1).status == FG_BUDGET_ACCEPT,
              "reservation rings retain per-frame ownership over repeated wraps");
    }
    CHECK(!fg_budget_feedback(&longrun,81,1) && !longrun.feedback_misses,
          "overwritten reservation identities never alias old real feedback");
}

static void external_interval(fg_budget_policy *p,fg_budget_external *e,uint64_t frame) {
    uint64_t ticket=p->last_ticket+1;
    fg_budget_external_sample(p,e,ticket,23,1);
    fg_budget_external_sample(p,e,ticket+1,1,1);
    fg_budget_synthetic(p,ticket+1,ticket+1);
    fg_budget_external_seal(p,e,frame,ticket+1);
}
static void external_gpu(void) {
    fg_budget_policy p={0}; fg_budget_external e={0}; warm(&p,23);
    fg_budget_external_begin(&e,3);
    external_interval(&p,&e,3);
    CHECK(fg_budget_completed(&p,3).status==FG_BUDGET_MISSING,
        "Vulkan completion alone cannot certify pending GL work");
    fg_budget_external_end(&p,&e,3,5,1);
    fg_budget_decision d=fg_budget_completed(&p,3);
    CHECK(d.status==FG_BUDGET_ACCEPT && near(d.real_ms,23) &&
        near(d.incremental_ms,6) && near(d.total_ms,29),
        "GL duration is charged once alongside disjoint Vulkan real/synthetic brackets");
    fg_budget_external_end(&p,&e,3,100,1);
    CHECK(near(fg_budget_completed(&p,3).total_ms,29),
        "late duplicate GL queries cannot rewrite finalized cost");
    fg_budget_external_begin(&e,4);
    fg_budget_external_end(&p,&e,4,8,1);
    external_interval(&p,&e,4);
    CHECK(fg_budget_completed(&p,4).status==FG_BUDGET_OVERLOAD &&
        near(fg_budget_completed(&p,4).total_ms,32),
        "GL timing may precede frame sealing and still enforces the 30ms total ceiling");
    fg_budget_external_begin(&e,5);
    external_interval(&p,&e,5);
    fg_budget_external_end(&p,&e,5,NAN,0);
    CHECK(fg_budget_completed(&p,5).status==FG_BUDGET_MISSING,
        "missing/disjoint/timed-out GL timing is not silently counted as zero");
    external_interval(&p,&e,6);
    CHECK(fg_budget_completed(&p,6).status==FG_BUDGET_ACCEPT &&
        near(fg_budget_completed(&p,6).total_ms,24),
        "camera fallback recovers without carrying a failed GL sample into future frames");
    for (uint64_t frame=7;frame<80;frame++) {
        fg_budget_external_begin(&e,frame);
        external_interval(&p,&e,frame);
        fg_budget_external_end(&p,&e,frame,2,1);
        CHECK(near(fg_budget_completed(&p,frame).total_ms,26),
            "bounded external timing identities survive repeated frame-ring wraps");
    }
    fg_budget_external_end(&p,&e,3,100,1);
    CHECK(near(fg_budget_completed(&p,79).total_ms,26),
        "an overwritten external frame identity cannot alter a current cost");
}

static void qcom_deadlines(void) {
    for (unsigned kind=0;kind<FG_QCOM_FAILURE_KINDS;kind++) {
        fg_qcom_policy p={0};
        CHECK(!fg_qcom_timing_failure(&p,kind,0) && p.startup_ignored==1,"pre-scene failures never permanently refuse QCOM");
        for (unsigned i=0;i<FG_QCOM_WARM_TICKS-1;i++) {
            fg_qcom_tick(&p,1);
            CHECK(!fg_qcom_timing_failure(&p,kind,0) && !p.misses,"startup deadline/timing/slow-span incidents never escalate");
        }
        CHECK(p.successes==299 && p.startup_ignored==300,"startup includes the observed 271-scene-tick Odin timeout");
        p=(fg_qcom_policy){0};
        for (unsigned i=0;i<FG_QCOM_WARM_TICKS;i++) fg_qcom_tick(&p,1);
        CHECK(!fg_qcom_timing_failure(&p,kind,0) && p.misses==1 && p.backoff==4,"first warm incident starts four-tick camera cooldown");
        CHECK(!fg_qcom_can_generate(&p) && !fg_qcom_timing_failure(&p,kind,0) && p.misses==1,
            "cooldown refuses generation and coalesces delayed callbacks");
        while (p.tick<p.cooldown_until) fg_qcom_tick(&p,1);
        CHECK(fg_qcom_can_generate(&p),"generation may resume after cooldown, once actual resources also retire");
        for (unsigned i=1;i<FG_QCOM_MISS_LIMIT;i++) {
            CHECK(fg_qcom_timing_failure(&p,(kind+i)%FG_QCOM_FAILURE_KINDS,0)==(i+1==FG_QCOM_MISS_LIMIT),
                "only sustained mixed post-warmup failures permanently refuse");
            while (p.tick<p.cooldown_until) fg_qcom_tick(&p,1);
        }
        p=(fg_qcom_policy){0};
        for (unsigned i=0;i<FG_QCOM_WARM_TICKS;i++) fg_qcom_tick(&p,1);
        CHECK(!fg_qcom_timing_failure(&p,kind,0),"isolated incident is recoverable");
        for (unsigned i=0;i<FG_QCOM_MISS_WINDOW;i++) fg_qcom_tick(&p,1);
        CHECK(!p.misses && !p.backoff && fg_qcom_can_generate(&p),"clean window clears escalation");
        CHECK(!fg_qcom_timing_failure(&p,kind,0) && p.misses==1,"old isolated failures cannot accumulate forever");
    }
}
static void qcom_adaptive_timing(void) {
    const unsigned backoff[]={4,8,16,30,30};
    for (unsigned first=0;first<FG_QCOM_FAILURE_KINDS;first++) {
        fg_qcom_policy p={0}; fg_qcom_job job={0};
        _Atomic int refused=0,suspended=0;
        for (unsigned i=0;i<FG_QCOM_WARM_TICKS;i++) fg_qcom_tick(&p,1);
        for (unsigned cycle=0;cycle<3;cycle++) {
            if (cycle) {
                uint64_t clean_until=p.window+FG_QCOM_MISS_WINDOW;
                while (p.tick<clean_until) fg_qcom_tick(&p,1);
                CHECK(!p.misses && !p.backoff,"adaptive clean window preserves the original escalation reset");
            }
            for (unsigned i=0;i<FG_QCOM_MISS_LIMIT;i++) {
                unsigned kind=(first+i)%FG_QCOM_FAILURE_KINDS;
                uint64_t incidents=cycle*FG_QCOM_MISS_LIMIT+i+1,frame=1000+incidents;
                CHECK(fg_qcom_job_begin(&job,1,2),"each missed adaptive output acquires real GL and Vulkan leases");
                atomic_store(&suspended,1);
                int permanent=fg_qcom_timing_failure(&p,kind,1);
                if (permanent) (void)fg_qcom_refuse(&refused);
                CHECK(!permanent && !fg_qcom_refused(&refused) && p.misses==i+1 &&
                    p.backoff==backoff[i] && p.timing_cooldowns==incidents,
                    "every mixed post-warm five-miss cycle keeps bounded 4/8/16/30 cooldown without permanent adaptive refusal");
                CHECK(!fg_qcom_can_generate(&p) &&
                    !fg_qcom_admitted(&refused,1,atomic_load(&suspended),1,0),
                    "recoverable timing still rejects generation throughout its cooldown");
                CHECK(!fg_qcom_timing_failure(&p,kind,1) && p.misses==i+1 &&
                    p.timing_cooldowns==incidents,
                    "delayed timing callbacks coalesce without counting another cooldown or reprobe");
                while (p.tick<p.cooldown_until) {
                    CHECK(!fg_qcom_rearm(&refused,&suspended,1,!fg_qcom_can_generate(&p),0,0),
                        "drained resources cannot rearm before the exact existing cooldown expires");
                    fg_qcom_tick(&p,1);
                }
                CHECK(fg_qcom_can_generate(&p) && !fg_qcom_job_available(&job) &&
                    fg_qcom_job_reads_source(&job,1),
                    "cooldown expiry permits policy reprobing but never retires pending GPU work");
                CHECK(!fg_qcom_rearm(&refused,&suspended,1,0,0,1) &&
                    !fg_qcom_rearm(&refused,&suspended,1,0,1,0) &&
                    !fg_qcom_rearm(&refused,&suspended,0,0,0,0),
                    "rearm still requires retired external costs, no quarantine and completed GL warmup");
                fg_qcom_job_gl_complete(&job);
                CHECK(!fg_qcom_job_available(&job),"timing recovery cannot discard the independent Vulkan output lease");
                fg_qcom_job_vk_complete(&job);
                CHECK(fg_qcom_job_available(&job) &&
                    fg_qcom_rearm(&refused,&suspended,1,0,0,0) &&
                    fg_qcom_admitted(&refused,1,atomic_load(&suspended),!fg_qcom_can_generate(&p),0),
                    "actual production rearm restores can-generate after cooldown and resource retirement, including the fifth miss");
                CHECK(fg_qcom_job_begin(&job,2,3) && fg_qcom_timing_reprobe(&p,frame,0) &&
                    !fg_qcom_timing_reprobe(&p,frame+1,1) && p.timing_reprobes==incidents,
                    "only an actual first dispatch after cooldown counts one adaptive timing reprobe");
                CHECK(!fg_qcom_timing_recovered(&p,frame-1,0) &&
                    !fg_qcom_timing_recovered(&p,frame,1) && p.timing_recoveries==incidents-1 &&
                    fg_qcom_timing_recovered(&p,frame,0) &&
                    !fg_qcom_timing_recovered(&p,frame,0) && p.timing_recoveries==incidents,
                    "recovery counts the exact frame/slot completion once, never an older output or another child of this frame");
                fg_qcom_job_gl_complete(&job); fg_qcom_job_vk_complete(&job);
            }
            CHECK(p.misses==FG_QCOM_MISS_LIMIT && fg_qcom_can_generate(&p) &&
                fg_qcom_admitted(&refused,1,atomic_load(&suspended),0,0),
                "repeated complete five-miss windows cannot starve adaptive QCOM permanently");
        }
        CHECK(p.failures[FG_QCOM_DEADLINE] && p.failures[FG_QCOM_TIMING] &&
            p.failures[FG_QCOM_SLOW] && p.timing_cooldowns==15 &&
            p.timing_reprobes==15 && p.timing_recoveries==15,
            "production counters retain all timing kinds, cooldowns, actual reprobes and exact recoveries across windows");
    }
}
static void lsfg_recovery(void) {
    /* LSFG keeps the shared deadline recovery policy. Same-device source
       retirement, not a timestamp or a cooldown, authorizes descriptor reuse. */
    fg_qcom_policy p={0};
    for (unsigned i=0;i<271;i++) fg_qcom_tick(&p,1);
    CHECK(!fg_qcom_failure(&p,FG_QCOM_TIMING) && p.startup_ignored==1 && !p.misses,
        "LSFG missing/timeout timing at scene271 enters camera cooldown, not permanent refusal");
    CHECK(!fg_qcom_can_generate(&p) && p.backoff==4,
        "LSFG startup failures still reject generation for the shared four-tick cooldown");
    while (p.successes<FG_QCOM_WARM_TICKS) fg_qcom_tick(&p,1);
    const unsigned kinds[]={FG_QCOM_DEADLINE,FG_QCOM_TIMING,FG_QCOM_SLOW,FG_QCOM_TIMING,FG_QCOM_DEADLINE};
    const unsigned backoff[]={8,16,30,30,30};
    for (unsigned i=0;i<FG_QCOM_MISS_LIMIT;i++) {
        CHECK(fg_qcom_failure(&p,kinds[i])==(i+1==FG_QCOM_MISS_LIMIT),
            "LSFG only permanently refuses five mixed post-warm incidents in120 ticks");
        CHECK(p.misses==i+1 && p.backoff==backoff[i],
            "LSFG timing/slow/deadline incidents share bounded escalating cooldown");
        CHECK(!fg_qcom_can_generate(&p) && !fg_qcom_failure(&p,kinds[i]) && p.misses==i+1,
            "LSFG delayed callbacks during cooldown cannot double-escalate one incident");
        while (p.tick<p.cooldown_until) fg_qcom_tick(&p,1);
    }
    CHECK(p.failures[FG_QCOM_DEADLINE] && p.failures[FG_QCOM_TIMING] && p.failures[FG_QCOM_SLOW],
        "LSFG recovery preserves categorized deadline/timing/slow diagnostics");
    for (unsigned i=0;i<FG_QCOM_MISS_WINDOW;i++) fg_qcom_tick(&p,1);
    CHECK(!p.misses && !p.backoff && fg_qcom_can_generate(&p),
        "LSFG clean window clears transient policy state, without authorizing resource reuse");
}

static void lsfg_admission(void) {
    fg_budget_policy p={0}; warm(&p,12);
    interval(&p,3,27,2);
    interval(&p,4,20,8);
    CHECK(fg_budget_admit(&p,5).status==FG_BUDGET_OVERLOAD,
        "camera independent percentiles combine unrelated heavy real/present costs");
    CHECK(fg_budget_lsfg_admit(&p,5).status==FG_BUDGET_ACCEPT &&
        near(fg_budget_lsfg_admit(&p,5).total_ms,29),
        "LSFG correlated engine-in-real totals count model once without unrelated p90 sums");
    interval(&p,5,31,0.1);
    CHECK(fg_budget_lsfg_admit(&p,6).status==FG_BUDGET_OVERLOAD,
        "LSFG newest real outlier cannot evade the real deadline guard");
    for (unsigned f=6;f<=14;f++) interval(&p,f,18,1);
    CHECK(fg_budget_lsfg_reserve(&p,15).status==FG_BUDGET_ACCEPT &&
        fg_budget_lsfg_reserve(&p,15).reason==FG_BUDGET_REASON_RESERVED,
        "LSFG eight-tick model history expires old heavy costs and reserves once");
    CHECK(fg_budget_completed(&p,5).status==FG_BUDGET_OVERLOAD,
        "LSFG actual completion retains unchanged strict 30ms ceiling");
    p.cooldown_until=20;
    CHECK(fg_budget_lsfg_admit(&p,16).reason==FG_BUDGET_REASON_FEEDBACK,
        "LSFG correlated statistics cannot bypass deadline feedback");
    p.frames[14%FG_BUDGET_FRAMES].status=FG_BUDGET_UNAVAILABLE;
    CHECK(fg_budget_lsfg_admit(&p,16).status==FG_BUDGET_MISSING,
        "LSFG never substitutes older valid samples for missing newest authority");
}

static unsigned qcom_calls;
static float qcom_observed;
static void qcom_callback(unsigned previous,unsigned current,unsigned output,float phase) {
    CHECK(previous==7 && current==11 && output==13,"QCOM callback receives exact texture identities");
    qcom_calls++; qcom_observed=phase;
}
static void qcom_phases(void) {
    const float phases[]={0.0f,nextafterf(0.0f,1.0f),.125f,.3f,.5f,.75f,nextafterf(1.0f,0.0f),1.0f};
    for (unsigned i=0;i<sizeof phases/sizeof phases[0];i++) {
        CHECK(fg_qcom_extrapolate(qcom_callback,7,11,13,phases[i]),"QCOM accepts every in-range finite phase");
        CHECK(!memcmp(&qcom_observed,&phases[i],sizeof(float)) && qcom_calls==i+1,
            "QCOM forwards endpoint/near-endpoint/nonmidpoint/midpoint float bits without quantization");
    }
    const float invalid[]={NAN,INFINITY,-INFINITY,-.001f,nextafterf(0.0f,-1.0f),nextafterf(1.0f,2.0f)};
    for (unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++)
        CHECK(!fg_qcom_extrapolate(qcom_callback,7,11,13,invalid[i]),"QCOM rejects invalid phase before dispatch");
    CHECK(qcom_calls==sizeof phases/sizeof phases[0] &&
        !fg_qcom_extrapolate(NULL,7,11,13,.5f),"refusal never invokes the GL callback or fallback");
    CHECK(fg_qcom_requested("qcom","locked") && fg_qcom_requested("qcom","adaptive"),
        "QCOM permits exact legacy locked and adaptive selection");
    const char *modes[]={NULL,"","interp","hybrid","camera","Adaptive","adaptive1","locked1"};
    for (unsigned i=0;i<sizeof modes/sizeof modes[0];i++)
        CHECK(!fg_qcom_requested("qcom",modes[i]),"QCOM does not relax non-adaptive legacy mode gates");
    CHECK(!fg_qcom_requested(NULL,"adaptive") && !fg_qcom_requested("QCOM","adaptive") &&
        !fg_qcom_requested("camera","adaptive") && !fg_qcom_requested("lsfg","adaptive"),
        "adaptive external engine remains exact and opt-in");
    CHECK(fg_qcom_adaptive_device("AYN Thor","kalama",0x5143,"Turnip Adreno (TM) 740"),
        "qualified Thor identity and exact A740 renderer are admitted");
    CHECK(!fg_qcom_adaptive_device("Odin 3","sun",0x5143,"Turnip Adreno (TM) 830") &&
        !fg_qcom_adaptive_device("Odin 3","kalama",0x5143,"Turnip Adreno (TM) 740") &&
        !fg_qcom_adaptive_device("AYN Thor","kalama",0x5143,"Turnip Adreno (TM) 830") &&
        !fg_qcom_adaptive_device("AYN Thor","unknown",0x5143,"Turnip Adreno (TM) 740") &&
        !fg_qcom_adaptive_device("AYN Thor","kalama",0,"Turnip Adreno (TM) 740") &&
        !fg_qcom_adaptive_device(NULL,"kalama",0x5143,"Turnip Adreno (TM) 740") &&
        !fg_qcom_adaptive_device("AYN Thor",NULL,0x5143,"Turnip Adreno (TM) 740") &&
        !fg_qcom_adaptive_device("AYN Thor","kalama",0x5143,NULL),
        "Odin, other A740 devices and absent/unqualified identity refuse before GL admission");
}

typedef struct { unsigned entry,egl_create,afme_lookup; } qcom_init_trace;
static int qcom_initialize_entry(void *context) {
    qcom_init_trace *trace=context;
    trace->entry++;
    /* Injected worker entry: these are the two forbidden initialization
       stages, not can_generate. Production passes its real EGL/GL init here. */
    trace->egl_create++;
    trace->afme_lookup++;
    return 1;
}
static void qcom_initialization(void) {
    _Atomic int permanent=0;
    const struct { const char *model,*device,*gpu; uint32_t vendor; } refused[]={
        {"Odin 3","sun","Turnip Adreno (TM) 830",0x5143},
        {"Odin 3","kalama","Turnip Adreno (TM) 740",0x5143},
        {"unknown","kalama","Turnip Adreno (TM) 740",0x5143},
        {"AYN Thor","unknown","Turnip Adreno (TM) 740",0x5143},
        {"AYN Thor","kalama","Turnip Adreno (TM) 740",0},
        {NULL,NULL,NULL,0}
    };
    for (unsigned i=0;i<sizeof refused/sizeof refused[0];i++) {
        qcom_init_trace trace={0};
        CHECK(!fg_qcom_initialize(&permanent,1,refused[i].model,refused[i].device,refused[i].vendor,
            refused[i].gpu,qcom_initialize_entry,&trace),
            "unqualified adaptive device refuses actual worker initialization");
        CHECK(!trace.entry && !trace.egl_create && !trace.afme_lookup,
            "refused Odin/unknown never enters EGL creation or AFME symbol lookup");
    }
    qcom_init_trace trace={0};
    CHECK(fg_qcom_initialize(&permanent,1,"AYN Thor","kalama",0x5143,"Turnip Adreno (TM) 740",
        qcom_initialize_entry,&trace) && trace.entry==1 && trace.egl_create==1 && trace.afme_lookup==1,
        "qualified Thor reaches the same actual initialization entry exactly once");
    trace=(qcom_init_trace){0};
    CHECK(fg_qcom_initialize(&permanent,0,"Odin 3","sun",0x5143,"Turnip Adreno (TM) 830",
        qcom_initialize_entry,&trace) && trace.entry==1,
        "new adaptive safety admission does not change legacy locked initialization");
}
static void qcom_permanent_refusal(void) {
    for (int adaptive=0;adaptive<=1;adaptive++) {
        fg_qcom_policy p={0};
        _Atomic int refused=0,suspended=1;
        qcom_init_trace trace={0};
        for (unsigned i=0;i<FG_QCOM_WARM_TICKS;i++) fg_qcom_tick(&p,1);
        CHECK(fg_qcom_admitted(&refused,1,0,0,0) &&
            fg_qcom_refuse(&refused) && !fg_qcom_refuse(&refused),
            "the production capability/import/fence/corruption refusal transition latches exactly once in either mode");
        CHECK(!fg_qcom_initialize(&refused,adaptive,"AYN Thor","kalama",0x5143,
            "Turnip Adreno (TM) 740",qcom_initialize_entry,&trace) &&
            !trace.entry && !trace.egl_create && !trace.afme_lookup,
            "even a qualified Thor cannot enter actual EGL/AFME initialization after permanent refusal");
        for (unsigned i=0;i<FG_QCOM_MISS_LIMIT*3;i++) {
            int permanent=fg_qcom_timing_failure(&p,i%FG_QCOM_FAILURE_KINDS,adaptive);
            if (permanent) (void)fg_qcom_refuse(&refused);
            while (p.tick<p.cooldown_until) fg_qcom_tick(&p,1);
            CHECK(fg_qcom_can_generate(&p) && fg_qcom_refused(&refused) &&
                !fg_qcom_rearm(&refused,&suspended,1,0,0,0) && atomic_load(&suspended) &&
                !fg_qcom_admitted(&refused,1,0,0,0),
                "recoverable timing policy and fully drained rearm cannot clear the real permanent admission latch");
        }
        for (unsigned i=0;i<FG_QCOM_MISS_WINDOW;i++) fg_qcom_tick(&p,1);
        CHECK(!p.misses && !p.backoff && fg_qcom_refused(&refused) &&
            !fg_qcom_admitted(&refused,1,0,0,0) &&
            !fg_qcom_initialize(&refused,adaptive,"AYN Thor","kalama",0x5143,
                "Turnip Adreno (TM) 740",qcom_initialize_entry,&trace) && !trace.entry,
            "clean windows reset timing escalation only, never permanent refusal or its no-GL admission boundary");
    }
}
static void qcom_inflight_jobs(void) {
    fg_qcom_job jobs[2]={{0}};
    CHECK(fg_qcom_job_begin(&jobs[0],1,2) && fg_qcom_job_begin(&jobs[1],1,2),
        "two fixed output slots retain simultaneous jobs from the same immutable pair");
    CHECK(!fg_qcom_job_begin(&jobs[0],2,3) && !fg_qcom_job_begin(&jobs[1],2,3) &&
        fg_qcom_job_reads_source(&jobs[0],1) && fg_qcom_job_reads_source(&jobs[1],2),
        "in-flight jobs cannot overwrite their output buffers, descriptors, queries or inputs");
    fg_qcom_job_vk_complete(&jobs[0]); /* canceled presentation; GL not harvested */
    CHECK(!fg_qcom_job_available(&jobs[0]) && !fg_qcom_job_begin(&jobs[0],2,3) &&
        fg_qcom_job_reads_source(&jobs[0],1),
        "covering Vulkan completion alone cannot retire an outstanding GL writer/input reader");
    fg_qcom_job_gl_complete(&jobs[1]); /* GL first; Vulkan consumer still owns output */
    CHECK(!fg_qcom_job_available(&jobs[1]) && !fg_qcom_job_begin(&jobs[1],2,3) &&
        !fg_qcom_job_reads_source(&jobs[1],2),
        "GL completion alone cannot reuse output while its Vulkan reader or semaphore use remains");
    fg_qcom_job_gl_complete(&jobs[0]);
    CHECK(fg_qcom_job_available(&jobs[0]) && fg_qcom_job_begin(&jobs[0],2,3) &&
        !fg_qcom_job_available(&jobs[1]),
        "only the exact slot whose GL and Vulkan owners both retired may start its next job");
    fg_qcom_job_vk_complete(&jobs[1]);
    CHECK(fg_qcom_job_available(&jobs[1]) && fg_qcom_job_begin(&jobs[1],2,3),
        "reverse-order retirement releases the second bounded slot independently");
    fg_qcom_job_quarantine(&jobs[0]);
    fg_qcom_job_vk_complete(&jobs[0]); fg_qcom_job_gl_complete(&jobs[0]);
    CHECK(!fg_qcom_job_available(&jobs[0]) && !fg_qcom_job_begin(&jobs[0],0,1) &&
        fg_qcom_job_reads_source(&jobs[0],2),
        "failed post-dispatch handoff never fabricates completion or recycles quarantined private pixels");
    for (unsigned i=0;i<1000;i++) {
        fg_qcom_job_gl_complete(&jobs[1]); fg_qcom_job_vk_complete(&jobs[1]);
        CHECK(fg_qcom_job_begin(&jobs[1],i%4,(i+1)%4),
            "arbitrarily many generations reuse one fixed slot only after both completion authorities");
    }
}

/* TOTK_FG_ADAPTIVE_BUDGET=1 policy (adaptive slot scheduler only). */
static int same_decision(fg_budget_decision a, fg_budget_decision b) { return !memcmp(&a,&b,sizeof a); }
/* Frozen r7 reservation body, so the shared helper refactor is pinned exactly. */
static fg_budget_decision r7_reserve(fg_budget_policy *p, uint64_t frame) {
    fg_budget_decision d=fg_budget_admit(p,frame);
    if (d.status != FG_BUDGET_ACCEPT) return d;
    p->reserved_frames[frame % FG_BUDGET_FRAMES]=frame;
    p->last_reserved=frame; p->reservations++;
    if (d.probe) { p->last_probe=frame; p->probes++; }
    return d;
}
static void gap_policy(fg_budget_policy *p, double real, double incremental, uint64_t last) {
    *p=(fg_budget_policy){0}; warm(p,real); interval(p,3,real,incremental);
    for (uint64_t frame=4; frame <= last; frame++) interval(p,frame,real,-1);
}
static void adaptive_budget_stale(void) {
    /* r7 shape: one measured child, then a >4-tick gap; r7 refuses probe-rate. */
    fg_budget_policy p; gap_policy(&p,20,4,8); p.last_probe=5;
    fg_budget_decision r7=fg_budget_admit(&p,9), d=fg_budget_admit_stale(&p,9);
    CHECK(r7.probe && r7.reason == FG_BUDGET_REASON_PROBE_RATE && !r7.stale_incremental &&
          r7.incremental_samples == 1 && r7.newest_incremental_frame == 3,
          "r7: a six-tick-old measured incremental forces a rate-limited unestimated probe");
    CHECK(d.status == FG_BUDGET_ACCEPT && d.reason == FG_BUDGET_REASON_NONE && !d.probe &&
          d.stale_incremental && near(d.real_ms,20) && near(d.incremental_ms,4) && near(d.total_ms,24) &&
          near(d.margin_ms,1000.0/30.0-24) && d.incremental_frame == 3 && !d.retry_frame,
          "selector: the in-window measured incremental replaces the probe under the same ceiling");
    fg_budget_policy q=p;
    d=fg_budget_reserve_stale(&q,9);
    CHECK(d.status == FG_BUDGET_ACCEPT && q.reservations == p.reservations+1 && q.last_reserved == 9 &&
          q.reserved_frames[9 % FG_BUDGET_FRAMES] == 9 && q.probes == p.probes && q.last_probe == 5,
          "a stale-estimate reservation is not a probe and never consumes the probe rate");
    CHECK(fg_budget_admit_stale(&q,9).reason == FG_BUDGET_REASON_RESERVED,
          "an existing reservation still blocks duplicate work");

    fg_budget_policy edge; gap_policy(&edge,26,4,8);
    d=fg_budget_admit_stale(&edge,9);
    CHECK(d.status == FG_BUDGET_ACCEPT && near(d.total_ms,30),"the 30 ms ceiling stays inclusive");
    fg_budget_policy over; gap_policy(&over,27,4,8);
    r7=fg_budget_admit(&over,9);
    CHECK(r7.status == FG_BUDGET_ACCEPT && r7.probe,"r7 would start an unestimated probe at 27 ms real");
    d=fg_budget_reserve_stale(&over,9);
    CHECK(d.status == FG_BUDGET_OVERLOAD && d.reason == FG_BUDGET_REASON_STALE_ESTIMATE &&
          d.stale_incremental && near(d.total_ms,31) && !over.reservations && !over.probes && !over.last_probe,
          "a measured over-ceiling total refuses instead of probing and reserves nothing");

    fg_budget_policy edge_age; gap_policy(&edge_age,20,4,34);
    d=fg_budget_admit_stale(&edge_age,35);
    CHECK(d.stale_incremental && d.status == FG_BUDGET_ACCEPT && d.newest_incremental_frame == 3,
          "a sample 32 intervals old is still inside the p90 window");
    fg_budget_policy old; gap_policy(&old,20,4,35);
    r7=fg_budget_admit(&old,36); d=fg_budget_admit_stale(&old,36);
    CHECK(!r7.incremental_samples && r7.probe && same_decision(r7,d),
          "without an in-window incremental sample the original probe decision is unchanged");
    d=fg_budget_reserve_stale(&old,36);
    CHECK(d.status == FG_BUDGET_ACCEPT && d.probe && old.probes == 1 && old.last_probe == 36,
          "the remaining probe path still consumes the probe rate");

    fg_budget_policy cool=p; cool.cooldown_until=12;
    d=fg_budget_admit_stale(&cool,9);
    CHECK(same_decision(fg_budget_admit(&cool,9),d) && d.reason == FG_BUDGET_REASON_FEEDBACK,
          "presenter feedback cooldown is unchanged");
    fg_budget_policy heavy; gap_policy(&heavy,20,4,7); interval(&heavy,8,31,-1);
    d=fg_budget_admit_stale(&heavy,9);
    CHECK(same_decision(fg_budget_admit(&heavy,9),d) && d.reason == FG_BUDGET_REASON_REAL,
          "a newest real-alone overload is unchanged");
    fg_budget_policy none={0};
    CHECK(same_decision(fg_budget_admit(&none,1),fg_budget_admit_stale(&none,1)),"missing evidence is unchanged");
    fg_budget_policy broken; gap_policy(&broken,20,4,8);
    broken.frames[3 % FG_BUDGET_FRAMES].incremental_ms=NAN;
    CHECK(same_decision(fg_budget_admit(&broken,9),fg_budget_admit_stale(&broken,9)),
          "a non-finite stored incremental never becomes an estimate");

    /* Deterministic walk: fresh/sample-free decisions and r7 reservations stay
     * byte-identical; only stale probe decisions change, always to the ceiling. */
    fg_budget_policy w={0}; warm(&w,20);
    uint32_t seed=12345; unsigned gap=0, unchanged=0, stale_accept=0, stale_refuse=0;
    for (uint64_t frame=3; frame < 4000; frame++) {
        seed=seed*1664525u+1013904223u;
        if (!(seed % 23) && frame > 40) w.last_probe=frame-1-(seed >> 9) % 35;
        if (!(seed % 89)) w.cooldown_until=frame+(seed >> 12) % 5;
        fg_budget_decision a=fg_budget_admit(&w,frame), b=fg_budget_admit_stale(&w,frame);
        int stale=a.probe && a.incremental_samples && a.status != FG_BUDGET_MISSING &&
            a.reason != FG_BUDGET_REASON_RESERVED && a.reason != FG_BUDGET_REASON_FEEDBACK &&
            a.reason != FG_BUDGET_REASON_REAL;
        if (!stale) {
            CHECK(same_decision(a,b),"non-stale decisions are byte-identical");
            unchanged++;
        } else {
            const fg_budget_frame *i=&w.frames[a.incremental_frame % FG_BUDGET_FRAMES];
            CHECK(b.stale_incremental && !b.probe && near(b.real_ms,a.real_ms) &&
                  near(b.incremental_ms,i->incremental_ms) && near(b.total_ms,a.real_ms+i->incremental_ms) &&
                  (b.status == FG_BUDGET_ACCEPT) == (b.total_ms <= FG_BUDGET_TARGET_MS) &&
                  b.reason == (b.status == FG_BUDGET_ACCEPT ? FG_BUDGET_REASON_NONE : FG_BUDGET_REASON_STALE_ESTIMATE),
                  "stale decisions apply the unchanged total ceiling to the in-window p90");
            if (b.status == FG_BUDGET_ACCEPT) stale_accept++; else stale_refuse++;
        }
        fg_budget_policy r=w, s=w;
        fg_budget_decision rr=r7_reserve(&r,frame), sr=fg_budget_reserve(&s,frame);
        CHECK(same_decision(rr,sr) && !memcmp(&r,&s,sizeof r),"refactored r7 reservation is identical");
        if (!(seed % 5)) w=r;
        int child=0;
        if (gap) gap--;
        else if (!(seed % 7)) gap=(seed >> 3) % 40;
        else child=1;
        interval(&w,frame,15+(seed >> 8) % 160 / 10.0,child ? 3+(seed >> 4) % 20 / 10.0 : -1);
    }
    CHECK(unchanged && stale_accept && stale_refuse,"walk covers unchanged, stale-accepted and stale-refused decisions");
}

int main(void) {
    accounting(); headroom(); bootstrap(); percentiles(); stale_incremental(); feedback(); locked_feedback(); unavailable();
    external_gpu(); qcom_deadlines(); qcom_adaptive_timing(); qcom_phases(); qcom_initialization();
    qcom_permanent_refusal(); qcom_inflight_jobs();
    lsfg_recovery(); lsfg_admission(); adaptive_budget_stale();
    printf("framegen GPU budget goldens: %u checks\n",checks);
    return 0;
}
