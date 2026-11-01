#include "fps_adaptive_runtime.h"
#ifdef GPU_VK
#include "../android/gfx/fps_auto_scene.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int selected;
/* TOTK_FG_ADAPTIVE_PIPE_SETTLE=1 (exact; default off): a pipeline-creation burst
 * holds adaptive generation for FPS_AUTO_PIPE_WINDOW_NS (1 s, re-extended by
 * each further creation in the burst window) instead of the 10 s settle that
 * fps_auto uses. r7/r11 traces: in-game bursts were shader warm-up during the
 * camera route, with valid simulation steps; a 10 s hold dropped ~10 s of GEN.
 * First-world/loading/low-draw settles and grace holds are unchanged; invalid
 * steps still bump the epoch. Set once in configure(), read under mu. */
static uint64_t pipe_settle_ns = FPS_AUTO_SCENE_SETTLE_NS;
/* The guest acquire/present owner supplies simulation samples. The mutex also
 * admits frontend lifecycle/scene notifications and presenter snapshot reads. */
static struct {
    runtime_fps_adaptive_frame frame;
    fps_auto_scene scene;
    uint64_t epoch, sim_ns, last_step_ns, last_acquire_ns;
    double fraction_ns;
    int clock, sample_valid, scene_ready, window_missing, paused;
} state = {.epoch = 1};
/* Independent observation window: temporal resets must not erase evidence of
 * stalls or invalid captures. This is game cadence, not generated/scanout Hz. */
static struct {
    uint64_t frames,first_ns,last_ns,valid,invalid,bins[6];
    double sum_ms,min_ms,max_ms;
} sim_stats;
static void simulation_stats(uint64_t now,float seconds) {
    if (!sim_stats.frames) sim_stats.first_ns=now;
    sim_stats.last_ns=now; sim_stats.frames++;
    if (isfinite(seconds) && seconds>0) {
        double ms=(double)seconds*1000;
        unsigned bin=ms<8.333333?0:ms<16.666667?1:ms<25?2:ms<33.333333?3:ms<=40?4:5;
        sim_stats.bins[bin]++;
        if (!sim_stats.valid || ms<sim_stats.min_ms) sim_stats.min_ms=ms;
        if (ms>sim_stats.max_ms) sim_stats.max_ms=ms;
        sim_stats.sum_ms+=ms; sim_stats.valid++;
    } else sim_stats.invalid++;
    if (sim_stats.frames==300) {
        double elapsed=now>sim_stats.first_ns?(now-sim_stats.first_ns)/1e9:0;
        fprintf(stderr,"[adaptive-sim] last300 real-game-fps=%.6f consumed-sim-dt-ms mean=%.6f min=%.6f max=%.6f bins-lt8.33-lt16.67-lt25-lt33.33-le40-gt40=%llu,%llu,%llu,%llu,%llu,%llu valid=%llu invalid=%llu; no generated/presented cadence; <=60 real cap provisional until device qualification\n",
            elapsed>0?299/elapsed:0,sim_stats.valid?sim_stats.sum_ms/sim_stats.valid:0,
            sim_stats.valid?sim_stats.min_ms:0,sim_stats.max_ms,
            (unsigned long long)sim_stats.bins[0],(unsigned long long)sim_stats.bins[1],
            (unsigned long long)sim_stats.bins[2],(unsigned long long)sim_stats.bins[3],
            (unsigned long long)sim_stats.bins[4],(unsigned long long)sim_stats.bins[5],
            (unsigned long long)sim_stats.valid,(unsigned long long)sim_stats.invalid);
        memset(&sim_stats,0,sizeof sim_stats);
    }
}
/* TOTK_FRAMEGEN_JOB_TRACE=1 (exact) [adaptive-scene] transition log. Passive:
 * this bookkeeping is written only while tracing and never read by a decision;
 * lines are formatted under mu and printed after unlock (no new lock/wait).
 * frame = ordinal of runtime_fps_adaptive_present calls (guest presents seen
 * by adaptive), not the Vulkan presenter frame; join on CLOCK_MONOTONIC now_ns. */
