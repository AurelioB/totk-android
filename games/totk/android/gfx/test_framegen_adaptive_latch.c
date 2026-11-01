/* Host-only tests for the two independent opt-ins TOTK_FG_ADAPTIVE_LATTICE
 * (phase re-anchor) and TOTK_FG_ADAPTIVE_EARLY_APPLY (SF-deadline GEN release):
 * selector, fgs_reanchor identity/occupancy/predictor rules, early release and
 * admission guards, and per-selector + combined flag-off equivalence. Synthetic API timelines;
 * no Android, SurfaceFlinger, GPU, clocks or sleeps. Nothing here proves a
 * display outcome; it pins the scheduler arithmetic and r7 equivalence.
 * cc -std=c11 -O2 -Wall -Wextra -Werror test_framegen_adaptive_latch.c -o <test> */
#include "framegen_adaptive_latch.h"
#include "framegen_adaptive_defaults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c,message) do { checks++; if (!(c)) { \
    fprintf(stderr,"adaptive latch: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)
static const uint64_t base=UINT64_C(10000000000);
static const uint64_t period=UINT64_C(16660581);   /* r7 frozen effective period */
static const uint64_t vsync=UINT64_C(16655950);    /* r7 SF chained-fit period (approx) */
static const uint64_t margin=UINT64_C(2000000);
static const uint64_t sf_lead=UINT64_C(12000000);  /* synthetic expected-deadline span */

static uint64_t target(const fgs_state *s, uint64_t ordinal) {
    uint64_t t=0;
    CHECK(fgs_slot_target(s,ordinal,&t),"test lattice target is representable");
    return t;
}
/* Synthetic API snapshot on an independent physical lattice. */
static runtime_present_timelines timelines(uint64_t first_expected, uint64_t step,
        uint64_t deadline_span, unsigned count, uint64_t refresh) {
    runtime_present_timelines t={.frame_ns=first_expected-step,.sampled_ns=first_expected-step,
        .generation=1,.refresh_ns=refresh,.count=count};
    for (unsigned i=0;i<count;i++) t.entries[i]=(runtime_present_timeline_entry){
        .expected_ns=first_expected+i*step,.deadline_ns=first_expected+i*step-deadline_span,
        .vsync_id=(int64_t)(100+i)};
    return t;
}
static void inject_ready(fgs_state *s, uint64_t token, uint64_t ordinal, uint64_t real_ordinal,
        uint64_t ready) {
    s->pending[0]=(fgs_request){.token=token,.frame=1,.source_epoch=1,
        .real_slot=fgs_make_slot(s,real_ordinal,target(s,real_ordinal)),
        .slot=fgs_make_slot(s,ordinal,target(s,ordinal)),.publication_ns=ready,
        .deadline_ns=target(s,ordinal)-s->margin_ns};
    s->pending_count=1;
    if (s->next_token<token) s->next_token=token;
    fgs_cancel late;
    CHECK(fgs_mark_ready(s,token,ready,&late)==FGS_OK,"injected request becomes ready before its cutoff");
}

static void selector(void) {
    CHECK(fga_latch_request(NULL)==0,"unset lattice selector stays off");
    CHECK(fga_latch_request("0")==0,"exact zero stays off");
    CHECK(fga_latch_request("1")==1,"only exact one enables the lattice policy");
    const char *invalid[]={"","01","00","true","on","2","-1","1 "," 1","1\n","10"};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)
        CHECK(fga_latch_request(invalid[i])==-1,"malformed spelling is an explicit refusal, never enabled");
    /* SLOTS=1 implies CHUNK=1 and LATTICE=1 unless explicitly overridden. */
    CHECK(fga_default_on_request(NULL)==1,"absent scheduler-default selector is on");
    CHECK(fga_default_on_request("1")==1,"exact one stays on");
    CHECK(fga_default_on_request("0")==0,"exact zero restores the r7 path");
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)
        CHECK(fga_default_on_request(invalid[i])==-1,"malformed default-on spelling is refused (off), never silently on");
    /* Default-off opt-ins (EARLY_APPLY, BUDGET, PIPE_SETTLE, DIRECT_OUT). */
    CHECK(fga_default_off_request(NULL)==0 && fga_default_off_request("0")==0 &&
        fga_default_off_request("1")==1,"default-off selector: absent/0 off, exact 1 on");
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)
        CHECK(fga_default_off_request(invalid[i])==-1,"malformed default-off spelling is refused (off)");
    const char *source=NULL;
    CHECK(fga_selector_effective(NULL,1,&source)==1 && !strcmp(source,"default"),"absent default-on resolves on/default");
    CHECK(fga_selector_effective("0",1,&source)==0 && !strcmp(source,"explicit"),"explicit zero resolves off/explicit");
    CHECK(fga_selector_effective("on",1,&source)==0 && !strcmp(source,"invalid"),"invalid default-on resolves off/invalid");
    CHECK(fga_selector_effective(NULL,0,&source)==0 && !strcmp(source,"default"),"absent opt-in resolves off/default");
    CHECK(fga_selector_effective("1",0,&source)==1 && !strcmp(source,"explicit"),"exact one opt-in resolves on/explicit");
    CHECK(fga_selector_effective("2",0,&source)==0 && !strcmp(source,"invalid"),"invalid opt-in resolves off/invalid");
    CHECK(!strcmp(fga_selector_env(FGA_SELECTOR_LEAN),"TOTK_FG_ADAPTIVE_LEAN") &&
        !strcmp(fga_selector_env(FGA_SELECTOR_DIRECT_OUT),"TOTK_FG_ADAPTIVE_DIRECT_OUT") &&
        !fga_selector_env(FGA_SELECTORS),"selector table names the exact environment variables");
    const char *values[FGA_SELECTORS]={NULL,"0",NULL,"1","yes","1",NULL};
    char summary[512];
    int length=fga_selector_summary(summary,sizeof summary,values);
    CHECK(length>0 && length<(int)sizeof summary && !strcmp(summary,
        "CHUNK=1(source=default) LATTICE=0(source=explicit) EARLY_APPLY=0(source=default) "
        "BUDGET=1(source=explicit) PIPE_SETTLE=0(source=invalid) LEAN=1(source=explicit) "
        "DIRECT_OUT=0(source=default)"),"startup summary lists every resolved selector value and source");
    char small[16];
    CHECK(fga_selector_summary(small,sizeof small,values)==length && strlen(small)==sizeof small-1,
        "truncated summary reports its full length and stays terminated");
    /* LEAN is a scheduler default (absent -> on); only exact 0 opts out. */
    const char *defaults[FGA_SELECTORS]={NULL,NULL,NULL,NULL,NULL,NULL,NULL};
    CHECK(fga_selector_summary(summary,sizeof summary,defaults)>0 && !strcmp(summary,
        "CHUNK=1(source=default) LATTICE=1(source=default) EARLY_APPLY=0(source=default) "
        "BUDGET=0(source=default) PIPE_SETTLE=0(source=default) LEAN=1(source=default) "
        "DIRECT_OUT=0(source=default)"),"absent selectors: CHUNK, LATTICE and LEAN on, the rest off");
    const char *lean_off[FGA_SELECTORS]={NULL,NULL,NULL,NULL,NULL,"0",NULL};
    CHECK(fga_selector_summary(summary,sizeof summary,lean_off)>0 &&
        strstr(summary,"LEAN=0(source=explicit)"),"exact LEAN=0 opts out explicitly");
    const char *lean_bad[FGA_SELECTORS]={NULL,NULL,NULL,NULL,NULL,"yes",NULL};
    CHECK(fga_selector_summary(summary,sizeof summary,lean_bad)>0 &&
        strstr(summary,"LEAN=0(source=invalid)"),"malformed LEAN spelling is refused (off), never silently on");
    CHECK(fga_default_on_request(NULL)==1 && fga_default_on_request("0")==0 &&
        fga_default_on_request("1")==1 && fga_default_on_request("yes")==-1,
        "the LEAN consumer's parser: absent/1 on, exact 0 off, other spellings invalid (off)");
}

