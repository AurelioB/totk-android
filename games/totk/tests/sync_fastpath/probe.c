/* MIT. CPU-only fixture using the production predicates, G, queues and parks.
 * SYNC_LEGACY builds the same benchmark against the pinned baseline runtime. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#ifndef SYNC_RUNTIME
#define SYNC_RUNTIME "../../runtime/threads.c"
#endif
#include SYNC_RUNTIME

uint64_t runtime_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); abort(); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
static void need(int ok, const char *s) { if (!ok) runtime_fail(s); }
static void init(thr_t *t, int priority, uint64_t mask) {
    memset(t, 0, sizeof *t); t->prio = priority; t->mask = mask;
    t->core = 0; t->cur_core = -1; t->started = 1;
    need(!pthread_cond_init(&t->cv, NULL), "condition init");
    t->handle = h_new(O_THREAD, t);
}
static void destroy(thr_t *t) {
    need(t->cur_core < 0 && !t->wkind && !t->cqueued, "no stranded ownership or queue");
    g_h[t->handle - 0x1000].type = O_FREE;
    pthread_cond_destroy(&t->cv);
}
static void await_kind(thr_t *t, int kind) {
    uint64_t start = now_ns();
    for (;;) {
        pthread_mutex_lock(&G); int ok = t->wkind == kind; pthread_mutex_unlock(&G);
        if (ok) return;
        need(now_ns() - start < 3000000000ull, "wait enrollment deadline"); sched_yield();
    }
}
#ifndef SYNC_LEGACY
static void deterministic(void) {
    thr_t t, owner, a, b; init(&t, 30, 1); init(&owner, 48, 1); init(&a, 30, 1); init(&b, 30, 1);
    pthread_mutex_lock(&G);
    uint64_t issued = g_sync_counts.wake_issued, parks = g_sync_counts.block_parks;
    t.wkind = 3; g_wait = &t; wake(&t, 17);
    need(t.woke && t.wres == 17 && !g_wait, "predicate published without host sleeper");
    need(g_sync_counts.wake_issued == issued + 1, "signal-before-park uses unconditional host notification");
    t.woke = 0; sync_signal(&t); sync_signal(&t);
    need(g_sync_counts.wake_issued == issued + 3, "duplicate host notifications remain unconditional");
    need(block_inner(&t, 3, 0) == R_TIMEOUT && !t.wkind && !g_wait && g_sync_counts.block_parks == parks, "immediate timeout never parks");
    need(block_inner(&t, 3, 100000) == R_TIMEOUT && !t.wkind && g_sync_counts.block_parks == parks + 1, "timed park accounting without sleeper bookkeeping");
    issued = g_sync_counts.wake_issued; g_ready_cores = 1;
    t.wkind = 3; g_wait = &t; wake(&t, 0);
    need(t.cur_core == 0 && g_sync_counts.wake_issued == issued + 2, "object-ready plus immediate core grant retain both host notifications");
    core_put(&t); g_ready_cores = 0;
    core_take(&owner); tk_core_queue(&a); tk_core_queue(&b); core_put(&owner);
    need(a.cur_core == 0 && b.cqueued, "equal-priority core FIFO"); core_put(&a); core_put(&b);
    uint32_t mutex_word = owner.handle | WAITERS_BIT;
    g_tk_async = 1; a.wkind = 1; a.waddr = (uintptr_t)&mutex_word; g_wait = &a;
    need(tk_prio(&owner) == a.prio, "mutex owner inherits effective priority");
    a.wkind = 0; g_wait = NULL; g_tk_async = 0;
    pthread_mutex_unlock(&G);
    destroy(&t); destroy(&owner); destroy(&a); destroy(&b);
}
#endif

typedef struct { thr_t t; unsigned rounds, done; int64_t timeout, result; int cancel; _Atomic int phase; } job_t;
static void *single_wait(void *arg) {
    job_t *j = arg; pthread_setspecific(g_keythr, &j->t);
    pthread_mutex_lock(&G); core_take(&j->t); pthread_mutex_unlock(&G);
    if (j->cancel) {
        uint64_t regs[31] = {0}; regs[3] = (uint64_t)-1;
        need(svc_thread(0x18, regs), "dispatch synchronization wait"); j->result = (int64_t)regs[0];
        pthread_mutex_lock(&G);
    } else {
        pthread_mutex_lock(&G); atomic_store(&j->phase, 1); j->result = block(&j->t, 3, j->timeout);
    }
    core_put(&j->t); pthread_mutex_unlock(&G); return NULL;
}
static void cancellation_and_timeout(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        job_t j = {.timeout = 200000, .cancel = (int)(i & 1)}; init(&j.t, 30, 7);
        pthread_t pt; need(!pthread_create(&pt, NULL, single_wait, &j), "wait thread");
        if (j.cancel) {
            await_kind(&j.t, 4); uint64_t regs[31] = {j.t.handle};
            need(svc_thread(0x19, regs) && regs[0] == 0, "cancel synchronization SVC");
        }
        need(!pthread_join(pt, NULL), "wait join");
        need(j.result == (j.cancel ? R_CANCELLED : R_TIMEOUT), "timeout/cancellation result"); destroy(&j.t);
    }
}
static void timeout_races(unsigned count) {
    unsigned woken = 0, expired = 0;
    for (unsigned i = 0; i < count; i++) {
        job_t j = {.timeout = 200000}; init(&j.t, 30, 7);
        pthread_t pt; need(!pthread_create(&pt, NULL, single_wait, &j), "timeout-race thread");
        while (!atomic_load(&j.phase)) sched_yield();
        if (i & 1) { struct timespec delay = {0, 200000}; nanosleep(&delay, NULL); }
        pthread_mutex_lock(&G); if (j.t.wkind == 3) wake(&j.t, 0); pthread_mutex_unlock(&G);
        need(!pthread_join(pt, NULL), "timeout-race join");
        need(j.result == 0 || j.result == R_TIMEOUT, "racing signal/timeout returns a legal result");
        if (j.result) expired++; else woken++; destroy(&j.t);
    }
    printf("RACES attempts=%u woken=%u expired=%u\n", count, woken, expired);
}
static void *worker(void *arg) {
    job_t *j = arg; pthread_setspecific(g_keythr, &j->t);
    pthread_mutex_lock(&G); core_take(&j->t);
    for (unsigned i = 0; i < j->rounds; i++) {
        need(block(&j->t, 3, -1) == 0, "fanout wake result");
        need(j->t.cur_core >= 0 && core_allowed(&j->t, j->t.cur_core) && g_core_owner[j->t.cur_core] == &j->t, "exclusive eligible ownership on resume");
        pthread_mutex_unlock(&G);
        uint64_t work = i;
        for (unsigned k = 0; k < 128; k++) work = work * 6364136223846793005ull + 1;
        __asm__ volatile("" : "+r"(work) :: "memory");
        pthread_mutex_lock(&G);
        j->done++;
    }
    core_put(&j->t); pthread_mutex_unlock(&G); return NULL;
}
static void fanout(unsigned workers, unsigned rounds) {
    job_t jobs[24]; pthread_t pts[24]; need(workers <= 24, "bounded worker fixture");
    for (unsigned n = 0; n < workers; n++) {
        jobs[n] = (job_t){.rounds = rounds}; init(&jobs[n].t, 40 + n % 4, n % 4 ? 1ull << (n % 3) : 7);
        need(!pthread_create(&pts[n], NULL, worker, &jobs[n]), "fanout thread");
    }
    for (unsigned r = 0; r < rounds; r++) {
        for (unsigned n = 0; n < workers; n++) await_kind(&jobs[n].t, 3);
        pthread_mutex_lock(&G);
        for (unsigned n = 0; n < workers; n++) wake(&jobs[n].t, 0);
        pthread_mutex_unlock(&G);
    }
    for (unsigned n = 0; n < workers; n++) { need(!pthread_join(pts[n], NULL), "fanout join"); need(jobs[n].done == rounds, "all jobs completed"); destroy(&jobs[n].t); }
    need(!g_wait && !g_core_wait && !g_core_owner[0] && !g_core_owner[1] && !g_core_owner[2], "all three cores and wait queues drained");
}
static double cpu_seconds(void) {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}
int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 3000;
    setenv("TOTK_ASYNC_PREEMPT", "0", 1); setenv("TOTK_STARVE", "0", 1);
    g_cores_on = 1; g_svc_profile = 0; g_ready_cores = 0; g_tk_async = 0;
    need(!pthread_key_create(&g_keythr, NULL), "scheduler TLS");
#ifndef SYNC_LEGACY
    sync_config();
#ifndef SYNC_BENCH_ONLY
    deterministic();
#endif
#endif
    cancellation_and_timeout(100); timeout_races(500);
    uint64_t start = now_ns(); double cpu = cpu_seconds(); fanout(1, rounds);
    printf("BENCH pingpong jobs=%u wall_ms=%.3f cpu_ms=%.3f\n", rounds, (now_ns() - start) / 1e6, (cpu_seconds() - cpu) * 1000);
    start = now_ns(); cpu = cpu_seconds(); fanout(24, rounds);
    printf("BENCH fanout workers=24 jobs=%u wall_ms=%.3f cpu_ms=%.3f\n", rounds * 24, (now_ns() - start) / 1e6, (cpu_seconds() - cpu) * 1000);
#ifndef SYNC_LEGACY
    printf("COUNTS kick=%d issued=%llu block=%llu core=%llu\n", g_sync_kick,
        (unsigned long long)g_sync_counts.wake_issued,
        (unsigned long long)g_sync_counts.block_parks, (unsigned long long)g_sync_counts.core_parks);
    sync_stats_frame(300);
#endif
    printf("PASS sync-fastpath rounds=%u completed=%u timeout=50 cancellation=50\n", rounds, rounds * 25);
    return 0;
}
