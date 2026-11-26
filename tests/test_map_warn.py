"""Production policy, sampler and overlay CPU checks; no Android/GPU claims."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'

PROBE = r'''
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
static _Atomic unsigned created;
static int observed_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*fn)(void *), void *arg) {
    atomic_fetch_add(&created, 1);
    return pthread_create(thread, attr, fn, arg);
}
#define pthread_create observed_create
#include "nvn_dirty.c"
#undef pthread_create
#include "vk_overlay.h"
int runtime_handheld_mode(void) { return 1; }
int vk_wb_alias_enabled(void) { return 0; } /* This warning-only fixture does not link the Vulkan pool backend. */
static void policy(void) {
    nvn_map_warn_thresholds t = {80,88,84};
    nvn_map_warn_state s = {0};
    const struct { uint64_t count; int notice; unsigned logs; } cases[] = {
        {52423,0,0}, {52424,0,1}, {52424,0,0}, {57666,0,0}, {57667,1,2},
        {65530,1,0}, {55046,1,0}, {55045,0,0}, {57666,0,0}, {57667,1,0},
        {0,0,4}, {0,0,0}, {57667,1,0}, {1,0,0}
    };
    for (unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        nvn_map_warn_result r=nvn_map_warn_step(s,cases[i].count,65530,t);
        assert(r.state.notice==cases[i].notice && r.logs==cases[i].logs); s=r.state;
    }
    assert(nvn_map_warn_boundary(UINT64_MAX,100)==UINT64_MAX);
    assert(nvn_map_warn_boundary(101,80)==81);
    assert(nvn_map_warn_step((nvn_map_warn_state){0},88,100,t).state.notice);
    assert(!nvn_map_warn_step((nvn_map_warn_state){.notice=1},83,100,t).state.notice);
    assert(!nvn_map_warn_step((nvn_map_warn_state){.notice=1},88,0,t).state.notice);
    assert(vma_read("/no-such-map-warn-file",1)==0);
    map_warn_sample(57667,0); assert(nvn_dirty_map_warn_notice()); /* Linux default */
    map_warn_sample(0,0); assert(!nvn_dirty_map_warn_notice());
    map_warn_sample(0,0); assert(!nvn_dirty_map_warn_notice());
}
static void panel(void) {
    for (int mode=0;mode<3;mode++) for (int fg=0;fg<2;fg++) {
        vk_overlay_panel p={.map_notice=1,.notice_only=!mode,.compact=mode==1,.system=1};
        assert(vk_overlay_configure(&p,1920,1080,fg,1,"tr"));
        assert(p.layout.rows<=VK_OVERLAY_MAX_ROWS);
        assert(p.layout.notice_row+2==p.layout.rows);
        assert(p.layout.width>=25*VK_OVERLAY_CELL_W+2*VK_OVERLAY_PAD);
        if (!mode) assert(p.layout.rows==2 && !p.layout.graph_w);
        uint32_t pixels[VK_OVERLAY_W*VK_OVERLAY_MAX_H];
        vk_overlay_raster(&p,1,pixels);
        unsigned at=(VK_OVERLAY_PAD+p.layout.notice_row*VK_OVERLAY_ROW_H)*p.layout.width;
        unsigned lit=0;
        for (unsigned i=at;i<at+VK_OVERLAY_GLYPH_H*p.layout.width;i++) lit+=pixels[i]!=pixels[0];
        assert(lit>0); /* actual font raster, not just a reserved blank row */
        p.map_notice=0; p.notice_only=0;
        assert(vk_overlay_configure(&p,1920,1080,fg,1,"tr"));
        assert(!p.layout.map_notice);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    if (!strcmp(argv[1],"policy")) { policy(); panel(); return 0; }
    if (!strcmp(argv[1],"marker")) { vk_overlay_mode(); vk_overlay_mode(); return 0; }
    if (!strcmp(argv[1],"test-sequence")) {
        map_warn_configure(); /* pure sampling test: no worker is created */
        assert(g_map_warn_enabled && !atomic_load(&created));
        for (unsigned sample=1;sample<=30;sample++) {
            map_warn_sample(8542,65530); /* unchanged, safely low real-count representative */
            if (g_map_warn_mode==3) {
                assert(nvn_dirty_map_warn_notice()==(sample<=24));
                assert(g_map_warn_thresholds.notice_percent==(sample<=12 ? 3u : 100u));
                assert(g_map_warn_thresholds.clear_percent==(sample<=24 ? 2u : 99u));
                assert(g_map_warn_test_samples==(sample<25 ? sample : 25));
                if (sample>=13 && sample<=24)
                    assert(8542<nvn_map_warn_boundary(65530,g_map_warn_thresholds.notice_percent));
            } else {
                assert(!nvn_dirty_map_warn_notice() && !g_map_warn_test_samples);
                assert(g_map_warn_thresholds.log_percent==80 && g_map_warn_thresholds.notice_percent==88 &&
                       g_map_warn_thresholds.clear_percent==84);
            }
        }
        assert(nvn_map_warn_test_thresholds(UINT_MAX).clear_percent==99);
        return 0;
    }
    if (!strcmp(argv[1],"config")) {
        nvn_dirty_map_warn_set_frame_origin(1000000000);
        assert(!atomic_load(&created)); /* publishing the origin never starts the sampler */
        int enabled=nvn_dirty_map_warn_init();
        printf("%d %u %u %u %u %d\n",enabled,atomic_load(&created),
            g_map_warn_thresholds.log_percent,g_map_warn_thresholds.notice_percent,
            g_map_warn_thresholds.clear_percent,nvn_dirty_map_warn_notice()); return 0;
    }
    if (!strcmp(argv[1],"worker")) {
        nvn_dirty_map_warn_set_frame_origin(1000000000);
        assert(atomic_load(&g_map_warn_frame_origin)==(g_map_warn_mode>=2 ? 1000000000u : 0));
        assert(nvn_dirty_map_warn_init()); assert(!nvn_dirty_lag_enabled());
        assert(atomic_load(&created)==1);
        uint64_t start=vma_clock_ns(), stats[4]={0};
        do { usleep(10000); nvn_dirty_lag_vma_scan_stats(stats,0); }
        while (!stats[0] && vma_clock_ns()-start<15000000000ull);
        assert(stats[0]==1 && stats[2]>0 && g_vma_test_reads==0);
        assert(vma_clock_ns()-start>=4900000000ull);
        assert(g_map_warn_state.logged_early==0); /* real small host count, not a fake threshold */
        printf("worker scans=%llu total_ns=%llu\n",(unsigned long long)stats[0],(unsigned long long)stats[2]);
        return 0;
    }
    if (!strcmp(argv[1],"cost")) {
        const char *value=getenv("MAP_WARN_BENCH_ROWS");
        unsigned rows=value ? strtoul(value,0,10) : 0;
        size_t length=(size_t)rows*getpagesize();
        void *region=NULL;
        if (rows) {
            uint64_t maximum=vma_read("/proc/sys/vm/max_map_count",0);
            assert(maximum>rows+1024 && rows<=60000);
            region=mmap(NULL,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            assert(region!=MAP_FAILED);
            for (unsigned i=0;i<rows;i+=2)
                assert(!mprotect((char *)region+(size_t)i*getpagesize(),getpagesize(),PROT_READ));
        }
        uint64_t start=vma_clock_ns(), count=0, scans=100;
        for (unsigned i=0;i<scans;i++) count=vma_read("/proc/self/maps",1);
        uint64_t elapsed=vma_clock_ns()-start;
        if (region) assert(!munmap(region,length));
        printf("rows=%llu scans=%llu total_ns=%llu\n",(unsigned long long)count,
            (unsigned long long)scans,(unsigned long long)elapsed); return !count;
    }
    return 2;
}
'''


class MapWarning(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('needs a host C compiler')
        scratch = ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='map-warn-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'probe.c'
        source.write_text(PROBE)
        cls.exe = source.with_suffix('')
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        result = subprocess.run([*cc, '-std=gnu11', '-O1', '-D_GNU_SOURCE', '-DGPU_VK',
            '-DTOTK_VK_TESTS', '-DTOTK_DEV_BUILD=0', '-ffunction-sections', '-fdata-sections',
            '-I' + str(GFX), '-I' + str(NVN), '-I' + str(NVN / 'include'),
            '-I' + str(ROOT / 'games/totk/runtime'),
            '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/host'),
            '-I' + str(ROOT / 'games/totk/android/compat/include'),
            '-include', str(ROOT / 'games/totk/android/compat/include/darwin_compat.h'),
            str(source), str(GFX / 'vk_overlay.c'), '-Wl,--gc-sections', '-pthread', '-lm',
            *flags, '-o', str(cls.exe)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_probe(self, mode, **settings):
        env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        env.update(settings)
        return subprocess.run([str(self.exe), mode], env=env, capture_output=True,
                              text=True, check=True, timeout=20)

    def test_boundaries_hysteresis_unknown_and_notice_pixels(self):
        result = self.run_probe('policy')
        self.assertEqual(result.stderr.count('maps count unavailable'), 1)
        self.assertEqual(result.stderr.count('map limit is approaching'), 1)
        self.assertEqual(result.stderr.count('SAVE AND RESTART GAME SOON'), 1)

    def test_effective_switch_marker_is_once_even_when_off(self):
        for value in ('0', '1', '2', '3'):
            result = self.run_probe('marker', TOTK_MAP_WARN=value, TOTK_OVERLAY='off')
            lines = [line for line in result.stderr.splitlines() if line.startswith('[totk-switches]')]
            self.assertEqual(lines, ['[totk-switches] v=1 TOTK_WB_ALIAS=off '
                f'TOTK_MAP_WARN={"off" if value == "0" else "on"} map_warn_mode={value}'])

    def test_off_starts_no_thread_and_has_no_notice(self):
        for value in (None, '0', 'off', 'true', '4', '02'):
            result = self.run_probe('config', **({} if value is None else {'TOTK_MAP_WARN': value}))
            self.assertEqual(result.stdout.strip(), '0 0 80 88 84 0')

    def test_enabled_only_startup_and_threshold_overrides(self):
        self.assertEqual(self.run_probe('config', TOTK_MAP_WARN='1').stdout.strip(), '1 1 80 88 84 0')
        self.assertEqual(self.run_probe('config', TOTK_MAP_WARN='2').stdout.strip(), '1 1 80 88 84 0')
        self.assertEqual(self.run_probe('config', TOTK_MAP_WARN='3',
            TOTK_MAP_WARN_NOTICE_PERCENT='bad').stdout.strip(), '1 1 1 3 2 0')
        valid = dict(TOTK_MAP_WARN='1', TOTK_MAP_WARN_LOG_PERCENT='1',
                     TOTK_MAP_WARN_NOTICE_PERCENT='3', TOTK_MAP_WARN_CLEAR_PERCENT='2')
        self.assertEqual(self.run_probe('config', **valid).stdout.strip(), '1 1 1 3 2 0')
        for name, value in (('TOTK_MAP_WARN_LOG_PERCENT', '101'),
                            ('TOTK_MAP_WARN_NOTICE_PERCENT', 'bad'),
                            ('TOTK_MAP_WARN_CLEAR_PERCENT', '88')):
            result = self.run_probe('config', TOTK_MAP_WARN='1', **{name: value})
            self.assertEqual(result.stdout.strip(), '1 1 80 88 84 0')
            self.assertIn('[map-warn]', result.stderr)

    def test_worker_samples_with_lag_off_and_no_caller_reads(self):
        result = self.run_probe('worker', TOTK_MAP_WARN='1')
        self.assertIn('worker scans=1', result.stdout)
        self.assertNotIn('[map-warn-scan]', result.stderr)

    def test_sampler_trace_has_monotonic_time_duration_rows_and_frame_origin(self):
        before = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        result = self.run_probe('worker', TOTK_MAP_WARN='2')
        after = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        lines = [line for line in result.stderr.splitlines() if '[map-warn-scan]' in line]
        self.assertEqual(len(lines), 1)
        self.assertLess(len(lines[0]), 256)  # one fixed, bounded record, not one per maps row
        fields = dict(re.findall(r'([a-z_]+)=(\d+)', lines[0]))
        self.assertLessEqual(before, int(fields['start_ns']))
        self.assertLessEqual(int(fields['start_ns']), after)
        self.assertGreaterEqual(int(fields['duration_us']), 0)
        self.assertLess(int(fields['duration_us']), (after - before) // 1000)
        self.assertGreater(int(fields['rows']), 0)
        self.assertEqual(fields['frame_origin_ns'], '1000000000')
        self.assertEqual(fields['warn'], '1')
        self.assertEqual(fields['notice'], '0')
        self.assertEqual(fields['test_phase'], '0')
        dirty = (NVN / 'nvn_dirty.c').read_text()
        worker = dirty.split('static void *vma_worker(', 1)[1].split('static unsigned vma_scan_period(', 1)[0]
        self.assertEqual(dirty.count('LOG("[map-warn-scan]'), 1)
        self.assertIn('LOG("[map-warn-scan]', worker)
        self.assertIn('g_map_warn_mode >= 2', worker)

    def test_test_only_sequence_holds_then_clears_without_count_change(self):
        for mode in ('1', '2'):
            result = self.run_probe('test-sequence', TOTK_MAP_WARN=mode)
            self.assertNotIn('[map-warn]', result.stderr)
        result = self.run_probe('test-sequence', TOTK_MAP_WARN='3')
        self.assertEqual(result.stderr.count('map limit is approaching'), 1)
        self.assertEqual(result.stderr.count('SAVE AND RESTART GAME SOON'), 1)
        # Direct policy execution never emits a per-scan record: only the real worker does.
        self.assertNotIn('[map-warn-scan]', result.stderr)

    def test_present_path_only_consumes_cached_notice(self):
        overlay = (GFX / 'vk_overlay.c').read_text()
        record = overlay[overlay.index('int vk_overlay_record('):]
        self.assertIn('map_warn_enabled && nvn_dirty_map_warn_notice()', record)
        self.assertIn('(!mode && !notice)', record)
        self.assertNotRegex(record, r'vma_read|vma_refresh|vma_report|/proc/|fopen\(|open\(')
        report = (GFX / 'nvn_vk.c').read_text().split('void vk_mem_report(void) {', 1)[1].split('\n}', 1)[0]
        self.assertNotRegex(report, r'vma_read|vma_refresh|vma_report\(|/proc/|fopen\(|open\(')
        self.assertIn('nvn_dirty_lag_vma_stats', report)
        dirty = (NVN / 'nvn_dirty.c').read_text()
        getter = dirty.split('int nvn_dirty_map_warn_notice(void) {', 1)[1].split('\n}', 1)[0]
        self.assertIn('atomic_load_explicit', getter)
        self.assertNotRegex(getter, r'pthread_|vma_read|/proc/')
        warning_only = dirty.split('/* No g_vma_mu, g_slot_mu, pool or runtime locks', 1)[1].split('\n        }', 1)[0]
        self.assertNotIn('pthread_mutex_lock', warning_only)


if __name__ == '__main__':
    unittest.main()
