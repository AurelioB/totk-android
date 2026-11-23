"""Production timeline/fence bookkeeping and nonblocking contention, no GPU/device.

CC may name a ThreadSanitizer compiler command. The report oracle is main
8bbbce5's deterministic three-frame output; only documented additive fields
are removed before comparing its exact SHA-256.
"""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'


class FrameTimeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not shutil.which(compiler[0]):
            raise unittest.SkipTest('C compiler required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='frame-timeline-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for name in ('frame_timeline', 'frame_fence'):
            binary = Path(cls.temp.name) / name
            subprocess.run([*compiler, '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-D_GNU_SOURCE', '-DTOTK_FRAME_TIMELINE_TEST', '-pthread',
                            str(GAME / f'android/gfx/test_{name}.c'),
                            str(GAME / 'runtime/frame_timeline.c'),
                            str(GAME / 'runtime/frame_fence.c'), '-lm', '-o', str(binary)], check=True)
            cls.binaries[name] = binary

    def fixture(self, name, *args):
        result = subprocess.run([str(self.binaries[name]), *args], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout.decode() + result.stderr.decode())
        return result.stdout

    def test_enabled_content_and_stalled_reporter(self):
        text = self.fixture('frame_timeline').decode()
        self.assertIn('stalled reporter, bounded producers, counted losses and coherent snapshots passed', text)

    def test_disabled(self):
        self.assertIn(b'disabled path passed', self.fixture('frame_timeline', 'off'))
        self.fixture('frame_fence', 'off')

    def test_fence_content(self):
        self.fixture('frame_fence')

    def test_deterministic_report_matches_original(self):
        report = self.fixture('frame_timeline', 'report')
        report = re.sub(rb' dropped_events=0 inconsistent_samples=0', b'', report)
        report = re.sub(rb' rec_wall_ns=[0-9.]+', b'', report)
        self.assertEqual(hashlib.sha256(report).hexdigest(),
                         'dfbac65547cd15fa0f97267090e4d40c39230188a350729261d8ac0c3d0ee307')
