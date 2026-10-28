/* MIT. Real scheduler + real recursive recording mutex, without a GPU/guest image. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../runtime/threads.c"
#include "../../android/gfx/vk_int.h"

_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); abort(); }
void *runtime_symbol(const char *s) { (void)s; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
static void need(int ok, const char *s) { if (!ok) runtime_fail(s); }
static void init(thr_t *t, int prio, uint64_t mask) {
    memset(t, 0, sizeof *t); t->prio = prio; t->mask = mask; t->core = 0;
    t->cur_core = -1; t->started = 1; pthread_cond_init(&t->cv, NULL);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); pthread_mutex_unlock(&G);
}
static void fini(thr_t *t) {
    need(t->cur_core < 0 && !t->cqueued && !t->wkind && !t->host_resume, "drained thread");
    pthread_mutex_lock(&G); g_h[t->handle - 0x1000].type = O_FREE; pthread_mutex_unlock(&G);
    pthread_cond_destroy(&t->cv);
}
static void pause_poll(uint64_t start) {
    need(now_ns() - start < 5000000000ull, "fixture deadline"); sched_yield();
}
static void await_atomic(_Atomic int *p, int value) {
    uint64_t start = now_ns(); while (atomic_load(p) != value) pause_poll(start);
}
static void await_queued(thr_t *t) {
    uint64_t start = now_ns();
    for (;;) {
        pthread_mutex_lock(&G); int yes = t->cqueued; pthread_mutex_unlock(&G);
        if (yes) return; pause_poll(start);
    }
}
static _Atomic int rec_ready, rec_release;
static void *recorder(void *p) {
    (void)p; vk_rec_lock(); atomic_store(&rec_ready, 1); await_atomic(&rec_release, 1); vk_rec_unlock(); return NULL;
}
static void owned(thr_t *t) {
    need(t->cur_core >= 0 && core_allowed(t, t->cur_core) && g_core_owner[t->cur_core] == t, "exclusive eligible core");
}
typedef struct { thr_t t; _Atomic int entered, finish; int original, exiting; uint32_t *guest_lock; } caller;
static void *texel(void *p) {
    caller *c = p; pthread_setspecific(g_keythr, &c->t);
    pthread_mutex_lock(&G); core_take(&c->t); c->original = c->t.cur_core; pthread_mutex_unlock(&G);
    runtime_host_wait wait; vk_rec_lock_texels(&wait);
    atomic_store(&c->entered, 1);
    vk_rec_unlock_texels(&wait);
    pthread_mutex_lock(&G); owned(&c->t);
    need(c->t.cur_core == c->original, "opaque HLE call retains original core");
    if (c->guest_lock) lock_handoff((uintptr_t)c->guest_lock);
    core_put(&c->t); pthread_mutex_unlock(&G);
    if (c->exiting) thread_exit_on_host(&c->t);
    atomic_store(&c->finish, 1); return NULL;
}
static void handoff(void) {
    caller a = {0}; thr_t b, h1, h2; init(&a.t, 40, 7); init(&b, 40, 1); init(&h1, 40, 2); init(&h2, 40, 4);
    pthread_mutex_lock(&G); core_take(&h1); core_take(&h2); pthread_mutex_unlock(&G);
    pthread_t rec, pt; atomic_store(&rec_ready, 0); atomic_store(&rec_release, 0);
    need(!pthread_create(&rec, NULL, recorder, NULL), "recorder create"); await_atomic(&rec_ready, 1);
    need(!pthread_create(&pt, NULL, texel, &a), "texel create");
    uint64_t start = now_ns();
    for (;;) {
        pthread_mutex_lock(&G); int away = a.t.host_resume != 0; pthread_mutex_unlock(&G);
        if (away) break; pause_poll(start);
    }
    pthread_setspecific(g_keythr, &b);
    pthread_mutex_lock(&G); core_take(&b); owned(&b);
    need(b.cur_core == 0 && a.t.cur_core == -1 && a.t.mask == 7, "another guest runs on freed core while host lock blocked");
    core_put(&h2); pthread_mutex_unlock(&G); /* Another core is free: A must still resume on core 0. */
    atomic_store(&rec_release, 1); await_atomic(&a.entered, 1); await_queued(&a.t);
    /* This would deadlock if A waited for a virtual core while retaining recording ownership. */
    runtime_host_wait nested; vk_rec_lock_texels(&nested); need(!nested.active, "uncontended fast path has no scheduler work");
    runtime_host_wait recursive; vk_rec_lock_texels(&recursive); need(!recursive.active, "recursive fast path retains core");
    vk_rec_unlock_texels(&recursive); vk_rec_unlock_texels(&nested);
    pthread_mutex_lock(&G); need(a.t.cur_core < 0, "no migration into free core during host return"); core_put(&b); core_put(&h1); pthread_mutex_unlock(&G);
    pthread_join(pt, NULL); pthread_join(rec, NULL); need(atomic_load(&a.finish), "host operation completes");
    pthread_setspecific(g_keythr, NULL); fini(&a.t); fini(&b); fini(&h1); fini(&h2);
}
static void *returner(void *p) { runtime_host_wait_end(p); return NULL; }
typedef struct { thr_t *t; runtime_host_wait *w; } return_arg;
static void *return_guest(void *p) {
    return_arg *a = p; pthread_setspecific(g_keythr, a->t); return returner(a->w);
}
static void ordering(int inheritance) {
    thr_t a, b, c, owner, waiter; init(&a, 45, 1); init(&b, 40, 1); init(&c, 40, 1); init(&owner, 50, 1); init(&waiter, 30, 1);
    pthread_setspecific(g_keythr, &a); pthread_mutex_lock(&G); core_take(&a); pthread_mutex_unlock(&G);
    runtime_host_wait w; runtime_host_wait_begin(&w); runtime_host_wait_acquired(&w);
    pthread_mutex_lock(&G); core_take(&owner); tk_core_queue(&b); tk_core_queue(&c);
    uint32_t word = a.handle | WAITERS_BIT;
    if (inheritance) { g_tk_async = 1; waiter.wkind = 1; waiter.waddr = (uintptr_t)&word; g_wait = &waiter; }
    else a.prio = 40;
    pthread_mutex_unlock(&G);
    return_arg arg = {&a, &w}; pthread_t pt; pthread_create(&pt, NULL, return_guest, &arg); await_queued(&a);
    pthread_mutex_lock(&G); core_put(&owner);
    if (inheritance) {
        need(a.cur_core == 0 && b.cqueued, "reacquire honors inherited priority");
        pthread_mutex_unlock(&G); pthread_join(pt, NULL); pthread_mutex_lock(&G); core_put(&a);
    }
    else need(b.cur_core == 0 && a.cqueued, "earlier equal-priority waiter beats reacquire");
    need(b.cur_core == 0 && c.cqueued, "first FIFO waiter"); core_put(&b);
    need(c.cur_core == 0, "second FIFO waiter"); core_put(&c);
    if (inheritance) { g_wait = NULL; waiter.wkind = 0; g_tk_async = 0; }
    pthread_mutex_unlock(&G); if (!inheritance) pthread_join(pt, NULL);
    pthread_mutex_lock(&G); if (!inheritance) { owned(&a); core_put(&a); } pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, NULL); fini(&a); fini(&b); fini(&c); fini(&owner); fini(&waiter);
}
static void cancellation(void) {
    thr_t t; init(&t, 40, 1); pthread_setspecific(g_keythr, &t);
    pthread_mutex_lock(&G); core_take(&t); pthread_mutex_unlock(&G);
    runtime_host_wait w; runtime_host_wait_begin(&w);
    uint64_t r[31] = {t.handle}; need(svc_thread(0x19, r) && !r[0], "cancel during host block");
    need(t.cancel && !t.wkind && !t.cqueued, "host block does not become guest object wait");
    runtime_host_wait_acquired(&w); runtime_host_wait_end(&w);
    memset(r, 0, sizeof r); r[3] = (uint64_t)-1;
    need(svc_thread(0x18, r) && r[0] == R_CANCELLED && !t.cancel, "pending cancellation consumed by next synchronization wait");
    pthread_mutex_lock(&G); need(block(&t, 3, 100000) == R_TIMEOUT, "timeout after host resume"); owned(&t); core_put(&t); pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, NULL); fini(&t);
}
typedef struct { thr_t t; unsigned rounds; } stress_arg;
static unsigned completed;
static void *stress(void *p) {
    stress_arg *a = p; pthread_setspecific(g_keythr, &a->t);
    for (unsigned i = 0; i < a->rounds; i++) {
        pthread_mutex_lock(&G); core_take(&a->t); owned(&a->t); int original = a->t.cur_core; pthread_mutex_unlock(&G);
        runtime_host_wait w; vk_rec_lock_texels(&w);
        completed++;
        vk_rec_unlock_texels(&w);
        pthread_mutex_lock(&G); owned(&a->t); need(a->t.cur_core == original, "stress same-core resume"); core_put(&a->t); pthread_mutex_unlock(&G);
    }
    return NULL;
}
static void controls(void) {
    runtime_host_wait w; runtime_host_wait_begin(&w); need(!w.active, "host-only thread is ignored");
    thr_t t; init(&t, 40, 7); pthread_setspecific(g_keythr, &t);
    runtime_host_wait_begin(&w); need(!w.active, "already-away caller is ignored");
    pthread_mutex_lock(&G); core_take(&t); pthread_mutex_unlock(&G);
    int enabled = g_vcore_release; g_vcore_release = 0;
    runtime_host_wait_begin(&w); need(!w.released && t.cur_core == 0, "startup-off restores old occupancy");
    runtime_host_wait_acquired(&w); runtime_host_wait_end(&w); g_vcore_release = enabled;
    pthread_mutex_lock(&G); core_put(&t); pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, NULL); fini(&t);
}
static void mask_change(void) {
    thr_t t; init(&t, 40, 7); pthread_setspecific(g_keythr, &t);
    pthread_mutex_lock(&G); core_take(&t); pthread_mutex_unlock(&G);
    runtime_host_wait w; runtime_host_wait_begin(&w);
    uint64_t r[31] = {t.handle, 1, 2};
    need(svc_thread(0x0f, r) && !r[0], "explicit mask change while away");
    runtime_host_wait_acquired(&w); runtime_host_wait_end(&w);
    pthread_mutex_lock(&G); owned(&t);
    need(t.cur_core == 1 && t.mask == 2 && !t.host_resume, "resume respects explicit mask change");
    core_put(&t); pthread_mutex_unlock(&G); pthread_setspecific(g_keythr, NULL); fini(&t);
}
static void await_released(thr_t *t) {
    uint64_t start = now_ns();
    for (;;) {
        pthread_mutex_lock(&G); int released = t->host_resume != 0; pthread_mutex_unlock(&G);
        if (released) return; pause_poll(start);
    }
}
static void shutdown_while_released(void) {
    /* Android close/destroy uses _exit, not an asynchronous pthread cancellation
     * or a concurrent destructor of renderer/scheduler state. Test that exact
     * process-termination model while the real recording mutex is blocked. */
    pid_t pid = fork(); need(pid >= 0, "shutdown fork");
    if (!pid) {
        caller a = {0}; init(&a.t, 40, 1);
        atomic_store(&rec_ready, 0); atomic_store(&rec_release, 0);
        pthread_t rec, pt; pthread_create(&rec, NULL, recorder, NULL); await_atomic(&rec_ready, 1);
        pthread_create(&pt, NULL, texel, &a); await_released(&a.t);
        _exit(0);
    }
    int status; need(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status),
        "process shutdown never waits for released guest/recording lock");
}
static void closed_handle_while_released(void) {
    caller *a = calloc(1, sizeof *a); need(a != NULL, "teardown caller allocation");
    init(&a->t, 40, 1); a->exiting = 1; uint32_t handle = a->t.handle;
    atomic_store(&rec_ready, 0); atomic_store(&rec_release, 0);
    pthread_t rec, pt; pthread_create(&rec, NULL, recorder, NULL); await_atomic(&rec_ready, 1);
    pthread_create(&pt, NULL, texel, a); await_released(&a->t);
    thr_t b; init(&b, 40, 1); pthread_setspecific(g_keythr, &b);
    pthread_mutex_lock(&G); core_take(&b); owned(&b); pthread_mutex_unlock(&G);
    uint64_t r[31] = {handle}; need(svc_thread(0x16, r) && !r[0], "close live released thread handle");
    pthread_mutex_lock(&G);
    need(a->t.closed && !a->t.exited && !h_peek(handle, O_THREAD), "handle closes but live released thread remains allocated");
    pthread_mutex_unlock(&G);
    atomic_store(&rec_release, 1); await_atomic(&a->entered, 1); await_queued(&a->t);
    pthread_mutex_lock(&G); core_put(&b); pthread_mutex_unlock(&G);
    pthread_join(pt, NULL); pthread_join(rec, NULL); /* Production ExitThread frees closed thr_t, after core resume. */
    pthread_setspecific(g_keythr, NULL); fini(&b);
}
typedef struct { thr_t t; uint32_t *word, owner; } lock_waiter;
static void *guest_mutex_waiter(void *p) {
    lock_waiter *b = p; pthread_setspecific(g_keythr, &b->t);
    pthread_mutex_lock(&G); core_take(&b->t); owned(&b->t); pthread_mutex_unlock(&G);
    uint64_t r[31] = {b->owner, (uintptr_t)b->word, b->t.handle};
    need(svc_thread(0x1a, r) && !r[0], "borrower waits for guest lock held by released caller");
    pthread_mutex_lock(&G); owned(&b->t); lock_handoff((uintptr_t)b->word); core_put(&b->t); pthread_mutex_unlock(&G);
    return NULL;
}
static void held_guest_mutex(void) {
    caller a = {0}; init(&a.t, 40, 1); uint32_t word = a.t.handle | WAITERS_BIT; a.guest_lock = &word;
    lock_waiter b = {.word = &word, .owner = a.t.handle}; init(&b.t, 30, 1);
    atomic_store(&rec_ready, 0); atomic_store(&rec_release, 0);
    pthread_t rec, pt, waiter; pthread_create(&rec, NULL, recorder, NULL); await_atomic(&rec_ready, 1);
    pthread_create(&pt, NULL, texel, &a); await_released(&a.t);
    pthread_create(&waiter, NULL, guest_mutex_waiter, &b);
    uint64_t start = now_ns();
    for (;;) {
        pthread_mutex_lock(&G); int waiting = b.t.wkind == 1; pthread_mutex_unlock(&G);
        if (waiting) break; pause_poll(start);
    }
    atomic_store(&rec_release, 1);
    pthread_join(pt, NULL); pthread_join(waiter, NULL); pthread_join(rec, NULL);
    need(!word, "guest mutex and borrowed core make progress without a lock cycle");
    fini(&a.t); fini(&b.t);
}
int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1000;
    setenv("TOTK_ASYNC_PREEMPT", "0", 1); setenv("TOTK_STARVE", "0", 1);
    g_cores_on = 1; g_svc_profile = 0; g_ready_cores = 0; g_tk_async = 0;
    pthread_key_create(&g_keythr, NULL); vcore_config();
    int enabled = g_vcore_release; controls();
    if (enabled) {
        handoff(); ordering(0); ordering(1); cancellation(); mask_change();
        held_guest_mutex(); closed_handle_while_released(); shutdown_while_released();
    }
    stress_arg a[24]; pthread_t pt[24];
    for (unsigned i = 0; i < 24; i++) { init(&a[i].t, 40 + i % 4, i % 4 ? 1ull << (i % 3) : 7); a[i].rounds = rounds; pthread_create(&pt[i], NULL, stress, &a[i]); }
    for (unsigned i = 0; i < 24; i++) { pthread_join(pt[i], NULL); fini(&a[i].t); }
    need(completed == 24 * rounds, "all stress host jobs complete");
    need(!g_core_wait && !g_wait && !g_core_owner[0] && !g_core_owner[1] && !g_core_owner[2], "all three cores drained");
    if (g_vcore_stats && enabled) need(g_vcore_counts.releases && g_vcore_counts.blocked_ns && g_vcore_counts.reacquire_ns, "released wait counters populated");
    if (!g_vcore_stats) need(!g_vcore_counts.releases && !g_vcore_counts.contended, "disabled statistics stay empty");
    vcore_stats_frame(300); need(!g_vcore_counts.releases && !g_vcore_counts.contended, "300-present window resets");
    printf("PASS vcore-release enabled=%d stats=%d threads=24 cores=3 jobs=%u deterministic=%d\n", enabled, g_vcore_stats, completed, enabled ? 9 : 1);
    return 0;
}
