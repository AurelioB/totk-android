"""Execute production auto40 clock/reservation and producer policy on a host.
Android callback timestamps and graphics buffers are explicit doubles, not panel
cadence or GPU evidence. No sleeps, device access, or supported-mode inference.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'
GFX = ROOT / 'games/totk/android/gfx'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


class AndroidFps40Pacing(unittest.TestCase):
    def compile_probe(self, source, directory):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('A native C compiler is required')
        path = Path(directory) / 'probe.c'
        executable = Path(directory) / 'probe'
        path.write_text(source)
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        self.command([*compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                      '-I', str(ROOT), str(path), *flags, '-o', str(executable)])
        return executable

    def command(self, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_observed_clock_backend_and_launch_overrides(self):
        frontend = (RUNTIME / 'frontend_android.c').read_text()
        backend = (GFX / 'vk_draw.c').read_text()
        source = r'''
#define _GNU_SOURCE
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include "games/totk/runtime/present_clock.h"
#include "games/totk/android/gfx/vk_present_slots.h"
#include "games/totk/android/gfx/display_clock.h"
#include "games/totk/android/gfx/display_pace.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOGV(...) fprintf(stderr, __VA_ARGS__)
static int present_slots_on, present_slots_auto, display_refresh_observed;
static pthread_mutex_t display_mu = PTHREAD_MUTEX_INITIALIZER;
static display_pacer display_clock;
static uint64_t now = 1000000000, invalidations;
static uint64_t runtime_now_ns(void) { return now; }
static uint64_t host_now_ns(void) { return now; }
static void present_timeline_invalidate_locked(void) { invalidations++; }
'''
        source += section(frontend, 'static int64_t display_period_ns =', 'static unsigned present_pending;')
        source += section(frontend, 'int runtime_android_present_clock(', 'int runtime_android_present_timeline_active(')
        source += section(frontend, 'static void display_refresh(', 'static void display_frame_observe(')
        source += 'static void configure_slots(const char *slots) {\n' + section(
            frontend, '    int slot_mode = vk_present_slots_config(slots);', '    present_timeline_configure();') + '}\n'
        source += r'''
typedef struct {
    uint64_t generation, desired_ns, copy_open_ns, refresh_ns, slot_skipped;
    uint32_t interval;
} pres_buf;
static vk_present_interp g_pres_interp = {.retained = -1};
static int vk_fg_adaptive(void) { return 0; }
static int runtime_fps_hybrid_selected(void) { return 0; }
static int runtime_fps_hybrid_interpolating(void) { return 0; }
static uint64_t fg_adaptive_first_slot(uint64_t callback, uint64_t refresh, uint64_t time) {
    (void)callback; (void)refresh; (void)time; assert(0); return 0;
}
'''
        source += section(backend, 'static void pres_slot_reserve(', '/* Only admitted locked AHB pairs')
        source += r'''
static pres_buf picture(double period) {
    pres_buf b = {.generation = 1, .desired_ns = now};
    pres_slot_reserve(&b, period);
    return b;
}
static void callback(uint64_t frame) { assert(display_pace_tick(&display_clock, frame)); now = frame + 1; }
static void legacy(pres_buf b) { assert(!b.interval && !b.refresh_ns && !b.copy_open_ns); }
int main(int argc, char **argv) {
    assert(argc == 2);
    const uint64_t hz120 = 8333333, hz60 = 16666667;
    const char *scenario = argv[1];
    const char *slots = !strcmp(scenario, "fixed") ? "1" : !strcmp(scenario, "off") ? "0" : "auto";
    assert(!setenv("TOTK_PRESENT_SLOTS", slots, 1));
    assert(!setenv("TOTK_UNCAPPED", !strcmp(scenario, "uncapped0") ? "0" : "1", 1));
    assert(!setenv("TOTK_UNCAPPED_HZ", !strcmp(scenario, "cap55") ? "55" : "40", 1));
    /* A validated request is deliberately not observed refresh evidence. */
    assert(!setenv("TOTK_FRAME_RATE_HINT", "1", 1));
    assert(!setenv("TOTK_FRAME_RATE_HINT_HZ", "120", 1));
    configure_slots(slots);
    callback(now);
    uint64_t frame = 77, refresh = 88;
    if (!strcmp(scenario, "fixed")) {
        assert(runtime_android_present_clock(&frame, &refresh) && refresh == hz60);
        pres_buf b = picture(1.0 / 30);
        assert(b.interval == 2 && b.refresh_ns == hz60); /* Legacy slot1 unchanged. */
        return 0;
    }
    if (strcmp(scenario, "auto")) {
        display_refresh((int64_t)hz120, NULL);
        callback(now + hz120);
        legacy(picture(0.025)); /* slot0 and developer uncapped0/cap55 opt out. */
        return 0;
    }
    assert(!runtime_android_present_clock(&frame, &refresh) && frame == 77 && refresh == 88);
    legacy(picture(0.025)); /* Callback plus assumed startup60 is still unknown. */
    display_refresh((int64_t)hz60, NULL);
    assert(!runtime_android_present_clock(&frame, &refresh)); /* First matching60 declaration needs new phase. */
    callback(now + hz60);
    assert(runtime_android_present_clock(&frame, &refresh) && refresh == hz60);
    legacy(picture(0.025)); /* Hint120 does not make actual60 become120. */
    display_refresh((int64_t)hz120, NULL);
    assert(!runtime_android_present_clock(&frame, &refresh)); /* Old60 phase cannot acquire period120. */
    callback(now + hz120);
    assert(runtime_android_present_clock(&frame, &refresh) && refresh == hz120);
    pres_buf queued = picture(0.025), saved = queued;
    assert(queued.interval == 3 && queued.refresh_ns == hz120);
    assert(queued.desired_ns == display_clock.frame_ns + 3 * hz120);
    assert(queued.copy_open_ns == queued.desired_ns);
    display_refresh((int64_t)hz60, NULL);
    assert(!runtime_android_present_clock(&frame, &refresh));
    pres_buf gap = picture(0.025); legacy(gap);
    assert(gap.desired_ns > queued.desired_ns); /* No old future target overtaken. */
    callback(now + hz60);
    pres_buf sixty = picture(0.025); legacy(sixty);
    assert(sixty.desired_ns > gap.desired_ns);
    for (unsigned i = 0; i < 120; i++) {
        callback(now + 25000000);
        sixty = picture(0.025); legacy(sixty);
        assert(sixty.desired_ns == now); /* No rounded30fps lattice or accumulated slot phase. */
    }
    display_refresh((int64_t)hz120, NULL);
    assert(!runtime_android_present_clock(&frame, &refresh));
    callback(now + hz120);
    pres_buf resumed = picture(0.025);
    assert(resumed.interval == 3 && resumed.desired_ns >= sixty.desired_ns + 3 * hz120);
    now = display_clock.frame_ns + 2 * hz120;
    assert(runtime_android_present_clock(&frame, &refresh));
    now++;
    assert(!runtime_android_present_clock(&frame, &refresh)); /* Stale callback: no blocking snapshot. */
    pres_buf stale = picture(0.025); legacy(stale);
    assert(stale.desired_ns > resumed.desired_ns);
    callback(now + hz120);
    resumed = picture(0.025);
    assert(resumed.interval == 3 && resumed.desired_ns >= stale.desired_ns + 3 * hz120);
    assert(!memcmp(&queued, &saved, sizeof saved));
    assert(invalidations == 3);
    (void)display_callback_observation;
    (void)display_fence_observation;
    (void)display_callback_measured_ns;
    (void)display_callback_calibrated;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='android-fps40-') as directory:
            probe = self.compile_probe(source, directory)
            for scenario in ('auto', 'uncapped0', 'cap55', 'off', 'fixed'):
                with self.subTest(scenario=scenario):
                    self.command([str(probe), scenario])

    def test_existing_producer_remains_twenty_five_ms_at_actual_sixty_and_120(self):
        producer = (NVN / 'hle_nvn.c').read_text()
        stubs = (GFX / 'nvn_stubs.c').read_text()
        source = r'''
#define _GNU_SOURCE
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "games/totk/android/gfx/vk_present_slots.h"
#include "games/totk/android/gfx/acquire_slack.h"
#include "games/totk/android/gfx/predict_pace.h"
#define LOG(...) fprintf(stderr, __VA_ARGS__)
enum { MFX_OFF, MFX_HALF, MFX_30, MFX_MAX };
#define UNCAPPED_HZ 120
typedef struct { int interval; } win_t;
static int g_uncapped, g_paced_hz, g_hz = -1, g_display_mode;
static int g_use_guest_ticks;
static unsigned g_fps_auto_ticks;
static uint64_t actual_refresh;
static int mfx_enabled(void) { return MFX_OFF; }
static int display_pace, acquire_vsync, fps_auto;
static int dm_detclock_enabled(void) { return 0; }
static int acquire_clock_on(void) { return acquire_vsync; }
static int runtime_fps_adaptive_selected(void) { return 0; }
static int runtime_android_fps_auto_active(void) { return fps_auto; }
static int runtime_fps_hybrid_selected(void) { return 0; }
static uint64_t runtime_android_acquire_period(unsigned interval) { return acquire_vsync ? actual_refresh * interval : 0; }
static uint64_t runtime_android_display_period(unsigned interval) { return actual_refresh * interval; }
static int runtime_android_display_pace_active(void) { return display_pace; }
static int runtime_android_display_hz(void) { return (int)((1000000000 + actual_refresh / 2) / actual_refresh); }
static unsigned (*runtime_cutscene_present_ticks)(unsigned, uint64_t);
static int uncapped_interval_on(void) { return atoi(getenv("TOTK_UNCAPPED_INTERVAL")) != 0; }
static int scripted_camera, cutscene_calls;
static unsigned cutscene_floor(unsigned ticks, uint64_t refresh) {
    cutscene_calls++;
    /* Same physical-tick rounding as the runtime authority: a25ms tick would
     * round the30fps floor back to1. The producer must ask in guest60 units. */
    assert(refresh == 1000000000ull / (unsigned)(g_hz == 40 ? 60 : g_hz));
    unsigned floor = (unsigned)((1000000000ull / 30 + refresh / 2) / refresh);
    return scripted_camera && ticks < floor ? floor : ticks;
}
'''
        source += section(stubs, 'int mtl_display_hz(void) { return runtime_android_display_pace_active()', 'int mtl_link_wait_tick(')
        source += section(producer, 'static int uncapped_now(void)', '/* 30+30 on a display')
        source += section(producer, 'static int mfx_own_clock(void)', '/* Opt-in until live scene/clock')
        source += section(producer, 'static int uncapped_cap(', 'void nvn_present_config(')
        source += section(producer, 'static void present_cfg(', '/* Exercise production policy,')
        source += r'''
int main(void) {
    assert(!unsetenv("DM_PRESENT_HZ") && !unsetenv("DM_PRESENT_INTERVAL"));
    assert(!setenv("TOTK_REALTIME", "1", 1) && !setenv("TOTK_FPS60", "1", 1));
    assert(!setenv("TOTK_UNCAPPED", "1", 1) && !setenv("TOTK_UNCAPPED_HZ", "40", 1));
    assert(!setenv("TOTK_UNCAPPED_INTERVAL", "1", 1));
    win_t window = {.interval = 2};
    const uint64_t refreshes[] = {16666667, 8333333, 16666667, 8333333};
    for (unsigned i = 0; i < sizeof refreshes / sizeof *refreshes; i++) {
        actual_refresh = refreshes[i];
        if (i < 2) g_hz = -1; /* Independent launch at60/120, then live changes. */
        assert(present_period(&window) == 25000000 && g_hz == 40 && uncapped_now());
        assert(present_ticks(&window) == 1 && !g_display_mode);
    }
    runtime_cutscene_present_ticks = cutscene_floor;
    scripted_camera = 1; window.interval = 2;
    for (unsigned i = 0; i < 2; i++) {
        actual_refresh = refreshes[i];
        vk_present_slots slots = {0};
        uint64_t previous = 0;
        for (unsigned frame = 0; frame < 600; frame++) {
            window.interval = frame & 1 ? 1 : 2;
            cutscene_calls = 0;
            uint64_t period = present_period(&window);
            assert(period == 33333333 && cutscene_calls == 1);
            unsigned interval = i ? 4 : 2;
            uint64_t callback = 1000000000ull + (uint64_t)frame * interval * actual_refresh;
            vk_present_slot picture = vk_present_slots_auto40_reserve(&slots, period / 1e9,
                callback, actual_refresh, callback + 1);
            assert(picture.mode == VK_PRESENT_SLOTS_ACTIVE && picture.interval == interval);
            if (previous) assert(picture.desired_ns - previous == interval * actual_refresh);
            previous = picture.desired_ns;
        }
        scripted_camera = 0;
        assert(present_period(&window) == 25000000); /* Completion/expiry releases the floor immediately. */
        vk_present_slot resumed = vk_present_slots_auto40_reserve(&slots, present_period(&window) / 1e9,
            previous, actual_refresh, previous + 1);
        assert(resumed.interval == (i ? 3 : 0) && resumed.desired_ns > previous);
        scripted_camera = 1;
    }
    assert(!setenv("TOTK_UNCAPPED_INTERVAL", "0", 1));
    window.interval = 2;
    assert(present_period(&window) == 50000000); /* A slower explicit guest interval is not shortened. */
    assert(!setenv("TOTK_UNCAPPED_INTERVAL", "1", 1));
    runtime_cutscene_present_ticks = cutscene_floor;
    /* Unlimited keeps the same producer path, independent of panel callbacks:
     * no fixed slots or acquire slack, interval1, ceiling120, scripted-camera30. */
    assert(!setenv("TOTK_UNCAPPED_HZ", "120", 1)); g_hz = -1;
    scripted_camera = 0;
    for (unsigned i = 0; i < 2; i++) {
        actual_refresh = refreshes[i];
        assert(present_period(&window) == 1000000000ull / 120 && g_hz == 120 && uncapped_now());
        window.interval = 1;
        assert(present_period(&window) == 1000000000ull / 120);
        scripted_camera = 1;
        for (unsigned frame = 0; frame < 600; frame++) {
            window.interval = frame & 1 ? 1 : 2;
            cutscene_calls = 0;
            assert(present_period(&window) == 33333333 && cutscene_calls == 1);
        }
        scripted_camera = 0;
        assert(present_period(&window) == 1000000000ull / 120);
    }
    runtime_cutscene_present_ticks = NULL;
    assert(!setenv("TOTK_UNCAPPED_HZ", "55", 1)); g_hz = -1;
    assert(present_period(&window) == 1000000000 / 55 && g_hz == 55); /* Developer cap wins. */
    window.interval = 2;
    assert(!setenv("TOTK_UNCAPPED", "0", 1)); g_hz = -1;
    /* Guest intervals are 60Hz units even on a120Hz panel. The same bug
     * made the menu's30fps choice run at60 after a touch boosted refresh. */
    runtime_cutscene_present_ticks = NULL;
    for (unsigned mode = 0; mode < 2; mode++) {
        assert(!setenv("TOTK_FPS60", mode ? "1" : "0", 1));
        for (unsigned display = 0; display < 2; display++) {
            actual_refresh = display ? 8333333 : 16666667; g_hz = -1;
            for (unsigned guest = 1; guest <= 3; guest++) {
                window.interval = guest;
                assert(present_ticks(&window) == guest * (display ? 2 : 1));
                uint64_t period = present_period(&window);
                assert(period == guest * (display ? 2 : 1) * actual_refresh && !uncapped_now());
            }
        }
    }
    /* Do not reset the configured producer across observed mode changes.
     * Frame lag is a submission/fence policy, not a guest tick multiplier. */
    const unsigned displays[] = {60, 90, 100, 120, 144, 60, 120, 60};
    const unsigned slots[][2] = {{1,2}, {2,3}, {2,4}, {2,4}, {3,5}, {1,2}, {2,4}, {1,2}};
    for (unsigned lag = 0; lag <= 1; lag++) {
        assert(!setenv("TOTK_FRAME_LAG", lag ? "1" : "0", 1));
        for (unsigned mode = 0; mode < 2; mode++) {
            assert(!setenv("TOTK_FPS60", mode ? "1" : "0", 1)); g_hz = -1;
            acquire_slack_policy slack = {0};
            predict_pacer predict = {0};
            vk_present_slots reservations = {0};
            uint64_t callback = 1000000000ull;
            for (unsigned d = 0; d < sizeof displays / sizeof *displays; d++) {
                actual_refresh = (1000000000ull + displays[d] / 2) / displays[d];
                for (unsigned guest = 1; guest <= 2; guest++) {
                    window.interval = guest;
                    unsigned ticks = slots[d][guest - 1];
                    uint64_t period = present_period(&window);
                    assert(present_ticks(&window) == ticks && period == ticks * actual_refresh);
                    vk_present_slot slot = vk_present_slots_reserve(&reservations, 1,
                        callback, actual_refresh, ticks, callback);
                    assert(slot.mode == VK_PRESENT_SLOTS_ACTIVE && slot.interval == ticks);
                    assert(vk_present_slots_interval(period / 1e9, actual_refresh) == ticks);
                    acquire_slack_observe(&slack, slot.desired_ns, period);
                    assert(slack.target_ns == period);
                    predict_decision decision = predict_pacer_choose(&predict, slot.desired_ns, period);
                    assert(decision.period_ns == period && decision.lead_ns == period);
                    assert(decision.release_ns == slot.desired_ns - 2 * period);
                    callback = slot.desired_ns + period;
                }
            }
        }
    }
    /* Suspended FPS Auto still owns physical units on non-60Hz panels.
     * active here is the configured/active flag, with no auto tick output. */
    actual_refresh = 8333333; g_hz = -1; window.interval = 2; fps_auto = 1;
    assert(present_ticks(&window) == 2 && present_period(&window) == 16666666);
    fps_auto = 0;
    assert(present_ticks(&window) == 4 && present_period(&window) == 33333332);
    for (unsigned policy = 0; policy < 2; policy++) {
        display_pace = !policy; acquire_vsync = policy; g_hz = -1;
        assert(present_ticks(&window) == 2 && present_period(&window) == 16666666);
        assert(!g_use_guest_ticks);
    }
    display_pace = acquire_vsync = 0;
    /* Calibrated callback/fence clocks have small jitter. A60Hz panel must
     * not acquire a third slot when its measured period is16.65ms. */
    actual_refresh = 16650000; g_hz = -1; window.interval = 2;
    assert(present_ticks(&window) == 2);
    actual_refresh = 8325000; g_hz = -1;
    assert(present_ticks(&window) == 4);
    /* Locked frame generation supplies a 60 Hz profile and owns its interval. */
    assert(!setenv("DM_PRESENT_HZ", "60", 1)); g_hz = -1;
    assert(present_ticks(&window) == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='android-fps40-producer-') as directory:
            self.command([str(self.compile_probe(source, directory))])


if __name__ == '__main__':
    unittest.main()
