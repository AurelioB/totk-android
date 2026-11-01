/* MIT. Two-context transaction retirement. No allocation, GPU work or Android
 * calls. The owner and callback storage MUST outlive a quarantined queue.
 * finish runs outside mu and must return by its absolute MONOTONIC deadline:
 * 1 = callback/release obligations complete, 0 = unresolved ownership.
 * A timeout never retires an entry. Only a clean drain+join permits destroy. */
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

enum { VK_PRESENT_RETIRE_CAPACITY = 2 };
#define VK_PRESENT_RETIRE_BUDGET_NS UINT64_C(2000000000)
typedef struct {
    unsigned index;
    uint64_t sequence, generation;
} vk_present_retire_token;
typedef struct {
    const char *event;
    vk_present_retire_token token;
    uint64_t now_ns, wakes, retires;
    unsigned depth;
} vk_present_retire_event;
typedef int (*vk_present_retire_finish_fn)(void *, unsigned, uint64_t);
typedef void (*vk_present_retire_wake_fn)(void *);
typedef void (*vk_present_retire_event_fn)(void *, const vk_present_retire_event *);
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    vk_present_retire_finish_fn finish;
    vk_present_retire_wake_fn wake;
    vk_present_retire_event_fn event;
    void *context;
    vk_present_retire_token slots[VK_PRESENT_RETIRE_CAPACITY];
    unsigned char state[VK_PRESENT_RETIRE_CAPACITY]; /* free/reserved/submitted */
    unsigned fifo[VK_PRESENT_RETIRE_CAPACITY], head, submitted, used, cursor;
    uint64_t sequence, wakes, retires, drain_deadline;
    int started, exited, joined, stopping, draining, quarantined;
} vk_present_retire;

static inline uint64_t vk_present_retire_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static inline uint64_t vk_present_retire_deadline(void) {
    return vk_present_retire_now()+VK_PRESENT_RETIRE_BUDGET_NS;
}
static inline struct timespec vk_present_retire_time(uint64_t ns) {
    return (struct timespec){(time_t)(ns/UINT64_C(1000000000)),
        (long)(ns%UINT64_C(1000000000))};
}
static inline int vk_present_retire_selected(const char *value) {
    return value && !strcmp(value,"on");
}
static inline int vk_present_retire_admitted(int selected, int adaptive,
        int qualified, int backpressure_request) {
    return selected && adaptive && qualified && backpressure_request;
}
/* Fence failure releases the context after its callback but NEVER its owned
 * PREVIOUS buffer. Shared with the Android finish path and host FD fixtures. */