static void phase_thresholds(void) {
    fgs_state s;
    CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"lattice initializes");
    const uint64_t t=target(&s,40);
    struct { int64_t lag; unsigned reanchor; } cases[]={
        {FGA_LATCH_LAG_NS,0},{100000,0},{99999,1},{1500000,0},{1500001,1},
        {0,1},{-3000,1},{4000000,1},{-4000000,1}};
    for (unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        fga_latch_phase p=fga_latch_phase_check(&s,(uint64_t)((int64_t)t-cases[i].lag));
        CHECK(p.valid && p.ordinal==40 && p.lag_ns==cases[i].lag,"lag is signed T-vsync against the nearest ordinal");
        CHECK(p.reanchor==cases[i].reanchor,"re-anchor only outside [100us,1500us]");
    }
    uint64_t ordinal; int64_t error;
    CHECK(fgs_phase_error(&s,t+period/2,&ordinal,&error) && ordinal==40 && error==(int64_t)(period/2),
        "exact half-period tie keeps the earlier ordinal");
    CHECK(fgs_phase_error(&s,t+period/2+1,&ordinal,&error) && ordinal==41 && error<0,
        "past half a period the next ordinal is nearer");
    CHECK(!fgs_phase_error(&s,base-period,&ordinal,&error),"a vsync before the lattice's first half-slot has no ordinal");
}

/* Drive actual REAL commits until the cadence predictor is ACTIVE. */
static uint64_t train_active(fgs_state *s, uint64_t *frame) {
    uint64_t now=base+UINT64_C(5000000);
    for (unsigned k=0;k<14;k++,now+=UINT64_C(26700000)) {
        fgs_real_plan plan; fgs_batch batch;
        CHECK(fgs_peek_real(s,now,&plan)==FGS_OK,"training REAL is planned");
        CHECK(fgs_commit_real(s,plan.slot,now,++*frame,1,1,&batch)==FGS_OK,"training REAL commits");
        for (unsigned i=0;i<batch.request_count;i++) {
            fgs_cancel dropped;
            CHECK(fgs_cancel_gen(s,batch.requests[i].token,FGS_CANCEL_OWNER,&dropped)==FGS_OK,
                "training discards provisional GEN ownership");
        }
    }
    CHECK(s->mode==FGS_MODE_ACTIVE && s->samples==FGS_WARMUP_INTERVALS && !s->pending_count,
        "steady 26.7ms cadence trains an ACTIVE predictor");
    return now;
}

