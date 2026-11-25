"""Production fallback logging: three fixed classes, one line globally per rolling minute, concurrent-safe."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/runtime/companion_snapshot.c'


class CompanionFallbackLogging(unittest.TestCase):
    def test_global_minute_limit_classification_and_concurrent_claims(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('host C compiler required')
        source = SOURCE.read_text()
        start = source.index('static const char *layer_reject(')
        end = source.index('static void grid_axis(', start)
        prelude = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static uint64_t now_ms;
static int clock_failed;
static int sample_clock(clockid_t id, struct timespec *ts) {
    assert(id == CLOCK_MONOTONIC);
    ts->tv_sec = now_ms / 1000; ts->tv_nsec = (now_ms % 1000) * 1000000;
    return clock_failed ? -1 : 0;
}
#define clock_gettime sample_clock
'''
        tests = r'''
static _Atomic(unsigned) accepted;
static void *claim(void *unused) {
    (void)unused;
    for (int k = 0; k < 100; k++)
        if (layer_fallback_due(180000)) atomic_fetch_add(&accepted, 1);
    return 0;
}
int main(void) {
    const char *failure = "unreadable";
    assert(!layer_reject(&failure, "validation") && !strcmp(failure, "validation"));
    log_layer_fallback(failure);                 /* first failure prints immediately, including time zero */
    log_layer_fallback("changed");               /* same interval is shared by all classes */
    now_ms = 59999; log_layer_fallback("unreadable");
    now_ms = 60000; log_layer_fallback("unreadable");
    now_ms = 119999; log_layer_fallback("validation");
    now_ms = 120000; log_layer_fallback("changed");
    now_ms = 0; log_layer_fallback("validation"); /* backwards time cannot reopen a window */
    clock_failed = 1; now_ms = 180000; log_layer_fallback("validation"); clock_failed = 0;
    assert(!layer_fallback_due(179999));
    pthread_t threads[16];
    for (unsigned k = 0; k < 16; k++) assert(!pthread_create(&threads[k], 0, claim, 0));
    for (unsigned k = 0; k < 16; k++) assert(!pthread_join(threads[k], 0));
    assert(atomic_load(&accepted) == 1);
    assert(!layer_fallback_due(239999));
    assert(layer_fallback_due(240000));
}
'''
        with tempfile.TemporaryDirectory(prefix='companion-map-log-') as directory:
            work = Path(directory)
            probe = work / 'probe.c'
            probe.write_text(prelude + source[start:end] + tests)
            compiled = subprocess.run(list(map(str, [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                                                     '-pthread', probe, '-o', work / 'probe'])),
                                      capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([work / 'probe'], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stderr.splitlines(), [
                '[companion] map layer: environment not as expected, using MapData.CurrentLayer reason=' + reason
                for reason in ('validation', 'unreadable', 'changed')])