static inline void vk_present_retire_release_result(int released,
        _Atomic int *previous_busy, _Atomic int *copy_fallback) {
    if (released) {
        if (previous_busy) atomic_store(previous_busy,0);
    } else atomic_store(copy_fallback,1);
}
/* Caller holds mu; returned snapshots contain no borrowed mutable storage. */
static inline vk_present_retire_event vk_present_retire_snapshot(vk_present_retire *q,
        const char *event, vk_present_retire_token token) {
    return (vk_present_retire_event){event,token,q->event ? vk_present_retire_now() : 0,
        q->wakes,q->retires,q->used};
}
static inline void vk_present_retire_emit(vk_present_retire *q, vk_present_retire_event event) {
    if (q->event) q->event(q->context,&event);
}
static inline void vk_present_retire_notify(vk_present_retire *q) {
    if (q->wake) q->wake(q->context);
}
static inline void *vk_present_retire_worker(void *context) {
    vk_present_retire *q=context;
    pthread_mutex_lock(&q->mu);
    for (;;) {
        while (!q->submitted && !q->stopping && !q->quarantined) {
            pthread_cond_wait(&q->cv,&q->mu);
            q->wakes++;
        }
        if (q->quarantined || (!q->submitted && q->stopping)) break;
        unsigned index=q->fifo[q->head];
        vk_present_retire_token token=q->slots[index];
        uint64_t deadline=vk_present_retire_deadline();
        if (q->draining && q->drain_deadline<deadline) deadline=q->drain_deadline;
        vk_present_retire_event event=vk_present_retire_snapshot(q,"dequeue",token);
        pthread_mutex_unlock(&q->mu);
        vk_present_retire_emit(q,event);
        int finished=q->finish(q->context,index,deadline);
        pthread_mutex_lock(&q->mu);
        if (!finished || q->quarantined) {
            q->quarantined=q->stopping=1;
            event=vk_present_retire_snapshot(q,"quarantine",token);
            pthread_cond_broadcast(&q->cv);
            pthread_mutex_unlock(&q->mu);
            vk_present_retire_emit(q,event);
            vk_present_retire_notify(q);
            pthread_mutex_lock(&q->mu);
            break;
        }
        /* Only this worker removes FIFO HEAD. RESERVED slots never enter it. */
        q->head=(q->head+1)%VK_PRESENT_RETIRE_CAPACITY;
        q->submitted--; q->used--; q->state[index]=0; q->retires++;
        event=vk_present_retire_snapshot(q,"retire",token);
        pthread_cond_broadcast(&q->cv);
        pthread_mutex_unlock(&q->mu);
        vk_present_retire_emit(q,event);
        vk_present_retire_notify(q);
        pthread_mutex_lock(&q->mu);
    }
    q->exited=1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return NULL;
}
static inline int vk_present_retire_init(vk_present_retire *q,
        vk_present_retire_finish_fn finish, vk_present_retire_wake_fn wake,
        vk_present_retire_event_fn event, void *context) {
    if (!finish) return EINVAL;
    *q=(vk_present_retire){.finish=finish,.wake=wake,.event=event,.context=context};
    int error=pthread_mutex_init(&q->mu,NULL);
    if (error) return error;
    pthread_condattr_t attr;
    error=pthread_condattr_init(&attr);
    if (!error) {
        error=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
        if (!error) error=pthread_cond_init(&q->cv,&attr);
        pthread_condattr_destroy(&attr);
    }
    if (error) pthread_mutex_destroy(&q->mu);
    return error;
}
static inline int vk_present_retire_start(vk_present_retire *q) {
    pthread_mutex_lock(&q->mu);
    int error=q->started || q->quarantined ? EINVAL : 0;
    if (!error) {
        error=pthread_create(&q->thread,NULL,vk_present_retire_worker,q);
        if (!error) q->started=1;
    }
    pthread_mutex_unlock(&q->mu);
    return error;
}
static inline int vk_present_retire_matches(vk_present_retire *q, vk_present_retire_token token) {
    return token.index<VK_PRESENT_RETIRE_CAPACITY &&
        q->slots[token.index].sequence==token.sequence &&
        q->slots[token.index].generation==token.generation;
}
static inline int vk_present_retire_reserve(vk_present_retire *q, uint64_t generation,
        vk_present_retire_token *token) {
    pthread_mutex_lock(&q->mu);
    if (!q->started || q->stopping || q->draining || q->quarantined ||
            q->used==VK_PRESENT_RETIRE_CAPACITY) {
        pthread_mutex_unlock(&q->mu); return 0;
    }
    unsigned index=q->cursor;
    while (q->state[index]) index=(index+1)%VK_PRESENT_RETIRE_CAPACITY;
    *token=(vk_present_retire_token){index,++q->sequence,generation};
    q->slots[index]=*token; q->state[index]=1; q->used++;
    q->cursor=(index+1)%VK_PRESENT_RETIRE_CAPACITY;
    vk_present_retire_event event=vk_present_retire_snapshot(q,"reserve",*token);
    pthread_mutex_unlock(&q->mu);
    vk_present_retire_emit(q,event);
    return 1;
}
static inline int vk_present_retire_cancel(vk_present_retire *q, vk_present_retire_token token) {
    pthread_mutex_lock(&q->mu);
    int valid=vk_present_retire_matches(q,token) && q->state[token.index]==1 && !q->quarantined;
    vk_present_retire_event event={0};
    if (valid) {
        q->state[token.index]=0; q->used--;
        event=vk_present_retire_snapshot(q,"cancel",token);
        pthread_cond_broadcast(&q->cv);
    }
    pthread_mutex_unlock(&q->mu);
    if (valid) { vk_present_retire_emit(q,event); vk_present_retire_notify(q); }
    return valid;
}
static inline int vk_present_retire_publish(vk_present_retire *q, vk_present_retire_token token) {
    pthread_mutex_lock(&q->mu);
    /* Already-applied reservations remain owned even if a concurrent drain
     * has quarantined the owner; never turn an apply into a free context. */
    int valid=vk_present_retire_matches(q,token) && q->state[token.index]==1;
    vk_present_retire_event event={0};
    if (valid) {
        q->state[token.index]=2;
        q->fifo[(q->head+q->submitted)%VK_PRESENT_RETIRE_CAPACITY]=token.index;
        q->submitted++;
        event=vk_present_retire_snapshot(q,"publish",token);
        pthread_cond_broadcast(&q->cv);
    }
    pthread_mutex_unlock(&q->mu);
    if (valid) vk_present_retire_emit(q,event);
    return valid;
}
static inline unsigned vk_present_retire_count(vk_present_retire *q) {
    pthread_mutex_lock(&q->mu); unsigned count=q->used; pthread_mutex_unlock(&q->mu);
    return count;
}
/* Diagnostic identity snapshot only: no context/fence/buffer ownership is
 * transferred. Reserved-but-not-applied entries are deliberately excluded. */