static void reanchor_identity_and_predictor(void) {
    fgs_state s; uint64_t frame=0;
    CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"lattice initializes");
    (void)train_active(&s,&frame);
    fgs_state before=s;
    const uint64_t L=s.last_real.ordinal;
    uint64_t ordinal; int64_t phase;
    /* r7 drift: T sits 3ms AFTER its vsync E (lag +3ms). */
    uint64_t anchor_vsync=target(&s,L+2)-UINT64_C(3000000);
    CHECK(fgs_phase_error(&s,anchor_vsync,&ordinal,&phase) && ordinal==L+2 && phase==-3000000,
        "drifted-late lattice has lag +3ms");
    fgs_slot old=fgs_make_slot(&s,ordinal,target(&s,ordinal));
    fgs_reanchor_result r;
    CHECK(fgs_reanchor(&s,anchor_vsync,FGA_LATCH_LAG_NS,period,&r)==FGS_OK,"idle lattice re-anchors");
    CHECK(r.generation==before.generation+1 && s.generation==r.generation,"re-anchor is a generation bump");
    CHECK(r.anchor_ordinal==ordinal && r.old_lag_ns==3000000,"nearest ordinal keeps its number");
    CHECK(target(&s,ordinal)==anchor_vsync+(uint64_t)FGA_LATCH_LAG_NS,"that ordinal now targets vsync+lag");
    CHECK(fgs_validate_slot(&s,old)==FGS_STALE,"an old-generation identity is stale, never silently re-timed");
    CHECK(old.target_ns==before.base_ns+ordinal*period,"the old identity value itself is untouched");
    CHECK(fgs_validate_slot(&s,fgs_make_slot(&s,ordinal,target(&s,ordinal)))==FGS_OK,"new identity validates");
    CHECK(s.window_epoch==before.window_epoch && s.period_ns==period && s.margin_ns==margin,
        "window, period and latch margin are unchanged");
    CHECK(s.ewma_ns==before.ewma_ns && s.samples==before.samples && s.mode==before.mode &&
        s.have_publication==before.have_publication && s.last_publication_ns==before.last_publication_ns &&
        s.last_source_epoch==before.last_source_epoch && s.next_token==before.next_token,
        "cadence predictor and token sequence are retained");
    /* Old T(L)=E_L+3ms displays at E_L+P; new T'(L)=E_L+0.5ms displays there too. */
    CHECK(r.live==1 && !r.moved && s.last_real.generation==s.generation && s.last_real.ordinal==L,
        "live last REAL keeps the ordinal that displays on its original vsync");
    CHECK(s.sealed_before==L+1 && !fgs_slot_kind(&s,L),"floor passes the translated REAL; old entry cleared");

    /* Same next REAL on a reset lattice falls back to WARMUP; re-anchor stays ACTIVE.
     * The REAL is 5ms before the first unsealed target so one GEN hole follows. */
    uint64_t now=target(&s,s.sealed_before)-UINT64_C(5000000);
    CHECK(now>before.last_publication_ns,"next REAL follows the trained cadence");
    fgs_state reset=before; fgs_batch cancelled;
    CHECK(fgs_reset(&reset,before.window_epoch,anchor_vsync+(uint64_t)FGA_LATCH_LAG_NS,period,margin,&cancelled)==FGS_OK,
        "reference reset succeeds");
    fgs_real_plan a, b; fgs_batch ba, bb;
    CHECK(fgs_peek_real(&s,now,&a)==FGS_OK && fgs_peek_real(&reset,now,&b)==FGS_OK,"both plan the next REAL");
    CHECK(a.slot.ordinal==s.sealed_before,"next REAL takes the first unsealed ordinal");
    CHECK(fgs_commit_real(&s,a.slot,now,frame+1,1,1,&ba)==FGS_OK,"re-anchored REAL commits");
    CHECK(fgs_commit_real(&reset,b.slot,now,frame+1,1,1,&bb)==FGS_OK,"reset REAL commits");
    CHECK(ba.mode==FGS_MODE_ACTIVE && ba.request_count>=1,"re-anchor keeps the ACTIVE predictor and still requests GEN");
    CHECK(bb.mode==FGS_MODE_WARMUP && !bb.request_count,"reset would force WARMUP: the avoided r7 cost");
    for (unsigned i=0;i<ba.request_count;i++)
        CHECK(ba.requests[i].slot.generation==s.generation && ba.requests[i].real_slot.generation==s.generation,
            "new requests carry only the new identity");
}

/* Committed GEN at ordinal 2 under three old lags; desired=T-1 displays on the
 * first vsync >= T. The re-anchor must keep that display vsync and seal past it. */
