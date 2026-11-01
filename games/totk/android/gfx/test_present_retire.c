/* Actual production vk_present_retire worker; no Android, Vulkan or device.
 * Callback completion uses a monotonic pthread condition, and independently
 * ordered release fences use real pipe FDs. Barriers, not sleeps, order tests.
 * Clock bounds are liveness watchdogs, NOT performance measurements.
 *
 * Build: make -C games/totk/android TARGET=linux CC=cc OUT=<scratch> presentretiretest
 * Run:   <scratch>/presentretiretest
 * The caller runs these commands; adding this fixture does not execute them.
 * Android callback/SurfaceControl wiring and real GPU lease retirement still
 * require the existing integration/replay-pin tests; pipes are not sync_files.
 */
#define _POSIX_C_SOURCE 200809L
#include "vk_present_retire.h"
#include "vk_present_fence.h"
#include "framegen_adaptive_present.h"
#include "framegen_job_trace.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { HOST_BUFFERS = 4, HOST_JOBS = 256, HOST_EVENTS = 2048 };
static _Atomic unsigned checks;
#define CHECK(c) do { atomic_fetch_add_explicit(&checks,1,memory_order_relaxed); \
    if (!(c)) { fprintf(stderr,"present retire: %s (line %d)\n",#c,__LINE__); abort(); } \
} while (0)
#define NS_PER_MS UINT64_C(1000000)
#define WATCHDOG_NS UINT64_C(5000000000)

typedef enum { HOST_STAGED, HOST_ENTERED, HOST_CALLBACK_WAIT, HOST_RELEASE_WAIT,
    HOST_FINISHED, HOST_TIMED_OUT } host_phase;
typedef struct {
    vk_present_retire_token token;
    int previous, current, pipefd[2], callback_done, release_fd, allow_finish;
    unsigned entered, completed, retired, read_closes, write_closes, signals;
    host_phase phase;
} host_job;
typedef struct {
    vk_present_retire queue;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t presenter, finisher;
    host_job jobs[HOST_JOBS];
    unsigned context_job[VK_PRESENT_RETIRE_CAPACITY];
    _Atomic int busy[HOST_BUFFERS], copy_fallback;
    unsigned retained[HOST_BUFFERS], uses[HOST_BUFFERS], releases[HOST_BUFFERS];
    unsigned jobs_count, finish_calls, wakes, retire_count, quarantines, drains, joins;
    unsigned capacity_jobs, capacity_waits, reserve_attempts;
    unsigned published[HOST_JOBS], retire_order[HOST_JOBS];
    vk_present_retire_event events[HOST_EVENTS];
    unsigned event_count, max_depth, surface_live, surface_released, window_refs;
    int current;
} host_fixture;

static uint64_t now_ns(void) { return vk_present_retire_now(); }
static struct timespec absolute_time(uint64_t ns) { return vk_present_retire_time(ns); }
static void lock_host(host_fixture *f) { CHECK(!pthread_mutex_lock(&f->mu)); }
static void unlock_host(host_fixture *f) { CHECK(!pthread_mutex_unlock(&f->mu)); }
static void changed(host_fixture *f) { CHECK(!pthread_cond_broadcast(&f->cv)); }
static void wait_changed(host_fixture *f, uint64_t deadline) {
    struct timespec ts=absolute_time(deadline);
    CHECK(!pthread_cond_timedwait(&f->cv,&f->mu,&ts));
}
static int same_token(vk_present_retire_token a, vk_present_retire_token b) {
    return a.index==b.index && a.sequence==b.sequence && a.generation==b.generation;
}
static unsigned token_job(host_fixture *f, vk_present_retire_token token) {
    for (unsigned id=1;id<=f->jobs_count;id++)
        if (same_token(f->jobs[id].token,token)) return id;
    return 0;
}
static void invariant_locked(host_fixture *f) {
    CHECK(f->max_depth<=2 && VK_PRESENT_RETIRE_CAPACITY==2 && VK_PRESENT_FLIGHT_CAPACITY==2);
    for (unsigned b=0;b<HOST_BUFFERS;b++) {
        CHECK(f->busy[b]<=1 && f->uses[b]==f->releases[b]+f->busy[b]);
        CHECK(!f->retained[b] || f->busy[b]);
    }
    if (f->current>=0) CHECK(f->busy[f->current]);
    for (unsigned id=1;id<=f->jobs_count;id++) {
        host_job *j=&f->jobs[id];
        CHECK(j->entered<=1 && j->completed<=1 && j->retired<=1);
        CHECK(j->read_closes<=1 && j->write_closes<=1 && j->signals<=1);
        CHECK(j->retired<=j->completed && j->completed==j->read_closes);
        CHECK(j->token.index<2 && j->token.sequence && j->token.generation);
    }
    if (f->surface_released) {
        CHECK(!f->surface_live && !f->window_refs && f->joins && f->current<0);
        for (unsigned b=0;b<HOST_BUFFERS;b++) CHECK(!f->busy[b]);
    }
}
static void event_hook(void *context, const vk_present_retire_event *event) {
    host_fixture *f=context;
    /* Reentrant read proves event callbacks do not run under the queue lock. */
    CHECK(vk_present_retire_count(&f->queue)<=2);
    lock_host(f);
    CHECK(f->event_count<HOST_EVENTS && event->depth<=2 && event->now_ns);
    f->events[f->event_count++]=*event;
    if (event->depth>f->max_depth) f->max_depth=event->depth;
    if (!strcmp(event->event,"retire")) {
        unsigned id=token_job(f,event->token);
        CHECK(id && f->jobs[id].completed && !f->jobs[id].retired);
        f->jobs[id].retired++;
        f->retire_order[f->retire_count++]=id;
        CHECK(event->retires==f->retire_count);
    } else if (!strcmp(event->event,"quarantine")) f->quarantines++;
    else if (!strcmp(event->event,"drain")) f->drains++;
    else if (!strcmp(event->event,"join")) f->joins++;
    invariant_locked(f); changed(f); unlock_host(f);
}
static void wake_hook(void *context) {
    host_fixture *f=context;
    /* Reentrant reserve-count read also proves notification is outside mu. */
    CHECK(vk_present_retire_count(&f->queue)<=2);
    lock_host(f); f->wakes++; changed(f); unlock_host(f);
}
static int unresolved(host_fixture *f, host_job *j) {
    lock_host(f); j->phase=HOST_TIMED_OUT; invariant_locked(f); changed(f); unlock_host(f);
    return 0;
}
static int finish_hook(void *context, unsigned index, uint64_t deadline) {
    host_fixture *f=context;
    CHECK(index<2 && vk_present_retire_count(&f->queue)>0);
    CHECK(!pthread_equal(pthread_self(),f->presenter));
    lock_host(f);
    unsigned id=f->context_job[index];
    CHECK(id && id<=f->jobs_count);
    host_job *j=&f->jobs[id];
    CHECK(j->token.index==index && !j->entered && !j->completed);
    if (f->finish_calls) CHECK(pthread_equal(pthread_self(),f->finisher));
    f->finisher=pthread_self(); f->finish_calls++; j->entered++;
    j->phase=HOST_ENTERED; changed(f);
    while (!j->allow_finish || !j->callback_done) {
        if (j->allow_finish) { j->phase=HOST_CALLBACK_WAIT; changed(f); }
        struct timespec ts=absolute_time(deadline);
        int rc=pthread_cond_timedwait(&f->cv,&f->mu,&ts);
        CHECK(!rc || rc==ETIMEDOUT);
        if (rc==ETIMEDOUT) { unlock_host(f); return unresolved(f,j); }
    }
    int fd=j->release_fd;
    CHECK(fd>=0 && fd==j->pipefd[0]);
    j->phase=HOST_RELEASE_WAIT; changed(f); unlock_host(f);
    int signaled=0;
    for (;;) {
        uint64_t now=now_ns();
        if (now>=deadline) return unresolved(f,j);
        uint64_t left=(deadline-now+NS_PER_MS-1)/NS_PER_MS;
        struct pollfd p={.fd=fd,.events=POLLIN};
        int rc=poll(&p,1,(int)left);
        if (rc<0 && errno==EINTR) continue;
        CHECK(rc>=0);
        if (!rc) return unresolved(f,j);
        if (p.revents&POLLIN) {
            char value=0;
            ssize_t got=read(fd,&value,1);
            if (got<0 && errno==EINTR) continue;
            CHECK(got==1 && (value=='S' || value=='E'));
            signaled=value=='S';
        } else CHECK(p.revents&(POLLERR|POLLNVAL|POLLHUP));
        break;
    }
    lock_host(f);
    CHECK(!j->completed && !j->read_closes && f->busy[j->previous]);
    CHECK(!close(fd)); j->read_closes++; j->release_fd=-1;
    vk_present_retire_release_result(signaled,&f->busy[j->previous],&f->copy_fallback);
    if (signaled) f->releases[j->previous]++;
    else f->retained[j->previous]=1;
    /* CURRENT is never released by its own transaction's completion. */
    if (j->current>=0) CHECK(f->busy[j->current]);
    else CHECK(f->current<0);
    j->completed++; j->phase=HOST_FINISHED; f->context_job[index]=0;
    invariant_locked(f); changed(f); unlock_host(f);
    return 1;
}
static void host_init(host_fixture *f) {
    memset(f,0,sizeof *f);
    for (unsigned b=0;b<HOST_BUFFERS;b++) atomic_init(&f->busy[b],0);
    atomic_init(&f->copy_fallback,0);
    CHECK(!pthread_mutex_init(&f->mu,NULL));
    pthread_condattr_t attr;
    CHECK(!pthread_condattr_init(&attr));
    CHECK(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
    CHECK(!pthread_cond_init(&f->cv,&attr)); CHECK(!pthread_condattr_destroy(&attr));
    f->presenter=pthread_self(); f->current=0;
    f->busy[0]=f->uses[0]=f->surface_live=f->window_refs=1;
    CHECK(!vk_present_retire_init(&f->queue,finish_hook,wake_hook,event_hook,f));
    CHECK(!vk_present_retire_start(&f->queue));
    CHECK(!vk_present_retire_destroy(&f->queue)); /* Idle is not joined. */
}
static vk_present_retire_token reserve(host_fixture *f, uint64_t generation) {
    vk_present_retire_token token={0};
    CHECK(vk_present_retire_reserve(&f->queue,generation,&token));
    CHECK(vk_present_retire_count(&f->queue)<=2);
    return token;
}
static unsigned stage(host_fixture *f, vk_present_retire_token token, int current, int permit) {
    lock_host(f);
    CHECK(token.index<2 && !f->context_job[token.index] && f->current>=0);
    CHECK(current<0 || (current<HOST_BUFFERS && !f->busy[current]));
    CHECK(f->jobs_count+1<HOST_JOBS);
    unsigned id=++f->jobs_count; host_job *j=&f->jobs[id];
    *j=(host_job){.token=token,.previous=f->current,.current=current,
        .release_fd=-1,.allow_finish=permit,.phase=HOST_STAGED};
    CHECK(!pipe(j->pipefd));
    f->context_job[token.index]=id; f->current=current;
    if (current>=0) { f->busy[current]=1; f->uses[current]++; }
    invariant_locked(f); unlock_host(f);
    return id;
}
static void publish(host_fixture *f, unsigned id) {
    lock_host(f); CHECK(!f->published[id]); f->published[id]=1;
    vk_present_retire_token token=f->jobs[id].token; unlock_host(f);
    CHECK(vk_present_retire_publish(&f->queue,token));
    CHECK(!vk_present_retire_publish(&f->queue,token));
    CHECK(!vk_present_retire_cancel(&f->queue,token));
}
static unsigned apply(host_fixture *f, uint64_t generation, int current, int permit) {
    unsigned id=stage(f,reserve(f,generation),current,permit); publish(f,id); return id;
}
static void callback(host_fixture *f, unsigned id) {
    lock_host(f); host_job *j=&f->jobs[id];
    CHECK(id && id<=f->jobs_count && !j->callback_done && j->release_fd<0);
    j->release_fd=j->pipefd[0]; j->callback_done=1;
    changed(f); unlock_host(f);
}
static void release_signal(host_fixture *f, unsigned id, int success) {
    lock_host(f); host_job *j=&f->jobs[id];
    CHECK(!j->signals && !j->write_closes);
    char value=success ? 'S':'E';
    CHECK(write(j->pipefd[1],&value,1)==1);
    CHECK(!close(j->pipefd[1])); j->signals++; j->write_closes++;
    changed(f); unlock_host(f);
}
static void allow_finish(host_fixture *f, unsigned id) {
    lock_host(f); CHECK(!f->jobs[id].allow_finish); f->jobs[id].allow_finish=1;
    changed(f); unlock_host(f);
}
static void wait_phase(host_fixture *f, unsigned id, host_phase phase) {
    uint64_t deadline=now_ns()+WATCHDOG_NS;
    lock_host(f); while (f->jobs[id].phase<phase) wait_changed(f,deadline); unlock_host(f);
}
static void wait_retires(host_fixture *f, unsigned count) {
    uint64_t deadline=now_ns()+WATCHDOG_NS;
    lock_host(f); while (f->retire_count<count) wait_changed(f,deadline); unlock_host(f);
}
static void wait_counter(host_fixture *f, unsigned *counter, unsigned count) {
    uint64_t deadline=now_ns()+WATCHDOG_NS;
    lock_host(f); while (*counter<count) wait_changed(f,deadline); unlock_host(f);
}
static void assert_owned(host_fixture *f, unsigned id) {
    lock_host(f); host_job *j=&f->jobs[id];
    CHECK(!j->completed && !j->retired && !j->read_closes);
    CHECK(f->busy[j->previous] && (j->current<0 || f->busy[j->current]));
    invariant_locked(f); unlock_host(f);
}
static int surface_release(host_fixture *f) {
    if (!vk_present_retire_idle(&f->queue) || vk_present_retire_quarantined(&f->queue)) return 0;
    lock_host(f);
    int safe=f->joins && f->current<0 && !f->surface_released;
    for (unsigned b=0;b<HOST_BUFFERS;b++) if (f->busy[b]) safe=0;
    if (safe) { f->surface_live=0; f->window_refs=0; f->surface_released=1; }
    invariant_locked(f); unlock_host(f); return safe;
}
static void remove_current(host_fixture *f, uint64_t generation) {
    unsigned id=apply(f,generation,-1,1);
    callback(f,id); release_signal(f,id,1); wait_retires(f,id);
}
static void host_destroy(host_fixture *f, int retained) {
    CHECK(vk_present_retire_drain_join(&f->queue,now_ns()+WATCHDOG_NS));
    CHECK(vk_present_retire_idle(&f->queue));
    lock_host(f);
    CHECK(f->joins==1 && f->retire_count==f->jobs_count);
    for (unsigned id=1;id<=f->jobs_count;id++) {
        CHECK(f->jobs[id].completed==1 && f->jobs[id].retired==1);
        CHECK(f->jobs[id].read_closes==1 && f->jobs[id].write_closes==1);
    }
    invariant_locked(f); unlock_host(f);
    CHECK(surface_release(f)==!retained);
    CHECK(vk_present_retire_destroy(&f->queue));
    CHECK(!pthread_cond_destroy(&f->cv)); CHECK(!pthread_mutex_destroy(&f->mu));
}

static void all_callback_release_orderings(void) {
    unsigned schedules=0;
    for (unsigned a=0;a<4;a++) for (unsigned b=0;b<4;b++)
    for (unsigned c=0;c<4;c++) for (unsigned d=0;d<4;d++) {
        if (a==b || a==c || a==d || b==c || b==d || c==d) continue;
        unsigned order[]={a,b,c,d}, seen=0; host_fixture f; host_init(&f);
        unsigned first=apply(&f,17,1,1), second=apply(&f,17,2,1);
        wait_phase(&f,first,HOST_CALLBACK_WAIT);
        vk_present_retire_token rejected={0};
        CHECK(vk_present_retire_count(&f.queue)==2);
        CHECK(!vk_present_retire_reserve(&f.queue,17,&rejected));
        vk_present_retire_token live[VK_PRESENT_RETIRE_CAPACITY];
        CHECK(vk_present_retire_inflight(&f.queue,live)==2);
        CHECK((same_token(live[0],f.jobs[first].token) && same_token(live[1],f.jobs[second].token)) ||
            (same_token(live[1],f.jobs[first].token) && same_token(live[0],f.jobs[second].token)));
        for (unsigned e=0;e<4;e++) {
            unsigned op=order[e], id=op%2+1; seen|=1u<<op;
            if (op<2) callback(&f,id); else release_signal(&f,id,1);
            unsigned ready1=(seen&5)==5, ready2=ready1 && (seen&10)==10;
            if (ready1) wait_retires(&f,ready2 ? 2:1);
            lock_host(&f);
            CHECK(f.jobs[first].retired==ready1 && f.jobs[second].retired==ready2);
            CHECK(f.busy[0]==!ready1 && f.busy[1]==!ready2 && f.busy[2]);
            CHECK(f.jobs[first].token.generation==17 && f.jobs[second].token.generation==17);
            invariant_locked(&f); unlock_host(&f);
        }
        CHECK(f.retire_order[0]==first && f.retire_order[1]==second);
        CHECK(vk_present_retire_inflight(&f.queue,live)==0);
        remove_current(&f,17); host_destroy(&f,0); schedules++;
    }
    CHECK(schedules==24);
}
static void callback_before_publish_and_cancel(void) {
    host_fixture f; host_init(&f);
    vk_present_retire_token old=reserve(&f,31), other=reserve(&f,31);
    CHECK(vk_present_retire_count(&f.queue)==2);
    vk_present_retire_token live[VK_PRESENT_RETIRE_CAPACITY];
    CHECK(vk_present_retire_inflight(&f.queue,live)==0); /* reservations are not applied */
    CHECK(vk_present_retire_cancel(&f.queue,old));
    CHECK(!vk_present_retire_cancel(&f.queue,old));
    CHECK(!vk_present_retire_publish(&f.queue,old));
    vk_present_retire_token replacement=reserve(&f,32);
    CHECK(replacement.sequence!=old.sequence && replacement.generation==32);
    vk_present_retire_token wrong=replacement; wrong.generation=31;
    CHECK(!vk_present_retire_publish(&f.queue,wrong));
    CHECK(!vk_present_retire_cancel(&f.queue,wrong));
    wrong=replacement; wrong.index=2;
    CHECK(!vk_present_retire_cancel(&f.queue,wrong));
    CHECK(vk_present_retire_cancel(&f.queue,other));
    unsigned id=stage(&f,replacement,1,1);
    callback(&f,id); release_signal(&f,id,1);
    CHECK(!f.finish_calls && !f.jobs[id].entered && !f.jobs[id].completed);
    assert_owned(&f,id); CHECK(vk_present_retire_count(&f.queue)==1);
    publish(&f,id); wait_retires(&f,1);
    CHECK(!vk_present_retire_publish(&f.queue,old));
    CHECK(!vk_present_retire_cancel(&f.queue,replacement));
    remove_current(&f,32); host_destroy(&f,0);
}
static void callback_publication_interleavings(void) {
    unsigned schedules=0;
    for (unsigned a=0;a<3;a++) for (unsigned b=0;b<3;b++) for (unsigned c=0;c<3;c++) {
        if (a==b || a==c || b==c) continue;
        unsigned order[]={a,b,c}, seen=0; host_fixture f; host_init(&f);
        unsigned id=stage(&f,reserve(&f,33),1,1);
        for (unsigned step=0;step<3;step++) {
            unsigned op=order[step]; seen|=1u<<op;
            if (op==0) callback(&f,id);
            else if (op==1) release_signal(&f,id,1);
            else publish(&f,id);
            if (seen==7) wait_retires(&f,1);
            else {
                assert_owned(&f,id);
                if (!(seen&4)) {
                    lock_host(&f); CHECK(!f.finish_calls && !f.jobs[id].entered); unlock_host(&f);
                }
            }
        }
        remove_current(&f,33); host_destroy(&f,0); schedules++;
    }
    CHECK(schedules==6);
}
static void publication_order_and_wraparound(void) {
    host_fixture f; host_init(&f);
    vk_present_retire_token first=reserve(&f,41), second=reserve(&f,41);
    /* The FIFO is actual apply/publication order, not unused-reservation order. */
    unsigned a=stage(&f,second,1,1), b=stage(&f,first,2,1);
    callback(&f,b); release_signal(&f,b,1); publish(&f,a); publish(&f,b);
    wait_phase(&f,a,HOST_CALLBACK_WAIT); assert_owned(&f,a); assert_owned(&f,b);
    callback(&f,a); release_signal(&f,a,1); wait_retires(&f,2);
    CHECK(f.retire_order[0]==a && f.retire_order[1]==b);
    uint64_t sequence=second.sequence;
    for (unsigned n=0;n<80;n++) {
        int next=f.current==0 ? 1:0;
        unsigned id=apply(&f,42+n/8,next,1);
        CHECK(f.jobs[id].token.sequence>sequence); sequence=f.jobs[id].token.sequence;
        if (n&1) { release_signal(&f,id,1); callback(&f,id); }
        else { callback(&f,id); release_signal(&f,id,1); }
        wait_retires(&f,id);
        CHECK(!vk_present_retire_publish(&f.queue,first));
        CHECK(!vk_present_retire_cancel(&f.queue,second));
    }
    remove_current(&f,52); host_destroy(&f,0);
}

typedef struct { host_fixture *f; vk_present_retire_token token; int done; } capacity_call;
static void *capacity_waiter(void *context) {
    capacity_call *call=context; host_fixture *f=call->f;
    int counted=0;
    for (;;) {
        lock_host(f); unsigned wake=f->wakes; f->reserve_attempts++; unlock_host(f);
        if (vk_present_retire_reserve(&f->queue,61,&call->token)) break;
        lock_host(f);
        if (!counted) { f->capacity_jobs++; counted=1; }
        f->capacity_waits++; changed(f);
        uint64_t deadline=now_ns()+WATCHDOG_NS;
        while (f->wakes==wake) wait_changed(f,deadline);
        unlock_host(f);
    }
    lock_host(f); call->done=1; changed(f); unlock_host(f); return NULL;
}
static void capacity_wait_and_presenter_arbitration(void) {
    host_fixture f; host_init(&f);
    unsigned first=apply(&f,61,1,0), second=apply(&f,61,2,1);
    wait_phase(&f,first,HOST_ENTERED);
    capacity_call call={.f=&f}; pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,capacity_waiter,&call));
    wait_counter(&f,&f.capacity_waits,1);
    /* Spurious/repeated presenter notifications are harmless, not polling. */
    for (unsigned n=2;n<=4;n++) { wake_hook(&f); wait_counter(&f,&f.capacity_waits,n); }
    lock_host(&f); CHECK(f.capacity_jobs==1 && !call.done && !f.retire_count);
    CHECK(f.finish_calls==1 && !f.jobs[second].entered); unlock_host(&f);
    /* Deliberately stalled actual worker cannot block unrelated arbitration. */
    fg_adaptive_present_state state={0}, saved=state;
    fg_adaptive_present_job job={.slot=UINT64_C(1016666667),.frame=10,.epoch=3,.generation=61,.generated=1};
    fg_adaptive_present_context c={.now=UINT64_C(1000000000),.epoch=3,.generation=61,.window=1};
    CHECK(fg_adaptive_present_deadline(job.slot)==job.slot-UINT64_C(2000000));
    CHECK(fg_adaptive_present_decide(&state,job,c)==FG_ADAPTIVE_PRESENT_WAIT);
    c.queued_real=1; CHECK(fg_adaptive_present_decide(&state,job,c)==FG_ADAPTIVE_PRESENT_YIELD);
    c.ready_real=1; CHECK(fg_adaptive_present_decide(&state,job,c)==FG_ADAPTIVE_PRESENT_SUPPRESSED);
    c.ready_real=c.queued_real=0; c.window=0;
    CHECK(fg_adaptive_present_decide(&state,job,c)==FG_ADAPTIVE_PRESENT_WINDOW);
    c.window=1; c.now=job.slot;
    CHECK(fg_adaptive_present_decide(&state,job,c)==FG_ADAPTIVE_PRESENT_EXPIRED);
    CHECK(!memcmp(&state,&saved,sizeof state));
    fg_job_trace trace={.frame=10,.submit=job.slot-20,.pickup=job.slot-10,
        .source_ready=job.slot-8,.output_ready=job.slot-6,.target=job.slot,.decision=job.slot+1};
    const char *terminal=fg_job_trace_class(trace);
    trace.pickup=trace.submit; /* Earlier bookkeeping alone is NOT success. */
    CHECK(!strcmp(terminal,fg_job_trace_class(trace)));
    CHECK(!strcmp(terminal,"presenter-drain-or-deschedule-late"));
    CHECK(fg_adaptive_child_status(job.slot-2,job.slot-1,job.slot,job.slot-1)==FRAMEGEN_CHILD_READY);
    CHECK(fg_adaptive_child_status(job.slot,job.slot-1,job.slot,job.slot-1)==FRAMEGEN_CHILD_SOURCE_LATE);
    CHECK(fg_adaptive_child_status(0,job.slot-1,job.slot,job.slot-1)==FRAMEGEN_CHILD_SOURCE_MISSING);
    CHECK(fg_adaptive_child_status(job.slot-2,job.slot-1,job.slot,job.slot)==FRAMEGEN_CHILD_DEADLINE);
    assert_owned(&f,first); assert_owned(&f,second);
    callback(&f,second); release_signal(&f,second,1); /* Later complete cannot overtake. */
    callback(&f,first); release_signal(&f,first,1); allow_finish(&f,first);
    CHECK(!pthread_join(thread,NULL)); CHECK(call.done);
    CHECK(vk_present_retire_count(&f.queue)<=2 && f.capacity_jobs==1);
    CHECK(vk_present_retire_cancel(&f.queue,call.token));
    wait_retires(&f,2); remove_current(&f,61); host_destroy(&f,0);
}

