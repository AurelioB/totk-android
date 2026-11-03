"""TOTK_AUTOLOAD in the Android app (games/totk/runtime/autoload.c): the app reads settings.env and totk.env in
start_game, long after the library's constructors ran at System.loadLibrary, so the setting must be read by an explicit
runtime_autoload_start after those files, not by a constructor. The desktop / command-line build keeps reading it at
load. autoload.c is compiled both ways with a driver standing in for the controller (A presses are counted).
CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
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

DRIVER = r'''
#include "runtime.h"
#include "autoload.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "fail: %s\n", s); exit(3); }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static _Atomic int presses;
void runtime_input_key_synthetic(unsigned code, int down) { if (code == 38 && down) presses++; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "late")) {   /* what start_game does: the environment files first, then the start */
        setenv("TOTK_AUTOLOAD", getenv("LATE_VALUE") ? getenv("LATE_VALUE") : "1", 1);
        setenv("TOTK_AUTOLOAD_PERIOD_MS", "300", 1);
        runtime_autoload_start();
        runtime_autoload_start();   /* idempotent */
    } else if (!strcmp(argv[1], "start")) runtime_autoload_start();
    runtime_autoload_save_file_opened("save:/slot_00/caption.sav");
    usleep(800000);
    printf("%d\n", (int)presses);
    return 0;
}
'''


class AutoloadStart(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='autoload-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        (temp / 'driver.c').write_text(DRIVER)
        cls.binary = {}
        for name, flags in (('app', ['-DTOTK_ANDROID_APP']), ('desktop', [])):
            out = temp / name
            result = subprocess.run([*cc, '-std=gnu11', '-D_GNU_SOURCE', *flags, '-I', str(RUNTIME), str(temp / 'driver.c'),
                                     str(RUNTIME / 'autoload.c'), '-o', str(out), '-lpthread'], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binary[name] = out

    def presses(self, build, mode, **env):
        clean = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        clean.update(env)
        run = subprocess.run([str(self.binary[build]), mode], env=clean, capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        return int(run.stdout)

    def test_app_reads_the_setting_after_its_environment_files(self):
        self.assertGreaterEqual(self.presses('app', 'late'), 2)

    def test_app_has_no_constructor_that_would_read_it_too_early(self):
        self.assertEqual(self.presses('app', 'none', TOTK_AUTOLOAD='1', TOTK_AUTOLOAD_PERIOD_MS='300'), 0)

    def test_zero_means_off(self):
        self.assertEqual(self.presses('app', 'late', LATE_VALUE='0'), 0)

    def test_desktop_still_reads_it_at_load(self):
        self.assertGreaterEqual(self.presses('desktop', 'none', TOTK_AUTOLOAD='1', TOTK_AUTOLOAD_PERIOD_MS='300'), 2)
        # The ordering that broke the app: set after load, the constructor has already decided.
        self.assertEqual(self.presses('desktop', 'late'), 0)

    def test_start_game_starts_it_after_the_environment_files(self):
        frontend = (RUNTIME / 'frontend_android.c').read_text()
        start = frontend[frontend.index('static int start_game('):]
        self.assertLess(start.index('env_file(p);'), start.index('runtime_autoload_start();'))
        self.assertLess(start.index('runtime_autoload_start();'), start.index('pthread_create(&t, &at, game_thread'))


if __name__ == '__main__':
    unittest.main()