static void occupancy_translation(void) {
    struct {
        int64_t old_lag;
        uint64_t gen_ordinal, real_ordinal, sealed;
        unsigned moved;
    } cases[]={
        {3000000,2,1,3,0},   /* r7 drift: T after E, displays E+P; new T' after E too */
        {0,1,0,2,1},         /* r7 reset: T==E displays at E; new ordinal 1 displays at E */
        {-1500000,1,0,2,1},  /* T before E displays at E */
    };
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        fgs_state s;
        CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"lattice initializes");
        fgs_real_plan plan; fgs_batch batch;
        uint64_t t1=target(&s,1);
        CHECK(fgs_peek_real(&s,t1-UINT64_C(3000000),&plan)==FGS_OK && plan.slot.ordinal==1,"REAL takes ordinal 1");
        CHECK(fgs_commit_real(&s,plan.slot,t1-UINT64_C(3000000),1,1,1,&batch)==FGS_OK,"REAL commits");
        inject_ready(&s,9,2,1,t1);
        fgs_request admitted; fgs_gen_commit commit;
        uint64_t t2=target(&s,2), apply=t2-UINT64_C(1400000);
        CHECK(fgs_admit_gen(&s,9,apply,NULL,&admitted)==FGS_OK,"GEN admitted at r7 time");
        CHECK(fgs_commit_gen(&s,9,apply,&commit)==FGS_OK,"GEN is irrevocably applied at ordinal 2");
        uint64_t vsync_e=(uint64_t)((int64_t)t2-cases[c].old_lag);
        /* Physical display vsync of desired t2-1: first vsync >= t2. */
        uint64_t display=vsync_e>=t2 ? vsync_e : vsync_e+period;
        fgs_reanchor_result r;
        CHECK(fgs_reanchor(&s,vsync_e,FGA_LATCH_LAG_NS,period,&r)==FGS_OK,"idle lattice re-anchors");
        CHECK(r.live==1 && r.moved==cases[c].moved,"only the live GEN is translated, by display vsync");
        CHECK(s.sealed_before==cases[c].sealed,"floor passes the GEN's translated ordinal");
        CHECK(s.last_real.ordinal==cases[c].real_ordinal,"last REAL follows the same translation");
        uint64_t gen_new=cases[c].gen_ordinal;
        CHECK(target(&s,gen_new)>=display-period && target(&s,gen_new)<display &&
            target(&s,gen_new)+period>=display,"translated GEN ordinal displays on the same vsync");
        CHECK(target(&s,gen_new)-(uint64_t)FGA_LATCH_LAG_NS+period==display,
            "translated ordinal is exactly lag after the vsync before the display");
        CHECK(!fgs_slot_kind(&s,2) && !fgs_slot_kind(&s,3),"no stale old-numbering occupancy survives");
        fgs_real_plan next;
        CHECK(fgs_peek_real(&s,apply+UINT64_C(100000),&next)==FGS_OK,"next REAL plans");
        CHECK(next.slot.ordinal>=s.sealed_before && next.slot.ordinal>gen_new &&
            next.slot.generation==s.generation,"a REAL never lands on or before the irrevocable GEN");
        CHECK(target(&s,next.slot.ordinal)-(uint64_t)FGA_LATCH_LAG_NS+period>display,
            "the REAL's display vsync is strictly after the GEN's display vsync");
    }
}

static void refusals_and_overflow(void) {
    fgs_state s, before; fgs_reanchor_result r;
    memset(&s,0,sizeof s);
    CHECK(fgs_reanchor(NULL,base,0,period,&r)==FGS_INVALID && fgs_reanchor(&s,base,0,period,NULL)==FGS_INVALID,
        "missing storage is invalid");
    CHECK(fgs_reanchor(&s,base,FGA_LATCH_LAG_NS,period,&r)==FGS_CLOCK,"unconfigured lattice refuses");
    CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"lattice initializes");
    inject_ready(&s,5,3,2,base);
    before=s;
    CHECK(fgs_reanchor(&s,target(&s,3)-3000,FGA_LATCH_LAG_NS,period,&r)==FGS_WAIT,
        "a pending GEN request refuses the re-anchor");
    CHECK(!memcmp(&s,&before,sizeof s),"pending refusal leaves state byte-identical");
    fgs_cancel dropped;
    CHECK(fgs_cancel_gen(&s,5,FGS_CANCEL_OWNER,&dropped)==FGS_OK,"pending request released");
    before=s;
    CHECK(fgs_reanchor(&s,target(&s,3),(int64_t)(period/2),period,&r)==FGS_INVALID,"lag must stay below half a refresh");
    CHECK(fgs_reanchor(&s,target(&s,3),-(int64_t)(period/2),period,&r)==FGS_INVALID,"negative lag likewise");
    CHECK(fgs_reanchor(&s,target(&s,3),0,UINT64_C(1000000),&r)==FGS_CLOCK,"period must exceed the latch margin");
    CHECK(fgs_reanchor(&s,base-period,0,period,&r)==FGS_OVERFLOW,"vsync before the lattice has no ordinal");
    CHECK(!memcmp(&s,&before,sizeof s),"every refusal leaves state byte-identical");
    s.generation=UINT64_MAX; before=s;
    CHECK(fgs_reanchor(&s,target(&s,3),FGA_LATCH_LAG_NS,period,&r)==FGS_OVERFLOW,
        "generation exhaustion never wraps into an old identity");
    CHECK(!memcmp(&s,&before,sizeof s),"overflow leaves state byte-identical");

    /* A display ordinal that cannot be represented refuses atomically. */
    CHECK(fgs_init(&s,1,UINT64_MAX-2*period-UINT64_C(1000000),period,margin)==FGS_OK,"edge lattice initializes");
    s.occupied[2]=(fgs_occupied){2,FGS_SLOT_GENERATED};
    s.have_history=1; s.high_ordinal=2;
    before=s;
    CHECK(fgs_reanchor(&s,target(&s,2)-UINT64_C(3000000),FGA_LATCH_LAG_NS,period,&r)==FGS_OVERFLOW,
        "unrepresentable display ordinal refuses");
    CHECK(!memcmp(&s,&before,sizeof s),"translation overflow leaves state byte-identical");
}

