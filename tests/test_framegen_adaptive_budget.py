"""Compile the production TOTK_FG_ADAPTIVE_BUDGET selector and budget admission.

No Android/Vulkan execution or hardware timing claim. The real vk_framegen.c
selector, pre-admission, work reservation and prepared audit run against the
pure budget headers; flag-off decisions, policy state and logs are compared
with the unchanged r7 functions.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'


def function(source, name):
    match = re.search(r'^(?:static )?(?:int|void|uint64_t) ' + name + r'\(', source, re.M)
    if match is None:
        raise AssertionError('missing production function ' + name)
    brace = source.index('{', match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


FIXTURE = r'''
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "framegen_dispatch_trace.h"
#include "framegen_qcom_policy.h"
#include "framegen_budget_lsfg.h"
static struct {
    int budget_override; fg_budget_policy budget; fg_qcom_prepared_policy prepared_budget;
    fg_budget_decision budget_pre; uint64_t budget_frame,budget_pre_frame,budget_reserved_frame;
    uint64_t budget_pre_skips[2],budget_skips;
    uint64_t adaptive_budget_frames[FG_BUDGET_FRAMES];
    uint64_t adaptive_budget_admits,adaptive_budget_refusals,adaptive_budget_reservations;
    uint64_t adaptive_budget_audits,adaptive_budget_overshoots,adaptive_budget_missing;
} F;
static pthread_mutex_t fg_stats_mu=PTHREAD_MUTEX_INITIALIZER;
_Thread_local fg_dispatch_trace *vk_fg_dispatch_trace_current;
_Atomic int vk_fg_terminal_owned_active;
static _Atomic int fg_adaptive_budget_config=-1;
static int adaptive=1,lsfg=0,getenv_calls;
static const char *env_value;
static char logs[1<<16];
static size_t log_size;
static unsigned log_lines;
static void logging(const char *format,...) {
    va_list args; va_start(args,format);
    int n=vsnprintf(logs+log_size,sizeof logs-log_size,format,args); va_end(args);
    assert(n>=0 && (size_t)n+1<sizeof logs-log_size); log_size+=(size_t)n;
    logs[log_size++]='\n'; logs[log_size]=0; log_lines++;
}
#define LOGV(...) logging(__VA_ARGS__)
static int vk_fg_interpolating(void) { return 1; }
static int vk_fg_lsfg_active(void) { return lsfg; }
static int vk_fg_adaptive(void) { return adaptive; }
static char *test_getenv(const char *name) {
    assert(!strcmp(name,"TOTK_FG_ADAPTIVE_BUDGET")); getenv_calls++; return (char *)env_value;
}
#define getenv test_getenv
'''

MAIN = r'''
static void reset(const char *value,int scheduler) {
    memset(&F,0,sizeof F); atomic_store(&fg_adaptive_budget_config,-1);
    atomic_store(&vk_fg_terminal_owned_active,scheduler); adaptive=1; lsfg=0;
    env_value=value; getenv_calls=0; log_size=0; logs[0]=0; log_lines=0;
    vk_fg_dispatch_trace_current=NULL;
}
static void interval(fg_budget_policy *p,uint64_t frame,double real,double incremental) {
    uint64_t ticket=p->last_ticket+1;
    fg_budget_sample(p,ticket,real,1);
    if (incremental>=0) { ticket++; fg_budget_sample(p,ticket,incremental,1); fg_budget_synthetic(p,ticket,ticket); }
    fg_budget_seal(p,frame,ticket);
}
/* r7 shape: one measured child at interval 3, then five child-free intervals. */
static void gap_state(double real,double incremental,uint64_t last_probe) {
    interval(&F.budget,1,real,-1); interval(&F.budget,2,real,-1);
    interval(&F.budget,3,real,incremental);
    for (uint64_t frame=4;frame<=8;frame++) interval(&F.budget,frame,real,-1);
    F.budget.last_probe=last_probe; F.budget_frame=8;
}
static int same(const void *a,const void *b,size_t n) { return !memcmp(a,b,n); }
static int zero_accounting(void) {
    for (unsigned i=0;i<FG_BUDGET_FRAMES;i++) if (F.adaptive_budget_frames[i]) return 0;
    return !F.adaptive_budget_admits && !F.adaptive_budget_refusals && !F.adaptive_budget_reservations &&
        !F.adaptive_budget_audits && !F.adaptive_budget_overshoots && !F.adaptive_budget_missing;
}
/* Charge interval 9 with one synthetic ticket, complete it, then audit. */
static void charge_and_audit(double real,double incremental) {
    uint64_t ticket=F.budget.last_ticket+2;
    assert(fg_qcom_prepared_charge(&F.prepared_budget,9,ticket,ticket));
    interval(&F.budget,9,real,incremental); F.budget_frame=9;
    fg_prepared_budget_audit();
}
int main(void) {
    struct { const char *value; int on; const char *log; } table[]={
        {NULL,0,NULL},{"0",0,NULL},{"1",1,"[framegen-adaptive-budget] enabled:"},
        {"",0,"[framegen-adaptive-budget] ignored:"},{"01",0,"[framegen-adaptive-budget] ignored:"},
        {"true",0,"[framegen-adaptive-budget] ignored:"},{"1 ",0,"[framegen-adaptive-budget] ignored:"}};
    for (unsigned i=0;i<sizeof table/sizeof *table;i++) {
        reset(table[i].value,1);
        assert(fg_adaptive_budget()==table[i].on && fg_adaptive_budget()==table[i].on);
        assert(fg_adaptive_budget_selected()==table[i].on && getenv_calls==1);
        assert(table[i].log ? log_lines==1 && strstr(logs,table[i].log)==logs : !log_size);
    }
    /* Never consulted outside the adaptive slot scheduler. */
    reset("1",0);
    assert(!fg_adaptive_budget() && !getenv_calls && !fg_adaptive_budget_selected() && !log_size);
    reset("1",1); adaptive=0;
    assert(!fg_adaptive_budget() && !getenv_calls && atomic_load(&fg_adaptive_budget_config)==-1);

    /* Flag-off (absent, "0", invalid): byte-identical r7 decisions, policy and logs. */
    const char *off[]={NULL,"0","x"};
    for (unsigned i=0;i<3;i++) for (uint64_t probe=0;probe<=5;probe+=5) {
        reset(off[i],1); gap_state(20,4,probe);
        fg_budget_policy shadow=F.budget;
        fg_budget_decision expected=fg_budget_admit(&shadow,9);
        int accepted=fg_budget_pre_admit();
        assert(accepted==(expected.status==FG_BUDGET_ACCEPT) && same(&F.budget_pre,&expected,sizeof expected));
        assert(probe ? expected.reason==FG_BUDGET_REASON_PROBE_RATE : expected.probe && accepted);
        expected=fg_budget_reserve(&shadow,9);
        assert(fg_budget_reserve_work()==accepted && same(&F.budget_pre,&expected,sizeof expected));
        assert(same(&F.budget,&shadow,sizeof shadow) && zero_accounting());
        assert(off[i] && !strcmp(off[i],"x") ? log_lines==1 && strstr(logs,"ignored") : !log_size);
    }

    /* Flag-on, stale total 24 ms: accepted without consuming the probe rate. */
    reset("1",1); gap_state(20,4,5);
    fg_dispatch_trace t={.enabled=1,.budget_status=-1,.budget_reason=-1,.probe=-1,.cooling=-1,.capacity=-1};
    vk_fg_dispatch_trace_current=&t;
    assert(fg_budget_pre_admit());
    assert(F.budget_pre.stale_incremental && !F.budget_pre.probe && F.budget_pre.status==FG_BUDGET_ACCEPT);
    assert(t.budget_reason==FG_BUDGET_REASON_NONE && t.probe==0 && t.cooling==0 && t.capacity==1);
    assert(strstr(logs,"[framegen-adaptive-budget] enabled:") &&
        strstr(logs,"[framegen-adaptive-budget-decision] frame=9 status=0 reason=none real_p90_ms=20.000000 stale_incremental_p90_ms=4.000000 total_ms=24.000000 ceiling_ms=30.000") &&
        strstr(logs,"newest_incremental=3 incremental_age=6") && strstr(logs,"stale_admits=1 stale_refusals=0"));
    assert(fg_budget_reserve_work());
    assert(F.budget.reservations==1 && !F.budget.probes && F.budget.last_probe==5 &&
        F.adaptive_budget_frames[9%FG_BUDGET_FRAMES]==9 && F.adaptive_budget_reservations==1);
    vk_fg_dispatch_trace_current=NULL;
    /* Overshoot of a stale-admitted charge: same immutable audit and K=8 back-off. */
    size_t before=log_size;
    charge_and_audit(27,4);
    char *prepared=strstr(logs+before,"[framegen-prepared-budget-actual] charge=9 ");
    char *stale=strstr(logs+before,"[framegen-adaptive-budget-actual] charge=9 admission=stale-estimate ");
    assert(prepared && stale && prepared<stale && strstr(prepared,"overshoot=1 audits=1 overshoots=1"));
    assert(strstr(stale,"actual_total_ms=31.000000 ceiling_ms=30.000 overshoot=1 stale_audits=1 stale_overshoots=1 stale_missing=0 backoff_intervals=8 cooldown_until=18"));
    assert(F.prepared_budget.cooldown_until==18 && F.adaptive_budget_audits==1 && F.adaptive_budget_overshoots==1);
    char flag_on_line[1024]; size_t n=strcspn(prepared,"\n"); assert(n<sizeof flag_on_line);
    memcpy(flag_on_line,prepared,n); flag_on_line[n]=0;
    /* The ordinary audit line is byte-identical to a flag-off probe admission. */
    reset(NULL,1); gap_state(20,4,0);
    assert(fg_budget_pre_admit() && F.budget_pre.probe && fg_budget_reserve_work() && F.budget.probes==1);
    charge_and_audit(27,4);
    assert(!strncmp(logs,flag_on_line,n) && logs[n]=='\n' && log_lines==1 && zero_accounting());

    /* Flag-on, stale total 31 ms: refused even though r7 would probe; nothing reserved. */
    reset("1",1); gap_state(27,4,0);
    fg_budget_policy shadow=F.budget;
    assert(fg_budget_admit(&shadow,9).probe && fg_budget_admit(&shadow,9).status==FG_BUDGET_ACCEPT);
    assert(!fg_budget_pre_admit() && F.budget_pre.reason==FG_BUDGET_REASON_STALE_ESTIMATE);
    assert(!strcmp(fg_dispatch_budget_reason(FG_BUDGET_REASON_STALE_ESTIMATE),"stale-estimate"));
    assert(strstr(logs,"frame=9 status=2 reason=stale-estimate") && strstr(logs,"stale_admits=0 stale_refusals=1"));
    assert(!fg_budget_reserve_work() && !F.budget.reservations && !F.budget.probes && !F.adaptive_budget_reservations);

    /* Prepared cooling/capacity still refuse after a stale budget acceptance. */
    reset("1",1); gap_state(20,4,5); F.prepared_budget.cooldown_until=10;
    assert(!fg_budget_pre_admit() && F.budget_pre.status==FG_BUDGET_ACCEPT && F.budget_pre.stale_incremental);

    /* Fresh incremental decisions and LSFG are untouched even when selected. */
    reset("1",1); gap_state(20,4,5); interval(&F.budget,9,20,4); F.budget_frame=9;
    shadow=F.budget;
    fg_budget_decision fresh=fg_budget_admit(&shadow,10);
    assert(fg_budget_pre_admit() && same(&F.budget_pre,&fresh,sizeof fresh));
    fresh=fg_budget_reserve(&shadow,10);
    assert(fg_budget_reserve_work() && same(&F.budget,&shadow,sizeof shadow) &&
        !F.adaptive_budget_frames[10%FG_BUDGET_FRAMES] && !F.adaptive_budget_reservations);
    reset("1",1); gap_state(20,4,5); lsfg=1; shadow=F.budget;
    fg_budget_decision lsfg_expected=fg_budget_lsfg_admit(&shadow,9);
    assert(fg_budget_pre_admit()==(lsfg_expected.status==FG_BUDGET_ACCEPT) &&
        same(&F.budget_pre,&lsfg_expected,sizeof lsfg_expected) && !getenv_calls);
    return 0;
}
'''


class AdaptiveBudget(unittest.TestCase):
    def test_selector_flag_off_equivalence_and_stale_accounting(self):
        backend = (GFX / 'vk_framegen.c').read_text()
        functions = '\n'.join(function(backend, name) for name in (
            'fg_adaptive_budget', 'fg_adaptive_budget_selected', 'fg_dispatch_budget_observe',
            'fg_prepared_budget_audit', 'fg_budget_reserve_work', 'fg_budget_pre_admit'))
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='adaptive-budget-', dir=scratch) as temp:
            path = Path(temp)
            (path / 'probe.c').write_text(FIXTURE + functions + MAIN)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                '-I' + str(GFX), str(path / 'probe.c'), '-pthread', '-lm', '-o', str(path / 'probe')], check=True)
            subprocess.run([str(path / 'probe')], check=True)


if __name__ == '__main__':
    unittest.main()