typedef struct { host_fixture *f; int stop, result; uint64_t deadline; } drain_call;
static void *drainer(void *context) {
    drain_call *call=context;
    call->result=call->stop ? vk_present_retire_drain_join(&call->f->queue,call->deadline) :
        vk_present_retire_drain(&call->f->queue,call->deadline);
    return NULL;
}
static void reservations_during_drain(void) {
    host_fixture f; host_init(&f);
    vk_present_retire_token first=reserve(&f,69), second=reserve(&f,69);
    drain_call call={.f=&f,.deadline=now_ns()+WATCHDOG_NS}; pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,drainer,&call)); wait_counter(&f,&f.drains,1);
    CHECK(!vk_present_retire_idle(&f.queue));
    CHECK(vk_present_retire_cancel(&f.queue,second));
    CHECK(vk_present_retire_cancel(&f.queue,first));
    CHECK(!pthread_join(thread,NULL)); CHECK(call.result && !f.finish_calls);
    CHECK(vk_present_retire_idle(&f.queue));
    /* A reservation already applied before drain still MUST publish and finish,
     * rather than being lost when new admission closes. */
    unsigned id=stage(&f,reserve(&f,70),1,1);
    callback(&f,id);
    call=(drain_call){.f=&f,.deadline=now_ns()+WATCHDOG_NS};
    CHECK(!pthread_create(&thread,NULL,drainer,&call)); wait_counter(&f,&f.drains,2);
    assert_owned(&f,id); publish(&f,id); wait_phase(&f,id,HOST_RELEASE_WAIT);
    assert_owned(&f,id); release_signal(&f,id,1);
    CHECK(!pthread_join(thread,NULL)); CHECK(call.result);
    remove_current(&f,70); host_destroy(&f,0);
}
static void window_and_mode_drain(void) {
    /* Old adaptive work must complete before any new window/mode ownership. */
    for (unsigned target_mode=0;target_mode<3;target_mode++) {
        host_fixture f; host_init(&f);
        unsigned a=apply(&f,71,1,1), b=apply(&f,71,2,1);
        wait_phase(&f,a,HOST_CALLBACK_WAIT);
        drain_call call={.f=&f,.deadline=now_ns()+WATCHDOG_NS}; pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,drainer,&call)); wait_counter(&f,&f.drains,1);
        vk_present_retire_token rejected={0};
        CHECK(!vk_present_retire_reserve(&f.queue,72,&rejected));
        CHECK(!surface_release(&f)); assert_owned(&f,a); assert_owned(&f,b);
        fg_adaptive_present_state state={0};
        fg_adaptive_present_job old={.slot=100,.frame=1,.epoch=1,.generation=71,.generated=1};
        fg_adaptive_present_context new_window={.now=99,.epoch=1,.generation=72,.window=1};
        CHECK(fg_adaptive_present_decide(&state,old,new_window)==FG_ADAPTIVE_PRESENT_WINDOW);
        CHECK(vk_present_retire_admitted(1,target_mode==2,1,1)==(target_mode==2));
        callback(&f,b); release_signal(&f,b,1); callback(&f,a);
        wait_phase(&f,a,HOST_RELEASE_WAIT); assert_owned(&f,a); assert_owned(&f,b);
        release_signal(&f,a,1); CHECK(!pthread_join(thread,NULL)); CHECK(call.result);
        CHECK(vk_present_retire_idle(&f.queue) && !surface_release(&f));
        if (target_mode==2) {
            /* A clean adaptive generation drain reopens worker admission. */
            unsigned next=apply(&f,72,0,1);
            CHECK(f.jobs[next].token.generation==72);
            callback(&f,next); release_signal(&f,next,1); wait_retires(&f,next);
        }
        /* This explicit removal closes the host stand-in ownership, not a
         * claim that locked/OFF new presents dispatch through the worker. */
        remove_current(&f,target_mode==2 ? 72:71); host_destroy(&f,0);
    }
}
static void shutdown_with_delayed_completion(void) {
    host_fixture f; host_init(&f);
    unsigned a=apply(&f,81,1,1), b=apply(&f,81,2,1);
    wait_phase(&f,a,HOST_CALLBACK_WAIT);
    /* Removal is a real ownership transaction too, never a timeout shortcut. */
    callback(&f,b); release_signal(&f,b,1);
    drain_call call={.f=&f,.deadline=now_ns()+WATCHDOG_NS}; pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,drainer,&call)); wait_counter(&f,&f.drains,1);
    CHECK(!surface_release(&f)); callback(&f,a); wait_phase(&f,a,HOST_RELEASE_WAIT);
    CHECK(!surface_release(&f)); release_signal(&f,a,1);
    CHECK(!pthread_join(thread,NULL)); CHECK(call.result);
    unsigned removal=apply(&f,81,-1,1); wait_phase(&f,removal,HOST_CALLBACK_WAIT);
    drain_call stop={.f=&f,.stop=1,.deadline=now_ns()+WATCHDOG_NS};
    CHECK(!pthread_create(&thread,NULL,drainer,&stop)); wait_counter(&f,&f.drains,2);
    CHECK(!surface_release(&f)); assert_owned(&f,removal);
    vk_present_retire_token rejected={0};
    CHECK(!vk_present_retire_reserve(&f.queue,82,&rejected));
    CHECK(!vk_present_retire_destroy(&f.queue));
    callback(&f,removal); wait_phase(&f,removal,HOST_RELEASE_WAIT);
    CHECK(!surface_release(&f)); release_signal(&f,removal,1);
    CHECK(!pthread_join(thread,NULL)); CHECK(stop.result && f.joins==1);
    CHECK(vk_present_retire_idle(&f.queue) && surface_release(&f));
    CHECK(vk_present_retire_destroy(&f.queue));
    CHECK(!pthread_cond_destroy(&f.cv)); CHECK(!pthread_mutex_destroy(&f.mu));
}
static void fence_failure_retains_previous(void) {
    host_fixture f; host_init(&f);
    unsigned a=apply(&f,91,1,1), b=apply(&f,91,2,1);
    callback(&f,b); release_signal(&f,b,1);
    callback(&f,a); release_signal(&f,a,0); wait_retires(&f,2);
    lock_host(&f);
    CHECK(f.copy_fallback && f.retained[0] && f.busy[0] && !f.releases[0]);
    CHECK(!f.busy[1] && f.busy[2]);
    CHECK(f.jobs[a].read_closes==1 && f.jobs[b].read_closes==1);
    CHECK(f.retire_order[0]==a && f.retire_order[1]==b); unlock_host(&f);
    /* Healthy ownership can finish without recycling the failed buffer. */
    unsigned c=apply(&f,91,1,1);
    callback(&f,c); release_signal(&f,c,1); wait_retires(&f,c);
    remove_current(&f,91); host_destroy(&f,1);
}