static void early_release(void) {
    fgs_state s;
    CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"lattice initializes");
    const uint64_t o=30, t=target(&s,o);
    fgs_slot slot=fgs_make_slot(&s,o,t);
    const uint64_t hold=t-margin;
    /* SF vsync is 0.5ms after T: desired=T-1 latches on that same vsync. */
    uint64_t v=t+UINT64_C(500000);
    runtime_present_timelines snap=timelines(v-2*vsync,vsync,sf_lead,6,period);

    fga_latch_release r=fga_latch_release_for(0,&s,slot,t-1,&snap,t-UINT64_C(15000000));
    CHECK(!r.release_ns && r.kind==FGA_LATCH_R7_DISABLED && r.r7_hold_ns==hold,"disabled is exactly r7");
    r=fga_latch_release_for(1,&s,slot,t-1,&snap,t-UINT64_C(15000000));
    CHECK(r.kind==FGA_LATCH_EARLY && r.latch.found && r.latch.entry.expected_ns==v &&
        r.latch.entry.vsync_id==102,"matched SF entry is the vsync desired can first latch on");
    CHECK(r.release_ns==v-sf_lead-FGA_LATCH_DEADLINE_GUARD_NS,"release is the matched deadline minus guard");
    CHECK(r.release_ns<hold && r.release_ns<t,"early release is strictly before r7 and before T");

    /* Lattice 3us LATE: desired>=V(o), so the next vsync's deadline governs. */
    runtime_present_timelines slipped=timelines(t-3000-2*vsync,vsync,sf_lead,6,period);
    r=fga_latch_release_for(1,&s,slot,t-1,&slipped,t-UINT64_C(15000000));
    CHECK(r.latch.found && r.latch.entry.expected_ns==t-3000+vsync,"strictly-later vsync is the latch entry");
    CHECK(r.kind==FGA_LATCH_R7_NOT_EARLIER && !r.release_ns,"its later deadline is never used to wait longer than r7");

    runtime_present_timelines far=timelines(t+2*period,vsync,sf_lead,4,period);
    r=fga_latch_release_for(1,&s,slot,t-1,&far,t-UINT64_C(15000000));
    CHECK(r.kind==FGA_LATCH_R7_NO_MATCH && !r.release_ns,"no entry within one refresh after T: r7");
    runtime_present_timelines before_t=timelines(t-5*vsync,vsync,sf_lead,4,period);
    r=fga_latch_release_for(1,&s,slot,t-1,&before_t,t-UINT64_C(15000000));
    CHECK(r.kind==FGA_LATCH_R7_NO_MATCH,"entries only before T: r7");

    r=fga_latch_release_for(1,&s,slot,t-1,&snap,v-sf_lead);
    CHECK(r.kind==FGA_LATCH_R7_LATE && !r.release_ns,"SF deadline already passed: r7");

    runtime_present_timelines tight=timelines(v-2*vsync,vsync,UINT64_C(400000),6,period);
    r=fga_latch_release_for(1,&s,slot,t-1,&tight,t-UINT64_C(15000000));
    CHECK(r.kind==FGA_LATCH_R7_NOT_EARLIER && !r.release_ns,"deadline-guard after r7 hold: r7");

    runtime_present_timelines deep=timelines(v-2*vsync,vsync,UINT64_C(25000000),6,period);
    r=fga_latch_release_for(1,&s,slot,t-1,&deep,t-UINT64_C(30000000));
    CHECK(r.kind==FGA_LATCH_EARLY && r.release_ns==t-period-margin+1,
        "release never precedes T-period-margin (REAL ordering floor)");

    runtime_present_timelines refresh=snap; refresh.refresh_ns=vsync;
    r=fga_latch_release_for(1,&s,slot,t-1,&refresh,t-UINT64_C(15000000));
    CHECK(r.kind==FGA_LATCH_R7_INVALID,"snapshot on a different refresh than the lattice: r7");
    fgs_slot stale=slot; stale.generation++;
    r=fga_latch_release_for(1,&s,stale,t-1,&snap,t-UINT64_C(15000000));
    CHECK(r.kind==FGA_LATCH_R7_INVALID,"stale slot identity: r7");

    fga_latch_match m=fga_latch_nearest(&slipped,t);
    CHECK(m.found && m.offset_ns==-3000 && m.entry.vsync_id==102,"nearest match reports signed phase");
    CHECK(!fga_latch_nearest(&far,t).found,"no nearest entry within half a refresh is explicit none");
}

