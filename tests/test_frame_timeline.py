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

    def test_actual_walk_and_assembly_probes_stay_off(self):
        binary = os.environ.get('TOTK_VK_TIMELINE_TEST')
        seed = os.environ.get('TOTK_SHADER_SEED_DIR')
        if not binary or not seed:
            self.skipTest('Set TOTK_VK_TIMELINE_TEST and TOTK_SHADER_SEED_DIR for production Vulkan probe gates')
        binary = Path(binary).resolve()
        seed = Path(seed).resolve()
        self.assertTrue(binary.is_file() and os.access(binary, os.X_OK), str(binary))
        self.assertTrue((seed / '.done').is_file(), 'complete owned shader seed required')
        env = {key: value for key, value in os.environ.items() if not key.startswith(('TOTK_', 'DM_'))}
        env.update(TOTK_SHADER_SEED_DIR=str(seed), TOTK_VK_REC_THREADS='2',
                   TOTK_FRAME_LAG='1', TOTK_VK_WRITEBACK='1', TOTK_VK_CHUNK_DRAWS='600',
                   TOTK_VK_COMPILE_THREADS='0')
        outputs = []
        for timeline in ('0', '1'):
            for profile in ('0', '1'):
                with self.subTest(timeline=timeline, profile=profile):
                    output = Path(self.temp.name) / f'identity-{timeline}-{profile}.bin'
                    run = subprocess.run([str(binary), '--rec-identity', str(output)],
                                         cwd=self.temp.name, capture_output=True, text=True, timeout=180,
                                         env=dict(env, TOTK_FRAME_TIMELINE=timeline, TOTK_VK_REC_PROFILE=profile))
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn(f'frame-tl probe-check: enabled={timeline} ', run.stdout)
                    if timeline == '0':
                        self.assertIn('enabled=0 walk=0 assembly=0', run.stdout)
                    outputs.append(output.read_bytes())
        self.assertTrue(all(data == outputs[0] for data in outputs), 'off/on profiler/timeline pixels and data differ')
