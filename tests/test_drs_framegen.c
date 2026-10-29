#include <assert.h>
#include <stdio.h>
#include "../games/totk/android/gfx/framegen_drs.h"
int main(void) {
    fg_drs_policy p={0};
    uint64_t t=FG_DRS_SECOND;
    assert(fg_drs_update(&p,t,0,1,4)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,t,1,0,NAN)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,t,1,1,4)==25.5);
    for (unsigned i=1;i<30;i++) {
        double cap=fg_drs_update(&p,t+i*FG_DRS_SECOND/30,1,i+1,8);
        assert(cap==25.5); /* no per-frame publication */
    }
    assert(fg_drs_update(&p,2*t,1,31,8)==24.5);
    assert(fg_drs_update(&p,3*t,1,61,8)==24.0);
    /* Safety restore ignores the 1-Hz rate limit; reactivation does not. */
    assert(fg_drs_update(&p,3*t+1,0,62,8)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,3*t+2,1,63,2)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,4*t,1,64,2)==27.5);
    assert(fg_drs_update(&p,5*t,1,65,0)==27.5); /* .5-ms recovery deadband */
    assert(fg_drs_update(&p,6*t,1,66,0)==27.75);
    double prior=p.budget_ms;
    for (unsigned i=7;i<40;i++) {
        double cap=fg_drs_update(&p,i*t,1,i+60,0);
        assert(cap>=prior&&cap-prior<=.250001&&cap<=30&&cap>=24);
        prior=cap;
    }
    assert(p.budget_ms>=29); /* recovery reaches within its .5-ms deadband */
    /* An overload can remove fresh incremental admissions for 30 ticks; hold
       the measured cost through those sparse probes, not forever. */
    uint64_t sample=p.sample_frame, sampled=p.sample_ns;
    assert(fg_drs_update(&p,sampled+FG_DRS_SECOND,1,sample,NAN)!=FG_DRS_NATIVE);
    assert(fg_drs_update(&p,sampled+FG_DRS_STALE+1,1,sample,NAN)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,sampled+FG_DRS_STALE+2,1,999,NAN)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,sampled+FG_DRS_STALE+3,1,1000,-1)==FG_DRS_NATIVE);
    assert(fg_drs_update(&p,sampled+FG_DRS_STALE+4,1,1001,INFINITY)==FG_DRS_NATIVE);
    puts("DRS framegen policy PASS: clamps, sparse probes, EWMA, 1Hz, hysteresis, recovery, immediate inactive/stale restoration");
}