static void early_admission(void) {
    fgs_state s;
    CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"lattice initializes");
    fgs_real_plan plan; fgs_batch batch;
    CHECK(fgs_peek_real(&s,base+UINT64_C(1000000),&plan)==FGS_OK,"REAL planned");
    CHECK(fgs_commit_real(&s,plan.slot,base+UINT64_C(1000000),1,1,1,&batch)==FGS_OK,"REAL commits");
    const uint64_t o=plan.slot.ordinal+1, t=target(&s,o);
    inject_ready(&s,7,o,plan.slot.ordinal,base+UINT64_C(5000000));
    const uint64_t release=t-UINT64_C(13000000);
    fgs_request a, b;
    for (uint64_t now=release-UINT64_C(2000000);now<=t+1;now+=UINT64_C(250000)) {
        fgs_status r7=fgs_admit_gen(&s,7,now,NULL,&a);
        fgs_status early=fgs_admit_gen_release(&s,7,now,NULL,release,&b);
        if (now<release || now>=t-margin) {
            CHECK(early==r7 && a.token==b.token && a.slot.ordinal==b.slot.ordinal &&
                a.deadline_ns==b.deadline_ns,"outside the early window admission is exactly r7");
        } else {
            CHECK(r7==FGS_WAIT && early==FGS_OK && b.token==7,"between release and r7 hold the ready GEN is admitted");
        }
        CHECK(fgs_admit_gen_release(&s,7,now,NULL,0,&b)==r7,"zero release is exactly r7");
        CHECK(fgs_admit_gen_release(&s,7,now,NULL,t-margin,&b)==r7,"a release at the r7 deadline never changes r7");
    }
    CHECK(fgs_admit_gen_release(&s,7,t+1,NULL,release,&b)==FGS_LATE,"never admitted after T");
    fgs_slot real=fgs_make_slot(&s,o,t);
    CHECK(fgs_admit_gen_release(&s,7,release,&real,release,&b)==FGS_REAL_COLLISION,
        "a known ready REAL still wins the early window");
    /* Not ready: no early bypass of readiness. */
    fgs_cancel dropped;
    CHECK(fgs_cancel_gen(&s,7,FGS_CANCEL_OWNER,&dropped)==FGS_OK,"request dropped");
    s.pending[0]=(fgs_request){.token=8,.frame=1,.source_epoch=1,.real_slot=plan.slot,
        .slot=fgs_make_slot(&s,o,t),.publication_ns=base,.deadline_ns=t-margin};
    s.pending_count=1;
    CHECK(fgs_admit_gen_release(&s,8,release,NULL,release,&b)==FGS_NOT_READY,"unready GEN is never admitted early");

    /* Presenter decision: earlier-only hold, identical when release is 0/later. */
    fg_adaptive_present_state state={0};
    fg_adaptive_present_job job={.slot=t,.frame=1,.epoch=3,.generation=7,.generated=1};
    for (uint64_t now=release-UINT64_C(1000000);now<=t;now+=UINT64_C(125000)) {
        fg_adaptive_present_context c={.now=now,.epoch=3,.generation=7,.window=1};
        fg_adaptive_present_result r7=fg_adaptive_present_decide(&state,job,c);
        c.release=t;
        CHECK(fg_adaptive_present_decide(&state,job,c)==r7,"a later release never delays r7");
        c.release=release;
        fg_adaptive_present_result early=fg_adaptive_present_decide(&state,job,c);
        CHECK(early==(now<release ? FG_ADAPTIVE_PRESENT_WAIT : now>=t ? FG_ADAPTIVE_PRESENT_EXPIRED :
            FG_ADAPTIVE_PRESENT_APPLY),"early hold applies from release up to T, never at/after T");
        c.queued_real=1;
        CHECK(fg_adaptive_present_decide(&state,job,c)==(now>=t ? FG_ADAPTIVE_PRESENT_EXPIRED :
            FG_ADAPTIVE_PRESENT_YIELD),"queued REAL still yields during the early window");
    }
    CHECK(fg_adaptive_present_hold(t,0)==fg_adaptive_present_deadline(t) &&
        fg_adaptive_present_hold(t,t)==fg_adaptive_present_deadline(t) &&
        fg_adaptive_present_hold(t,release)==release,"hold only ever moves earlier");
}

/* Replay the vk_draw.c call structure on a drifting physical clock. */
typedef struct {
    uint64_t kind, frame, generation, ordinal, target, time, status;
} replay_event;
typedef struct {
    replay_event events[4096];
    unsigned count, reanchors, early, gens, refused_pending;
} replay_trace;
static void record(replay_trace *trace, uint64_t kind, uint64_t frame, fgs_slot slot,
        uint64_t time, fgs_status status) {
    CHECK(trace->count<sizeof trace->events/sizeof *trace->events,"bounded replay");
    trace->events[trace->count++]=(replay_event){kind,frame,slot.generation,slot.ordinal,
        slot.target_ns,time,(uint64_t)status};
}
static runtime_present_timelines physical(uint64_t now, uint64_t lead) {
    uint64_t k=(now-base)/vsync;
    uint64_t first=base+(k ? k-1 : 0)*vsync;
    runtime_present_timelines t=timelines(first,vsync,lead,8,period);
    for (t.preferred=0;t.preferred<t.count && t.entries[t.preferred].deadline_ns<=now;t.preferred++) {}
    return t;
}
/* reference=1: literal r7 calls only (no latch header use at all). lattice and
 * early mirror TOTK_FG_ADAPTIVE_LATTICE / TOTK_FG_ADAPTIVE_EARLY_APPLY; lead is
 * the synthetic SF expected-deadline span. */
