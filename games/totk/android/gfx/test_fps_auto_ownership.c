/* Run with test_fps_auto_ownership.py --scratch <output directory>.
 * Production acquire/runtime/wait code is included verbatim by the runner.
 * A condition-variable handshake holds the old presenter until the real
 * same-texture acquire enters x_wait_job; timestamps never depend on sleeps. */
#define _POSIX_C_SOURCE 200809L
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include "fps_auto_runtime.h"
#include "fps_auto.h"
#include "fps_auto_scene.h"
#include "fps_hybrid_runtime.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t ms = 1000000, refresh = 16666667;
static uint64_t clock_ns, blocked_ns;
static pthread_mutex_t display_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t display_period_ns = 16666667;
static uint64_t runtime_now_ns(void) { return clock_ns; }
uint64_t runtime_android_fps_auto_blocked_ns(void) { return blocked_ns; }
static uint32_t runtime_thread_current_handle(void) { return 0x1001; }
static int runtime_fs_save_busy(void) { return 0; }
static void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
#include "fps_auto_frontend.inc"

typedef struct {
    int active, next, ntex, interval;
    void *tex[4];
    uint64_t avail[4], ticket[4];
} win_t;
typedef win_t NVNwindow;
typedef struct { int fenced; uint64_t fence; } sync_t;
typedef sync_t NVNsync;
typedef int NVNwindowAcquireTextureResult;
#define NVN(name, result, ...) static result h_##name(__VA_ARGS__)
#define NVN_BLOCKING() ((void)0)
enum { NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, TL_ACQ_IN, TL_ACQ_OUT,
    FF_SIGNAL_ACQUIRE, ACQUIRE_SLACK_OFF, ACQUIRE_SLACK_PREDICT };

static uint64_t g_frame = 1, g_acq_wait_ns, g_pq_wait_ns;
static unsigned g_fps_auto_ticks;
static win_t *g_xp_win;
static uint64_t g_xp_job[4], g_xq_done;
static pthread_mutex_t g_xq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xq_cv = PTHREAD_COND_INITIALIZER;
static unsigned replay_wait_entered;
static int pacing_mode, replay_on;
static uint64_t pacing_delay, replay_delay, resource_delay;
static uint64_t measured_pacing, measured_resource;
static uintptr_t old_texture;
static uint64_t old_id, old_present_ns, old_ready_ns;
static runtime_fps_auto_frame old_frame;
static int old_idx;

/* Instrument the blocking primitive, not the production wait condition or
 * acquire ordering. The old job cannot run before the acquire is waiting. */
static int fixture_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu) {
    assert(cv == &g_xq_cv && mu == &g_xq_mu);
    replay_wait_entered++;
    assert(!pthread_cond_broadcast(&g_xq_cv));
    return pthread_cond_wait(cv, mu);
}
#define pthread_cond_wait fixture_cond_wait
#include "fps_auto_x_wait.inc"
#undef pthread_cond_wait

