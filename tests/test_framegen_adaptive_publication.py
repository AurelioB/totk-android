"""Compile actual presenter arbitration/queue functions with scalar clocks/tickets.

This does not simulate Android/SF or prove display outcomes. The intentionally
phase-offset producer keys reproduce the reviewed pre-apply integration defect.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'


def function(source, name):
    # Find the declaration containing this exact function, not a nearby helper.
    pos = source.index(name + '(')
    start = source.rfind('\nstatic ', 0, pos) + 1
    brace = source.index('{', pos)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class AdaptivePublication(unittest.TestCase):
    def test_actual_presenter_rekeys_ready_real_before_generated_apply(self):
        source = (GFX / 'vk_draw.c').read_text()
        functions = '\n'.join(function(source, n) for n in (
            'pres_adaptive_job', 'pres_slots_ready_real_locked', 'pres_early_release_locked',
            'pres_adaptive_decide_locked', 'pres_queue_locked'))
        probe = r'''#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include "framegen_adaptive_publication.h"
#include "framegen_adaptive_present.h"
#include "framegen_adaptive.h"
#include "framegen_adaptive_latch.h"
#define NPRES 8
#define P UINT64_C(16666667)
#define BASE UINT64_C(1000000000)
static unsigned checks;
#define CHECK(x) do {checks++; if (!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1;}} while (0)
typedef struct {
    uint64_t desired_ns,frame,adaptive_epoch,generation,refresh_ns,copy_open_ns;
    uint64_t fence,source_fence,adaptive_token,adaptive_release_ns;
    int generated; unsigned adaptive_slots,interval,adaptive_latch_kind;
    uint64_t adaptive_source_ready_ns,adaptive_output_ready_ns;
    fgs_slot adaptive_slot;
    _Atomic int adaptive_suppressed;
} pres_buf;
typedef struct {uint64_t epoch;} runtime_fps_adaptive_frame;
static pres_buf g_pres[NPRES];
static int g_pres_q[NPRES*2],g_pres_qh,g_pres_qt;
static uint64_t now,ready[16]; static int done[16],window_live=1;
static void *g_win=&window_live;
static uint64_t g_win_generation=3,g_slot_clock_generation=4;
static int g_win_display_id=0,g_adaptive_presenting=-1;
static fgs_state g_slot_policy;
static fg_adaptive_present_state g_adaptive_publication;
static uint64_t host_now_ns(void) {return now;}
static uint64_t vk_present_ready_ns(uint64_t fence) {return ready[fence];}
static int mtl_wait(uint64_t fence,uint64_t timeout) {(void)timeout; return done[fence];}
static runtime_fps_adaptive_frame runtime_fps_adaptive_snapshot(void) {return (runtime_fps_adaptive_frame){7};}
static runtime_present_timelines fixture_clock;
static int latch_on; /* TOTK_FG_ADAPTIVE_EARLY_APPLY as admitted by pres_early_enabled() */
static int pres_early_enabled(void) {return latch_on;}
int runtime_android_adaptive_slot_timelines(runtime_present_timelines *out) {
    *out=fixture_clock; return 1;
}
int runtime_android_adaptive_slots_display_id(void) {return 0;}
static void pres_adaptive_wake_locked(void) {}
static void pres_slots_observe_real_locked(pres_buf *b,uint64_t ns) {(void)b;(void)ns;}
'''
        main = r'''
static void setup(int published_ready,int fence_done,uint64_t legacy_offset) {
    memset(g_pres,0,sizeof g_pres); memset(ready,0,sizeof ready); memset(done,0,sizeof done);
    memset(&g_adaptive_publication,0,sizeof g_adaptive_publication);
    g_pres_qh=0;g_pres_qt=1;g_pres_q[0]=1;
    g_adaptive_presenting=-1;
    fgs_batch batch; assert(fgs_reset(&g_slot_policy,3,BASE,P,2000000,&batch)==FGS_OK);
    fixture_clock=(runtime_present_timelines){.generation=4,.refresh_ns=P};
    now=BASE+P-2000000;
    fgs_request request={.token=1,.frame=10,.source_epoch=7,
        .real_slot=fgs_make_slot(&g_slot_policy,0,BASE),
        .slot=fgs_make_slot(&g_slot_policy,1,BASE+P),
        .publication_ns=BASE,.deadline_ns=now};
    g_slot_policy.pending[0]=request;g_slot_policy.pending_count=1;
    g_pres[0]=(pres_buf){.desired_ns=BASE+P,.frame=10,.adaptive_epoch=7,.generation=3,
        .refresh_ns=P,.fence=1,.source_fence=2,.adaptive_token=1,.generated=1,.adaptive_slots=1};
    g_pres[1]=(pres_buf){.desired_ns=BASE+P+legacy_offset,.frame=11,.adaptive_epoch=7,
        .generation=3,.refresh_ns=P,.fence=3,.adaptive_slots=1};
    ready[1]=now-1000000;ready[2]=now-2000000;
    ready[3]=published_ready?now-500000:0;done[3]=fence_done;
}
/* Existing r7 integration cases; main runs them with EARLY_APPLY off and on. With
 * no API entries (or no slot identity) EARLY_APPLY=1 must be decision-identical. */
static int scenarios(void) {
    /* Ready REAL has an old callback key five ms AFTER child S. Its admitted
     * scheduler key is S, so GEN must yield before any platform apply. */
    setup(1,1,5000000);
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_YIELD);
    CHECK(g_pres[1].adaptive_slot.target_ns==BASE+P);
    CHECK(g_pres[1].desired_ns==g_pres[0].desired_ns);
    CHECK(g_pres[1].refresh_ns==P && g_pres[1].interval==1);
    CHECK(g_slot_policy.pending_count==1); /* YIELD owns no GPU retirement. */
    pres_queue_locked(0);
    CHECK(g_pres_qt==2 && g_pres_q[0]==1 && g_pres_q[1]==0);
    CHECK(pres_adaptive_decide_locked(&g_pres[1])==FG_ADAPTIVE_PRESENT_APPLY);
    /* Multiple queued REALs are stably re-keyed ahead of GEN, no spin. */
    setup(1,1,5000000);g_pres[2]=g_pres[1];g_pres[2].frame=12;g_pres[2].desired_ns+=1000;
    g_pres_q[g_pres_qt++]=2;
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_YIELD);
    pres_queue_locked(0);
    CHECK(g_pres_q[0]==1 && g_pres_q[1]==2 && g_pres_q[2]==0);
    /* Not-ready REAL cannot win merely because it was enqueued. */
    setup(0,0,5000000);
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
    CHECK(g_pres[1].desired_ns==BASE+P+5000000);
    /* Timestamp retention is diagnostic, not authority to ignore a done real. */
    setup(0,1,5000000);
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_YIELD);
    /* A ready REAL that cannot reach S with the SAME latch margin belongs to
     * S+1; legacy key before S must not incorrectly suppress this child. */
    setup(1,1,0);now++;
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
    CHECK(g_pres[1].adaptive_slot.target_ns==BASE+2*P);
    /* Arrival wake-up must not permanently suppress GEN using that same
     * mismatched legacy key before the final authoritative replan. */
    setup(1,1,0);now++;g_pres_qt=0;g_adaptive_presenting=0;
    g_pres[1].desired_ns=BASE+P-1000000;
    pres_queue_locked(1);
    CHECK(!atomic_load(&g_pres[0].adaptive_suppressed));
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
    CHECK(g_pres[1].adaptive_slot.target_ns==BASE+2*P);
    /* Wrong epoch/window/older source never wins or changes keys. */
    for (int kind=0;kind<3;kind++) {
        setup(1,1,5000000);
        if (!kind) g_pres[1].adaptive_epoch++;
        else if(kind==1) g_pres[1].generation++;
        else g_pres[1].frame=9;
        CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
        CHECK(g_pres[1].desired_ns==BASE+P+5000000);
    }
    /* Legacy off path retains its original callback-lattice supersession. */
    setup(1,1,5000000);g_pres[0].adaptive_slots=g_pres[1].adaptive_slots=0;
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
    CHECK(g_pres[1].desired_ns==BASE+P+5000000);
    setup(1,1,0);g_pres_qt=0;g_adaptive_presenting=0;
    g_pres[0].adaptive_slots=g_pres[1].adaptive_slots=0;
    g_pres[1].desired_ns=BASE+P-1000000;
    pres_queue_locked(1);
    CHECK(atomic_load(&g_pres[0].adaptive_suppressed)); /* legacy unchanged */
    return 0;
}
/* EARLY_APPLY=1: a matched SF deadline releases a READY child before T-2ms, the
 * release is sticky through final re-checks, and a ready REAL still wins. */
static int lattice(void) {
    const uint64_t T=BASE+P, release=T-UINT64_C(9500000);
    for (int on=1;on>=0;on--) {
        latch_on=on;
        setup(0,0,5000000);
        now=T-UINT64_C(10000000);
        ready[1]=now-1000000;ready[2]=now-2000000;
        g_pres[0].adaptive_slot=g_slot_policy.pending[0].slot;
        fixture_clock=(runtime_present_timelines){.generation=4,.refresh_ns=P,.count=2,
            .entries={{BASE,BASE-UINT64_C(8000000),1},{T,T-UINT64_C(8000000),2}}};
        CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_WAIT);
        CHECK(g_pres[0].adaptive_release_ns==(on?release:0));
        CHECK(g_pres[0].adaptive_latch_kind==(on?(unsigned)FGA_LATCH_EARLY:0u));
        CHECK(g_pres[0].adaptive_output_ready_ns==ready[1] && g_pres[0].adaptive_source_ready_ns==ready[2]);
        now=release;
        CHECK(pres_adaptive_decide_locked(&g_pres[0])==(on?FG_ADAPTIVE_PRESENT_APPLY:FG_ADAPTIVE_PRESENT_WAIT));
        fixture_clock.entries[1].deadline_ns=now-1; /* SF deadline passes before final re-check */
        now+=100000;
        CHECK(pres_adaptive_decide_locked(&g_pres[0])==(on?FG_ADAPTIVE_PRESENT_APPLY:FG_ADAPTIVE_PRESENT_WAIT));
        CHECK(g_slot_policy.pending_count==1); /* decisions never commit */
        now=T-2000000;
        CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
    }
    /* A ready REAL still takes the slot inside the early window. */
    latch_on=1;
    setup(1,1,5000000);
    now=release;
    ready[1]=now-1000000;ready[2]=now-2000000;ready[3]=now-500000;
    g_pres[0].adaptive_slot=g_slot_policy.pending[0].slot;
    fixture_clock=(runtime_present_timelines){.generation=4,.refresh_ns=P,.count=2,
        .entries={{BASE,BASE-UINT64_C(8000000),1},{T,T-UINT64_C(8000000),2}}};
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_YIELD);
    CHECK(g_pres[1].adaptive_slot.target_ns==T);
    /* No matching API entry: exactly the r7 hold. */
    setup(0,0,5000000);
    now=T-UINT64_C(10000000);
    ready[1]=now-1000000;ready[2]=now-2000000;
    g_pres[0].adaptive_slot=g_slot_policy.pending[0].slot;
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_WAIT);
    CHECK(!g_pres[0].adaptive_release_ns && g_pres[0].adaptive_latch_kind==(unsigned)FGA_LATCH_R7_NO_MATCH);
    now=T-2000001;
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_WAIT);
    now=T-2000000;
    CHECK(pres_adaptive_decide_locked(&g_pres[0])==FG_ADAPTIVE_PRESENT_APPLY);
    latch_on=0;
    return 0;
}
int main(void) {
    for (latch_on=0;latch_on<2;latch_on++) if (scenarios()) return 1;
    latch_on=0;
    if (lattice()) return 1;
    printf("publication integration: %u checks\n",checks);return 0;
}
'''
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='adaptive-publication-',dir=scratch) as temp:
            path=Path(temp); (path/'probe.c').write_text(probe+functions+main)
            subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(GFX),str(path/'probe.c'),'-o',str(path/'probe')],check=True)
            subprocess.run([str(path/'probe')],check=True)

    def test_pressure_cancel_drains_unpublished_template_and_busy_slot(self):
        source=(GFX/'vk_draw.c').read_text().split('void vk_fg_slot_boundary(void)',1)[1]
        body='void vk_fg_slot_boundary(void)'+source.split('static void present_window',1)[0]
        probe=r'''
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include "framegen_adaptive_publication.h"
#include "framegen_adaptive.h"
#include "framegen_job_trace.h"
#define runtime_android_adaptive_slot_timelines runtime_test_adaptive_slot_timelines
#define runtime_android_adaptive_slots_display_id runtime_test_adaptive_slots_display_id
#define NPRES 2
#define VK_NULL_HANDLE 0
static char logs[16][1024];static unsigned log_count;
static void capture_log(const char *format,...) {
    if(log_count>=16)return;
    va_list args;va_start(args,format);
    vsnprintf(logs[log_count++],sizeof logs[0],format,args);va_end(args);
}
#define LOGV(...) capture_log(__VA_ARGS__)
enum {VK_FG_GENERATION_EMPTY,PREPARING,PREPARED,RECORDING,SUBMITTED,QUARANTINED};
typedef struct {uint64_t epoch;} runtime_fps_adaptive_frame;
typedef struct {int unused;} vk_post_output;
typedef struct {
    uint64_t refresh_ns,fence,source_fence,source_slot,desired_ns,copy_open_ns;
    uint64_t adaptive_token,adaptive_release_ns,record_done_ns,enqueue_ns,timeline_frame;
    unsigned timeline_role,adaptive_latch_kind;fgs_slot adaptive_slot;
    _Atomic int busy,adaptive_suppressed;
} pres_buf;
typedef struct {
    int *generation;int dispatching,cancelled,published,output_index;
    uint64_t epoch,window_generation,real_target,real_fence,frame;
    fg_adaptive_policy phase;fgs_request request;
} pres_slot_prepared;
static pres_slot_prepared g_slot_prepared[NPRES];
static pres_buf g_pres[NPRES];static unsigned g_slot_prepared_count;
static _Atomic int vk_fg_slot_boundary_active;
static pthread_mutex_t g_win_mu=PTHREAD_MUTEX_INITIALIZER;
static int requested=1;static void *g_win=&requested;
static uint64_t g_win_generation=4,g_slot_clock_generation=1;static int g_win_display_id;
static fgs_state g_slot_policy;
static struct {
    uint64_t first_ns,boundaries,opportunities,dispatched,cancelled,no_publication;
    uint64_t no_request,late,phase,unavailable,gpu_refused;
} g_slot_dispatch_stats;
static _Atomic uint64_t g_slot_on_time_applies,g_slot_late_applies;
static _Atomic uint64_t g_slot_template_refused,g_slot_irrevocable_real_conflicts,g_fg_generated,g_fg_pending;
static struct {uint64_t generated,phase[4];} g_adaptive_stats;
static int g_pres_qcom[NPRES],g_pres_lsfg[NPRES],g_bound_pipe,g_dyn_dirty,g_bound_set[1][2];
static int g_fg_job_trace,clock_ok=1,dispatch_ok=1;
static unsigned backend_cancel_reason;
static uint64_t host_now_ns(void) {return 1000000000;}
static int pres_slots_requested(void) {return requested;}
static runtime_fps_adaptive_frame runtime_fps_adaptive_snapshot(void) {return (runtime_fps_adaptive_frame){9};}
static int vk_fg_generation_state(const int *g) {return *g;}
static unsigned vk_fg_generation_cancel_reason(const int *g) {(void)g;return backend_cancel_reason;}
typedef struct {uint64_t observations,cancellations,appends,pending,quarantined;} vk_fg_snapshot_pressure_counts;
static vk_fg_snapshot_pressure_counts vk_fg_snapshot_pressure_get(void) {return (vk_fg_snapshot_pressure_counts){11,2,3,0,0};}
static int runtime_test_adaptive_slot_timelines(runtime_present_timelines *out) {
    *out=(runtime_present_timelines){.generation=1,.refresh_ns=16666667};return clock_ok;
}
static int runtime_test_adaptive_slots_display_id(void) {return 0;}
static void vk_fg_generation_cancel(int *g) {if (*g<RECORDING) *g=0;}
static int vk_fg_generation_dispatch(int *g,float t,uint64_t d,vk_post_output *o) {
    (void)g;(void)t;(void)d;(void)o;return dispatch_ok;
}
static uint64_t vk_fg_generation_seal(int *g) {(void)g;return 99;}
static void pres_queue_locked(int i) {(void)i;}
static pthread_cond_t g_pres_cv=PTHREAD_COND_INITIALIZER;
'''
        main=r'''
static int generation;
static fg_slot_cancel_reason run_once(int trace,int scenario) {
    memset(g_slot_prepared,0,sizeof g_slot_prepared);
    memset(g_pres,0,sizeof g_pres);memset(&g_slot_dispatch_stats,0,sizeof g_slot_dispatch_stats);
    memset(logs,0,sizeof logs);log_count=0;
    memset(&g_slot_policy,0,sizeof g_slot_policy);
    g_slot_policy.pending[0].token=17;g_slot_policy.pending_count=1;
    g_pres[0].refresh_ns=16666667;
    generation=PREPARED;requested=1;g_win=&requested;g_win_generation=4;g_slot_clock_generation=1;
    backend_cancel_reason=FG_SLOT_CANCEL_NONE;clock_ok=dispatch_ok=1;
    g_fg_generated=g_fg_pending=0;g_adaptive_stats.generated=0;
    memset(g_adaptive_stats.phase,0,sizeof g_adaptive_stats.phase);
    g_fg_job_trace=trace;g_slot_prepared[0]=(pres_slot_prepared){.generation=&generation,.epoch=9,
        .window_generation=4,.output_index=0,.published=1,.frame=123,.real_target=900000000,
        .request={.token=17,.deadline_ns=2000000000,.slot={.target_ns=1000000000}},
        .phase={.previous={.sim_ns=1,.anchor_ns=900000000,.epoch=9,.eligible=1},
            .current={.sim_ns=2,.anchor_ns=1100000000,.epoch=9,.eligible=1},
            .display_anchor_ns=900000000,.state=FG_ADAPTIVE_ACTIVE}};
    g_slot_prepared_count=1;atomic_store(&vk_fg_slot_boundary_active,1);atomic_store(&g_pres[0].busy,1);
    switch(scenario) {
    case 1: requested=0;break;
    case 2: g_slot_prepared[0].cancelled=1;break;
    case 3: g_win=0;break;
    case 4: g_slot_prepared[0].epoch=8;break;
    case 5: generation=VK_FG_GENERATION_EMPTY;backend_cancel_reason=FG_SLOT_CANCEL_SNAPSHOT_PRESSURE;break;
    case 6: generation=VK_FG_GENERATION_EMPTY;break;
    case 7: g_slot_prepared[0].request.token=0;break;
    case 8: g_slot_policy.pending[0].token=0;break;
    case 9: g_slot_prepared[0].request.deadline_ns=999999999;break;
    case 10: clock_ok=0;break;
    case 11: g_slot_prepared[0].phase.state=FG_ADAPTIVE_SCENE;break;
    case 12: dispatch_ok=0;break;
    default: break;
    }
    vk_fg_slot_boundary();
    if(!trace)return FG_SLOT_CANCEL_NONE;
    for(unsigned i=0;i<log_count;i++) {
        const char *at=strstr(logs[i],"reason=");
        if(at && strstr(logs[i],"[adaptive-slot-job]"))
            for(int r=FG_SLOT_CANCEL_NONE;r<=FG_SLOT_CANCEL_DISPATCH_REFUSED;r++) {
                const char *name=fg_slot_cancel_reason_name((fg_slot_cancel_reason)r);
                size_t length=strlen(name);
                if(!strncmp(at+7,name,length) && (at[7+length]==' ' || at[7+length]=='\0'))
                    return (fg_slot_cancel_reason)r;
            }
    }
    return FG_SLOT_CANCEL_NONE;
}
static void setup(int state) {
    memset(g_slot_prepared,0,sizeof g_slot_prepared);
    memset(g_pres,0,sizeof g_pres);memset(&g_slot_dispatch_stats,0,sizeof g_slot_dispatch_stats);
    generation=state;requested=1;g_win=&requested;g_fg_job_trace=0;
    g_slot_prepared[0]=(pres_slot_prepared){.generation=&generation,.epoch=9,
        .window_generation=4,.output_index=0,.published=0};
    g_slot_prepared_count=1;atomic_store(&vk_fg_slot_boundary_active,1);
    atomic_store(&g_pres[0].busy,1);
}
int main(void) {
    const fg_slot_cancel_reason expected[]={FG_SLOT_CANCEL_NONE,FG_SLOT_CANCEL_MODE,
        FG_SLOT_CANCEL_OWNER,FG_SLOT_CANCEL_WINDOW,FG_SLOT_CANCEL_EPOCH,
        FG_SLOT_CANCEL_SNAPSHOT_PRESSURE,FG_SLOT_CANCEL_EMPTY,FG_SLOT_CANCEL_NO_HOLE,
        FG_SLOT_CANCEL_POLICY,FG_SLOT_CANCEL_DEADLINE,FG_SLOT_CANCEL_CLOCK,
        FG_SLOT_CANCEL_PHASE,FG_SLOT_CANCEL_DISPATCH_REFUSED};
    for(int scenario=0;scenario<=12;scenario++) {
        fg_slot_cancel_reason off=run_once(0,scenario);
        unsigned off_cancelled=(unsigned)g_slot_dispatch_stats.cancelled;
        uint64_t off_dispatched=g_slot_dispatch_stats.dispatched;
        uint64_t off_opportunities=g_slot_dispatch_stats.opportunities;
        uint64_t off_generated=g_fg_generated,off_pending=g_fg_pending,off_ticket=g_pres[0].fence;
        unsigned off_busy=(unsigned)atomic_load(&g_pres[0].busy),off_prepared=g_slot_prepared_count;
        fg_slot_cancel_reason on=run_once(1,scenario);
        if(off!=FG_SLOT_CANCEL_NONE || on!=expected[scenario])
            fprintf(stderr,"scenario %d: trace-off=%u trace-on=%u expected=%u\n",
                scenario,(unsigned)off,(unsigned)on,(unsigned)expected[scenario]);
        assert(off==FG_SLOT_CANCEL_NONE && on==expected[scenario]);
        assert(off_cancelled==g_slot_dispatch_stats.cancelled &&
            off_dispatched==g_slot_dispatch_stats.dispatched &&
            off_opportunities==g_slot_dispatch_stats.opportunities &&
            off_generated==g_fg_generated && off_pending==g_fg_pending &&
            off_ticket==g_pres[0].fence && off_busy==(unsigned)atomic_load(&g_pres[0].busy) &&
            off_prepared==g_slot_prepared_count);
        if(scenario==0) assert(off_dispatched==1 && off_ticket==99);
        else assert(off_cancelled==1 && !off_ticket);
        if(scenario==0) {
            int saw_job=0,saw_detail=0;
            for(unsigned i=0;i<log_count;i++) {
                if(strstr(logs[i],"event=job")) {saw_job=1;assert(!strstr(logs[i],"reason="));}
                if(strstr(logs[i],"[adaptive-slot-job]")) {
                    saw_detail=1;assert(strstr(logs[i],"reason=none"));
                    assert(!strstr(logs[i],"event=job"));
                }
            }
            assert(saw_job && saw_detail);
        }
    }
    setup(VK_FG_GENERATION_EMPTY);
    vk_fg_slot_boundary();
    assert(!g_slot_prepared[0].generation && !g_slot_prepared_count);
    assert(!atomic_load(&g_pres[0].busy) && !atomic_load(&vk_fg_slot_boundary_active));
    assert(g_slot_dispatch_stats.cancelled==1 && !g_slot_dispatch_stats.no_publication);
    for (int state=PREPARING;state<=QUARANTINED;state++) {
        setup(state);vk_fg_slot_boundary();
        assert(g_slot_prepared[0].generation && g_slot_prepared_count==1);
        assert(atomic_load(&g_pres[0].busy) && atomic_load(&vk_fg_slot_boundary_active));
        assert(generation==state && g_slot_dispatch_stats.no_publication==1);
    }
    setup(VK_FG_GENERATION_EMPTY);requested=0;vk_fg_slot_boundary();
    assert(!g_slot_prepared_count && !atomic_load(&g_pres[0].busy));
    setup(VK_FG_GENERATION_EMPTY);g_win=0;vk_fg_slot_boundary();
    assert(!g_slot_prepared_count && !atomic_load(&g_pres[0].busy));
    return 0;
}
'''
        scratch=Path(os.environ.get('TMPDIR',ROOT/'.scratch'));scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='adaptive-pressure-cleanup-',dir=scratch) as temp:
            path=Path(temp);(path/'probe.c').write_text(probe+body+main)
            # -lm: the probe calls fmin/fmax, which GCC does not inline as clang does.
            subprocess.run(shlex.split(os.environ.get('CC','cc'))+
                ['-std=c11','-Wall','-Wextra','-Werror','-I'+str(GFX),
                 str(path/'probe.c'),'-pthread','-lm','-o',str(path/'probe')],check=True)
            subprocess.run([str(path/'probe')],check=True)


if __name__ == '__main__':
    unittest.main()
