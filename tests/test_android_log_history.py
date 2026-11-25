"""Exercise the same file descriptors the Android launcher redirects to stdout and stderr."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CAP = 512 * 1024
HEADING = b'[totk-log] ===== Launch '

DRIVER = r'''
#define _GNU_SOURCE
#include "log_history.h"
#include "log_mirror.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int lock, trim_error = 0;
    off_t start;
    int log = log_history_open(argv[1], &lock, &start, &trim_error);
    if (log < 0) { perror("log_history_open"); return 3; }
    if (trim_error) fprintf(stderr, "trim error: %d\n", trim_error);

    log_mirror_tail mirror = {.path = argv[1], .offset = start};
    if (log_mirror_open(&mirror)) return 4;
    char heading[sizeof TOTK_LOG_LAUNCH - 1];
    if (read(mirror.fd, heading, sizeof heading) != sizeof heading ||
        memcmp(heading, TOTK_LOG_LAUNCH, sizeof heading)) return 5;
    close(mirror.fd);
    if (mirror.watch >= 0) close(mirror.watch);

    if (!strcmp(argv[2], "hold")) {
        if (write(STDOUT_FILENO, "ready\n", 6) != 6) return 6;
        char done;
        if (read(STDIN_FILENO, &done, 1) != 1) return 7;
    } else {
        if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) return 8;
        char block[8192];
        ssize_t n;
        while ((n = read(STDIN_FILENO, block, sizeof block)) > 0)
            if (log_history_write(STDERR_FILENO, block, (size_t)n)) return 9;
        if (n < 0) return 10;
    }
    _exit(0);  /* Crash-path diagnostics use the raw descriptor and need no flush. */
}
'''


class AndroidLogHistory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tool = shlex.split(os.environ.get('CC', 'cc'))
        if not tool or not shutil.which(tool[0]) or not Path('/proc/self').exists():
            raise unittest.SkipTest('requires Linux and a native C compiler')
        scratch = ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        temp = tempfile.TemporaryDirectory(prefix='android-log-', dir=scratch)
        cls.addClassCleanup(temp.cleanup)
        cls.directory = Path(temp.name)
        cls.driver = cls.directory / 'driver'
        source = cls.directory / 'driver.c'
        source.write_text(DRIVER)
        subprocess.run([*tool, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'games/totk/runtime',
                        source, '-o', cls.driver], check=True, capture_output=True, text=True)

    def setUp(self):
        temp = tempfile.TemporaryDirectory(dir=self.directory)
        self.addCleanup(temp.cleanup)
        self.log = Path(temp.name) / 'totk.log'

    def launch(self, payload):
        run = subprocess.run([self.driver, self.log, 'launch'], input=payload, capture_output=True, check=True)
        return self.log.read_bytes(), run.stderr

    def test_keeps_ten_launches_in_order_under_the_history_budget(self):
        payload = b'rederer\n' * (CAP // 7 + 2)
        for i in range(12):
            report, _ = self.launch(f'settings-{i:02}\n'.encode() + payload + f'crash-{i:02}'.encode())
        self.assertEqual(report.count(HEADING), 10)
        self.assertEqual(re.findall(rb'settings-(\d\d)', report), [f'{i:02}'.encode() for i in range(2, 12)])
        self.assertEqual(re.findall(rb'crash-(\d\d)', report), [f'{i:02}'.encode() for i in range(2, 12)])
        self.assertEqual(report.count(b'Middle of this launch omitted'), 9)
        self.assertTrue(all(len(HEADING + launch) <= CAP for launch in report.split(HEADING)[1:-1]))
        self.assertGreater(len(report.split(HEADING)[-1]), CAP)
        self.assertTrue(report.endswith(b'crash-11'))

    def test_migrates_legacy_log_without_splitting_utf8(self):
        old = b'old settings\n' + '验证😀\n'.encode() * (CAP // 5) + b'old GPU crash'
        self.log.write_bytes(old)
        report, _ = self.launch(b'fresh settings\n')
        legacy, current = report.decode('utf-8').split(HEADING.decode())[1:]
        self.assertIn('before log history', legacy)
        self.assertIn('old settings', legacy)
        self.assertIn('old GPU crash', legacy)
        self.assertIn('fresh settings', current)

    def test_heading_spanning_scan_buffers_is_recognized(self):
        prefix = b'x' * (16384 - 10) + b'\n'
        self.log.write_bytes(prefix + HEADING + b'1 =====\nrun before latest\n')
        report, _ = self.launch(b'latest\n')
        self.assertEqual(report.count(HEADING), 3)
        self.assertIn(b'run before latest\n', report)

    def test_failed_compaction_preserves_the_existing_log(self):
        old = b'old failure\n'
        self.log.write_bytes(old)
        self.log.with_suffix('.log.tmp').mkdir()
        report, stderr = self.launch(b'new failure\n')
        self.assertIn(b'trim error', stderr)
        self.assertTrue(report.startswith(old))
        self.assertTrue(report.endswith(b'new failure\n'))

    def test_parallel_open_cannot_replace_the_active_log(self):
        with subprocess.Popen([self.driver, self.log, 'hold'], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as first:
            try:
                self.assertEqual(first.stdout.readline(), b'ready\n')
                before = self.log.read_bytes()
                second = subprocess.run([self.driver, self.log, 'launch'], input=b'unexpected', capture_output=True)
                self.assertEqual(second.returncode, 3)
                self.assertEqual(self.log.read_bytes(), before)
            finally:
                first.stdin.write(b'1')
                first.stdin.flush()
            self.assertEqual(first.wait(timeout=5), 0)
