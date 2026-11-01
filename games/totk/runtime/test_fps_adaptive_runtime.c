/* Deterministic production bridge fixtures: no clocks, sleeps, game, GPU or adb.
 * Build: make -C games/totk/android fpsadaptivetest TARGET=linux CC=cc OUT=<scratch>
 * Run: <scratch>/fpsadaptivetest
 * Forked startup cases keep the production pthread_once environment contract. */
#include "fps_adaptive_runtime.h"
#include "fps_hybrid_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const uint64_t second = 1000000000ull, refresh = 16666667;
static const char *const options[] = {
    "TOTK_FRAMEGEN", "TOTK_FRAMEGEN_ENGINE", "TOTK_FPS60", "TOTK_REALTIME",
    "TOTK_FPS_AUTO", "TOTK_UNCAPPED", "TOTK_PRESENT_SLOTS", "TOTK_NATIVE_UI",
    "TOTK_ASYNC_REPLAY", "DM_DETCLOCK", "DM_PRESENT_HZ", "DM_PRESENT_INTERVAL",
    "TOTK_ACQUIRE_SLACK", "TOTK_ACQUIRE_CLOCK", "TOTK_PACE", "TOTK_MFX"
};
static void baseline(void) {
    for (unsigned i = 0; i < sizeof options / sizeof *options; i++) unsetenv(options[i]);
    setenv("TOTK_FRAMEGEN", "adaptive", 1);
    setenv("TOTK_FRAMEGEN_ENGINE", "qcom", 1);
    setenv("TOTK_FPS60", "1", 1);
    setenv("TOTK_REALTIME", "1", 1);
    setenv("TOTK_NATIVE_UI", "1", 1);
    setenv("TOTK_PRESENT_SLOTS", "1", 1);
    setenv("TOTK_UNCAPPED", "0", 1);
    setenv("TOTK_ASYNC_REPLAY", "0", 1);
    setenv("TOTK_FPS_AUTO", "0", 1);
    setenv("TOTK_ACQUIRE_SLACK", "0", 1);
    setenv("TOTK_ACQUIRE_CLOCK", "0", 1);
    setenv("TOTK_PACE", "0", 1);
    setenv("TOTK_MFX", "0", 1);
}
static void child_ok(pid_t pid) {
    int status;
    assert(pid > 0 && waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void same(runtime_fps_adaptive_frame a, runtime_fps_adaptive_frame b) {
    assert(a.sim_ns == b.sim_ns && a.anchor_ns == b.anchor_ns &&
        a.epoch == b.epoch && a.eligible == b.eligible);
}
/* Frozen pre-adaptive hybrid startup expectations. No new native UI, explicit
 * realtime, engine or 120Hz allowances may leak into exact hybrid selection. */
static const struct gate {
    const char *name, *value;
    int adaptive, hybrid;
} gates[] = {
    {NULL, NULL, 1, 1},
    {"TOTK_FPS60", NULL, 0, 0}, {"TOTK_FPS60", "0", 0, 0},
    {"TOTK_FPS60", "01", 0, 0},
    {"TOTK_REALTIME", NULL, 0, 1}, {"TOTK_REALTIME", "0", 0, 0},
    {"TOTK_REALTIME", "01", 0, 0},
    {"TOTK_FPS_AUTO", "1", 0, 0}, {"TOTK_FPS_AUTO", NULL, 1, 1},
    {"TOTK_UNCAPPED", "1", 0, 0}, {"TOTK_UNCAPPED", NULL, 1, 1},
    {"DM_DETCLOCK", "16666667", 0, 0},
    {"TOTK_PRESENT_SLOTS", "0", 0, 0}, {"TOTK_PRESENT_SLOTS", NULL, 0, 1},
    {"TOTK_NATIVE_UI", "0", 0, 1}, {"TOTK_NATIVE_UI", NULL, 0, 1},
    {"TOTK_ASYNC_REPLAY", "1", 0, 0}, {"TOTK_ASYNC_REPLAY", NULL, 1, 1},
    {"TOTK_ASYNC_REPLAY", "invalid", 0, 0},
    {"DM_PRESENT_HZ", "60", 1, 1}, {"DM_PRESENT_HZ", "120", 1, 0},
    {"DM_PRESENT_HZ", "90", 0, 0}, {"DM_PRESENT_HZ", "0", 0, 0},
    {"DM_PRESENT_INTERVAL", "display", 0, 0}, {"DM_PRESENT_INTERVAL", "game", 1, 1},
    {"TOTK_ACQUIRE_SLACK", "1", 0, 0}, {"TOTK_ACQUIRE_SLACK", "auto", 0, 0},
    {"TOTK_ACQUIRE_CLOCK", "vsync", 0, 0}, {"TOTK_PACE", "display", 0, 0},
    {"TOTK_MFX", "1", 0, 0},
    {"TOTK_FRAMEGEN_ENGINE", "lsfg", 0, 1},
    {"TOTK_FRAMEGEN_ENGINE", "QCOM", 0, 1},
    {"TOTK_FRAMEGEN_ENGINE", NULL, 0, 1}
};
static void gate_matrix(void) {
    for (unsigned i = 0; i < sizeof gates / sizeof *gates; i++) for (int hybrid = 0; hybrid <= 1; hybrid++) {
        pid_t pid = fork();
        if (!pid) {
            baseline();
            if (hybrid) setenv("TOTK_FRAMEGEN", "hybrid", 1);
            const struct gate *g = &gates[i];
            if (g->name) {
                if (g->value) setenv(g->name, g->value, 1);
                else unsetenv(g->name);
            }
            assert(runtime_fps_adaptive_selected() == (hybrid ? 0 : g->adaptive));
            assert(runtime_fps_hybrid_selected() == (hybrid ? g->hybrid : 0));
            for (unsigned guest = 1; guest <= 3; guest++) {
                unsigned result = runtime_fps_adaptive_acquire(second + guest, refresh, guest);
                unsigned expected = !hybrid && g->adaptive && guest <= 2 ? 1 : guest;
                assert(result == expected);
                if (hybrid)
                    assert(runtime_fps_hybrid_acquire(second + guest, refresh, guest, 0, 0, 0) == guest);
            }
            if (hybrid || !g->adaptive) {
                runtime_fps_adaptive_step(second, .025f);
                runtime_fps_adaptive_frame f = runtime_fps_adaptive_present(second, 4000, 0);
                assert(!f.eligible && !f.epoch && !f.sim_ns && !f.anchor_ns);
            }
            _exit(0);
        }
        child_ok(pid);
    }
    const char *const modes[] = {NULL, "0", "locked", "interp", "camera", "Adaptive", "adaptive-extra"};
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) {
        pid_t pid = fork();
        if (!pid) {
            baseline();
            if (modes[i]) setenv("TOTK_FRAMEGEN", modes[i], 1); else unsetenv("TOTK_FRAMEGEN");
            assert(!runtime_fps_adaptive_selected() && !runtime_fps_hybrid_selected());
            assert(runtime_fps_adaptive_acquire(second, refresh, 2) == 2);
            _exit(0);
        }
        child_ok(pid);
    }
}
static runtime_fps_adaptive_frame frame(uint64_t *now, uint64_t dt, float seconds, uint64_t draws, int loading) {
    *now += dt;
    assert(runtime_fps_adaptive_acquire(*now - 1000000, refresh, 2) == 1);
    runtime_fps_adaptive_step(*now, seconds);
    return runtime_fps_adaptive_present(*now + 100000, draws, loading);
}
static runtime_fps_adaptive_frame warm(uint64_t *now) {
    runtime_fps_adaptive_frame f = {0};
    for (unsigned i = 0; i < 550; i++) f = frame(now, 20000000, .020f, 4000, 0);
    assert(f.eligible);
    return f;
}
static void accumulation(void) {
    assert(runtime_fps_adaptive_selected() && !runtime_fps_hybrid_selected());
    uint64_t now = second, raw_sum = 0;
    long double published_ns = 0;
    const uint64_t dt[] = {18000000, 28000000, 23000000, 19000000, 27000000};
    const float seconds[] = {.023f, .0221f, .0203f, 1.0f / 60.0f, .0267f};
    runtime_fps_adaptive_frame f = {0};
    for (unsigned i = 0; i < 6000; i++) {
        unsigned j = i % 5;
        raw_sum += dt[j];
        published_ns += (long double)seconds[j] * 1e9L;
        f = frame(&now, dt[j], seconds[j], 4000, 0);
        assert(f.sim_ns == (uint64_t)published_ns);
        assert(f.anchor_ns == now && f.epoch);
    }
    assert(f.eligible && f.sim_ns != raw_sum);
    runtime_fps_adaptive_frame saved = f;
    /* Readiness observation, even late, cannot move the immutable game anchor
     * or add raw elapsed time. No render-end/midpoint phase substitute. */
    f = runtime_fps_adaptive_present(now + 5000000, 4000, 0);
    same(saved, f);
    same(f, runtime_fps_adaptive_snapshot());
    f = frame(&now, 21000000, .019f, 4000, 0);
    assert(f.anchor_ns == now && saved.anchor_ns == now - 21000000);
    assert(f.sim_ns - saved.sim_ns != 21000000);
    assert(saved.eligible);
}
static void resets(void) {
    uint64_t now = second;
    runtime_fps_adaptive_frame f = warm(&now);
    const float invalid[] = {NAN, INFINITY, -INFINITY, 0, -.01f, .0001f, .041f};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        uint64_t old = f.epoch;
        f = frame(&now, 20000000, invalid[i], 4000, 0);
        assert(!f.eligible && f.epoch != old && !f.sim_ns && !f.anchor_ns);
        f = frame(&now, 20000000, .020f, 4000, 0);
        assert(f.eligible && f.sim_ns == (uint64_t)((double).020f * 1e9));
    }
    uint64_t epoch = f.epoch;
    f = frame(&now, 41000000, .025f, 4000, 0);
    assert(!f.eligible && f.epoch != epoch); /* Raw real interval floor, despite smoothed dt. */
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(f.eligible);
    epoch = f.epoch;
    runtime_fps_adaptive_step(now, .020f); /* Duplicate/reversed guest clock. */
    f = runtime_fps_adaptive_snapshot();
    assert(!f.eligible && f.epoch != epoch);
    epoch = f.epoch;
    runtime_fps_adaptive_step(now - 1, .020f);
    f = runtime_fps_adaptive_snapshot();
    assert(!f.eligible && f.epoch != epoch);
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(f.eligible);
    epoch = f.epoch;
    runtime_fps_adaptive_reset(); /* Known camera/history cut. */
    f = runtime_fps_adaptive_snapshot();
    assert(!f.eligible && f.epoch != epoch && !f.anchor_ns && !f.sim_ns);
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(f.eligible);
    epoch = f.epoch;
    runtime_fps_adaptive_lifecycle(-1, 1);
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(!f.eligible && f.epoch != epoch);
    runtime_fps_adaptive_lifecycle(-1, 0);
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(!f.eligible); /* Lifecycle restarts the shared 10s world-settle gate. */
    f = warm(&now);
    epoch = f.epoch;
    runtime_fps_adaptive_lifecycle(0, -1);
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(!f.eligible && f.epoch != epoch);
    runtime_fps_adaptive_lifecycle(1, -1);
    assert(warm(&now).eligible);
}
static void scenes_and_clocks(void) {
    uint64_t now = second;
    runtime_fps_adaptive_frame f = warm(&now);
    uint64_t epoch = f.epoch;
    f = frame(&now, 20000000, .020f, 100, 0); /* Existing scene-relative menu floor. */
    assert(!f.eligible && f.epoch != epoch && f.anchor_ns == now);
    for (unsigned i = 0; i < 499; i++) {
        f = frame(&now, 20000000, .020f, 4000, 0);
        assert(!f.eligible);
    }
    f = frame(&now, 20000000, .020f, 4000, 0);
    assert(f.eligible);
    epoch = f.epoch;
    f = frame(&now, 20000000, .020f, 4000, 1);
    assert(!f.eligible && f.epoch != epoch);
    f = warm(&now);
    epoch = f.epoch;
    for (unsigned i = 0; i < 7; i++) runtime_fps_adaptive_pipeline(now + i);
    assert(runtime_fps_adaptive_snapshot().epoch == epoch);
    runtime_fps_adaptive_pipeline(now + 7);
    f = runtime_fps_adaptive_snapshot();
    assert(!f.eligible && f.epoch != epoch);
    assert(!frame(&now, 20000000, .020f, 4000, 0).eligible);
    f = warm(&now);
    epoch = f.epoch;
    runtime_fps_adaptive_grace(now, second);
    f = runtime_fps_adaptive_snapshot();
    assert(!f.eligible && f.epoch != epoch);
    f = warm(&now);
    epoch = f.epoch;
    assert(runtime_fps_adaptive_acquire(now + 1, 8333333, 2) == 2);
    f = runtime_fps_adaptive_snapshot();
    assert(f.epoch != epoch && !f.eligible);
    epoch = f.epoch;
    assert(runtime_fps_adaptive_acquire(now + 2, 10000000, 2) == 2);
    f = runtime_fps_adaptive_snapshot();
    assert(f.epoch != epoch && !f.eligible);
    assert(runtime_fps_adaptive_acquire(now + 3, refresh, 3) == 3);
    assert(runtime_fps_adaptive_acquire(now + 4, refresh, 0) == 0);
    assert(runtime_fps_adaptive_acquire(now + 5, refresh, 1) == 1);
    assert(runtime_fps_adaptive_acquire(now + 6, 8333333, 2) == 2);
}
static void run(void (*fixture)(void)) {
    pid_t pid = fork();
    if (!pid) { baseline(); fixture(); _exit(0); }
    child_ok(pid);
}
/* TOTK_FG_ADAPTIVE_PIPE_SETTLE: exact "1" shortens only the pipeline-burst hold
 * to 1 s; off/"0"/invalid keep the 10 s settle. Loading still uses 10 s. */