/* The ENTIRE reachable graph is durable: queue mutex/cond/FIFO/thread state,
 * host callback mutex/cond/contexts/jobs/FDs and surface/window owner stand-ins.
 * No timeout path destroys even synchronization/event storage in these owners. */
static host_fixture quarantined_fixtures[6];
static void missing_callback_and_never_signaling_fence(void) {
    for (unsigned mode=0;mode<3;mode++) for (unsigned missing=0;missing<2;missing++) {
        host_fixture *f=&quarantined_fixtures[mode*2+missing]; host_init(f);
        unsigned a=apply(f,101+mode,1,1), b=apply(f,101+mode,2,1);
        if (missing) callback(f,a);
        callback(f,b); release_signal(f,b,1);
        wait_phase(f,a,missing ? HOST_RELEASE_WAIT:HOST_CALLBACK_WAIT);
        assert_owned(f,a); assert_owned(f,b); CHECK(!surface_release(f));
        uint64_t begin=now_ns();
        if (!mode) {
            /* Exercise the actual default steady-head two-second budget. */
            wait_counter(f,&f->quarantines,1);
            CHECK(now_ns()-begin<VK_PRESENT_RETIRE_BUDGET_NS+WATCHDOG_NS);
        } else {
            /* One TOTAL budget covers both heads; caller must not join live work. */
            uint64_t deadline=begin+50*NS_PER_MS;
            int clean=mode==1 ? vk_present_retire_drain(&f->queue,deadline) :
                vk_present_retire_drain_join(&f->queue,deadline);
            CHECK(!clean && now_ns()-begin<UINT64_C(1000000000));
        }
        CHECK(vk_present_retire_quarantined(&f->queue));
        CHECK(vk_present_retire_count(&f->queue)==2 && !vk_present_retire_idle(&f->queue));
        CHECK(!vk_present_retire_destroy(&f->queue) && !surface_release(f));
        vk_present_retire_token rejected={0};
        CHECK(!vk_present_retire_reserve(&f->queue,200,&rejected));
        CHECK(!vk_present_retire_cancel(&f->queue,f->jobs[a].token));
        assert_owned(f,a); assert_owned(f,b);
        if (mode) {
            lock_host(f);
            /* Drain returned while the same actual worker is still accessing
             * this quarantined owner's synchronization and callback storage. */
            CHECK(!f->joins && f->surface_live && f->window_refs==1);
            CHECK(f->jobs[a].phase==(missing ? HOST_RELEASE_WAIT:HOST_CALLBACK_WAIT));
            unlock_host(f);
            if (!missing) {
                /* Explicit late callback AFTER timed-out window teardown or
                 * shutdown, but BEFORE the worker exits. No release follows. */
                callback(f,a); wait_phase(f,a,HOST_RELEASE_WAIT);
                assert_owned(f,a); assert_owned(f,b);
                CHECK(vk_present_retire_quarantined(&f->queue));
            }
        }
    }
    for (unsigned n=0;n<6;n++) {
        host_fixture *f=&quarantined_fixtures[n];
        /* Reap finite worker only AFTER it exits; quarantine remains permanent. */
        CHECK(!vk_present_retire_drain_join(&f->queue,now_ns()+WATCHDOG_NS));
        lock_host(f);
        CHECK(f->joins==1 && f->quarantines && !f->retire_count);
        CHECK(f->finish_calls==1 && !f->jobs[2].entered);
        CHECK(f->jobs[1].phase==HOST_TIMED_OUT && f->surface_live && f->window_refs==1);
        unsigned quarantine_event=HOST_EVENTS, join_event=HOST_EVENTS;
        for (unsigned e=0;e<f->event_count;e++) {
            if (!strcmp(f->events[e].event,"quarantine") && quarantine_event==HOST_EVENTS)
                quarantine_event=e;
            if (!strcmp(f->events[e].event,"join")) join_event=e;
        }
        /* Joining an exited worker follows quarantine, not ownership release. */
        CHECK(quarantine_event<join_event && join_event<HOST_EVENTS);
        invariant_locked(f); unlock_host(f);
        CHECK(!vk_present_retire_destroy(&f->queue));
        /* Even a callback arriving after shutdown may only fill durable storage. */
        if (!n) callback(f,1); /* Steady-state callback arrives even after join. */
        release_signal(f,1,1);
        assert_owned(f,1); assert_owned(f,2);
        CHECK(vk_present_retire_count(&f->queue)==2 && !surface_release(f));
        /* Read ends and ownership deliberately remain quarantined, not closed
         * as a fake success. These static owners live until process exit. */
    }
}