static inline unsigned vk_present_retire_inflight(vk_present_retire *q,
        vk_present_retire_token out[VK_PRESENT_RETIRE_CAPACITY]) {
    if (!q || !out) return 0;
    pthread_mutex_lock(&q->mu);
    unsigned count=0;
    for (unsigned i=0;i<VK_PRESENT_RETIRE_CAPACITY;i++)
        if (q->state[i]==2) out[count++]=q->slots[i];
    pthread_mutex_unlock(&q->mu);
    return count;
}
static inline int vk_present_retire_idle(vk_present_retire *q) {
    return vk_present_retire_count(q)==0;
}
static inline int vk_present_retire_quarantined(vk_present_retire *q) {
    pthread_mutex_lock(&q->mu); int result=q->quarantined; pthread_mutex_unlock(&q->mu);
    return result;
}
/* A single caller owns lifecycle. Deadline covers the ENTIRE drain, not each
 * context. stop=1 drains then exits; plain drain pauses admission until idle. */
static inline int vk_present_retire_drain_impl(vk_present_retire *q, uint64_t deadline, int stop) {
    pthread_mutex_lock(&q->mu);
    q->draining=1; q->drain_deadline=deadline;
    if (stop) q->stopping=1;
    vk_present_retire_event event=vk_present_retire_snapshot(q,"drain",(vk_present_retire_token){0});
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    vk_present_retire_emit(q,event);
    pthread_mutex_lock(&q->mu);
    struct timespec absolute=vk_present_retire_time(deadline);
    while ((q->used || (stop && q->started && !q->exited)) && !q->quarantined) {
        int error=pthread_cond_timedwait(&q->cv,&q->mu,&absolute);
        if (error) {
            q->quarantined=q->stopping=1;
            pthread_cond_broadcast(&q->cv);
            break;
        }
    }
    int clean=!q->quarantined && !q->used;
    if (!stop && clean) { q->draining=0; q->drain_deadline=0; }
    event=vk_present_retire_snapshot(q,clean ? "drain-end" : "quarantine",(vk_present_retire_token){0});
    pthread_mutex_unlock(&q->mu);
    vk_present_retire_emit(q,event);
    if (!clean) vk_present_retire_notify(q);
    return clean;
}
static inline int vk_present_retire_drain(vk_present_retire *q, uint64_t deadline) {
    return vk_present_retire_drain_impl(q,deadline,0);
}
static inline int vk_present_retire_drain_join(vk_present_retire *q, uint64_t deadline) {
    int clean=vk_present_retire_drain_impl(q,deadline,1);
    pthread_mutex_lock(&q->mu);
    /* Even an unresolved callback may have let the finite worker exit. Joining
     * is then safe, but its storage/contexts must STILL remain quarantined. */
    struct timespec absolute=vk_present_retire_time(deadline);
    while (q->started && !q->exited) {
        if (pthread_cond_timedwait(&q->cv,&q->mu,&absolute)) break;
    }
    int join=q->started && q->exited && !q->joined;
    pthread_mutex_unlock(&q->mu);
    if (join) {
        int error=pthread_join(q->thread,NULL);
        pthread_mutex_lock(&q->mu);
        if (!error) q->joined=1;
        else { q->quarantined=1; clean=0; }
        vk_present_retire_event event=vk_present_retire_snapshot(q,"join",(vk_present_retire_token){0});
        pthread_mutex_unlock(&q->mu);
        vk_present_retire_emit(q,event);
    }
    return clean;
}
static inline int vk_present_retire_destroy(vk_present_retire *q) {
    pthread_mutex_lock(&q->mu);
    int safe=!q->quarantined && !q->used && (!q->started || q->joined);
    pthread_mutex_unlock(&q->mu);
    if (!safe) return 0;
    pthread_cond_destroy(&q->cv); pthread_mutex_destroy(&q->mu);
    return 1;
}
