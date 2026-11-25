"""RomFS proof reuse with the production manifest loader and synthetic inputs."""
from hashlib import sha256
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
COMPILER = shutil.which('cc')


@unittest.skipUnless(sys.platform.startswith('linux') and COMPILER, 'needs a Linux C compiler')
class RomfsOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix='totk-overlay-probe-')
        cls.addClassCleanup(tmp.cleanup)
        cls.probe = Path(tmp.name) / 'overlay-probe'
        build = subprocess.run([
            COMPILER, '-std=gnu11', '-D_GNU_SOURCE', '-ffunction-sections', '-fdata-sections',
            '-include', str(TOTK / 'android/compat/include/darwin_compat.h'),
            '-I', str(TOTK / 'android/compat/include'), '-I', str(TOTK / 'runtime'),
            '-I', str(TOTK / 'third_party/d2r_gfx/gfx/nvn/include'),
            '-Wl,--gc-sections', str(ROOT / 'tests/test_romfs_overlay.c'),
            str(TOTK / 'android/gfx/host_test_support.c'), '-o', str(cls.probe),
        ], text=True, capture_output=True)
        if build.returncode:
            raise AssertionError(build.stderr)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='totk-overlay-data-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.base = self.root / 'base'
        self.base.write_bytes(b'base00')
        self.mod = self.root / 'mod'
        self.mod.mkdir()
        self.replacement = self.mod / 'original'
        self.replacement.write_bytes(b'mod000')
        self.proof = self.root / 'state'
        self.proof.mkdir()
        self.write_manifest(b'mod000')

    def write_manifest(self, replacement):
        digest = sha256(replacement).hexdigest()
        original = sha256(b'base00').hexdigest()
        (self.mod / 'manifest.tsv').write_text(f'original\t{len(replacement)}\t{digest}\t{original}\n')

    def run_overlay(self, *parts, expected=0, **settings):
        env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_ROMFS_')}
        env.update(TOTK_ROMFS_PROOF_DIR=str(self.proof), **settings)
        result = subprocess.run([self.probe, self.base, self.mod, *parts], text=True,
                                capture_output=True, env=env, timeout=15)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result.stderr

    def test_cold_warm_and_forced_verification(self):
        self.assertIn('MB hashed', self.run_overlay())
        self.assertIn('reused SHA-256 verification', self.run_overlay())
        self.assertIn('MB hashed', self.run_overlay(TOTK_ROMFS_VERIFY='full'))
        self.assertIn('reused SHA-256 verification', self.run_overlay())

    def test_manifest_and_replacement_changes(self):
        self.run_overlay()
        self.write_manifest(b'mod001')
        self.assertIn('integrity mismatch', self.run_overlay(expected=2))
        self.replacement.write_bytes(b'mod001')
        self.assertIn('MB hashed', self.run_overlay())
        self.assertIn('reused SHA-256 verification', self.run_overlay())
        original_times = self.replacement.stat()
        self.replacement.write_bytes(b'mod002')
        os.utime(self.replacement, ns=(original_times.st_atime_ns, original_times.st_mtime_ns))
        self.assertIn('integrity mismatch', self.run_overlay(expected=2))

    def test_changed_base_and_split_part(self):
        self.run_overlay()
        self.base.write_bytes(b'base01')
        self.assertIn('integrity mismatch', self.run_overlay(expected=2))
        self.base.write_bytes(b'bas')
        part = self.root / 'part'
        part.write_bytes(b'e00')
        self.assertIn('MB hashed', self.run_overlay(part))
        self.assertIn('reused SHA-256 verification', self.run_overlay(part))
        part.write_bytes(b'e01')
        self.assertIn('integrity mismatch', self.run_overlay(part, expected=2))

    def test_corrupt_proof_is_rebuilt(self):
        self.run_overlay()
        record, = self.proof.iterdir()
        record.write_bytes(b'torn')
        self.assertIn('MB hashed', self.run_overlay())
        self.assertIn('reused SHA-256 verification', self.run_overlay())

    def test_changed_index_requires_a_fresh_check(self):
        self.run_overlay()
        changed = dict(TOTK_ROMFS_TEST_INDEX='f' * 64)
        self.assertIn('MB hashed', self.run_overlay(**changed))
        self.assertIn('reused SHA-256 verification', self.run_overlay(**changed))
