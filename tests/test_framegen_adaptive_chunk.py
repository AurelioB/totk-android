"""Exercise the real recorder chunk hook and optional allocator with scalar drivers.

No Vulkan/Android execution or timing claim. Extracted production code is tested
against the pre-diagnostic allocator, including ordered calls and skipped facts.
The prepared Vulkan fixture separately proves real prefix/child/tail ownership.
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


COMMON = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char calls[4096],logs[16384];
static size_t call_size,log_size;
static int mutex_locked;
static void record(const char *name) {
    int n=snprintf(calls+call_size,sizeof calls-call_size,"%s,",name);
    assert(n>0 && (size_t)n<sizeof calls-call_size);call_size+=(size_t)n;
}
static void logging(const char *format,...) {
    assert(!mutex_locked); /* An allocator diagnostic must never own g_mu. */
    va_list args;va_start(args,format);
    int n=vsnprintf(logs+log_size,sizeof logs-log_size,format,args);va_end(args);
    assert(n>=0 && (size_t)n+1<sizeof logs-log_size);log_size+=(size_t)n;
    logs[log_size++]='\n';logs[log_size]=0;
}
#define LOGV(...) logging(__VA_ARGS__)
'''

ALLOCATOR = r'''
#include "framegen_dispatch_trace.h"
#define RING 4
static struct slot { int busy,submit_busy; } g_ring[RING];
typedef struct slot slot_t;
static slot_t *current,*optional,*pending;
static unsigned pins,pin_queries,cur_queries,next_queries,pending_queries;
static unsigned replay_queries[RING];
static uint64_t until[RING];
static int g_optional_immutable,g_ring_next,g_mu,try_result,opened;
static _Atomic uint64_t g_done;
_Thread_local fg_dispatch_trace *vk_fg_dispatch_trace_current;
static slot_t **observed_cur(void) { cur_queries++;record("cur");return &current; }
static slot_t **observed_next(void) { next_queries++;record("next");return &optional; }
static slot_t **observed_pending(void) { pending_queries++;record("pending");return &pending; }
static unsigned observed_pins(void) { pin_queries++;record("pins");return pins; }
#define g_cur (*observed_cur())
#define g_optional_next (*observed_next())
#define g_replay_pending (*observed_pending())
#define g_replay_pins observed_pins()
static int trylock(int *mu) {
    assert(mu==&g_mu && !mutex_locked);record("trylock");
    if(!try_result) mutex_locked=1;
    return try_result;
}
static int unlock(int *mu) {
    assert(mu==&g_mu && mutex_locked);record("unlock");mutex_locked=0;return 0;
}
#define pthread_mutex_trylock trylock
#define pthread_mutex_unlock unlock
static void replay_retire(uint64_t done) { assert(mutex_locked && done==7);record("retire"); }
static uint64_t replay_until(unsigned slot) {
    assert(mutex_locked && slot<RING);replay_queries[slot]++;
    char name[32];snprintf(name,sizeof name,"until%u",slot);record(name);return until[slot];
}
static void slot_open(slot_t *s) {
    assert(!mutex_locked);record("open");opened=(int)(s-g_ring);current=s;
}
/* Exact allocator before inner diagnostics: independent behavior/call oracle. */
static int original_optional_begin(int immutable) {
    if (g_cur || g_optional_next) return 0;
    g_optional_immutable=0;
    if (pthread_mutex_trylock(&g_mu)) return 0;
    replay_retire(atomic_load(&g_done));
    slot_t *first=NULL,*second=NULL;
    if (!g_replay_pending && g_replay_pins<RING-1) for (unsigned n=0;n<RING;n++) {
        unsigned i=((unsigned)g_ring_next+n)%RING;
        slot_t *s=&g_ring[i];
        if (s->busy || s->submit_busy || replay_until(i)) continue;
        if (!first) first=s;
        else { second=s; break; }
    }
    if (second) {
        first->busy=second->busy=1;
        g_optional_next=second;
        g_ring_next=((unsigned)(second-g_ring)+1)%RING;
    }
    pthread_mutex_unlock(&g_mu);
    if (!second) return 0;
    slot_open(first);
    g_optional_immutable=immutable;
    return 1;
}
'''

ALLOCATOR_MAIN = r'''
typedef struct {
    int result,current,optional,immutable,next,opened;
    unsigned busy_mask,submit_mask,cur_queries,next_queries,pending_queries,pin_queries;
    unsigned replay_queries[RING];
} outcome;
static outcome run(unsigned config,int guard,int trace,int immutable) {
    memset(g_ring,0,sizeof g_ring);memset(until,0,sizeof until);
    memset(replay_queries,0,sizeof replay_queries);
    current=optional=pending=NULL;pins=0;pin_queries=cur_queries=next_queries=pending_queries=0;
    g_ring_next=3;g_optional_immutable=9;try_result=0;opened=-1;atomic_store(&g_done,7);
    assert(!mutex_locked);call_size=log_size=0;calls[0]=logs[0]=0;
    for(unsigned i=0;i<RING;i++) {
        unsigned state=(config>>(i*3))&7;
        g_ring[i].busy=state&1;g_ring[i].submit_busy=(state>>1)&1;until[i]=(state&4)?99:0;
    }
    if(guard==1) current=&g_ring[0];
    if(guard==2) optional=&g_ring[1];
    if(guard==3) try_result=16;
    if(guard==4) pending=&g_ring[2];
    if(guard==5) pins=RING-1;
    if(guard==6) try_result=22; /* Preserve any nonzero trylock result, not only EBUSY. */
    fg_dispatch_trace dispatch={.enabled=trace==1,.source=44,.charge=3,.deadline=1000};
    vk_fg_dispatch_trace_current=trace<0?NULL:&dispatch;
    int result=trace==2?original_optional_begin(immutable):submit_optional_begin(immutable);
    assert(!mutex_locked);
    outcome out={.result=result,.current=current?(int)(current-g_ring):-1,
        .optional=optional?(int)(optional-g_ring):-1,.immutable=g_optional_immutable,
        .next=g_ring_next,.opened=opened,.cur_queries=cur_queries,.next_queries=next_queries,
        .pending_queries=pending_queries,.pin_queries=pin_queries};
    for(unsigned i=0;i<RING;i++) {
        out.busy_mask|=(unsigned)(g_ring[i].busy!=0)<<i;
        out.submit_mask|=(unsigned)(g_ring[i].submit_busy!=0)<<i;
        out.replay_queries[i]=replay_queries[i];
    }
    vk_fg_dispatch_trace_current=NULL;
    return out;
}
static void same(outcome a,outcome b) {
#define SAME(field) assert(a.field==b.field)
    SAME(result);SAME(current);SAME(optional);SAME(immutable);SAME(next);SAME(opened);
    SAME(busy_mask);SAME(submit_mask);SAME(cur_queries);SAME(next_queries);
    SAME(pending_queries);SAME(pin_queries);
#undef SAME
    for(unsigned i=0;i<RING;i++) assert(a.replay_queries[i]==b.replay_queries[i]);
}
static void compare(unsigned config,int guard,int immutable,const char *reason,const char *facts) {
    outcome expected=run(config,guard,2,immutable);
    char expected_calls[sizeof calls];strcpy(expected_calls,calls);assert(!log_size);
    for(int trace=-1;trace<=1;trace++) {
        outcome actual=run(config,guard,trace,immutable);same(expected,actual);
        assert(!strcmp(expected_calls,calls));
        if(trace!=1) { assert(!log_size);continue; }
        assert(strstr(logs,"source=44 charge=3 deadline-ns=1000 stage=optional-begin"));
        assert(strstr(logs,"unqueried=-1"));
        if(reason) {
            char wanted[128];snprintf(wanted,sizeof wanted,"reason=%s ",reason);
            assert(strstr(logs,wanted));
        }
        if(facts) assert(strstr(logs,facts));
    }
}
int main(void) {
    compare(0,1,1,"open-primary","open-primary=1 optional-next=-1 mutex-result=-1 replay-pending=-1 replay-pins=-1");
    compare(0,2,1,"optional-next","open-primary=0 optional-next=1 mutex-result=-1");
    compare(0,3,1,"mutex-busy","mutex-result=16 replay-pending=-1 replay-pins=-1");
    compare(0,6,1,"mutex-busy","mutex-result=22 replay-pending=-1 replay-pins=-1");
    compare(0,4,1,"replay-pending","replay-pending=1 replay-pins=-1 pin-cap=3 scanned=-1 free-slots=-1");
    compare(0,5,1,"pin-cap","replay-pending=0 replay-pins=3 pin-cap=3 scanned=-1 free-slots=-1");
    compare(01111,0,1,"free-slots","busy-checked=4 busy-skips=4 submit-busy-checked=0 submit-busy-skips=0 replay-checked=0 replay-skips=0");
    compare(02222,0,1,"free-slots","busy-checked=4 busy-skips=0 submit-busy-checked=4 submit-busy-skips=4 replay-checked=0 replay-skips=0");
    compare(04444,0,1,"free-slots","busy-checked=4 busy-skips=0 submit-busy-checked=4 submit-busy-skips=0 replay-checked=4 replay-skips=4");
    compare(01110,0,1,"free-slots","scanned=4 free-slots=1");
    compare(0,0,1,"accepted","scanned=2 free-slots=2");
    /* All combinations of busy, submit-busy and replay lease at every slot,
     * including overlapping refusal facts: the original || order is authoritative. */
    for(unsigned config=0;config<(1u<<(RING*3));config++)
        for(int immutable=0;immutable<=1;immutable++) compare(config,0,immutable,NULL,NULL);
    return 0;
}
'''

CHUNK = r'''
#include "framegen_adaptive_defaults.h"
#define VK_PASS_END_SUBMISSION 1
static int g_in_sub,g_ncarry,callback_mode,service_mode,env_calls,trace_calls,callback_calls;
static int guest_slot,child_slot;
static int *g_cur;
static uint64_t g_gpu_guest_ns,g_gpu_origin_ticks,g_gpu_guest_frame,ticket,now;
static _Atomic int vk_fg_slot_boundary_active;
static const char *selector;
static int trace_enabled;
static uint64_t prefix[4],child[4],tail[4];
static const char *observed_getenv(const char *name) {
    assert(!strcmp(name,"TOTK_FG_ADAPTIVE_CHUNK"));env_calls++;return selector;
}
#define getenv observed_getenv
static int vk_fg_job_trace_enabled(void) { trace_calls++;return trace_enabled; }
static uint64_t host_now_ns(void) { return ++now; }
static uint64_t slot_submit(int with_posts,int reason) {
    assert(g_cur && reason==VK_PASS_END_SUBMISSION);record(with_posts?"tail":"submit");
    uint64_t *context=g_cur==&child_slot?child:with_posts?tail:prefix;
    context[0]=(g_in_sub||with_posts)?g_gpu_guest_ns:0;
    context[1]=(g_in_sub||with_posts)?g_gpu_origin_ticks:0;
    context[2]=(g_in_sub||with_posts)?g_gpu_guest_frame:0;
    context[3]=with_posts?(uint64_t)g_ncarry:0;
    if(with_posts) g_ncarry=0;
    g_cur=NULL;now+=50;return ++ticket;
}
static void vk_rec_service_run(int boundary) {
    assert(boundary==1);record("service");now+=20;
    if(service_mode==1) atomic_store(&vk_fg_slot_boundary_active,0);
    if(service_mode==2) g_cur=&guest_slot;
    if(service_mode==3) atomic_store(&vk_fg_slot_boundary_active,1);
}
static void vk_fg_slot_boundary(void) {
    record("boundary");callback_calls++;
    assert(!mutex_locked && !g_cur && !g_in_sub);
    assert(g_gpu_guest_ns==1011 && g_gpu_origin_ticks==2022 && g_gpu_guest_frame==3033 && g_ncarry==2);
    if(callback_mode==1) { /* Submitted optional child; no guest attribution or posts. */
        g_cur=&child_slot;slot_submit(0,VK_PASS_END_SUBMISSION);
    }
    if(callback_mode==2) return; /* Early refusal inside the real callback must not leak scope. */
}
'''

CHUNK_MAIN = r'''
static void setup(const char *env,int active,int trace) {
    selector=env;atomic_store(&vk_fg_slot_boundary_active,active);trace_enabled=trace;
    g_in_sub=1;g_cur=&guest_slot;g_ncarry=2;g_gpu_guest_ns=1011;
    g_gpu_origin_ticks=2022;g_gpu_guest_frame=3033;ticket=40;now=100;
    callback_mode=service_mode=env_calls=trace_calls=callback_calls=0;
    call_size=log_size=0;calls[0]=logs[0]=0;
    memset(prefix,0,sizeof prefix);memset(child,0,sizeof child);memset(tail,0,sizeof tail);
    vk_record_test_chunk_selector_reset();
}
static void context_unchanged(void) {
    assert(g_in_sub==1 && g_gpu_guest_ns==1011 && g_gpu_origin_ticks==2022 && g_gpu_guest_frame==3033);
    assert(g_ncarry==2);
}
int main(void) {
    setup("1",0,1);vk_submit_chunk();context_unchanged();
    assert(!env_calls && !trace_calls && !callback_calls && !log_size);
    assert(g_fg_adaptive_chunk==-1 && !strcmp(calls,"submit,service,"));
    /* Inactive calls cannot initialize/cache a future selector value. */
    selector="0";atomic_store(&vk_fg_slot_boundary_active,1);g_cur=&guest_slot;vk_submit_chunk();
    assert(env_calls==1 && !callback_calls && g_fg_adaptive_chunk==0);
    selector="1";g_cur=&guest_slot;vk_submit_chunk();assert(env_calls==1 && !callback_calls);
    /* Exact "0" is the begin/commit-only path; invalid spellings are off and logged once. */
    const char *off[]={"0","","01","true","2","-1"};
    for(unsigned i=0;i<sizeof off/sizeof *off;i++) {
        setup(off[i],1,1);vk_submit_chunk();context_unchanged();
        assert(env_calls==1 && !trace_calls && !callback_calls);
        assert(i==0 ? !log_size : strstr(logs,"[adaptive-chunk] selector=invalid")!=NULL);
        assert(!strcmp(calls,"submit,service,") && !g_cur && ticket==41);
    }
    /* Scheduler default: an absent selector enables the chunk boundary. */
    setup(NULL,1,0);vk_submit_chunk();context_unchanged();
    assert(env_calls==1 && callback_calls==1 && g_fg_adaptive_chunk==1);
    assert(!strcmp(calls,"submit,service,boundary,"));
    setup("1",1,1);g_in_sub=0;vk_submit_chunk();
    assert(!env_calls && !call_size && !log_size && g_fg_adaptive_chunk==-1);
    setup("1",1,1);g_cur=NULL;vk_submit_chunk();
    assert(!env_calls && !call_size && !log_size && g_fg_adaptive_chunk==-1);
    for(int trace=0;trace<=1;trace++) for(int mode=0;mode<=2;mode++) {
        setup("1",1,trace);callback_mode=mode;vk_submit_chunk();context_unchanged();
        assert(env_calls==1 && trace_calls==1 && callback_calls==1 && !g_cur);
        assert(prefix[0]==1011 && prefix[1]==2022 && prefix[2]==3033 && !prefix[3]);
        assert(!child[0] && !child[1] && !child[2] && !child[3]);
        assert(ticket==(mode==1?42:41));
        assert(!strcmp(calls,mode==1?"submit,service,boundary,submit,":"submit,service,boundary,"));
        if(trace) {
            assert(strstr(logs,"event=enter kind=chunk boundary_ns=174 submit_begin_ns=101 submit_return_ns=152 service_return_ns=173 real_ticket=41 open_primary=0"));
            assert(strstr(logs,"event=exit kind=chunk enter_ns=174"));
            assert(strstr(logs,"clock=CLOCK_MONOTONIC"));
        } else assert(!log_size);
        g_cur=&guest_slot;slot_submit(1,VK_PASS_END_SUBMISSION);
        assert(tail[0]==1011 && tail[1]==2022 && tail[2]==3033 && tail[3]==2 && !g_ncarry);
    }
    for(int service=1;service<=2;service++) {
        setup("1",1,1);service_mode=service;vk_submit_chunk();context_unchanged();
        assert(!callback_calls && strstr(logs,"event=skip kind=chunk"));
        assert(!strcmp(calls,"submit,service,"));
        assert(strstr(logs,service==1?"open_primary=0 active=0":"open_primary=1 active=1"));
    }
    /* Service may publish prepared work: admission uses the completed boundary,
     * not a pre-submit active snapshot. A late activation has no invented spans. */
    setup("1",0,1);service_mode=3;vk_submit_chunk();context_unchanged();
    assert(env_calls==1 && callback_calls==1 && !trace_calls && !log_size);
    assert(!strcmp(calls,"submit,service,boundary,"));
    setup("1",1,0);g_in_sub=7;callback_mode=2;vk_submit_chunk();
    assert(g_in_sub==7 && g_ncarry==2 && callback_calls==1 && !g_cur);
    return 0;
}
'''


class AdaptiveChunk(unittest.TestCase):
    def compile_and_run(self, text, prefix):
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=prefix, dir=scratch) as temp:
            path = Path(temp)
            (path / 'probe.c').write_text(text)
            # -Wno-sign-compare: GCC (the host `cc`) flags FG_DISPATCH_FACT's ?: arms of mixed signedness;
            # clang, which builds the shipped library, does not.
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-I' + str(GFX),
                str(path / 'probe.c'), '-o', str(path / 'probe')], check=True)
            subprocess.run([str(path / 'probe')], check=True)

    def test_inner_allocator_trace_preserves_original_calls_and_refusals(self):
        source = (GFX / 'nvn_vk.c').read_text()
        start = source.index('/* Diagnostic facts are recorded only at the original short-circuit predicates.')
        end = source.index('int vk_submit_optional_begin(void)', start)
        self.compile_and_run(COMMON + ALLOCATOR + source[start:end] + ALLOCATOR_MAIN,
                             'adaptive-chunk-allocator-')

    def test_selector_and_real_chunk_hook_restore_guest_scope(self):
        source = (GFX / 'nvn_vk.c').read_text()
        selector = re.search(r'^static int g_fg_adaptive_chunk=-1;$', source, re.M)
        if selector is None:
            self.fail('missing production chunk selector cache')
        functions = selector.group() + '\n' + '\n'.join(function(source, name) for name in (
            'fg_adaptive_chunk_on', 'vk_record_test_chunk_selector_reset', 'vk_submit_chunk'))
        self.compile_and_run(COMMON + CHUNK + functions + CHUNK_MAIN, 'adaptive-chunk-context-')


if __name__ == '__main__':
    unittest.main()
