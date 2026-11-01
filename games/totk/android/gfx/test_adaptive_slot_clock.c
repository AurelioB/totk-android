/* Pure production selector/snapshot checks: no Android runtime, clocks, sleeps
 * or GPU. Callback delivery and actual display binding require runtime coverage. */
#include "../../runtime/present_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr,"adaptive slot clock: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)
static const uint64_t second=UINT64_C(1000000000), hz60=UINT64_C(16666667);

static runtime_present_timelines snapshot(uint64_t refresh) {
    runtime_present_timelines s={.frame_ns=second,.sampled_ns=second+100000,
        .generation=7,.refresh_ns=refresh,.count=3,.preferred=0};
    const int64_t ids[]={0,42,7};
    for (unsigned i=0;i<s.count;i++) s.entries[i]=(runtime_present_timeline_entry){
        .expected_ns=second+(i+1)*refresh,.deadline_ns=second+(i+1)*refresh-2000000,
        .vsync_id=ids[i]};
    return s;
}
static int fresh(const runtime_present_timelines *s,uint64_t now,uint64_t epoch) {
    return runtime_present_adaptive_slot_timelines_fresh(s,now,epoch);
}
static void selector(void) {
    CHECK(!runtime_present_adaptive_slots_request(NULL),"unset stays off");
    CHECK(!runtime_present_adaptive_slots_request("0"),"exact zero stays off");
    CHECK(runtime_present_adaptive_slots_request("1")==1,"only exact one requests slots");
    const char *invalid[]={"","01","00","true","on","adaptive","2","-1","1 "," 1","1\n"};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)
        CHECK(runtime_present_adaptive_slots_request(invalid[i])==-1,
            "malformed opt-ins are distinguishable refusals, never active requests");
}
static void valid_and_freshness(void) {
    const uint64_t refreshes[]={hz60,UINT64_C(16683350),UINT64_C(8333333)};
    for (unsigned i=0;i<sizeof refreshes/sizeof *refreshes;i++) {
        runtime_present_timelines s=snapshot(refreshes[i]),before=s;
        CHECK(fresh(&s,second+1000000,second),"fresh current-epoch API snapshot is usable");
        CHECK(!memcmp(&s,&before,sizeof s),"validation never mutates copied API data");
        CHECK(fresh(&s,second+2*s.refresh_ns,second),"two-refresh freshness boundary is inclusive");
        CHECK(!fresh(&s,second+2*s.refresh_ns+1,second),"one ns beyond two refreshes is stale");
        CHECK(!fresh(&s,second+1000000,second+1),"queued pre-lifecycle callback cannot enter new epoch");
        CHECK(fresh(&s,second+1000000,second-1),"callback after epoch boundary is usable");
        CHECK(!fresh(&s,s.sampled_ns-1,second),"future sample is refused");
    }
    runtime_present_timelines s=snapshot(hz60);
    s.count=1;
    CHECK(fresh(&s,second+1000000,second),"one live preferred API timeline is sufficient clock data");
    CHECK(!fresh(&s,s.entries[0].deadline_ns,second),"expired final deadline is not an open timeline");
    s=snapshot(hz60);
    CHECK(fresh(&s,s.entries[0].deadline_ns,second),"expired earlier entry does not hide later live API candidates");
    s.preferred=2;
    CHECK(fresh(&s,second+1000000,second),"preferred index is retained instead of clamped or reordered");
}
static void invalid_metadata(void) {
    runtime_present_timelines base=snapshot(hz60),s;
    const uint64_t now=second+1000000;
    CHECK(!fresh(NULL,now,second),"missing snapshot is unavailable");
#define REFUSE(change, message) do { s=base; change; CHECK(!fresh(&s,now,second),message); } while (0)
    REFUSE(s.generation=0,"unknown generation is unavailable");
    REFUSE(s.count=0,"cleared lifecycle snapshot is unavailable");
    REFUSE(s.count=RUNTIME_PRESENT_TIMELINE_CAPACITY+1,"oversized snapshot cannot read beyond entries");
    REFUSE(s.preferred=s.count,"out-of-bound preferred timeline is not clamped");
    REFUSE(s.refresh_ns=0,"missing period is unavailable");
    REFUSE(s.refresh_ns=(uint64_t)INT64_MAX/2+1,"freshness period overflow is refused");
    REFUSE(s.frame_ns=0,"missing callback phase is unavailable");
    REFUSE(s.frame_ns=second-1,"pre-epoch callback is unavailable");
    REFUSE(s.sampled_ns=second-1,"sample cannot precede callback frame");
    REFUSE(s.sampled_ns=now+1,"sample cannot come from the future");
    REFUSE(s.entries[0].expected_ns=0,"missing expected-present is unavailable");
    REFUSE(s.entries[2].expected_ns=(uint64_t)INT64_MAX+1,"signed API timestamp overflow is unavailable");
    REFUSE(s.entries[0].deadline_ns=0,"missing deadline is not synthesized");
    REFUSE(s.entries[0].deadline_ns=s.entries[0].expected_ns+1,"deadline after expected presentation is invalid");
    REFUSE(s.entries[0].vsync_id=-1,"negative API ID is invalid");
    REFUSE(s.entries[2].vsync_id=s.entries[0].vsync_id,"duplicate IDs are invalid even when times differ");
    REFUSE(s.entries[1].expected_ns=s.entries[0].expected_ns,"repeated expected-present is invalid");
    REFUSE(s.entries[1].expected_ns=s.entries[0].expected_ns-1,"backward expected-present is invalid");
    REFUSE(s.entries[1].deadline_ns=s.entries[0].deadline_ns,"repeated deadline is invalid");
    REFUSE(s.entries[1].deadline_ns=s.entries[0].deadline_ns-1,"backward deadline is invalid");
#undef REFUSE
    CHECK(!fresh(&base,(uint64_t)INT64_MAX+1,second),"unsigned now beyond Android signed timestamps is invalid");
    CHECK(fresh(&base,now,second),"zero and nonmonotonic noncontiguous IDs remain valid API values");
}
static void host_unavailable(void) {
    runtime_present_timelines s=snapshot(hz60),before=s;
    CHECK(!runtime_android_adaptive_slots_requested(),"non-Android host does not request runtime slots");
    CHECK(!runtime_android_adaptive_slots_active(),"non-Android host has no API33 clock admission");
    CHECK(runtime_android_adaptive_slots_display_id()==-1,"unavailable host has no display binding");
    CHECK(!runtime_android_adaptive_slot_timelines(&s),"unavailable host cannot fabricate a timeline");
    CHECK(!memcmp(&s,&before,sizeof s),"unavailable snapshot leaves caller output untouched");
    CHECK(!runtime_android_adaptive_slot_timelines(NULL),"null output is harmless when unavailable");
    CHECK(!runtime_android_present_timeline_active(),"adaptive addition does not enable locked timeline API");
    CHECK(runtime_android_present_timeline_display_id()==-1,"locked display admission remains independent");
}
static void desired_encoding(void) {
    const uint64_t periods[]={UINT64_C(16666666),UINT64_C(8333333)};
    for (unsigned p=0;p<sizeof periods/sizeof *periods;p++)
        for (unsigned n=1;n<=1000;n++) {
            uint64_t expected=second+n*periods[p];
            uint64_t desired=0;
            CHECK(runtime_present_adaptive_slot_desired(expected,periods[p],&desired),
                "valid target explicitly encodes before any setter call");
            CHECK(desired==expected-1,"desired encodes strict eligibility one ns below exact target");
            CHECK(desired>expected-periods[p],"encoding cannot enter the previous physical slot");
        }
    uint64_t desired=123;
    CHECK(!runtime_present_adaptive_slot_desired(0,hz60,&desired) && desired==123,"missing target is refused without writing zero");
    CHECK(!runtime_present_adaptive_slot_desired(second,1,&desired) && desired==123,"one-ns slot cannot encode strict interior");
    CHECK(!runtime_present_adaptive_slot_desired(hz60,hz60,&desired) && desired==123,"missing previous slot is refused");
    CHECK(!runtime_present_adaptive_slot_desired(UINT64_MAX,hz60,&desired) && desired==123,"signed platform overflow is refused");
    CHECK(runtime_present_adaptive_slot_desired((uint64_t)INT64_MAX,hz60,&desired) &&
        desired==(uint64_t)INT64_MAX-1,"INT64_MAX itself encodes without overflow");
    desired=123;
    CHECK(!runtime_present_adaptive_slot_desired((uint64_t)INT64_MAX+1,hz60,&desired) &&
        desired==123,"first unencodable signed target is explicit refusal");
    CHECK(!runtime_present_adaptive_slot_desired(second,hz60,NULL),"missing output cannot become desired zero");
}
static void final_publication_clock(void) {
    runtime_present_timelines s=snapshot(hz60),original=s;
    uint64_t now=second+UINT64_C(1000000);
    CHECK(fresh(&s,now,second) &&
        runtime_present_adaptive_slot_clock_matches(&s,7,hz60,0,0),
        "dispatched child can publish only against its fresh admitted clock");
    s.generation++;
    CHECK(fresh(&s,now,second) &&
        !runtime_present_adaptive_slot_clock_matches(&s,7,hz60,0,0),
        "calibration invalidation after dispatch rejects final publication without a newer real");
    s=original; s.refresh_ns=UINT64_C(8333333);
    CHECK(fresh(&s,now,second) &&
        !runtime_present_adaptive_slot_clock_matches(&s,7,hz60,0,0),
        "60-to-120 refresh change rejects an already submitted old-period child");
    s=original;
    CHECK(!runtime_present_adaptive_slot_clock_matches(&s,7,hz60,0,1),
        "surface display change rejects final publication");
    CHECK(!runtime_present_adaptive_slot_clock_matches(&s,7,hz60,-1,0),
        "unknown source display never authenticates a queued child");
    CHECK(!(fresh(&s,second+2*hz60+1,second) &&
        runtime_present_adaptive_slot_clock_matches(&s,7,hz60,0,0)),
        "clock becoming stale after dispatch refuses final publication");
    CHECK(!runtime_present_adaptive_slot_clock_matches(NULL,7,hz60,0,0),
        "absent clock cannot authenticate a queued child");
    CHECK(!memcmp(&s,&original,sizeof s),"final clock checks never mutate immutable ownership metadata");
}
int main(void) {
    selector(); valid_and_freshness(); invalid_metadata(); host_unavailable(); desired_encoding();
    final_publication_clock();
    printf("adaptive slot clock: %u checks passed\n",checks);
    return 0;
}