static inline uint64_t now_ns(void) { return clock_ns; }
static uint64_t g_game_controller, g_game_sys;
static int runtime_frame_diag_pre14;
static uint64_t runtime_android_display_period(unsigned ticks) { return refresh*ticks; }
#include "fps_hybrid_acquire.inc"
static inline uint64_t guest_ns(void) { return clock_ns; }
static inline int totk_ft_on(void) { return 1; }
static inline uint64_t totk_ft_now(void) { return clock_ns; }
static inline uint64_t totk_ft_current(void) { return g_frame; }
static inline void totk_tl(int event, uint64_t a, uint64_t b) { (void)event; (void)a; (void)b; }
static inline void totk_ft_acquire_wait(uint64_t frame, uint64_t pacing, uint64_t resource) {
    assert(frame == g_frame); measured_pacing = pacing; measured_resource = resource;
}
static inline void totk_ft_acquire(uint64_t frame, uint64_t begin, uint64_t end) {
    assert(frame == g_frame && end >= begin);
}
static inline void totk_ff_signal(uintptr_t sync, uint64_t frame, uint64_t value,
        uint64_t now, uint32_t thread, unsigned kind, unsigned producer, unsigned flags) {
    (void)sync; (void)frame; (void)value; (void)now; (void)thread;
    (void)kind; (void)producer; (void)flags;
}
static inline int dm_detclock_enabled(void) { return 0; }
static inline uint64_t present_period(const win_t *o) { (void)o; return refresh * g_fps_auto_ticks; }
static inline unsigned present_ticks(const win_t *o) { (void)o; return g_fps_auto_ticks; }
static inline int acquire_clock_on(void) { return pacing_mode == 0; }
static inline void set_time_constraint(uint64_t period) { assert(period); }
static inline void rt_comp_tick(uint64_t period) { assert(period); }
static inline void game_controller_tick(uint64_t frame, int decision) {
    assert(frame == g_frame + 1 && !decision);
}
static inline void runtime_android_acquire_wait(unsigned interval) { assert(interval == 1); clock_ns += pacing_delay; }
static inline int acquire_slack_mode(void) { return pacing_mode == 2 ? ACQUIRE_SLACK_PREDICT : ACQUIRE_SLACK_OFF; }
static inline unsigned acquire_slack_lead(void) { return 0; }
static inline int runtime_android_predict_acquire(uintptr_t texture, unsigned interval) {
    assert(texture == old_texture && interval == 1); clock_ns += pacing_delay; return 1;
}
static inline int runtime_android_acquire_slack_wait(unsigned interval, unsigned lead) {
    (void)interval; (void)lead; abort();
}
static inline int mtl_link_active(void) { return 0; }
static inline int uncapped_now(void) { return 0; }
static inline int mfx_own_clock(void) { return 0; }
static inline int mtl_link_wait_tick(uint64_t timeout, unsigned interval) {
    (void)timeout; (void)interval; abort();
}
static inline void sleep_until_ns(uint64_t deadline) { assert(deadline >= clock_ns); clock_ns = deadline; }
static inline void trow(uint64_t frame, const char *key, double value) { (void)frame; (void)key; (void)value; }
static inline int async_replay(void) { return replay_on; }
static inline void mtl_present_wait(uint64_t ticket) { assert(ticket == 123); clock_ns += resource_delay; }
static inline uint64_t fence_now(int *token) { *token = 0; return 0; }
#include "fps_auto_acquire.inc"

static runtime_fps_auto_frame *pending(uintptr_t texture) {
    for (unsigned i = 0; i < 16; i++)
        if (fps_auto.pending[i].texture == texture) return &fps_auto.pending[i].frame;
    assert(0 && "expected production pending observation");
    return NULL;
}

static void *old_presenter(void *unused) {
    (void)unused;
    assert(!pthread_mutex_lock(&g_xq_mu));
    while (!replay_wait_entered) assert(!pthread_cond_wait(&g_xq_cv, &g_xq_mu));
    assert(!pthread_mutex_unlock(&g_xq_mu));
    /* This is the actual frontend consumer used by the backend's old queued
     * XJ_PRESENT. An early begin would replace it with a new, unpresented ID. */
    runtime_android_fps_auto_take(old_texture, &old_frame);
    assert(old_frame.id == old_id && old_frame.present_ns == old_present_ns);
    clock_ns += replay_delay;
    old_ready_ns = clock_ns;
    g_xp_win->ticket[old_idx] = 123;
    assert(!pthread_mutex_lock(&g_xq_mu));
    g_xq_done = g_xp_job[old_idx];
    assert(!pthread_cond_broadcast(&g_xq_cv));
    assert(!pthread_mutex_unlock(&g_xq_mu));
    return NULL;
}