static _Atomic int scene_trace_on;
static struct {
    uint64_t presents, suppressed, hold_logged_ns, pipelines;
    const char *epoch_reason, *present_reason;
    int have_present, eligible;
} scene_trace;
typedef struct { unsigned n; char line[2][512]; } scene_trace_batch;
static const char *const scene_names[] = {"ready", "before-world", "loading", "low-draws", "settling"};
static const char *const scene_reasons[] = {"scene-ready", "scene-before-world", "scene-loading",
    "scene-low-draws", "scene-settling"};
static int scene_tracing(void) { return atomic_load_explicit(&scene_trace_on, memory_order_relaxed); }
static uint64_t scene_trace_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static void scene_trace_vadd(scene_trace_batch *b, const char *event, const char *reason,
        uint64_t now, uint64_t prev_epoch, const char *fmt, va_list ap) {
    if (b->n >= sizeof b->line / sizeof *b->line) return;
    char *s = b->line[b->n++];
    size_t cap = sizeof b->line[0];
    int k = snprintf(s, cap, "[adaptive-scene] event=%s reason=%s frame=%llu now_ns=%llu epoch=%llu prev_epoch=%llu eligible=%d scene_ready=%d clock=%d sample_valid=%d paused=%d window_missing=%d hold_until_ns=%llu anchor_ns=%llu suppressed=%llu",
        event, reason, (unsigned long long)scene_trace.presents, (unsigned long long)now,
        (unsigned long long)state.epoch, (unsigned long long)prev_epoch, state.frame.eligible,
        state.scene_ready, state.clock, state.sample_valid, state.paused, state.window_missing,
        (unsigned long long)state.scene.hold_until_ns, (unsigned long long)state.frame.anchor_ns,
        (unsigned long long)scene_trace.suppressed);
    if (k > 0 && (size_t)k < cap) vsnprintf(s + k, cap - (size_t)k, fmt, ap);
    scene_trace.suppressed = 0;
    scene_trace.hold_logged_ns = state.scene.hold_until_ns;
}
static void scene_trace_add(scene_trace_batch *b, const char *event, const char *reason,
        uint64_t now, uint64_t prev_epoch, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void scene_trace_add(scene_trace_batch *b, const char *event, const char *reason,
        uint64_t now, uint64_t prev_epoch, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    scene_trace_vadd(b, event, reason, now, prev_epoch, fmt, ap);
    va_end(ap);
}
/* Every epoch bump is a transition, except a same-reason repeat while no valid
 * sample existed since the previous one (paused/hitch/cancel streaks): those are
 * counted into the next line's suppressed= field instead. */
static void scene_trace_epoch(scene_trace_batch *b, const char *reason, uint64_t now,
        uint64_t prev_epoch, int valid_before, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void scene_trace_epoch(scene_trace_batch *b, const char *reason, uint64_t now,
        uint64_t prev_epoch, int valid_before, const char *fmt, ...) {
    if (!valid_before && scene_trace.epoch_reason == reason) {
        scene_trace.suppressed++;
        return;
    }
    scene_trace.epoch_reason = reason;
    va_list ap;
    va_start(ap, fmt);
    scene_trace_vadd(b, "epoch", reason, now, prev_epoch, fmt, ap);
    va_end(ap);
}
static void scene_trace_flush(const scene_trace_batch *b) {
    for (unsigned i = 0; i < b->n; i++) fprintf(stderr, "%s\n", b->line[i]);
}
/* Mirrors the invalid-sample predicate in runtime_fps_adaptive_step, in order. */
static const char *scene_trace_step_reason(uint64_t now, float seconds, uint64_t last) {
    return !now ? "step-zero-now" :
        !isfinite(seconds) || seconds < .001f || seconds > .040f ? "step-seconds" :
        last && (now <= last || now - last < 1000000 || now - last > 40000000) ? "step-interval" :
        state.paused ? "step-paused" : state.window_missing ? "step-window-missing" : "step-unknown";
}
/* Mirrors the eligibility conjunction in runtime_fps_adaptive_present, in order. */
static const char *scene_trace_eligibility(unsigned scene, uint64_t now) {
    return scene != FPS_SCENE_READY ? (scene < 5 ? scene_reasons[scene] : "scene-unknown") :
        !state.clock ? "clock" : !state.sample_valid ? "sample" : state.paused ? "paused" :
        state.window_missing ? "window-missing" : now < state.frame.anchor_ns ? "anchor-future" :
        now - state.frame.anchor_ns > 250000000 ? "anchor-stale" : "ready";
}
static void scene_trace_present(scene_trace_batch *b, const char *event, const char *reason,
        uint64_t now, uint64_t prev_epoch, unsigned scene, uint64_t draws, int loading) {
    uint64_t draw_floor = state.scene.scene_draws / 3;
    if (draw_floor < 512) draw_floor = 512;
    long long age = (long long)now - (long long)state.frame.anchor_ns;
    scene_trace_add(b, event, reason, now, prev_epoch,
        " scene=%s draws=%llu loading=%d scene_draws=%llu floor=%llu world_seen=%u anchor_age_ns=%lld",
        scene < 5 ? scene_names[scene] : "unknown", (unsigned long long)draws, loading,
        (unsigned long long)state.scene.scene_draws, (unsigned long long)draw_floor,
        state.scene.world_seen, age);
}

static int exact(const char *name, const char *value) {
    const char *s = getenv(name); return s && !strcmp(s, value);
}
static int nonzero(const char *name) {
    const char *s = getenv(name); return s && *s && strcmp(s, "0");
}
static int differs(const char *name, const char *value) {
    const char *s = getenv(name); return s && strcmp(s, value);
}
static void configure(void) {
    if (!exact("TOTK_FRAMEGEN", "adaptive")) return;
    const char *reason = NULL;
#if !(defined(__ANDROID__) && defined(TOTK_ANDROID_APP)) && !defined(TOTK_VK_TESTS)
    reason = "requires the native Android Vulkan app";
#elif !defined(TOTK_GAME_V143) && !defined(TOTK_VK_TESTS)
    reason = "requires game version 1.4.3 realtime publication";
#endif
    if (exact("TOTK_FRAMEGEN_ENGINE", "lsfg"))
        reason = "adaptive LSFG disabled pending doubling qualification";
    else if (!exact("TOTK_FRAMEGEN_ENGINE", "qcom"))
        reason = "requires exact TOTK_FRAMEGEN_ENGINE=qcom";
    else if (!exact("TOTK_FPS60", "1")) reason = "requires TOTK_FPS60=1";
    else if (!exact("TOTK_REALTIME", "1")) reason = "requires TOTK_REALTIME=1";
    else if (nonzero("TOTK_FPS_AUTO")) reason = "requires TOTK_FPS_AUTO=0";
    else if (nonzero("TOTK_UNCAPPED") || nonzero("DM_DETCLOCK")) reason = "requires capped measured realtime";
    else if (!exact("TOTK_PRESENT_SLOTS", "1")) reason = "requires TOTK_PRESENT_SLOTS=1";
    else if (!exact("TOTK_NATIVE_UI", "1")) reason = "requires TOTK_NATIVE_UI=1";
    else if (nonzero("TOTK_ASYNC_REPLAY")) reason = "requires TOTK_ASYNC_REPLAY=0 (immutable producer mapping)";
    else if (differs("DM_PRESENT_INTERVAL", "game") ||
             (getenv("DM_PRESENT_HZ") && !exact("DM_PRESENT_HZ", "60") && !exact("DM_PRESENT_HZ", "120")))
        reason = "requires native 60/120Hz game pacing; physical refresh is authoritative";
    else if (nonzero("TOTK_ACQUIRE_SLACK") || nonzero("TOTK_ACQUIRE_CLOCK") ||
             differs("TOTK_PACE", "0") || nonzero("TOTK_MFX"))
        reason = "requires ACQUIRE_SLACK=0 ACQUIRE_CLOCK=0 PACE=0 MFX=0";
    if (reason) {
        fprintf(stderr, "[adaptive] refused: %s; existing native policy retained\n", reason);
        return;
    }
    selected = 1;
    fprintf(stderr, "[adaptive] enabled: physical-refresh pacing, strict <=60 real-frame release cap; phase from consumed post-selector simulation seconds at guest present, never render completion; native real UI; scene readiness and two-source warmup required; fade/LOD safety provisional pending device logs\n");
    const char *settle = getenv("TOTK_FG_ADAPTIVE_PIPE_SETTLE");
    if (settle && !strcmp(settle, "1")) {
        pipe_settle_ns = FPS_AUTO_PIPE_WINDOW_NS;
        fprintf(stderr, "[adaptive-pipe-settle] selector=1 enabled=1 settle_ns=%llu default_settle_ns=%llu (pipeline bursts only; loading/world/grace holds unchanged)\n",
            (unsigned long long)pipe_settle_ns, (unsigned long long)FPS_AUTO_SCENE_SETTLE_NS);
    } else if (settle && strcmp(settle, "0"))
        fprintf(stderr, "[adaptive-pipe-settle] selector=invalid raw=\"%.32s\" enabled=0 (exact \"1\" required; default %llu ns settle)\n",
            settle, (unsigned long long)FPS_AUTO_SCENE_SETTLE_NS);
    if (exact("TOTK_FRAMEGEN_JOB_TRACE", "1")) {
        atomic_store_explicit(&scene_trace_on, 1, memory_order_relaxed);
        fprintf(stderr, "[adaptive-scene] event=config reason=job-trace; logging-only epoch/eligibility transitions, no decision or timing change\n");
    }
}
int runtime_fps_adaptive_selected(void) { pthread_once(&once, configure); return selected; }
static void reset_locked(void) {
    if (++state.epoch == 0) state.epoch = 1;
    state.sim_ns = state.last_step_ns = 0;
    state.fraction_ns = 0;
    state.sample_valid = 0;
    state.frame = (runtime_fps_adaptive_frame){.epoch = state.epoch};
}
void runtime_fps_adaptive_reset(void) {
    if (!runtime_fps_adaptive_selected()) return;
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    uint64_t now = tracing ? scene_trace_clock() : 0;
    pthread_mutex_lock(&mu);
    uint64_t prev = state.epoch;
    int valid = state.sample_valid;
    reset_locked();
    if (tracing)
        scene_trace_epoch(&tb, "external-reset", now, prev, valid, " origin=%s",
            "runtime_fps_adaptive_reset");
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
}
static int refresh_clock(uint64_t refresh) {
    return refresh >= 16000000 && refresh <= 17300000 ? 60 :
           refresh >= 8000000 && refresh <= 8650000 ? 120 : 0;
}
unsigned runtime_fps_adaptive_acquire(uint64_t now, uint64_t refresh, unsigned guest) {
    if (!runtime_fps_adaptive_selected()) return guest;
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    pthread_mutex_lock(&mu);
    int clock = refresh_clock(refresh);
    if (!now || guest < 1 || guest > 2) clock = 0;
    if (clock != state.clock || (state.last_acquire_ns && now <= state.last_acquire_ns)) {
        uint64_t prev = state.epoch;
        int valid = state.sample_valid;
        reset_locked();
        if (tracing)
            scene_trace_epoch(&tb, clock != state.clock ? "acquire-clock" : "acquire-order", now,
                prev, valid, " refresh_ns=%llu guest=%u new_clock=%d last_acquire_ns=%llu",
                (unsigned long long)refresh, guest, clock, (unsigned long long)state.last_acquire_ns);
    }
    state.clock = clock;
    state.last_acquire_ns = now;
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
    /* v143 fade/LOD integer increments truncate above ~63.5 real fps. Until
     * those guest sites are patched,120Hz scanout must retain real<=60Hz. */
    return clock ? (clock==120 ? 2u : 1u) : guest;
}
void runtime_fps_adaptive_step(uint64_t now, float seconds) {
    if (!runtime_fps_adaptive_selected()) return;
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    pthread_mutex_lock(&mu);
    simulation_stats(now,seconds);
    uint64_t last = state.last_step_ns;
    /* An invalid/paused sample contributes no simulated time. The 40ms real
     * floor invalidates history too, rather than connecting across the hitch.
     * Keep the new anchor only for judging the next real interval. */
    if (!now || !isfinite(seconds) || seconds < .001f || seconds > .040f ||
        (last && (now <= last || now - last < 1000000 || now - last > 40000000)) ||
        state.paused || state.window_missing) {
        uint64_t prev = state.epoch;
        int valid = state.sample_valid;
        reset_locked();
        state.last_step_ns = now;
        if (tracing)
            scene_trace_epoch(&tb, scene_trace_step_reason(now, seconds, last), now, prev, valid,
                " seconds=%.9g last_step_ns=%llu interval_ns=%lld", (double)seconds,
                (unsigned long long)last, last ? (long long)now - (long long)last : -1LL);
        pthread_mutex_unlock(&mu);
        scene_trace_flush(&tb);
        return;
    }
    double ns = (double)seconds * 1e9 + state.fraction_ns;
    uint64_t step_ns = (uint64_t)ns;
    if (state.sim_ns > UINT64_MAX - step_ns) {
        uint64_t prev = state.epoch;
        int valid = state.sample_valid;
        reset_locked();
        if (tracing)
            scene_trace_epoch(&tb, "sim-overflow", now, prev, valid, " step_ns=%llu",
                (unsigned long long)step_ns);
    }
    state.sim_ns += step_ns;
    state.fraction_ns = ns - (double)step_ns;
    state.last_step_ns = now;
    state.sample_valid = 1;
    state.frame = (runtime_fps_adaptive_frame){.sim_ns = state.sim_ns,
        .anchor_ns = now, .epoch = state.epoch};
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
}
runtime_fps_adaptive_frame runtime_fps_adaptive_present(uint64_t now, uint64_t draws, int loading) {
    if (!runtime_fps_adaptive_selected()) return (runtime_fps_adaptive_frame){0};
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    pthread_mutex_lock(&mu);
    if (tracing) scene_trace.presents++;
    unsigned scene = fps_auto_scene_observe(&state.scene, now, draws, loading);
    int ready = scene == FPS_SCENE_READY;
    uint64_t bumped_from = 0;
    if (state.scene_ready && !ready) {
        /* Scene eligibility is evidence, not a replacement phase clock. Keep
         * this real's immutable simulation/anchor, invalidate all old pairs. */
        bumped_from = state.epoch;
        if (++state.epoch == 0) state.epoch = 1;
        state.frame.epoch = state.epoch;
    }
    state.scene_ready = ready;
    state.frame.eligible = ready && state.clock && state.sample_valid &&
        !state.paused && !state.window_missing && now >= state.frame.anchor_ns &&
        now - state.frame.anchor_ns <= 250000000;
    if (tracing) {
        const char *reason = scene_trace_eligibility(scene, now);
        if (bumped_from) {
            scene_trace.epoch_reason = scene_reasons[scene < 5 ? scene : 0];
            scene_trace_present(&tb, "epoch", scene_trace.epoch_reason, now, bumped_from,
                scene, draws, loading);
        }
        if (!scene_trace.have_present || scene_trace.eligible != state.frame.eligible ||
                scene_trace.present_reason != reason)
            scene_trace_present(&tb, state.frame.eligible ? "eligible" : "ineligible", reason, now,
                state.epoch, scene, draws, loading);
        scene_trace.have_present = 1;
        scene_trace.eligible = state.frame.eligible;
        scene_trace.present_reason = reason;
    }
    runtime_fps_adaptive_frame frame = state.frame;
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
    return frame;
}
runtime_fps_adaptive_frame runtime_fps_adaptive_snapshot(void) {
    if (!runtime_fps_adaptive_selected()) return (runtime_fps_adaptive_frame){0};
    pthread_mutex_lock(&mu);
    runtime_fps_adaptive_frame frame = state.frame;
    pthread_mutex_unlock(&mu);
    return frame;
}
void runtime_fps_adaptive_lifecycle(int window, int paused) {
    /* May run before env loading. Do not freeze pthread_once selection here;
     * the trace flag is set only by an already-completed adaptive selection. */
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    uint64_t now = tracing ? scene_trace_clock() : 0;
    pthread_mutex_lock(&mu);
    uint64_t prev = state.epoch;
    int valid = state.sample_valid;
    if (window >= 0) state.window_missing = !window;
    if (paused >= 0) state.paused = paused;
    reset_locked();
    state.scene = (fps_auto_scene){0};
    state.scene_ready = 0;
    if (tracing)
        scene_trace_epoch(&tb, "lifecycle", now, prev, valid, " window_arg=%d paused_arg=%d",
            window, paused);
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
}
void runtime_fps_adaptive_pipeline(uint64_t now) {
    if (!runtime_fps_adaptive_selected()) return;
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    pthread_mutex_lock(&mu);
    if (tracing) scene_trace.pipelines++;
    if (fps_auto_scene_pipeline_settle(&state.scene, now, pipe_settle_ns)) {
        uint64_t prev = state.epoch;
        int valid = state.sample_valid;
        reset_locked();
        state.scene_ready = 0;
        if (tracing)
            scene_trace_epoch(&tb, "pipeline-burst", now, prev, valid,
                " pipeline_count=%u pipeline_start_ns=%llu pipeline_calls=%llu burst=%u window_ns=%llu settle_ns=%llu",
                state.scene.pipeline_count, (unsigned long long)state.scene.pipeline_start_ns,
                (unsigned long long)scene_trace.pipelines,
                FPS_AUTO_PIPE_BURST, (unsigned long long)FPS_AUTO_PIPE_WINDOW_NS,
                (unsigned long long)pipe_settle_ns);
    }
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
}
void runtime_fps_adaptive_grace(uint64_t now, uint64_t duration) {
    if (!duration || !runtime_fps_adaptive_selected()) return;
    int tracing = scene_tracing();
    scene_trace_batch tb; tb.n = 0;
    pthread_mutex_lock(&mu);
    uint64_t prev = state.epoch, old_hold = state.scene.hold_until_ns;
    int valid = state.sample_valid, bumped = 0;
    fps_auto_scene_grace(&state.scene, now, duration);
    if (state.scene_ready || state.frame.eligible) {
        reset_locked();
        bumped = 1;
    }
    state.scene_ready = 0;
    if (tracing) {
        uint64_t hold = state.scene.hold_until_ns, logged = scene_trace.hold_logged_ns;
        if (bumped)
            scene_trace_epoch(&tb, "grace", now, prev, valid, " duration_ns=%llu old_hold_until_ns=%llu",
                (unsigned long long)duration, (unsigned long long)old_hold);
        /* Hold-only extensions: first after expiry, then at most per +1s. */
        else if (hold > old_hold && (logged <= now || (hold > logged && hold - logged >= 1000000000ull)))
            scene_trace_add(&tb, "hold", "grace", now, prev, " duration_ns=%llu old_hold_until_ns=%llu",
                (unsigned long long)duration, (unsigned long long)old_hold);
    }
    pthread_mutex_unlock(&mu);
    scene_trace_flush(&tb);
}
#endif