static uint64_t pipe_recover_ns(const char *value, int loading_check) {
    if (value) setenv("TOTK_FG_ADAPTIVE_PIPE_SETTLE", value, 1);
    else unsetenv("TOTK_FG_ADAPTIVE_PIPE_SETTLE");
    assert(runtime_fps_adaptive_selected());
    uint64_t now = second;
    runtime_fps_adaptive_frame f = warm(&now);
    uint64_t epoch = f.epoch, start = now;
    if (loading_check) {
        f = frame(&now, 20000000, .020f, 4000, 1);
    } else {
        for (unsigned i = 0; i < 8; i++) runtime_fps_adaptive_pipeline(now + i);
        f = runtime_fps_adaptive_snapshot();
    }
    assert(!f.eligible && f.epoch != epoch);
    for (unsigned i = 0; i < 1000; i++) {
        f = frame(&now, 20000000, .020f, 4000, 0);
        if (f.eligible) return now - start;
    }
    return UINT64_MAX;
}
static void pipe_settle_off(void) {
    uint64_t t = pipe_recover_ns(NULL, 0);
    assert(t >= 10 * second && t < 11 * second);
}
static void pipe_settle_zero(void) {
    uint64_t t = pipe_recover_ns("0", 0);
    assert(t >= 10 * second && t < 11 * second);
}
static void pipe_settle_invalid(void) {
    uint64_t t = pipe_recover_ns("true", 0);
    assert(t >= 10 * second && t < 11 * second);
}
static void pipe_settle_on(void) {
    uint64_t t = pipe_recover_ns("1", 0);
    assert(t >= second && t < second + second / 2);
}
static void pipe_settle_on_loading_unchanged(void) {
    uint64_t t = pipe_recover_ns("1", 1);
    assert(t >= 10 * second && t < 11 * second);
}
static void pipe_settle_on_extends(void) {
    setenv("TOTK_FG_ADAPTIVE_PIPE_SETTLE", "1", 1);
    uint64_t now = second;
    runtime_fps_adaptive_frame f = warm(&now);
    uint64_t start = now;
    for (unsigned i = 0; i < 8; i++) runtime_fps_adaptive_pipeline(now + i);
    /* Creations keep arriving inside the burst window: the hold moves with them. */
    for (unsigned i = 0; i < 40; i++) {
        f = frame(&now, 20000000, .020f, 4000, 0);
        if (i % 10 == 9) runtime_fps_adaptive_pipeline(now);
        assert(!f.eligible);
    }
    uint64_t last = now;
    for (unsigned i = 0; i < 1000 && !f.eligible; i++) f = frame(&now, 20000000, .020f, 4000, 0);
    assert(f.eligible && now - last >= second && now - start < 3 * second);
}
static void early_lifecycle(void) {
    unsetenv("TOTK_FRAMEGEN");
    runtime_fps_adaptive_lifecycle(1, 0);
    baseline();
    assert(runtime_fps_adaptive_selected()); /* Notification did not freeze selection. */
}
/* JOB_TRACE passivity: one scripted transition sequence covering every runtime
 * epoch/eligibility reason runs in forked arms with the trace unset, exact "1"
 * and non-exact "01". Every acquire result, snapshot and present stamp is
 * folded into a digest that must match; only the exact arm may log. */
