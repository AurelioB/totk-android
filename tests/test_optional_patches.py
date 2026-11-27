"""Optional guest installers on the complete pinned Android rewrite geometry."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/totk/scripts'
WORKSPACE = ROOT.parents[1]
MAIN_NSO = Path(os.environ.get('TOTK_MAIN_NSO', WORKSPACE / 'build/TOTK_build/totk/extracted/v143/exefs/main'))
REWRITE = Path(os.environ.get('TOTK_ANDROID_MAIN_REWRITE', WORKSPACE / 'build/TOTK_build/totk/rewritten-android/v143/main.patched.bin'))
SEEDS = [
    0x105bbcc, 0x105bbd0,
    0xa7ac4, 0x95dd24, 0x6155e4, 0x615660, 0x45c700, 0x2b23c7c, 0x42c3bc, 0xa5a188, 0x2db238, 0xa5a670,
    0xa5a18c, 0xa5a660, 0xa5a664, 0xa5a668, 0xa5a66c, 0xa5a674,
    0x584e84, 0x584e88, 0x584e8c, 0x584e90, 0x584790, 0x584b68, 0xa1bb0, 0xa1bb4, 0x2d02094,
    0x5848a8, 0x5848ac, 0x5848b0, 0x2db2b8, 0x2db354, 0x2db3f0,
]


class OptionalPatches(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(SCRIPTS))
        self.addCleanup(sys.path.remove, str(SCRIPTS))
        import fixture_seeds
        self.seeds = fixture_seeds

    def exercise(self, real):
        with tempfile.TemporaryDirectory(prefix='optional-patches-') as directory:
            directory = Path(directory)
            fixture = self.seeds.build(SCRIPTS / 'v143_optional_patch_fixture.c', directory / 'fixture', standin=not real)
            seed_file = None
            args = []
            if real:
                original = self.seeds.read_main(MAIN_NSO)
                seed_file = self.seeds.write_seeds(directory / 'seeds.bin', original, SEEDS)
                pins = json.loads((ROOT / 'games/totk/assets/expected-v1.4.3.json').read_text())
                self.assertEqual(hashlib.sha256(REWRITE.read_bytes()).hexdigest(), pins['android_rewritten_sha256']['main.patched.bin'])
                self.assertEqual(REWRITE.stat().st_size, 0x451cdd8)
                args = [REWRITE]
            try:
                result = self.seeds.run(fixture, args, seed_file)
            except subprocess.CalledProcessError as error:
                self.fail(f'installer fixture exited {error.returncode}:\n{error.stdout}\n{error.stderr}')
            self.assertIn('5760 combinations/orders, 50 atomic nonfatal refusals', result.stdout)
            for name in ('menu capture fix', 'thermal-step', 'FPS60 hardening', 'DRS60', 'DRS framegen', 'FPS60 hysteresis'):
                self.assertIn(f'{name} skipped:', result.stderr)
            print(result.stdout.strip())

    def test_dataless_combinations_and_atomic_refusals(self):
        self.exercise(False)

    @unittest.skipUnless(importlib.util.find_spec('lz4') and MAIN_NSO.is_file() and REWRITE.is_file(),
                         'player 1.4.3 NSO/lz4 and pinned Android rewrite required')
    def test_real_nso_matrix_and_full_rewrite_startup(self):
        self.exercise(True)