typedef struct { int policy, defer, next[5]; vk_present_flight states[8]; } legacy_trace;
static legacy_trace legacy_state_trace(int enabled, const char *setting, int api, int symbol) {
    legacy_trace t; memset(&t,0,sizeof t);
    t.policy=vk_present_backpressure_policy(enabled,setting,api,symbol);
    t.defer=vk_present_flight_defer(t.policy,16,200,100);
    vk_present_flight f={0}; t.states[0]=f;
    t.next[0]=vk_present_flight_next(&f); CHECK(vk_present_flight_submit(&f,0)); t.states[1]=f;
    t.next[1]=vk_present_flight_next(&f); CHECK(vk_present_flight_submit(&f,1)); t.states[2]=f;
    t.next[2]=vk_present_flight_next(&f); CHECK(!vk_present_flight_retire(&f,1)); t.states[3]=f;
    CHECK(vk_present_flight_retire(&f,0)); t.states[4]=f;
    t.next[3]=vk_present_flight_next(&f); CHECK(vk_present_flight_submit(&f,0)); t.states[5]=f;
    CHECK(!vk_present_flight_retire(&f,0) && vk_present_flight_retire(&f,1)); t.states[6]=f;
    CHECK(vk_present_flight_retire(&f,0)); t.next[4]=vk_present_flight_next(&f); t.states[7]=f;
    return t;
}
static void off_locked_policy_reference(void) {
    const char *selectors[]={NULL,"","off","on","ON","1","true","on ","offx"};
    const char *backpressure[]={NULL,"serial","request","invalid"};
    const char *settings[]={NULL,"0","1"}; const int apis[]={30,31,35};
    CHECK(VK_PRESENT_RETIRE_BUDGET_NS==UINT64_C(2000000000));
    for (unsigned v=0;v<sizeof selectors/sizeof *selectors;v++) {
        int selected=vk_present_retire_selected(selectors[v]); CHECK(selected==(v==3));
        for (unsigned mode=0;mode<3;mode++) for (unsigned bp=0;bp<4;bp++)
        for (unsigned s=0;s<3;s++) for (unsigned api=0;api<3;api++)
        for (int symbol=0;symbol<=1;symbol++) for (int qualified=0;qualified<=1;qualified++) {
            int adaptive=mode==2, locked=mode==1;
            int old_gate=locked || vk_present_adaptive_backpressure_requested(adaptive,backpressure[bp]);
            legacy_trace before=legacy_state_trace(old_gate,settings[s],apis[api],symbol);
            int admitted=vk_present_retire_admitted(selected,adaptive,qualified,
                before.policy==VK_PRESENT_BP_REQUEST);
            CHECK(admitted==(selected && adaptive && qualified && before.policy==VK_PRESENT_BP_REQUEST));
            if (!adaptive || !selected || !qualified || before.policy!=VK_PRESENT_BP_REQUEST) CHECK(!admitted);
            /* The new selector does not participate in legacy policy/flight
             * helpers. Compare policy AND recorded state bytes, not APK bytes. */
            legacy_trace after=legacy_state_trace(old_gate,settings[s],apis[api],symbol);
            CHECK(!memcmp(&before,&after,sizeof before));
        }
    }
    /* All directed mode transitions preserve the unchanged legacy helper's
     * outstanding contexts; no new selector clears or retires those bytes. */
    for (unsigned from=0;from<3;from++) for (unsigned to=0;to<3;to++) {
        if (from==to) continue;
        vk_present_flight flight={0}; CHECK(vk_present_flight_submit(&flight,0));
        CHECK(vk_present_flight_submit(&flight,1)); vk_present_flight before=flight;
        CHECK(vk_present_retire_admitted(1,to==2,1,1)==(to==2));
        CHECK(!memcmp(&before,&flight,sizeof before));
        CHECK(!vk_present_flight_retire(&flight,1));
        CHECK(vk_present_flight_retire(&flight,0) && vk_present_flight_retire(&flight,1));
    }
}
int main(void) {
    off_locked_policy_reference(); all_callback_release_orderings();
    callback_before_publish_and_cancel(); callback_publication_interleavings();
    publication_order_and_wraparound(); capacity_wait_and_presenter_arbitration();
    reservations_during_drain(); window_and_mode_drain();
    shutdown_with_delayed_completion(); fence_failure_retains_previous();
    missing_callback_and_never_signaling_fence();
    printf("present retire: %u checks passed (actual two-context worker, 24 pipe/callback schedules, immutable tokens, notification-only capacity waits, bounded quarantine and join ownership)\n",
        atomic_load_explicit(&checks,memory_order_relaxed));
    return 0;
}
