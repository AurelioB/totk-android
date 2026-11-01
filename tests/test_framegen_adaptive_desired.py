"""Exercise production transaction admission with counted Android API calls.

The fixture compiles extracted vk_draw.c functions, not a parallel admission
model. Platform setters/apply are recording sinks; no Android/GPU success or
actual-present claim is made. TMPDIR selects disposable compiler output.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / 'games/totk/android/gfx/vk_draw.c'


def production_function(source, name):
    match = re.search(r'^static [^;{}\n]*\b' + re.escape(name) +
                      r'\s*\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'missing production function {name}')
    start = match.end() - 1
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]',
                             source[start:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[match.start():start + token.end()] + '\n'
    raise AssertionError(f'unclosed production function {name}')


class FramegenAdaptiveDesired(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='adaptive-desired-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        source = DRAW.read_text()
        admission = ''.join(production_function(source, name) for name in (
            'pres_slots_desired', 'pres_slots_end_locked',
            'pres_slots_real_fallback_locked', 'pres_slots_stage_real_locked',
            'pres_transaction_set_desired_locked', 'pres_adaptive_job'))
        transaction = production_function(source, 'pres_transaction')
        probe = directory / 'probe.c'
        probe.write_text(PRELUDE + admission + TRANSACTION_SUPPORT + transaction + MAIN)
        cls.probe = directory / 'probe'
        flags = [flag for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for flag in shlex.split(os.environ.get(name, ''))]
        result = subprocess.run([
            *compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
            *flags, '-pthread', '-I', str(ROOT / 'games/totk/android/gfx'),
            '-I', str(ROOT / 'games/totk/runtime'), str(probe), '-o', str(cls.probe)
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        outputs = []
        for trace in ('0', '1'):
            with self.subTest(name=name, trace=trace):
                env = os.environ.copy()
                env['TOTK_FRAMEGEN_JOB_TRACE'] = trace
                result = subprocess.run([str(self.probe), name], capture_output=True,
                                        text=True, timeout=10, env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outputs.append(result.stdout)
        return outputs[0]

    def test_generated_signed_max_reaches_setter_and_apply(self):
        self.run_case('generated-max')

    def test_generated_max_plus_one_never_reaches_setter_or_apply(self):
        self.run_case('generated-overflow')

    def test_generated_invalid_encodings_never_become_immediate_zero(self):
        self.run_case('generated-invalid')

    def test_real_signed_max_uses_scheduler_encoding(self):
        self.run_case('real-max')

    def test_real_max_plus_one_cancels_and_uses_logged_signed_host_now(self):
        self.assertIn('mode=REAL-only scheduler=0', self.run_case('real-overflow'))

    def test_unavailable_clock_uses_explicit_real_only_fallback(self):
        self.assertIn('reason=clock-unavailable', self.run_case('real-no-clock'))

    def test_invalid_fallback_host_now_never_reaches_setter_or_apply(self):
        self.assertIn('event=real-only-refused', self.run_case('real-invalid-now'))

    def test_final_encoding_change_cancels_staged_transaction(self):
        self.run_case('final-invalid')
        self.run_case('final-changed')

    def test_lattice_real_rephases_before_plan_and_reports_pre_apply_time(self):
        self.run_case('latch-real')

    def test_early_apply_alone_never_rephases_real_but_reports(self):
        self.run_case('early-real')

    def test_both_selectors_rephase_and_report(self):
        self.run_case('both-real')

    def test_flag_off_keeps_legacy_floor_guard_and_zero_semantics(self):
        self.run_case('legacy')

    def test_presenter_has_no_unchecked_desired_setter(self):
        source = DRAW.read_text()
        self.assertEqual(source.count('ASurfaceTransaction_setDesiredPresentTime('), 1)
        self.assertIn('ASurfaceTransaction_setDesiredPresentTime(',
                      production_function(source, 'pres_transaction_set_desired_locked'))


PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include "present_clock.h"
#include "vk_present_slots.h"
#include "framegen_adaptive_slots.h"
#include "framegen_adaptive_present.h"
#define NPRES 2
#define DIE(...) abort()
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"line %d: %s\n",__LINE__,#condition); return 1; } } while (0)
typedef struct { uint64_t id; } identity;
typedef struct {
    uint64_t desired_ns, refresh_ns, copy_open_ns, adaptive_epoch, adaptive_token;
    uint64_t generation, frame, fence, source_fence, timeline_frame, framegen_real, ready_ns;
    uint64_t record_done_ns, enqueue_ns, dequeue_ns, own_ready_ns, pair_ready_ns;
    unsigned adaptive_slots, adaptive_eligible, adaptive_real_conflicts, interval;
    unsigned timeline_role, queue_depth;
    int generated, interpolated;
    _Atomic uint64_t diag_apply_ns;
    fgs_slot adaptive_slot;
    runtime_present_timeline_entry display_timeline;
    identity prediction, auto_frame;
} pres_buf;
typedef struct { int unused; } ASurfaceTransaction;
typedef struct { int unused; } ASurfaceControl;
typedef struct { unsigned index; uint64_t sequence, generation; } vk_present_retire_token;
typedef struct {
    uint64_t id, generation, desired_ns, refresh_ns, apply_ns;
    unsigned interval;
} cadence;
typedef struct {
    uint64_t frame, framegen_real, source_frame, generation, apply_ns;
    int generated, interpolated;
    identity prediction;
    cadence cadence;
} vk_present_fence_entry;
typedef struct { uint64_t cadence_next; } observer;
typedef struct {
    ASurfaceControl *surface;
    int previous, done, release_fd, retire_async;
    pres_buf *current;
    uint64_t timeline_frame, timeline_apply;
    unsigned pending_transactions, queue_depth;
    uint64_t record_done_ns, enqueue_ns, dequeue_ns, own_ready_ns, pair_ready_ns;
    vk_present_retire_token retire_token;
    vk_present_fence_entry snapshot;
    observer *observer;
} pres_complete;
static pres_buf g_pres[NPRES];
static struct { uint64_t generation; int cancelled; } g_slot_prepared[NPRES];
static fgs_state g_slot_policy;
static uint64_t g_slot_clock_generation, g_slot_window_generation, g_slot_source_epoch;
static int g_win_display_id, g_pres_timeline, g_pres_slot_diag, g_adaptive_presenting=-1;
static int g_fg_job_trace;
static unsigned g_pres_floor_ns, g_pres_guard_ns;
static pthread_mutex_t g_win_mu=PTHREAD_MUTEX_INITIALIZER;
static struct { pres_complete *contexts[2]; } g_pres_retire;
static uint64_t g_pres_retire_sequence;
static fg_adaptive_present_state g_adaptive_publication;
static _Atomic uint64_t g_timeline_applied[2], g_timeline_deadline_late[2], g_timeline_ready_known[2];
static _Atomic int64_t g_timeline_ready_slack[2], g_timeline_apply_slack[2];
static uint64_t fixture_now=UINT64_C(1000000000);
static runtime_present_timelines fixture_clock;
static int fixture_active=1, fixture_clock_present=1, routed_cancellations;
static unsigned setters, applies, callbacks, zero_setters, nonpositive_setters;
static int64_t setter_value;
static int mutate_final;
static char fixture_logs[4096];
static void fixture_log(const char *format,...) {
    size_t used=strlen(fixture_logs);
    va_list ap; va_start(ap,format);
    vsnprintf(fixture_logs+used,sizeof fixture_logs-used,format,ap);
    va_end(ap);
}
#define LOGV fixture_log
static uint64_t host_now_ns(void) { return fixture_now; }
int runtime_android_adaptive_slots_active(void) { return fixture_active; }
int runtime_android_adaptive_slots_display_id(void) { return 0; }
int runtime_android_adaptive_slot_timelines(runtime_present_timelines *out) {
    if (!fixture_clock_present) return 0;
    *out=fixture_clock; return 1;
}
static void pres_slots_route_locked(const fgs_batch *batch) {
    routed_cancellations+=(int)batch->cancel_count;
}
static int fixture_latch, fixture_early, latch_reanchors, latch_applied;
static uint64_t latch_apply_ns, latch_publication_ns;
static int pres_latch_enabled(void) { return fixture_latch; }
static int pres_early_enabled(void) { return fixture_early; }
static void pres_latch_reanchor_locked(const runtime_present_timelines *clock,const pres_buf *b) {
    (void)clock; (void)b; latch_reanchors++;
}
static void pres_latch_applied_locked(pres_buf *b,uint64_t apply,uint64_t publication,uint64_t desired) {
    (void)b; (void)desired; latch_applied++; latch_apply_ns=apply; latch_publication_ns=publication;
}
static void ASurfaceTransaction_setDesiredPresentTime(ASurfaceTransaction *tx,int64_t value) {
    (void)tx; setters++; setter_value=value;
    zero_setters+=value==0; nonpositive_setters+=value<=0;
}
'''

TRANSACTION_SUPPORT = r'''
static fg_adaptive_present_result pres_adaptive_decide_locked(pres_buf *b) {
    if (mutate_final==1) b->desired_ns=(uint64_t)INT64_MAX+1;
    if (mutate_final==2) b->desired_ns-=1;
    return FG_ADAPTIVE_PRESENT_APPLY;
}
static void ASurfaceTransaction_apply(ASurfaceTransaction *tx) { (void)tx; applies++; }
static void pres_complete_cb(void *context,void *stats) { (void)context; (void)stats; }
static void ASurfaceTransaction_setOnComplete(ASurfaceTransaction *tx,void *context,
        void (*callback)(void *,void *)) { (void)tx; (void)context; (void)callback; callbacks++; }
static void pres_retire_sync_event(pres_complete *c,const char *event,unsigned pending) {
    (void)c; (void)event; (void)pending;
}
static int totk_ft_on(void) { return 0; }
static uint64_t totk_ft_now(void) { return 0; }
static void totk_ft_submission(uint64_t frame,uint64_t time,int kind) { (void)frame; (void)time; (void)kind; }
static void totk_ft_compositor(uint64_t frame,uint64_t time,int kind) { (void)frame; (void)time; (void)kind; }
static void pres_slots_published_locked(pres_buf *b,uint64_t now,vk_present_retire_token token) {
    (void)b; (void)now; (void)token;
}
static void vk_overlay_presented(uint64_t now,int generated) { (void)now; (void)generated; }
static void pres_child_handoff(void) {}
static void pres_job_trace(const pres_buf *b,const char *event) { (void)b; (void)event; }
enum { PRESENT_PREDICT_AHB, FT_SUBMIT_AHB_APPLY };
static void pres_predict_feedback(pres_buf *b,int path,uint64_t ready,uint64_t preparation,uint64_t setup) {
    (void)b; (void)path; (void)ready; (void)preparation; (void)setup;
}
static void pres_transaction_finish(pres_complete *c) { (void)c; }
'''

MAIN = r'''
static int run(pres_buf *b,pres_complete *c) {
    ASurfaceTransaction tx={0}; ASurfaceControl surface={0};
    if (b->adaptive_epoch) pthread_mutex_lock(&g_win_mu);
    return pres_transaction(&tx,c,&surface,-1,b,0,1,1,b->adaptive_epoch!=0);
}
static void initialize(pres_buf *b,pres_complete *c,observer *o,uint64_t target,int generated) {
    const uint64_t period=UINT64_C(16666667);
    *b=(pres_buf){.desired_ns=target,.refresh_ns=period,.adaptive_epoch=3,.generation=7,
        .adaptive_slots=1,.adaptive_eligible=1,.adaptive_token=42,.generated=generated,
        .frame=11,.interval=1,.adaptive_slot={.target_ns=target}};
    *c=(pres_complete){.observer=o}; *o=(observer){0};
    fixture_clock=(runtime_present_timelines){.generation=5,.refresh_ns=period,.count=1,
        .entries={{.expected_ns=target}}};
    (void)fgs_init(&g_slot_policy,7,target,period,UINT64_C(2000000));
    g_slot_clock_generation=5; g_slot_window_generation=7; g_slot_source_epoch=3;
    g_slot_policy.pending[0]=(fgs_request){.token=42}; g_slot_policy.pending_count=1;
    g_slot_prepared[0].generation=1;
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    const char *trace=getenv("TOTK_FRAMEGEN_JOB_TRACE");
    g_fg_job_trace=trace && !strcmp(trace,"1");
    pres_buf *b=&g_pres[0]; pres_complete c; observer o;
    initialize(b,&c,&o,(uint64_t)INT64_MAX,1);
    if (!strcmp(argv[1],"generated-max")) {
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_APPLY);
        CHECK(setters==1 && applies==1 && callbacks==1 && !nonpositive_setters);
        CHECK(setter_value==INT64_MAX-1 && c.snapshot.cadence.desired_ns==(uint64_t)setter_value);
    } else if (!strcmp(argv[1],"generated-overflow")) {
        b->desired_ns=b->adaptive_slot.target_ns=(uint64_t)INT64_MAX+1;
        uint64_t untouched=123;
        CHECK(!pres_slots_desired(b,&untouched) && untouched==123);
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_SUPPRESSED);
        CHECK(!setters && !applies && !callbacks && !zero_setters && !o.cadence_next && c.done);
    } else if (!strcmp(argv[1],"generated-invalid")) {
        uint64_t targets[]={0,1,UINT64_C(16666667),UINT64_MAX};
        for (unsigned i=0;i<sizeof targets/sizeof targets[0];i++) {
            b->desired_ns=b->adaptive_slot.target_ns=targets[i];
            CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_SUPPRESSED);
        }
        b->desired_ns=b->adaptive_slot.target_ns=(uint64_t)INT64_MAX; b->refresh_ns=1;
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_SUPPRESSED);
        CHECK(!setters && !applies && !callbacks && !nonpositive_setters);
    } else if (!strcmp(argv[1],"real-max")) {
        b->generated=0;
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_APPLY);
        CHECK(setters==1 && applies==1 && !nonpositive_setters && setter_value==INT64_MAX-1);
        CHECK(c.snapshot.cadence.desired_ns==(uint64_t)setter_value && b->adaptive_slot.target_ns==(uint64_t)INT64_MAX);
    } else if (!strcmp(argv[1],"real-overflow") || !strcmp(argv[1],"real-no-clock")) {
        initialize(b,&c,&o,(uint64_t)INT64_MAX+1,0);
        if (!strcmp(argv[1],"real-no-clock")) fixture_clock_present=0;
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_APPLY);
        CHECK(setters==1 && applies==1 && !nonpositive_setters && (uint64_t)setter_value==fixture_now);
        CHECK(c.snapshot.cadence.desired_ns==fixture_now && b->desired_ns==fixture_now);
        CHECK(!b->adaptive_slot.target_ns && !b->adaptive_token && !b->adaptive_eligible && !b->refresh_ns && !b->interval);
        CHECK(!g_slot_policy.configured && !g_slot_policy.pending_count && routed_cancellations==1 && g_slot_prepared[0].cancelled);
        CHECK(strstr(fixture_logs,"event=real-only-fallback") && strstr(fixture_logs,"mode=REAL-only scheduler=0"));
        puts(fixture_logs);
    } else if (!strcmp(argv[1],"real-invalid-now")) {
        fixture_clock_present=0; b->generated=0;
        uint64_t times[]={0,(uint64_t)INT64_MAX+1};
        for (unsigned i=0;i<sizeof times/sizeof times[0];i++) {
            fixture_now=times[i];
            CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_SUPPRESSED);
        }
        CHECK(!setters && !applies && !callbacks && !nonpositive_setters && !o.cadence_next);
        CHECK(!g_slot_policy.configured && !b->adaptive_slot.target_ns);
        CHECK(strstr(fixture_logs,"event=real-only-refused")); puts(fixture_logs);
    } else if (!strcmp(argv[1],"final-invalid") || !strcmp(argv[1],"final-changed")) {
        mutate_final=!strcmp(argv[1],"final-invalid") ? 1 : 2;
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_SUPPRESSED);
        CHECK(setters==1 && !applies && callbacks==1 && !nonpositive_setters);
        CHECK(!o.cadence_next && !c.current && !c.snapshot.cadence.desired_ns && c.done);
    } else if (!strcmp(argv[1],"latch-real") || !strcmp(argv[1],"early-real") ||
            !strcmp(argv[1],"both-real")) {
        fixture_latch=strcmp(argv[1],"early-real")!=0;
        fixture_early=strcmp(argv[1],"latch-real")!=0;
        initialize(b,&c,&o,UINT64_C(5000000000),0);
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_APPLY);
        /* Only LATTICE re-phases at REAL staging; either selector feeds the hook. */
        CHECK(setters==1 && applies==1 && latch_reanchors==fixture_latch && latch_applied==1);
        CHECK(latch_apply_ns==fixture_now && latch_publication_ns==fixture_now);
        CHECK(b->adaptive_slot.target_ns==UINT64_C(5000000000) && setter_value==INT64_C(4999999999));
    } else if (!strcmp(argv[1],"legacy")) {
        b->adaptive_slots=0; b->adaptive_epoch=0; b->generated=0;
        b->desired_ns=UINT64_C(123456789); g_pres_floor_ns=1; g_pres_guard_ns=125000;
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_APPLY);
        CHECK((uint64_t)setter_value==b->desired_ns-125001 && c.snapshot.cadence.desired_ns==b->desired_ns);
        b->desired_ns=0;
        CHECK(run(b,&c)==FG_ADAPTIVE_PRESENT_APPLY);
        CHECK(setters==2 && applies==2 && setter_value==0 && zero_setters==1);
    } else return 2;
    if (g_fg_job_trace && applies && b->adaptive_epoch)
        CHECK(strstr(fixture_logs,"stage=transaction-published"));
    if (!g_fg_job_trace)
        CHECK(!strstr(fixture_logs,"stage=transaction-published"));
    if (!fixture_latch) CHECK(!latch_reanchors);
    if (!fixture_latch && !fixture_early && !g_fg_job_trace) CHECK(!latch_applied);
    if (g_fg_job_trace && applies && b->adaptive_epoch && b->adaptive_slots) CHECK(latch_applied==(int)applies);
    return 0;
}
'''


if __name__ == '__main__':
    unittest.main()
