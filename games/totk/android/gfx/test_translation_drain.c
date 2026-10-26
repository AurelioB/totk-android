/* Real worker and drain functions, deterministic fake renderer callbacks. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
enum { XJ_SUBMIT = 1, XJ_PRESENT, XJ_TEXREL, XQ = 64, XF = 4096 };
typedef struct { int unused; } tex_t;
typedef struct { int unused; } win_t;
typedef struct { int kind, n; uint64_t *h; void *ucap; win_t *win; int idx; double period; int crop[4]; uint64_t frame, sub, guest_ns, origin_ticks; tex_t tex; } xjob;
static xjob g_xq[XQ];
static uint64_t g_xq_in, g_xq_done, g_xsub_done, g_xsub_fence[XF];
static pthread_mutex_t g_xq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xq_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_x_drains, g_x_drain_ns;
static pthread_mutex_t handshake = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handshake_cv = PTHREAD_COND_INITIALIZER;
static int entered_wait, callbacks, released_core;
static int async_replay(void) { return 1; }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void fake_setname(const char *name) { (void)name; }
#define pthread_setname_np fake_setname
#define NVN_BLOCKING() (++released_core)
static void submit_exec(int, const uint64_t *, void *, uint64_t, uint64_t, uint64_t);
static void present_exec(win_t *, int, double, const int[4], uint64_t);
static void mtl_tex_release(const tex_t *);
static uint64_t mtl_last_fence(void) { return 123; }
void set_time_constraint_comp_x(uint64_t a, uint64_t b) { (void)a; (void)b; }
static int observed_wait(pthread_cond_t *cv, pthread_mutex_t *mu) {
    /* The first phase has work ready throughout: a worker wait here is a bug. */
    pthread_mutex_lock(&handshake); entered_wait = 1;
    pthread_cond_broadcast(&handshake_cv); pthread_mutex_unlock(&handshake);
    return pthread_cond_wait(cv, mu);
}
#define pthread_cond_wait observed_wait
#include "translation_drain.inc"
#undef pthread_cond_wait
static void invalidate_callback(void) {
    assert(t_on_x_thread);
    assert(g_xq_done < g_xq_in); /* Current and later jobs are still outstanding. */
    nvn_x_drain(); ++callbacks;
}
static void submit_exec(int n, const uint64_t *h, void *p, uint64_t ns, uint64_t ticks, uint64_t frame) {
    (void)h; (void)p; (void)ns; (void)ticks; (void)frame;
    if (n == -1) pthread_exit(NULL);
    invalidate_callback();
}
static void present_exec(win_t *w, int i, double p, const int crop[4], uint64_t frame) {
    (void)w; (void)i; (void)p; (void)crop; (void)frame; invalidate_callback();
}
void live_del_x(const tex_t *t) { (void)t; }
static void mtl_tex_release(const tex_t *t) { (void)t; invalidate_callback(); }
static void *guest_drain(void *unused) { (void)unused; assert(!t_on_x_thread); nvn_x_drain(); return NULL; }
int main(void) {
    alarm(5); /* A removed guard fails deterministically rather than hanging the suite. */
    g_xq[0] = (xjob){.kind = XJ_SUBMIT, .sub = 1};
    g_xq[1] = (xjob){.kind = XJ_PRESENT};
    g_xq[2] = (xjob){.kind = XJ_TEXREL};
    g_xq[3] = (xjob){.kind = XJ_SUBMIT, .n = -1}; g_xq_in = 4;
    pthread_t worker; assert(!pthread_create(&worker, NULL, x_thread, NULL));
    assert(!pthread_join(worker, NULL));
    assert(callbacks == 3 && g_xq_done == 3 && g_xsub_done == 1 && g_xsub_fence[1] == 123);
    assert(!entered_wait && !released_core && !atomic_load(&g_x_drains));
    /* TLS must not forgive another thread's unfinished work. Wait until it is
     * in the real wait before publishing completion; no sleep/timing race. */
    pthread_t guest; assert(!pthread_create(&guest, NULL, guest_drain, NULL));
    pthread_mutex_lock(&handshake);
    while (!entered_wait) pthread_cond_wait(&handshake_cv, &handshake);
    pthread_mutex_unlock(&handshake);
    pthread_mutex_lock(&g_xq_mu); assert(g_xq_done == 3); g_xq_done = 4;
    pthread_cond_broadcast(&g_xq_cv); pthread_mutex_unlock(&g_xq_mu);
    assert(!pthread_join(guest, NULL));
    assert(released_core == 1 && atomic_load(&g_x_drains) == 1);
    alarm(0); puts("PASS: submit/present/retirement self-drain; guest wait and TLS isolation");
}