static void ownership_case(int mode, int idx, int asynchronous) {
    int textures[2];
    win_t window = {.active = 2, .next = idx + 2, .ntex = 2, .interval = 1,
        .tex = {&textures[0], &textures[1]}};
    uintptr_t texture = (uintptr_t)window.tex[idx];
    const uint64_t start = 1000000000, preparation = ms, allowance = ms;
    const uint64_t pre_acquire = 10 * ms, work = 3 * ms, kernel_wait = 2 * ms, tail = 2 * ms;
    pacing_mode = mode; replay_on = asynchronous;
    pacing_delay = 8 * ms; replay_delay = asynchronous ? 7 * ms : 0; resource_delay = 5 * ms;
    clock_ns = start - 20 * ms; blocked_ns = 4 * ms;
    memset(&fps_auto, 0, sizeof fps_auto);
    atomic_store(&fps_auto_on, 1);
    fps_auto_reset_locked(clock_ns, refresh);
    fps_auto.guest_interval = 1;
    fps_auto.scene.world_seen = 1;
    /* Anchor the real presenter owner/counter stream on the other image. */
    uintptr_t anchor_texture = (uintptr_t)window.tex[1 - idx];
    runtime_android_fps_auto_begin(anchor_texture, 1, clock_ns, 0);
    clock_ns = start - 10 * ms;
    runtime_android_fps_auto_present(anchor_texture, clock_ns);
    runtime_fps_auto_frame anchor;
    runtime_android_fps_auto_take(anchor_texture, &anchor);
    assert(anchor.id == 1);
    clock_ns = start - 5 * ms;
    runtime_android_fps_auto_begin(texture, 1, clock_ns, 0);
    pending(texture)->scene_eligible = 1;
    clock_ns = start;
    runtime_android_fps_auto_present(texture, clock_ns);
    old_texture = texture; old_idx = idx;
    old_id = pending(texture)->id; old_present_ns = clock_ns;
    assert(old_id == 2 && pending(texture)->blocked_valid);
    assert(pending(texture)->game_interval_ns == 10 * ms);
    old_frame = (runtime_fps_auto_frame){0};
    old_ready_ns = 0; replay_wait_entered = 0;
    g_xp_win = &window;
    memset(g_xp_job, 0, sizeof g_xp_job);
    g_xp_job[idx] = 7; g_xq_done = 6;
    g_acq_wait_ns = g_pq_wait_ns = measured_pacing = measured_resource = 0;
    pthread_t presenter;
    if (asynchronous) assert(!pthread_create(&presenter, NULL, old_presenter, NULL));
    else {
        /* Synchronous present has already taken its immutable observation. */
        runtime_android_fps_auto_take(texture, &old_frame);
        old_ready_ns = clock_ns + 2 * ms;
        window.ticket[idx] = 123;
    }
    clock_ns = start + pre_acquire;
    window.avail[idx] = clock_ns + pacing_delay;
    uint64_t release = clock_ns + pacing_delay;
    NVNsync sync = {0};
    int acquired = -1;
    assert(h_nvnWindowAcquireTexture(&window, &sync, &acquired) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
    if (asynchronous) assert(!pthread_join(presenter, NULL));
    assert(acquired == idx && window.next == idx + 3 && sync.fenced == 3);
    assert(!!replay_wait_entered == asynchronous);
    assert(old_frame.id == old_id && old_frame.present_ns == old_present_ns);
    assert(old_frame.release_ns == start - 5 * ms && old_frame.pacing_ns == 0);
    runtime_fps_auto_frame *new_pending = pending(texture);
    assert(new_pending->id == 3 && !new_pending->present_ns);
    assert(new_pending->release_ns == release && new_pending->pacing_ns == pacing_delay);
    assert(new_pending->owner == old_frame.owner && new_pending->generation == old_frame.generation);
    assert(new_pending->blocked_at_release_ns == blocked_ns);
    assert(clock_ns == release + replay_delay + resource_delay);
    assert(measured_pacing == pacing_delay && measured_resource == replay_delay + resource_delay);
    assert(g_pq_wait_ns == resource_delay);

    /* Old feedback is intentionally delivered AFTER the replacement exists.
     * It must train only its captured old ID/timestamp/span, not the new slot. */
    uint64_t old_span = runtime_fps_auto_ready_span(&old_frame, old_ready_ns, preparation, allowance);
    assert(old_span == old_frame.game_interval_ns + old_ready_ns - old_present_ns + preparation + allowance);
    runtime_android_fps_auto_feedback(&old_frame, old_ready_ns, preparation, allowance);
    assert(fps_auto.trace.n == 1 && fps_auto.trace.span_sum == old_span);
    assert(fps_auto.trace.cpu_sum == 10 * ms && fps_auto.trace.tail_sum == old_ready_ns - old_present_ns);
    assert(pending(texture)->id == 3 && !pending(texture)->present_ns);

    new_pending->scene_eligible = 1;
    clock_ns += work + kernel_wait;
    blocked_ns += kernel_wait;
    uint64_t new_present_ns = clock_ns;
    runtime_android_fps_auto_present(texture, new_present_ns);
    runtime_fps_auto_frame new_frame;
    runtime_android_fps_auto_take(texture, &new_frame);
    assert(new_frame.id == old_id + 1 && new_frame.present_ns == new_present_ns);
    assert(new_frame.release_ns == release && new_frame.pacing_ns == pacing_delay);
    assert(new_frame.blocked_valid && new_frame.blocked_ns == kernel_wait);
    assert(new_frame.post_gate_blocked_ns == kernel_wait);
    assert(new_frame.game_interval_ns == pre_acquire + pacing_delay + replay_delay + resource_delay + work + kernel_wait);
    uint64_t busy = pre_acquire + replay_delay + resource_delay + work;
    assert(runtime_fps_auto_busy_span(&new_frame) == busy);
    uint64_t new_ready_ns = new_present_ns + tail;
    uint64_t new_span = runtime_fps_auto_ready_span(&new_frame, new_ready_ns, preparation, allowance);
    assert(new_span == busy + tail + preparation + allowance);
    runtime_android_fps_auto_feedback(&new_frame, new_ready_ns, preparation, allowance);
    assert(fps_auto.trace.n == 2 && fps_auto.trace.span_sum == old_span + new_span);
    assert(fps_auto.trace.cpu_sum == 10 * ms + busy);
    assert(fps_auto.trace.blocked_sum == kernel_wait);
    assert(fps_auto.trace.tail_sum == old_ready_ns - old_present_ns + tail);
    assert(old_frame.id == old_id && old_frame.present_ns == old_present_ns);
    runtime_fps_auto_frame empty;
    runtime_android_fps_auto_take(texture, &empty);
    assert(!empty.id && !empty.present_ns);
}

static void hybrid_v143_case(void) {
    /* Odin's live min/max remains2/3 even when current/candidate is1/1.
     * Use the actual NVN adapter, not an already-decoded valid=true input. */
    uint64_t sys[0x300 / 8] = {0};
    uint32_t *controller = (uint32_t *)((uint8_t *)sys + 0x200);
    controller[0] = 2; controller[1] = 3;
    float bias[2] = {.05f, .02f};
    memcpy(controller + 2, bias, sizeof bias);
    sys[0x120 / 8] = 400000; sys[0x128 / 8] = (uint64_t)-100000;
    g_game_sys = (uintptr_t)sys; g_game_controller = (uintptr_t)controller;
    setenv("TOTK_FRAMEGEN", "hybrid", 1); setenv("TOTK_FPS60", "1", 1);
    setenv("TOTK_FPS_AUTO", "0", 1); setenv("TOTK_ACQUIRE_SLACK", "0", 1);
    clock_ns = 1000000000ull;
    for (unsigned i = 0; i < 1700; i++) {
        clock_ns += 20000000; /* sustained50fps, including10s scene settling */
        unsigned ticks = hybrid_acquire(1);
        assert(ticks == (runtime_fps_hybrid_interpolating() ? 2u : 1u));
        runtime_fps_hybrid_present(clock_ns + 1, 4000, 0);
    }
    assert(runtime_fps_hybrid_interpolating());
    /* Frame-lag GPU end minus a newer CPU origin can be negative. It must
     * not starve raw-rate drop evidence or falsely prove recovery. */
    for (unsigned i = 0; i < 600; i++) {
        clock_ns += 33333333;
        assert(hybrid_acquire(1) == 2);
        runtime_fps_hybrid_present(clock_ns + 1, 4000, 0);
    }
    sys[0x120 / 8] = sys[0x128 / 8] = 100000;
    for (unsigned i = 0; i < 600; i++) {
        clock_ns += runtime_fps_hybrid_interpolating() ? 33333333 : refresh;
        hybrid_acquire(1);
        runtime_fps_hybrid_present(clock_ns + 1, 4000, 0);
    }
    assert(!runtime_fps_hybrid_interpolating());
    puts("hybrid1.4.3 bounds2/3 at50fps: drop despite unavailable GPU work; no false recovery; valid headroom recovers");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--hybrid")) {
        hybrid_v143_case();
        return 0;
    }
    for (int mode = 0; mode < 3; mode++) {
        ownership_case(mode, 0, 1);
        ownership_case(mode, 1, 1);
        ownership_case(mode, 0, 0);
    }
    puts("FPSauto ownership: held async replay, same-texture wrap, immutable old/new IDs and present timestamps, feedback ownership, pacing exclusion and replay/resource delay attribution passed");
    return 0;
}