static uint64_t trace_digest = 1469598103934665603ull, trace_records;
static void mix(uint64_t v) {
    for (unsigned i = 0; i < 8; i++) {
        trace_digest ^= (v >> (8 * i)) & 255;
        trace_digest *= 1099511628211ull;
    }
    trace_records++;
}
static void note(runtime_fps_adaptive_frame f) {
    mix(f.sim_ns); mix(f.anchor_ns); mix(f.epoch); mix((uint64_t)f.eligible);
}
static runtime_fps_adaptive_frame traced(uint64_t *now, uint64_t dt, float seconds, uint64_t draws, int loading) {
    *now += dt;
    mix(runtime_fps_adaptive_acquire(*now - 1000000, refresh, 2));
    runtime_fps_adaptive_step(*now, seconds);
    note(runtime_fps_adaptive_snapshot());
    runtime_fps_adaptive_frame f = runtime_fps_adaptive_present(*now + 100000, draws, loading);
    note(f);
    return f;
}
static void settle(uint64_t *now) {
    runtime_fps_adaptive_frame f = {0};
    for (unsigned i = 0; i < 550; i++) f = traced(now, 20000000, .020f, 4000, 0);
    assert(f.eligible);
}
static void trace_script(void) {
    const uint64_t ms = 1000000;
    uint64_t now = second;
    runtime_fps_adaptive_frame f;
    settle(&now); /* acquire-clock, scene-settling, ready */
    for (unsigned i = 0; i < 5; i++) assert(!traced(&now, 20 * ms, .041f, 4000, 0).eligible);
    assert(traced(&now, 20 * ms, .020f, 4000, 0).eligible);
    assert(!traced(&now, 41 * ms, .025f, 4000, 0).eligible);
    assert(traced(&now, 20 * ms, .020f, 4000, 0).eligible);
    runtime_fps_adaptive_reset();
    note(runtime_fps_adaptive_snapshot());
    assert(traced(&now, 20 * ms, .020f, 4000, 0).eligible);
    now += 20 * ms; /* Late presenter read of a valid stamp. */
    mix(runtime_fps_adaptive_acquire(now - ms, refresh, 2));
    runtime_fps_adaptive_step(now, .020f);
    f = runtime_fps_adaptive_present(now + 300 * ms, 4000, 0);
    note(f);
    assert(!f.eligible);
    now += 300 * ms; /* Keep presenter observations monotonic; 320ms real gap. */
    assert(!traced(&now, 20 * ms, .020f, 4000, 0).eligible);
    assert(traced(&now, 20 * ms, .020f, 4000, 0).eligible);
    assert(!traced(&now, 20 * ms, .020f, 100, 0).eligible);
    settle(&now);
    assert(!traced(&now, 20 * ms, .020f, 4000, 1).eligible);
    settle(&now);
    for (unsigned i = 0; i < 9; i++) runtime_fps_adaptive_pipeline(now + i);
    note(runtime_fps_adaptive_snapshot());
    settle(&now);
    runtime_fps_adaptive_grace(now, second);
    note(runtime_fps_adaptive_snapshot());
    runtime_fps_adaptive_grace(now + 1, 3 * second);
    runtime_fps_adaptive_grace(now + 2, second / 2);
    note(runtime_fps_adaptive_snapshot());
    settle(&now);
    now += ms;
    mix(runtime_fps_adaptive_acquire(now, 8333333, 2));
    note(runtime_fps_adaptive_snapshot());
    mix(runtime_fps_adaptive_acquire(now, 8333333, 2));
    note(runtime_fps_adaptive_snapshot());
    settle(&now);
    now += 20 * ms;
    mix(runtime_fps_adaptive_acquire(now - ms, refresh, 3));
    runtime_fps_adaptive_step(now, .020f);
    f = runtime_fps_adaptive_present(now + 100000, 4000, 0);
    note(f);
    assert(!f.eligible);
    settle(&now);
    runtime_fps_adaptive_lifecycle(-1, 1);
    note(runtime_fps_adaptive_snapshot());
    for (unsigned i = 0; i < 3; i++) assert(!traced(&now, 20 * ms, .020f, 4000, 0).eligible);
    runtime_fps_adaptive_lifecycle(-1, 0);
    note(runtime_fps_adaptive_snapshot());
    settle(&now);
}
static char *trace_arm(const char *value, uint64_t result[2]) {
    int err[2], res[2];
    assert(!pipe(err) && !pipe(res));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(err[0]); close(res[0]);
        if (dup2(err[1], 2) != 2) _exit(2);
        close(err[1]);
        baseline();
        if (value) setenv("TOTK_FRAMEGEN_JOB_TRACE", value, 1);
        else unsetenv("TOTK_FRAMEGEN_JOB_TRACE");
        trace_script();
        uint64_t r[2] = {trace_digest, trace_records};
        _exit(write(res[1], r, sizeof r) == (ssize_t)sizeof r ? 0 : 3);
    }
    close(err[1]); close(res[1]);
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    assert(buf);
    for (;;) {
        if (cap - n < 4096) { buf = realloc(buf, cap *= 2); assert(buf); }
        ssize_t k = read(err[0], buf + n, cap - n - 1);
        if (k <= 0) break;
        n += (size_t)k;
    }
    buf[n] = 0;
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs(buf, stderr);
        assert(!"trace arm failed");
    }
    assert(read(res[0], result, 2 * sizeof *result) == (ssize_t)(2 * sizeof *result));
    close(err[0]); close(res[0]);
    return buf;
}
static unsigned occurrences(const char *s, const char *needle) {
    unsigned n = 0;
    for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
static void trace_passive(void) {
    uint64_t off[2], on[2], invalid[2];
    char *off_log = trace_arm(NULL, off), *on_log = trace_arm("1", on), *invalid_log = trace_arm("01", invalid);
    assert(off[1] > 10000 && off[0] == on[0] && off[1] == on[1]);
    assert(off[0] == invalid[0] && off[1] == invalid[1]);
    assert(strstr(off_log, "[adaptive] enabled") && !strstr(off_log, "[adaptive-scene]"));
    assert(strstr(invalid_log, "[adaptive] enabled") && !strstr(invalid_log, "[adaptive-scene]"));
    static const char *const expected[] = {
        "[adaptive-scene] event=config reason=job-trace",
        "event=epoch reason=acquire-clock ", "event=ineligible reason=scene-settling ",
        "event=eligible reason=ready ", "event=ineligible reason=sample ",
        "event=epoch reason=step-interval ", "event=epoch reason=external-reset ",
        "event=ineligible reason=anchor-stale ", "event=epoch reason=scene-low-draws ",
        "event=ineligible reason=scene-low-draws ", "event=epoch reason=scene-loading ",
        "event=ineligible reason=scene-loading ", "event=epoch reason=grace ",
        "event=hold reason=grace ", "event=epoch reason=acquire-order ",
        "event=ineligible reason=clock ", "event=epoch reason=lifecycle ",
        "suppressed=4 scene=ready ", "suppressed=2 window_arg=-1 paused_arg=0",
        "pipeline_count=8 ", "pipeline_calls=8 "
    };
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; i++) {
        if (!strstr(on_log, expected[i])) fprintf(stderr, "missing trace token: %s\n%s", expected[i], on_log);
        assert(strstr(on_log, expected[i]));
    }
    /* Streaks collapse to one line plus suppressed= counts. */
    assert(occurrences(on_log, "event=epoch reason=step-seconds ") == 1);
    assert(occurrences(on_log, "event=epoch reason=pipeline-burst ") == 1);
    assert(occurrences(on_log, "event=epoch reason=step-paused ") == 1);
    assert(occurrences(on_log, "event=hold reason=grace ") == 1);
    assert(occurrences(on_log, "event=eligible ") == occurrences(on_log, "event=eligible reason=ready "));
    /* Transitions only: about 4,000 presents produce a bounded line count and
     * never two consecutive identical presenter eligibility states. */
    unsigned lines = occurrences(on_log, "[adaptive-scene] ");
    assert(lines >= 20 && lines < 80);
    char previous[128] = "";
    for (const char *p = on_log; (p = strstr(p, "[adaptive-scene] event=")); p++) {
        const char *event = p + strlen("[adaptive-scene] ");
        if (strncmp(event, "event=eligible ", 15) && strncmp(event, "event=ineligible ", 17)) continue;
        const char *end = strstr(event, " frame=");
        assert(end && (size_t)(end - event) < sizeof previous);
        char key[128];
        memcpy(key, event, (size_t)(end - event));
        key[end - event] = 0;
        assert(strcmp(key, previous));
        strcpy(previous, key);
    }
    free(off_log); free(on_log); free(invalid_log);
}
int main(void) {
    gate_matrix();
    run(early_lifecycle);
    run(accumulation);
    run(resets);
    run(scenes_and_clocks);
    run(pipe_settle_off);
    run(pipe_settle_zero);
    run(pipe_settle_invalid);
    run(pipe_settle_on);
    run(pipe_settle_on_loading_unchanged);
    run(pipe_settle_on_extends);
    trace_passive();
    puts("adaptive runtime: published-float accumulation, immutable guest anchors, epochs, scene readiness, physical interval1, frozen legacy gates, opt-in 1 s pipeline-burst settle and passive JOB_TRACE scene transitions passed");
    return 0;
}
