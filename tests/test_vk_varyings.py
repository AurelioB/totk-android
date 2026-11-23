"""Production varying replacement chain, disabled work, validation and ownership races."""
from pathlib import Path
import itertools
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
GFX = TOTK / 'android/gfx'
CC = shlex.split(os.environ.get('CC', 'cc'))


@unittest.skipUnless(sys.platform.startswith('linux') and CC and shutil.which(CC[0]), 'needs Linux C compiler')
class VaryingRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vk-varyings-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.flags = ['-std=gnu11', '-D_GNU_SOURCE', '-DGPU_VK', '-O1', '-g',
                     '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
        for folder in ('runtime', 'third_party/d2r_gfx/gfx/nvn', 'third_party/d2r_gfx/gfx/nvn/include',
                       'third_party/d2r_gfx/host', 'android/compat/include'):
            cls.flags += ['-I' + str(TOTK / folder)]
        cls.flags += ['-include', str(TOTK / 'android/compat/include/darwin_compat.h')]
        cls.sources = [str(GFX / name) for name in ('test_vk_shader_lifetime.c', 'vk_seed_io.c', 'host_test_support.c')]
        cls.probe = cls.work / 'varyings'
        subprocess.run([*CC, *cls.flags, *cls.sources, '-lpthread', '-ldl', '-o', str(cls.probe)],
                       check=True, capture_output=True, text=True, timeout=120)

    def run_arm(self, binary=None, validate=False, **settings):
        game = Path(tempfile.mkdtemp(dir=self.work))
        env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        env.update(TEST_VARYINGS='1', TOTK_VK_SEED_IO='0')
        env.update(settings)
        result = subprocess.run([str(binary or self.probe)], cwd=game, env=env, text=True,
                                capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('off-arm identity, accounting and concurrency passed', result.stdout)
        if validate:
            modules = sorted(game.glob('module-*.spv'))
            self.assertTrue(modules)
            for module in modules:
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(module)],
                               check=True, capture_output=True, text=True)
        return result

    def test_independent_defaults(self):
        result = self.run_arm()
        self.assertIn('TOTK_VK_VARYING_FIX=1 (startup-only)', result.stderr)
        self.assertIn('TOTK_VK_NAN_MINMAX=0 (startup-only, experimental)', result.stderr)
        self.assertIn('session shader_pairs patched=64 fallback=1', result.stderr)

    @unittest.skipUnless(shutil.which('spirv-val'), 'needs spirv-val (spirv-tools)')
    def test_all_policies_and_replacement_validation(self):
        for vary, nan, fold, position in itertools.product('01', repeat=4):
            with self.subTest(vary=vary, nan=nan, fold=fold, position=position):
                self.run_arm(validate=True, TOTK_VK_VARYING_FIX=vary, TOTK_VK_NAN_MINMAX=nan,
                             TOTK_VK_INT_FOLD=fold, TOTK_VK_POS_INVARIANT=position,
                             TOTK_VK_POS_NOCONTRACT=position)

    def test_thread_sanitizer(self):
        probe = self.work / 'varyings-tsan'
        build = subprocess.run([*CC, *self.flags, '-fsanitize=thread', *self.sources,
                                '-lpthread', '-ldl', '-o', str(probe)], capture_output=True, text=True)
        if build.returncode:
            self.skipTest('compiler has no -fsanitize=thread: ' + build.stderr[-1000:])
        for indexed in ('0', '1'):
            with self.subTest(indexed=indexed):
                self.run_arm(probe, TOTK_VK_SEED_IO=indexed, TOTK_VK_VARYING_FIX='1',
                             TOTK_VK_NAN_MINMAX='0', TOTK_VK_INT_FOLD='1',
                             TSAN_OPTIONS='halt_on_error=1 exitcode=66')

    def test_malformed_modules_under_sanitizers(self):
        probe = self.work / 'varyings-asan'
        build = subprocess.run([*CC, '-std=c11', '-D_GNU_SOURCE', '-O1', '-g',
                                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                str(GFX / 'test_vk_position.c'), '-o', str(probe)], capture_output=True, text=True)
        if build.returncode:
            self.skipTest('compiler has no address/undefined sanitizers: ' + build.stderr[-1000:])
        result = subprocess.run([str(probe)], capture_output=True, text=True,
                                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                                         UBSAN_OPTIONS='halt_on_error=1'), timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
