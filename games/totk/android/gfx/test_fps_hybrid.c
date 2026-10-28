/* No sleeps/devices: wall-time traces and the production runtime bridge. */
#include "fps_hybrid.h"
#include "vk_present_slots.h"
#include "../../runtime/fps_hybrid_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t second=1000000000ull, refresh=16666667ull;
static uint64_t trace(fps_hybrid_policy *p,uint64_t now,uint64_t duration,
        uint64_t dt,double score,int valid) {
    uint64_t end=now+duration;
    while (now+dt<=end) {
        now+=dt;
        fps_hybrid_work(p,now,valid,score,score);
        fps_hybrid_apply(p,now);
        fps_hybrid_present(p,now,1);
    }
    return now;
}
static void traces(void) {
    fps_hybrid_policy p;
    uint64_t now=second;
    fps_hybrid_init(&p,fps_hybrid_defaults(),now);
    now=trace(&p,now,60*second,refresh,.5,1);
    assert(!p.interp && p.switches==0);
    puts("hybrid sustained60: native, switches=0");

    fps_hybrid_init(&p,fps_hybrid_defaults(),now);
    now=trace(&p,now,30*second,25000000,1.3,1);
    assert(p.interp && p.switches==1 && p.time_ns[0]>=10*second && p.time_ns[0]<11*second);
    now=trace(&p,now,30*second,33333333,1.3,1);
    assert(p.interp && p.switches==1); /* Paced30 itself cannot prove60. */
    /* A partial preexisting overloaded window must expire before the next
     * complete eight-second headroom window can release the mode. */
    now=trace(&p,now,17*second,33333333,.5,1);
    assert(!p.interp && p.switches==2);
    puts("hybrid sustained40 + headroom recovery: interp then native, switches=2");

    fps_hybrid_init(&p,fps_hybrid_defaults(),now);
    /* Alternate complete windows around threshold. Four seconds of bad
     * evidence cannot become a ten-second episode by counting frames. */
    for (unsigned i=0;i<20;i++) {
        now=trace(&p,now,4*second,20000000,1.1,1);
        now=trace(&p,now,6*second,refresh,.5,1);
    }
    assert(!p.interp && p.switches==0);
    puts("hybrid oscillating50/60 around55: native, switches=0");

    fps_hybrid_init(&p,fps_hybrid_defaults(),now);
    for (unsigned i=0;i<30;i++) {
        now=trace(&p,now,2*second,refresh,.5,1);
        now=trace(&p,now,100000000,100000000,3,1);
    }
    assert(!p.interp && p.switches==0);
    puts("hybrid isolated100ms spikes: native, switches=0");

    fps_hybrid_init(&p,fps_hybrid_defaults(),now);
    now=trace(&p,now,12*second,25000000,1.3,1);
    assert(p.interp);
    now=trace(&p,now,7*second,33333333,.5,1);
    fps_hybrid_work(&p,now+second,1,.5,.5); now+=second;
    now=trace(&p,now,7*second,33333333,.5,1);
    assert(p.interp); /* Suspend/idle does not pay recovery. */
    now=trace(&p,now,9*second,33333333,.5,0);
    assert(p.interp); /* Missing/frozen controller cannot prove recovery. */
    now=trace(&p,now,9*second,33333333,.5,1);
    assert(!p.interp && p.switches==2);
    fps_hybrid_time(&p,now-second);
    assert(p.desired==p.interp && !p.slow_windows && !p.work_start);
    puts("hybrid gap/unknown/reversed clock: evidence reset, no false recovery");
}
static void handover(void) {
    vk_present_slots slots={0}; vk_present_interp pair={.retained=-1};
    uint64_t now=second;
    vk_present_slot a=vk_present_slots_reserve(&slots,1,now,refresh,1,now);
    assert(pair.retained==-1); /* Transient native interval2 never pushes. */
    a=vk_present_slots_reserve(&slots,1,now,refresh,2,now);
    assert(vk_present_interp_push(&pair,0,2,1,a,now).reason==VK_INTERP_WARMUP);
    a=vk_present_slots_reserve(&slots,1,now,refresh,2,now);
    vk_present_interp_pair child=vk_present_interp_push(&pair,1,3,1,a,now);
    assert(child.real==0 && child.midpoint_ns==a.desired_ns+refresh);
    vk_present_slot native=vk_present_slots_reserve(&slots,1,now,refresh,1,now);
    assert(native.desired_ns==child.midpoint_ns);
    if (native.desired_ns<=vk_present_interp_child_floor(&pair))
        native=vk_present_slots_reserve(&slots,1,now,refresh,1,now);
    assert(native.desired_ns>child.midpoint_ns);
    assert(vk_present_interp_leave(&pair)==1 && pair.retained==-1);
    assert(vk_present_interp_leave(&pair)==-1);
    a=vk_present_slots_reserve(&slots,1,now,refresh,2,now);
    assert(vk_present_interp_push(&pair,2,4,1,a,now).reason==VK_INTERP_WARMUP);
    puts("hybrid slot handover: native/EMPTY/pair/discard-retained/native/EMPTY; queued child never overtaken");
    /* Sustained native overload may leave its lattice one refresh behind
     * the next now. Simply changing to interval2 preserves that late phase,
     * which a paced real30 producer cannot catch up. Entry seeds full lead
     * without changing the last target floor or an already copied slot. */
    now+=10*second;
    slots=(vk_present_slots){.desired_ns=now-3*refresh,.refresh_ns=refresh};
    vk_present_slot queued={.desired_ns=slots.desired_ns};
    vk_present_slots inherited=slots;
    vk_present_slot late=vk_present_slots_reserve(&inherited,1,now,refresh,2,now);
    assert(late.desired_ns==now-refresh);
    slots.refresh_ns=0; /* same entry rephase as pres_slot_reserve */
    vk_present_slot entry=vk_present_slots_reserve(&slots,1,now,refresh,2,now);
    assert(entry.desired_ns==now+2*refresh && entry.desired_ns>queued.desired_ns);
    assert(queued.desired_ns==now-3*refresh);
    puts("hybrid entry: native late phase rebased to full real30 lead; queued target untouched");
}
static void runtime(void) {
    setenv("TOTK_FRAMEGEN","hybrid",1); setenv("TOTK_FPS60","1",1);
    setenv("TOTK_FPS_AUTO","0",1); setenv("TOTK_ACQUIRE_SLACK","0",1);
    setenv("TOTK_HYBRID_DROP_SECONDS","1",1); setenv("TOTK_HYBRID_DWELL_SECONDS","1",1);
    setenv("TOTK_HYBRID_RECOVER_SECONDS","1",1);
    assert(runtime_fps_hybrid_selected() && !runtime_fps_hybrid_interpolating());
    uint64_t now=second;
    for (unsigned i=0;i<700;i++) {
        now+=25000000;
        unsigned effective=runtime_fps_hybrid_acquire(now,refresh,1,1,1.3,1.3);
        assert(effective==(runtime_fps_hybrid_interpolating()?2u:1u));
        runtime_fps_hybrid_present(now+1,4000,0);
    }
    assert(runtime_fps_hybrid_interpolating());
    /* Unknown estimates cannot release even though real30 is steady. */
    for (unsigned i=0;i<100;i++) {
        now+=33333333;
        assert(runtime_fps_hybrid_acquire(now,refresh,1,0,.5,.5)==2);
        runtime_fps_hybrid_present(now+1,4000,0);
    }
    for (unsigned i=0;i<100;i++) {
        now+=runtime_fps_hybrid_interpolating()?33333333:refresh;
        runtime_fps_hybrid_acquire(now,refresh,1,1,.5,.5);
        runtime_fps_hybrid_present(now+1,4000,0);
    }
    assert(!runtime_fps_hybrid_interpolating());
    assert(runtime_fps_hybrid_acquire(now+refresh,refresh,3,1,.5,.5)==3);
    puts("hybrid production runtime: startup, 10s scene settle, real30 floor, recovery, guest interval3 preserved");
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--refused")) {
        assert(!runtime_fps_hybrid_selected());
        assert(!runtime_fps_hybrid_interpolating());
        assert(runtime_fps_hybrid_acquire(second,refresh,1,1,.5,.5)==1);
        puts("hybrid incompatible startup refused without changing native pacing"); return 0;
    }
    traces(); handover(); runtime(); puts("fps hybrid: all fixtures passed"); return 0;
}