static void replay(int lattice, int early, int reference, uint64_t lead, replay_trace *trace) {
    memset(trace,0,sizeof *trace);
    fgs_state s;
    CHECK(fgs_init(&s,1,base,period,margin)==FGS_OK,"replay lattice initializes");
    uint64_t now=base+UINT64_C(5000000);
    for (uint64_t frame=1;frame<=400;frame++,now+=UINT64_C(26700000)) {
        if (!reference && lattice) {
            runtime_present_timelines clock=physical(now,lead);
            const runtime_present_timeline_entry *anchor=&clock.entries[clock.preferred];
            fga_latch_phase phase=fga_latch_phase_check(&s,anchor->expected_ns);
            if (phase.valid && phase.reanchor) {
                fgs_reanchor_result r;
                fgs_status status=fgs_reanchor(&s,anchor->expected_ns,FGA_LATCH_LAG_NS,period,&r);
                if (status==FGS_OK) trace->reanchors++;
                else { CHECK(status==FGS_WAIT,"replay re-anchor refuses only for pending GEN"); trace->refused_pending++; }
            }
        }
        fgs_real_plan plan; fgs_batch batch;
        CHECK(fgs_peek_real(&s,now,&plan)==FGS_OK,"replay REAL plans");
        fgs_status status=fgs_commit_real(&s,plan.slot,now,frame,1,1,&batch);
        CHECK(status==FGS_OK,"replay REAL commits without duplicate/stale identity");
        record(trace,0,frame,plan.slot,now,status);
        for (unsigned i=0;i<batch.request_count;i++) {
            const fgs_request q=batch.requests[i];
            uint64_t ready=now+UINT64_C(4000000);
            fgs_cancel late;
            status=fgs_mark_ready(&s,q.token,ready,&late);
            if (status!=FGS_OK) { record(trace,2,frame,q.slot,ready,status); continue; }
            uint64_t release=0;
            if (!reference) {
                runtime_present_timelines clock=physical(ready,lead);
                release=fga_latch_release_for(early,&s,q.slot,q.slot.target_ns-1,&clock,ready).release_ns;
            }
            uint64_t hold=fg_adaptive_present_hold(q.slot.target_ns,release);
            uint64_t apply=ready>hold ? ready : hold;
            if (release && apply<q.deadline_ns) trace->early++;
            fgs_request admitted;
            status=reference ? fgs_admit_gen(&s,q.token,apply,NULL,&admitted) : release ?
                fgs_admit_gen_release(&s,q.token,apply,NULL,release,&admitted) :
                fgs_admit_gen(&s,q.token,apply,NULL,&admitted);
            CHECK(status==FGS_OK,"replay GEN admitted at its hold");
            CHECK(!release || apply<=q.deadline_ns,"early arm never applies later than r7");
            CHECK(release || apply==(ready>q.deadline_ns ? ready : q.deadline_ns),
                "without an early release the GEN applies exactly at the r7 hold");
            CHECK(apply<q.slot.target_ns,"never applied at/after T");
            fgs_gen_commit commit;
            status=fgs_commit_gen(&s,q.token,apply,&commit);
            CHECK(status==FGS_OK,"replay GEN commits");
            trace->gens++;
            record(trace,1,frame,q.slot,apply,status);
        }
    }
}
static int same_events(const replay_trace *a, const replay_trace *b) {
    return a->count==b->count && !memcmp(a->events,b->events,a->count*sizeof *a->events);
}
/* REAL plans/targets only (kind 0): EARLY_APPLY must not move any REAL. */
static int same_reals(const replay_trace *a, const replay_trace *b) {
    unsigned i=0, j=0;
    for (;;) {
        while (i<a->count && a->events[i].kind) i++;
        while (j<b->count && b->events[j].kind) j++;
        if (i==a->count || j==b->count) return i==a->count && j==b->count;
        if (memcmp(&a->events[i],&b->events[j],sizeof a->events[i])) return 0;
        i++; j++;
    }
}
/* Lag T-E against the physical vsync at or before T. After a re-anchor the
 * anchor ordinal sits in [100us,1500us]; the REAL is that ordinal or the one
 * before it, i.e. up to one refresh of drift (4.6us) lower. */
static void check_lag_window(const replay_trace *t) {
    unsigned within=0, reals=0;
    for (unsigned i=0;i<t->count;i++) {
        if (t->events[i].kind) continue;
        reals++;
        uint64_t k=(t->events[i].target-base)/vsync;
        uint64_t lag=t->events[i].target-(base+k*vsync);
        within+=lag>=95000 && lag<=1500000;
    }
    CHECK(reals==400 && within==reals,"every REAL target sits [~95us,1500us] after its SF vsync");
}
/* r7 present regime: the GEN apply (T-2ms..T-1.4ms) must stay >=13.7ms before
 * its display vsync D (first vsync >= T) and before D's synthetic deadline. */
