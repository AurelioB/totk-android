"""Compile production prepared dispatch/seal/QCOM guards with scalar drivers.

No Android/Vulkan execution or hardware timing claim. The same real functions
run with trace=0/1; injected driver refusals compare ordered calls and ownership.
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
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>
#include "framegen_dispatch_trace.h"
#include "framegen_qcom_policy.h"
#include "framegen_budget_lsfg.h"
#define FG_OUTPUTS 4
#define SOURCES 4
#define VK_PRESENT_SLOT_CAPACITY 4
#define QCOM_TIMEOUT_NS UINT64_C(100000000)
#define VK_SUCCESS 0
#define EGL_NO_SYNC_KHR 0
#define EGL_SYNC_NATIVE_FENCE_ANDROID 1
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 2
#define EGL_NONE 0
#define GL_TIME_ELAPSED_EXT 1
#define GL_NO_ERROR 0
#define GL_GPU_DISJOINT_EXT 2
#define GL_QUERY_RESULT_AVAILABLE_EXT 3
#define GL_QUERY_RESULT_EXT 4
#define VK_QUERY_RESULT_64_BIT 1
#define VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR 1
#define VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR 2
#define VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT 1
#define VK_SEMAPHORE_IMPORT_TEMPORARY_BIT 1
#define VK_OVERLAY_FG_QCOM 0
#define VK_OVERLAY_FG_EXTRAP 0
#define VK_OVERLAY_FG_INTERP 0
#define VK_OVERLAY_FG_COOLDOWN 0
#define VK_OVERLAY_FG_WARMUP 0
#define VK_OVERLAY_FG_ACTIVE 0
#define FG_SLOT_CANCEL_NONE 0
#define FG_SLOT_CANCEL_OWNER 1
typedef uint64_t VkImage;
typedef uint64_t VkImageView;
typedef unsigned GLuint;
typedef unsigned GLenum;
typedef unsigned long long GLuint64;
typedef int GLint;
typedef int EGLint;
typedef int EGLSyncKHR;
typedef int EGLBoolean;
typedef struct { int sType; uint64_t semaphore; int handleType; } VkSemaphoreGetFdInfoKHR;
typedef struct { int sType; uint64_t semaphore; int flags,handleType,fd; } VkImportSemaphoreFdInfoKHR;
typedef struct { unsigned value; } vk_post_output;
typedef struct { uint64_t frame,epoch; } vk_fg_generation_stamp;
enum { VK_FG_GENERATION_EMPTY,VK_FG_GENERATION_PREPARING,VK_FG_GENERATION_PREPARED,
    VK_FG_GENERATION_RECORDING,VK_FG_GENERATION_SUBMITTED,VK_FG_GENERATION_QUARANTINED };
typedef struct vk_fg_generation {
    _Atomic int state,canceled; int immutable_terminal;
    uint64_t lease,ticket,first_ticket,charge_frame,cb;
    vk_fg_generation_stamp stamp;
    unsigned slot,source,previous,profile_mask; int profile_copy_started;
    double external_ms; vk_post_output output; fg_dispatch_trace trace;
} vk_fg_generation;
typedef struct { unsigned source,previous,profile_mask; int profile_copy_started; _Atomic int ended,busy; } fg_output;
static struct {
    _Atomic unsigned job_trace;
    int budget_override; fg_budget_policy budget; fg_qcom_prepared_policy prepared_budget;
    fg_budget_decision budget_pre; uint64_t budget_frame,budget_pre_frame,budget_reserved_frame;
    uint64_t budget_pre_skips[2],budget_skips,passes[4],recorded,output_budget_frame[FG_OUTPUTS];
    uint64_t adaptive_budget_frames[FG_BUDGET_FRAMES],adaptive_budget_admits,
        adaptive_budget_refusals,adaptive_budget_reservations;
    fg_output output[FG_OUTPUTS];
} F;
static struct { unsigned generated[SOURCES]; } FQ;
static struct { int dev; unsigned timestamp_bits; struct { struct { double timestampPeriod; } limits; } props; } V;
static pthread_mutex_t fg_stats_mu=PTHREAD_MUTEX_INITIALIZER;
static int fg_generation_test_bridge;
static uint64_t fg_generation_charge_frame;
static _Atomic int refused;
static _Atomic int fg_reset_requested;
_Thread_local fg_dispatch_trace *vk_fg_dispatch_trace_current;
typedef struct { VkImage image; VkImageView view; unsigned texture,w,h; int initialized; } qcom_image;
typedef struct { qcom_image scene; int copied; } qcom_source;
typedef struct {
    qcom_image output; uint64_t to_gl,from_gl; GLuint query;
    uint64_t frame,started,input_lower_ns,call_begin_ns,call_end_ns,flush_end_ns;
    int monitor,input_monitor,disjoint,bad; fg_qcom_job job; fg_dispatch_trace trace;
} qcom_output;
static struct {
    pthread_mutex_t mu,policy_mu;
    qcom_source source[SOURCES]; qcom_output output[VK_PRESENT_SLOT_CAPACITY];
    int diag,display,initialized,copy_queries,priority;
    _Atomic int live,warmed,suspended,cooling; _Atomic unsigned pending_costs,pending_jobs,quarantined;
    uint64_t generated,call_cpu_ns,call_cpu_max,timeout,missing,copy_samples,timed,slow,disjoint;
    double copy_sum_ms,copy_max_ms,sum_ms,max_ms,fence_sum_ms,fence_max_ms,charged_sum_ms,charged_max_ms;
    fg_qcom_policy policy;
    int (*semaphore_fd)(int,const VkSemaphoreGetFdInfoKHR *,int *);
    int (*import_semaphore)(int,const VkImportSemaphoreFdInfoKHR *);
    struct {
        EGLSyncKHR (*create_sync)(int,int,const EGLint *);
        EGLBoolean (*wait_sync)(int,EGLSyncKHR,int);
        void (*destroy_sync)(int,EGLSyncKHR);
        void (*begin_query)(unsigned,unsigned);
        void (*end_query)(unsigned);
        void (*extrapolate)(unsigned,unsigned,unsigned,float);
        GLenum (*glGetError)(void);
        void (*glFlush)(void);
        int (*dup_fence)(int,EGLSyncKHR);
        void (*glGetIntegerv)(unsigned,GLint *);
        void (*query_available)(unsigned,unsigned,GLuint *);
        void (*query_result)(unsigned,unsigned,GLuint64 *);
    } a;
} Q={.mu=PTHREAD_MUTEX_INITIALIZER,.policy_mu=PTHREAD_MUTEX_INITIALIZER};
static char calls[16384],logs[131072];
static size_t call_size,log_size;
static const char *scenario;
static uint64_t now;
static unsigned active_calls,dup_calls,sync_calls;
static int is(const char *name) { return !strcmp(scenario,name); }
static void record(const char *name) {
    int n=snprintf(calls+call_size,sizeof calls-call_size,"%s,",name);
    assert(n>0 && (size_t)n<sizeof calls-call_size); call_size+=(size_t)n;
}
static void logging(const char *format,...) {
    va_list args; va_start(args,format);
    int n=vsnprintf(logs+log_size,sizeof logs-log_size,format,args); va_end(args);
    assert(n>=0 && (size_t)n+1<sizeof logs-log_size); log_size+=(size_t)n;
    logs[log_size++]='\n'; logs[log_size]=0;
}
#define LOGV(...) logging(__VA_ARGS__)
static uint64_t host_now_ns(void) { return now; }
static uint64_t monotonic_ns(void) { return now+10; }
static int vk_fg_enabled(void) { record("enabled"); return !is("framegen-disabled"); }
static int vk_fg_interpolating(void) { record("interpolating"); return 1; }
static int vk_fg_lsfg_active(void) { record("lsfg-active"); return 0; }
static int vk_fg_adaptive(void) { record("adaptive"); return 1; }
/* TOTK_FG_ADAPTIVE_BUDGET is off here; test_framegen_adaptive_budget.py owns it. */
static int fg_adaptive_budget(void) { record("adaptive-budget"); return 0; }
static int adaptive_requested(void) { record("adaptive-requested"); return 1; }
static int vk_fg_qcom_requested(void) { record("qcom-requested");return !is("qcom-not-requested"); }
static int vk_fg_locked(void) { record("locked");return 0; }
static int vk_fg_qcom_can_generate(void) {
    record("can-generate"); if(is("deadline-before-optional")) now=1000;
    return !is("qcom-cannot-generate");
}
/* Production: TEST bridge (never set here) else vk_fg_qcom_can_generate(). */
static int fg_generation_engine_ready(void) {
    return fg_generation_test_bridge ? 1 : vk_fg_qcom_can_generate();
}
static int vk_fg_qcom_active(void) {
    record("qcom-active"); active_calls++;
    return !(is("qcom-inactive-before-harvest") ||
        (is("qcom-inactive-after-harvest") && active_calls==2));
}
static int vk_fg_generation_state(const vk_fg_generation *g) { record("generation-state"); return g?atomic_load(&g->state):0; }
static void vk_fg_generation_cancel(vk_fg_generation *g) { record("generation-cancel"); atomic_store(&g->state,0); }
static void fg_prepared_budget_audit(void) { record("budget-audit"); }
static int optional_begin(void) {
    record("optional-begin");
    if(is("budget-reserve")) F.budget.last_reserved=3;
    if(is("reserve-cooling")) F.prepared_budget.cooldown_until=4;
    if(is("reserve-capacity")) F.prepared_budget.interval[3].frame=4;
    if(is("deadline-after-reserve")) now=1000;
    return !is("optional-begin-refused");
}
static int vk_submit_optional_begin(void) { record("optional-mutable"); return optional_begin(); }
static int vk_submit_optional_begin_immutable(void) { record("optional-immutable"); return optional_begin(); }
static void vk_submit_optional_cancel(void) { record("optional-cancel"); }
static void vk_submit_optional_end(void) { record("optional-end"); }
static uint64_t mtl_last_fence(void) { record("last-fence"); return 20; }
static uint64_t vk_submit_now(void) { record("submit-now"); return is("seal-ticket-zero") || is("refused-submit-ticket-zero")?0:22; }
static uint64_t vk_cb(void) { record("cb"); return 1; }
static void vk_submit_adopt_primary(uint64_t cb) { (void)cb; record("adopt"); }
static void vk_fg_synthetic_range(uint64_t first,uint64_t last) { (void)first;(void)last;record("synthetic-range"); }
static int vk_replay_lease_end(uint64_t lease,uint64_t ticket) { (void)lease;(void)ticket;record("lease-end");return !is("replay-lease-end-refused"); }
static int current(void) { record("current"); return !is("context-current-refused"); }
static void unbind(void) { record("unbind"); }
static int output_resources(qcom_output *s,unsigned w,unsigned h) {
    (void)s;(void)w;(void)h;record("output-resources");
    if(is("deadline-before-signal")) now=1000;
    return !is("output-resources-refused") && !is("refused-submit-ticket-zero");
}
static void vk_submit_external_enable(void) { record("external-enable"); }
static uint64_t vk_submit_signal(uint64_t semaphore) { (void)semaphore;record("signal");return !is("external-signal-refused"); }
static void vk_submit_wait(uint64_t semaphore) { (void)semaphore;record("submit-wait"); }
static int semaphore_fd(int device,const VkSemaphoreGetFdInfoKHR *info,int *fd) {
    (void)device;(void)info;record("semaphore-fd");*fd=10;return is("input-fence-export-refused")?-1:VK_SUCCESS;
}
static int duplicate(int fd) {
    record("dup");dup_calls++;
    return ((is("input-monitor-dup-refused") && dup_calls==1) ||
        (is("output-monitor-dup-refused") && dup_calls==2))?-1:(fd==12?13:11);
}
static int close_fd(int fd) { (void)fd;record("close");return 0; }
#define dup duplicate
#define close close_fd
static EGLSyncKHR create_sync(int display,int kind,const EGLint *attrs) {
    (void)display;(void)kind;(void)attrs;record("create-sync");sync_calls++;
    return ((is("egl-input-sync-refused") && sync_calls==1) ||
        (is("output-sync-create-refused") && sync_calls==2))?EGL_NO_SYNC_KHR:1;
}
static EGLBoolean wait_sync(int display,EGLSyncKHR sync,int flags) {
    (void)display;(void)sync;(void)flags;record("wait-sync");
    if(is("deadline-after-server-wait")) now=1000;
    return !is("egl-server-wait-refused");
}
static void destroy_sync(int display,EGLSyncKHR sync) { (void)display;(void)sync;record("destroy-sync"); }
static void begin_query(unsigned target,unsigned query) { (void)target;(void)query;record("begin-query"); }
static void end_query(unsigned target) { (void)target;record("end-query"); }
static void extrapolate(unsigned old,unsigned current_texture,unsigned output,float phase) {
    assert(old==1 && current_texture==2 && output==3 && phase==0.5f);record("afme");
}
static GLenum gl_error(void) { record("gl-error");return is("afme-gl-error")?1:GL_NO_ERROR; }
static void flush(void) { record("gl-flush"); }
static int dup_fence(int display,EGLSyncKHR sync) { (void)display;(void)sync;record("dup-fence");return is("output-fence-export-refused")?-1:12; }
static int import_semaphore(int device,const VkImportSemaphoreFdInfoKHR *info) {
    (void)device;(void)info;record("import-semaphore");return is("vulkan-output-fence-import-refused")?-1:VK_SUCCESS;
}
static void vk_fg_qcom_refuse(const char *reason) { (void)reason;record("qcom-refuse"); }
static void vk_fg_qcom_cost_begin(uint64_t frame) { assert(frame==3);record("cost-begin"); }
static void vk_fg_qcom_cost_end(uint64_t frame,double ms,int valid) { (void)frame;(void)ms;(void)valid;record("cost-end"); }
static void timing_log(const char *event,uint64_t frame,unsigned kind) { (void)event;(void)frame;(void)kind;record("timing-log"); }
static void ownership(uint64_t cb,qcom_image *image,int acquire,int discard) { (void)cb;(void)image;(void)acquire;(void)discard;record("ownership"); }
static void failure(unsigned kind) { (void)kind;record("failure"); }
static const char *priority_name(int priority) { (void)priority;return "fixture"; }
static void gl_integer(unsigned what,GLint *value) { (void)what;record("gl-integer");*value=0; }
static void query_available(unsigned query,unsigned what,GLuint *value) { (void)query;(void)what;record("query-available");*value=!is("afme-harvest-timeout"); }
static void query_result(unsigned query,unsigned what,GLuint64 *value) { (void)query;(void)what;record("query-result");*value=20; }
static int poll_fence(struct pollfd *p,unsigned long count,int timeout) { (void)count;assert(timeout==0);record("poll");p->revents=POLLIN;return 1; }
#define poll poll_fence
static int fence_timestamp(int fd,uint64_t *ns,char *label,size_t capacity) { (void)label;(void)capacity;record("fence-timestamp");*ns=fd==13?90:80;return 1; }
static int vkGetQueryPoolResults(int device,int pool,unsigned first,unsigned count,size_t bytes,void *data,size_t stride,int flags) {
    (void)device;(void)pool;(void)first;(void)count;(void)bytes;(void)stride;(void)flags;record("copy-query-results");
    uint64_t *ns=data;ns[0]=1;ns[1]=2;return VK_SUCCESS;
}
void vk_fg_qcom_harvest(void);
int vk_fg_qcom_output_dispatch(unsigned,unsigned,unsigned,uint64_t,float,uint64_t);
/* Count the already-defined pure policy queries at each real callsite too.
 * A telemetry-only second probe would change this ordered receipt. */
static int queried_cooling(const fg_qcom_prepared_policy *p,uint64_t frame) {
    record("prepared-cooling");return fg_qcom_prepared_cooling(p,frame);
}
static int queried_capacity(const fg_qcom_prepared_policy *p,uint64_t frame) {
    record("prepared-capacity");return fg_qcom_prepared_capacity(p,frame);
}
static fg_budget_decision queried_admit(const fg_budget_policy *p,uint64_t frame) {
    record("budget-admit");return fg_budget_admit(p,frame);
}
static fg_budget_decision queried_reserve(fg_budget_policy *p,uint64_t frame) {
    record("budget-reserve");return fg_budget_reserve(p,frame);
}
#define fg_qcom_prepared_cooling queried_cooling
#define fg_qcom_prepared_capacity queried_capacity
#define fg_budget_admit queried_admit
#define fg_budget_reserve queried_reserve
'''

MAIN = r'''
static void setup(const char *name,int trace,vk_fg_generation *g) {
    memset(&F,0,sizeof F);memset(&FQ,0,sizeof FQ);
    memset(Q.source,0,sizeof Q.source);memset(Q.output,0,sizeof Q.output);
    memset(&Q.policy,0,sizeof Q.policy);Q.diag=0;Q.initialized=1;
    atomic_store(&Q.live,1);atomic_store(&Q.pending_costs,0);atomic_store(&Q.pending_jobs,0);
    atomic_store(&Q.quarantined,0);Q.generated=Q.call_cpu_ns=Q.call_cpu_max=0;
    Q.timeout=Q.missing=Q.copy_samples=Q.timed=Q.slow=Q.disjoint=0;
    Q.copy_sum_ms=Q.copy_max_ms=Q.sum_ms=Q.max_ms=Q.fence_sum_ms=Q.fence_max_ms=Q.charged_sum_ms=Q.charged_max_ms=0;
    Q.semaphore_fd=semaphore_fd;Q.import_semaphore=import_semaphore;
    Q.a.create_sync=create_sync;Q.a.wait_sync=wait_sync;Q.a.destroy_sync=destroy_sync;
    Q.a.begin_query=begin_query;Q.a.end_query=end_query;Q.a.extrapolate=extrapolate;
    Q.a.glGetError=gl_error;Q.a.glFlush=flush;Q.a.dup_fence=dup_fence;
    Q.a.glGetIntegerv=gl_integer;Q.a.query_available=query_available;Q.a.query_result=query_result;
    V.timestamp_bits=64;V.props.limits.timestampPeriod=1;
    Q.source[0].copied=Q.source[1].copied=1;
    Q.source[0].scene=(qcom_image){.texture=1,.w=8,.h=8};
    Q.source[1].scene=(qcom_image){.texture=2,.w=8,.h=8};
    Q.output[0].output=(qcom_image){.texture=3,.image=31,.view=32};
    Q.output[0].monitor=Q.output[0].input_monitor=-1;
    Q.output[0].job=(fg_qcom_job){.previous=0,.current=1,.prepared=1,.leased=1};
    F.budget_frame=2;atomic_store(&F.job_trace,trace);
    fg_budget_sample(&F.budget,1,12,1);fg_budget_seal(&F.budget,1,1);
    fg_budget_sample(&F.budget,2,12,1);fg_budget_seal(&F.budget,2,2);
    memset(g,0,sizeof *g);atomic_store(&g->state,VK_FG_GENERATION_PREPARED);
    g->stamp=(vk_fg_generation_stamp){.frame=44,.epoch=7};g->source=1;g->previous=0;
    g->lease=5;g->cb=9;g->output.value=77;g->immutable_terminal=1;
    atomic_store(&fg_reset_requested,0);fg_generation_charge_frame=0;
    vk_fg_dispatch_trace_current=NULL;scenario=name;now=100;active_calls=dup_calls=sync_calls=0;
    call_size=log_size=0;calls[0]=logs[0]=0;
    if(is("generation-not-prepared")) atomic_store(&g->state,VK_FG_GENERATION_QUARANTINED);
    if(is("generation-canceled")) atomic_store(&g->canceled,1);
    if(is("reset-requested")) atomic_store(&fg_reset_requested,1);
    if(is("budget-pre-admit")) {F.budget_pre_frame=3;F.budget_pre.status=FG_BUDGET_MISSING;F.budget_pre.reason=FG_BUDGET_REASON_MISSING;}
    if(is("charge-interval-already-used")) fg_generation_charge_frame=3;
    if(is("pre-cooling")) F.prepared_budget.cooldown_until=4;
    if(is("pre-capacity")) F.prepared_budget.interval[3].frame=4;
    if(is("previous-index-range")) g->previous=SOURCES;
    if(is("source-index-range")) g->source=SOURCES;
    if(is("source-pair-identical")) g->previous=g->source;
    if(is("output-slot-range")) g->slot=VK_PRESENT_SLOT_CAPACITY;
    if(is("output-not-prepared")) Q.output[0].job.prepared=0;
    if(is("previous-owner-mismatch")) Q.output[0].job.previous=2;
    if(is("source-owner-mismatch")) Q.output[0].job.current=2;
    if(is("source-not-copied")) Q.source[1].copied=0;
    if(is("previous-not-copied")) Q.source[0].copied=0;
    if(is("source-width-mismatch")) Q.source[0].scene.w++;
    if(is("source-height-mismatch")) Q.source[0].scene.h++;
}
typedef struct {
    int result,state,canceled,pending_costs,pending_jobs,quarantine,prepared,pending,leased;
    uint64_t ticket,lease,first,charge,reservations,last_reserved,probes,last_probe,reserved;
    unsigned output,children;
} outcome;
static outcome run(const char *name,int trace) {
    vk_fg_generation g;vk_post_output output={0};setup(name,trace,&g);
    float phase=is("phase-nonfinite")?NAN:is("phase-nonpositive")?0:is("phase-above-one")?1.5f:0.5f;
    uint64_t deadline=is("deadline-zero")?0:is("deadline-entry")?100:1000;
    int result=vk_fg_generation_dispatch(&g,phase,deadline,is("output-null")?NULL:&output);
    assert(!vk_fg_dispatch_trace_current);
    if(result==1) {
        if(is("generation-not-recording")) atomic_store(&g.state,VK_FG_GENERATION_PREPARED);
        if(is("prepared-charge-refused")) g.first_ticket=0;
        uint64_t ticket=vk_fg_generation_seal(&g);
        if(is("generation-not-recording") || is("seal-ticket-zero") || is("replay-lease-end-refused")) assert(!ticket);
        else assert(ticket==22);
        if(is("harvest") || is("afme-harvest-timeout")) {
            now=is("afme-harvest-timeout")?QCOM_TIMEOUT_NS+1000:200;
            vk_fg_qcom_harvest();
        }
    }
    return (outcome){.result=result,.state=atomic_load(&g.state),.canceled=atomic_load(&g.canceled),
        .pending_costs=atomic_load(&Q.pending_costs),.pending_jobs=atomic_load(&Q.pending_jobs),
        .quarantine=Q.output[0].job.quarantined,.prepared=Q.output[0].job.prepared,
        .pending=Q.output[0].job.pending,.leased=Q.output[0].job.leased,
        .ticket=g.ticket,.lease=g.lease,.first=g.first_ticket,.charge=g.charge_frame,
        .reservations=F.budget.reservations,.last_reserved=F.budget.last_reserved,
        .probes=F.budget.probes,.last_probe=F.budget.last_probe,.reserved=F.budget_reserved_frame,
        .output=output.value,.children=F.prepared_budget.interval[3].children};
}
static void compare(const char *name,const char *reason) {
    outcome off=run(name,0);char expected_calls[sizeof calls];strcpy(expected_calls,calls);
    assert(!log_size);
    outcome on=run(name,1);
#define SAME(field) assert(off.field==on.field)
    SAME(result);SAME(state);SAME(canceled);SAME(pending_costs);SAME(pending_jobs);
    SAME(quarantine);SAME(prepared);SAME(pending);SAME(leased);SAME(ticket);SAME(lease);
    SAME(first);SAME(charge);SAME(reservations);SAME(last_reserved);SAME(probes);SAME(last_probe);
    SAME(reserved);SAME(output);SAME(children);
#undef SAME
    if(strcmp(expected_calls,calls)) fprintf(stderr,"call mismatch for %s\noff %s\non %s\n",name,expected_calls,calls);
    assert(!strcmp(expected_calls,calls));
    if(reason) {
        char wanted[128];snprintf(wanted,sizeof wanted,"reason=%s ",reason);
        if(!strstr(logs,wanted)) fprintf(stderr,"missing %s in %s:\n%s\n",wanted,name,logs);
        assert(strstr(logs,wanted));
    }
    assert(strstr(logs,"source=44 charge=3 output="));
    assert(strstr(logs,"final-gpu-done=UNKNOWN"));
    assert(strstr(logs,"charge-authority=prospective-until-range-charge"));
    if(is("phase-nonfinite") || is("budget-pre-admit") || is("optional-begin-refused"))
        assert(strstr(logs,"first-ticket=0 ticket=0 range-observed=0 charge-committed=0 charge-status=not-attempted"));
    if(is("success") || is("output-resources-refused"))
        assert(strstr(logs,"first-ticket=21 ticket=22 range-observed=1 charge-committed=1 charge-status=accepted charge-authority=prepared-range-charge-result"));
    if(is("refused-submit-ticket-zero")) {
        assert(strstr(logs,"first-ticket=21 ticket=0 range-observed=0 charge-committed=0 charge-status=not-attempted"));
        assert(!strstr(logs,"charge-committed=1"));
    }
    if(is("prepared-charge-refused"))
        assert(strstr(logs,"first-ticket=0 ticket=22 range-observed=0 charge-committed=0 charge-status=refused"));
    if(is("pre-cooling") || is("reserve-cooling")) {
        assert(strstr(logs,"prepared-cooling=1 prepared-capacity=-1"));
    }
    if(is("pre-capacity") || is("reserve-capacity")) assert(strstr(logs,"prepared-capacity=0"));
    if(is("budget-reserve")) assert(strstr(logs,"budget-reason=reserved"));
    if(is("harvest")) {
        assert(strstr(logs,"afme-output-signal-ns=90 afme-output-clock=kernel-CLOCK_MONOTONIC"));
        assert(strstr(logs,"harvest-cpu-ns=210 harvest-clock=CLOCK_MONOTONIC"));
        assert(strstr(logs,"afme-only=1 final-gpu-done=UNKNOWN"));
    }
    if(strstr(name,"output-sync") || strstr(name,"output-fence-export") ||
        is("afme-gl-error") || is("output-monitor-dup-refused") || is("vulkan-output-fence-import-refused")) {
        assert(on.result==-1 && on.state==VK_FG_GENERATION_QUARANTINED && on.quarantine);
        assert(strstr(logs,"reason=post-dispatch-quarantined"));
        assert(strstr(logs,"reason=qcom-output-quarantined"));
        assert(strstr(logs,"reason=qcom-output-refused"));
        assert(strstr(logs,"first-ticket=21 ticket=22 range-observed=1 charge-committed=1 charge-status=accepted"));
    }
}
int main(void) {
    const char *guards[]={"generation-not-prepared","output-null","deadline-zero","deadline-entry",
        "phase-nonfinite","phase-nonpositive","phase-above-one","generation-canceled","reset-requested",
        "framegen-disabled","qcom-cannot-generate","budget-pre-admit","charge-interval-already-used",
        "deadline-before-optional","optional-begin-refused","budget-reserve","deadline-after-reserve",
        "previous-index-range","source-index-range","source-pair-identical","output-slot-range",
        "qcom-inactive-before-harvest","qcom-inactive-after-harvest","output-not-prepared",
        "previous-owner-mismatch","source-owner-mismatch","source-not-copied","previous-not-copied",
        "source-width-mismatch","source-height-mismatch","context-current-refused","output-resources-refused",
        "deadline-before-signal","external-signal-refused","input-fence-export-refused","input-monitor-dup-refused",
        "egl-input-sync-refused","egl-server-wait-refused","deadline-after-server-wait","output-sync-create-refused",
        "output-fence-export-refused","afme-gl-error","output-monitor-dup-refused","vulkan-output-fence-import-refused",
        "generation-not-recording","seal-ticket-zero","replay-lease-end-refused","prepared-charge-refused"};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++) compare(guards[i],guards[i]);
    compare("success",NULL);compare("harvest",NULL);compare("afme-harvest-timeout","afme-harvest-timeout");
    compare("refused-submit-ticket-zero","refused-submit-ticket-zero");
    compare("pre-cooling","budget-pre-admit");compare("pre-capacity","budget-pre-admit");
    compare("reserve-cooling","budget-reserve");compare("reserve-capacity","budget-reserve");
    /* QCOM wrapper supplies nonnull output pointers and a valid generation
     * phase. Exercise otherwise-unreachable QCOM guards through its REAL path. */
    const char *extra[]={"image-null","view-null","qcom-phase-invalid"};
    for(unsigned i=0;i<3;i++) {
        char expected_calls[sizeof calls];
        for(int trace=0;trace<=1;trace++) {
            vk_fg_generation g;setup(extra[i],trace,&g);
            fg_dispatch_trace t={.enabled=trace,.source=44,.charge=3,.output=0,.phase=0.5f,.deadline=1000};
            vk_fg_dispatch_trace_current=&t;VkImage image;VkImageView view;
            assert(!extrapolate_output(0,1,0,3,i==2?NAN:0.5f,i==0?NULL:&image,i==1?NULL:&view,1,1000));
            vk_fg_dispatch_trace_current=NULL;
            if(!trace) {strcpy(expected_calls,calls);assert(!log_size);}
            else {assert(!strcmp(expected_calls,calls));assert(strstr(logs,extra[i]));}
        }
    }
    /* Snapshot the exact atomics already evaluated by production admission;
     * trace-off must not write facts, trace-on must not re-probe admission. */
    const char *admission[]={"qcom-not-requested","qcom-warmup","qcom-suspended",
        "qcom-cooling","qcom-quarantined","qcom-latched","active"};
    for(unsigned i=0;i<sizeof admission/sizeof *admission;i++) {
        char expected_calls[sizeof calls];int off=0;
        for(int trace=0;trace<=1;trace++) {
            vk_fg_generation g;setup(admission[i],trace,&g);
            atomic_store(&Q.warmed,!is("qcom-warmup"));
            atomic_store(&Q.suspended,is("qcom-suspended"));
            atomic_store(&Q.cooling,is("qcom-cooling"));
            atomic_store(&Q.quarantined,is("qcom-quarantined"));
            atomic_store(&refused,is("qcom-latched"));
            fg_dispatch_trace t={.enabled=trace,.qcom_warmed=-1,.qcom_suspended=-1,
                .qcom_cooling=-1,.qcom_quarantined=-1,.qcom_admitted=-1};
            vk_fg_dispatch_trace_current=&t;
            int accepted=observed_qcom_active();
            vk_fg_dispatch_trace_current=NULL;
            assert(accepted==is("active"));
            if(!trace) {
                off=accepted;strcpy(expected_calls,calls);
                assert(t.qcom_admitted==-1 && t.qcom_warmed==-1 && t.qcom_suspended==-1 &&
                    t.qcom_cooling==-1 && t.qcom_quarantined==-1);
            } else {
                assert(off==accepted && !strcmp(expected_calls,calls));
                if(is("qcom-not-requested")) {
                    assert(t.qcom_admitted==-1 && t.qcom_warmed==-1);
                } else {
                    assert(t.qcom_admitted==accepted);
                    assert(t.qcom_warmed==atomic_load(&Q.warmed));
                    assert(t.qcom_suspended==atomic_load(&Q.suspended));
                    assert(t.qcom_cooling==atomic_load(&Q.cooling));
                    assert(t.qcom_quarantined==(int)atomic_load(&Q.quarantined));
                }
            }
            assert(!log_size);
        }
    }
    return 0;
}
'''


class DispatchTrace(unittest.TestCase):
    def test_real_guards_preserve_outcomes_and_ordered_calls(self):
        backend = (GFX / 'vk_framegen.c').read_text()
        qcom = (GFX / 'vk_framegen_qcom.c').read_text()
        functions = '\n'.join(function(backend, name) for name in (
            'vk_fg_job_trace_enabled', 'fg_dispatch_trace_event',
            'fg_dispatch_budget_observe', 'fg_budget_reserve_work', 'fg_budget_pre_admit',
            'fg_generation_unreserve', 'fg_generation_dispatch_result'))
        functions += '\n' + function(qcom, 'vk_fg_qcom_active').replace(
            'int vk_fg_qcom_active(', 'int observed_qcom_active(', 1)
        functions += '\n' + function(qcom, 'extrapolate_output')
        functions += '\n' + function(qcom, 'vk_fg_qcom_output_dispatch')
        functions += '\n' + function(qcom, 'vk_fg_qcom_harvest')
        functions += '\n' + function(backend, 'vk_fg_generation_dispatch')
        functions += '\n' + function(backend, 'vk_fg_generation_seal')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dispatch-trace-', dir=scratch) as temp:
            path = Path(temp)
            (path / 'probe.c').write_text(FIXTURE + functions + MAIN)
            # -Wno-sign-compare: GCC (the host `cc`) flags FG_DISPATCH_FACT's ?: arms of mixed signedness;
            # clang, which builds the shipped library, does not.
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-sign-compare',
                '-I' + str(GFX), str(path / 'probe.c'), '-pthread', '-lm', '-o', str(path / 'probe')], check=True)
            subprocess.run([str(path / 'probe')], check=True)


if __name__ == '__main__':
    unittest.main()