static void check_gen_display(const replay_trace *t, uint64_t lead) {
    unsigned gens=0;
    for (unsigned i=0;i<t->count;i++) {
        if (t->events[i].kind!=1) continue;
        gens++;
        uint64_t target_ns=t->events[i].target, apply=t->events[i].time;
        uint64_t k=(target_ns-base+vsync-1)/vsync, display=base+k*vsync;
        uint64_t latest=target_ns-UINT64_C(1400000);
        CHECK(apply<=latest && display-latest>=UINT64_C(13700000),
            "GEN apply stays >=13.7ms before its display vsync");
        CHECK(latest<display-lead,"GEN apply precedes the display vsync's SF deadline");
    }
    CHECK(gens>50,"display check exercised GEN publications");
}
static void selector_equivalence(void) {
    static replay_trace r7, off, lattice, lattice_off_early, early16, early16_off_lattice, early12;
    static replay_trace lattice18, both;
    /* r7 and LATTICE-alone use a 12ms SF span; lattice18/both use 18ms. */
    replay(0,0,1,sf_lead,&r7);
    CHECK(r7.count>400 && r7.gens>50,"reference replay exercises REAL and GEN publications");

    /* Both selectors off, for every off spelling: decision-for-decision r7. */
    const char *offs[]={NULL,"0","true","01"};
    for (unsigned i=0;i<sizeof offs/sizeof *offs;i++)
        for (unsigned j=0;j<sizeof offs/sizeof *offs;j++) {
            replay(fga_latch_request(offs[i])==1,fga_latch_request(offs[j])==1,0,sf_lead,&off);
            CHECK(same_events(&off,&r7) && !off.reanchors && !off.early,
                "LATTICE and EARLY_APPLY off (unset/0/invalid) are decision-for-decision r7");
        }

    /* LATTICE alone: re-anchors, never releases early, GENs at the r7 hold. */
    replay(fga_latch_request("1")==1,fga_latch_request(NULL)==1,0,sf_lead,&lattice);
    replay(1,fga_latch_request("0")==1,0,sf_lead,&lattice_off_early);
    CHECK(same_events(&lattice,&lattice_off_early),"EARLY_APPLY unset and 0 are identical under LATTICE=1");
    CHECK(lattice.reanchors>=2 && !lattice.early,"LATTICE=1 alone re-anchors and never releases early");
    CHECK(!same_events(&lattice,&r7),"LATTICE=1 alone is observably different");
    check_lag_window(&lattice);
    check_gen_display(&lattice,sf_lead);

    /* EARLY_APPLY alone: lattice untouched (every REAL identical to r7). */
    replay(fga_latch_request(NULL)==1,fga_latch_request("1")==1,0,UINT64_C(16000000),&early16);
    replay(fga_latch_request("0")==1,1,0,UINT64_C(16000000),&early16_off_lattice);
    CHECK(same_events(&early16,&early16_off_lattice),"LATTICE unset and 0 are identical under EARLY_APPLY=1");
    CHECK(!early16.reanchors && early16.early>0,"EARLY_APPLY=1 alone releases early without re-anchoring");
    CHECK(same_reals(&early16,&r7),"EARLY_APPLY=1 alone never moves a REAL plan or target");
    /* On r7's slipped lattice (T later than SF vsync) the latch entry is the
     * NEXT vsync: with a 12ms SF span, early needs span > vsync-drift+0.5ms. */
    replay(0,1,0,sf_lead,&early12);
    CHECK(same_events(&early12,&r7) && !early12.early,
        "EARLY_APPLY=1 alone with a 12ms SF span on the drifting r7 lattice stays exactly r7");

    /* Both: with T just after E the latch entry is E+P; early fires only when
     * the SF span exceeds vsync-lag+0.5ms, so use an 18ms span here. */
    replay(1,0,0,UINT64_C(18000000),&lattice18);
    replay(1,1,0,UINT64_C(18000000),&both);
    CHECK(lattice18.reanchors>=2 && !lattice18.early,"LATTICE alone with 18ms span re-anchors, no early");
    CHECK(both.reanchors>=2 && both.early>0,"LATTICE+EARLY_APPLY re-anchor and release early");
    CHECK(same_reals(&both,&lattice18),"adding EARLY_APPLY never moves a LATTICE REAL plan or target");
    CHECK(both.reanchors==lattice18.reanchors,"EARLY_APPLY does not change re-anchor decisions");
    check_lag_window(&both);
    /* 12ms span with both on: the latch entry E+P's deadline is after T-2ms. */
    replay(1,1,0,sf_lead,&early12);
    CHECK(!early12.early && same_events(&early12,&lattice),
        "LATTICE+EARLY_APPLY with a 12ms span equals LATTICE alone");
}

int main(void) {
    selector();
    phase_thresholds();
    reanchor_identity_and_predictor();
    occupancy_translation();
    refusals_and_overflow();
    early_release();
    early_admission();
    selector_equivalence();
    printf("adaptive latch: %u checks\n",checks);
    return 0;
}
