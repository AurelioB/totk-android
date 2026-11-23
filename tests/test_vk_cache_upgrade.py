"""Policy-cache upgrades through the production Vulkan import and prewarm paths, host only."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'games/totk/android'
ICDS = sorted(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'))


@unittest.skipUnless(ICDS and all(shutil.which(tool) for tool in ('make', 'cc', 'glslangValidator'))
                     and Path('/usr/include/vulkan/vulkan.h').is_file(), 'needs host Vulkan fixture tools')
class CacheUpgradeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cache-upgrade-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.build = cls.work / 'build'
        subprocess.run(['make', '-C', str(ANDROID), 'TARGET=linux', f'OUT={cls.build}', '-j2',
                        'vkpipetest', str(cls.build / 'vkpcachetest')], check=True, capture_output=True, text=True)
        cls.env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        cls.env.update(VK_DRIVER_FILES=str(ICDS[0]), VK_ICD_FILENAMES=str(ICDS[0]),
                       XDG_CACHE_HOME=str(cls.work / 'cache'), TOTK_VK_INT_FOLD='1',
                       TOTK_VK_VARYING_FIX='0', TOTK_VK_NAN_MINMAX='0', TOTK_FRAME_LAG='0',
                       TOTK_VK_REC_THREADS='0', TOTK_VK_SEED_IO='0', TOTK_PSO_PREWARM='0')
        cls.run_fixture('vkpcachetest')
        cls.source = cls.work / 'state/pcache-reload.bin.intfold-v1'

    @classmethod
    def run_fixture(cls, binary, *args, env=None):
        result = subprocess.run([str(cls.build / binary), *map(str, args)], cwd=cls.work,
                                env=cls.env if env is None else env, capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def test_policy_switches_and_predecessor_chain(self):
        # All three bits include legacy -> folded -> varying, both new switches
        # together, independent NaN selection, and switching back to another arm.
        for fold in ('0', '1'):
            for vary, nan in (('0', '0'), ('1', '0'), ('0', '1'), ('1', '1')):
                env = dict(self.env, TOTK_VK_INT_FOLD=fold, TOTK_VK_VARYING_FIX=vary, TOTK_VK_NAN_MINMAX=nan)
                for case in (0, 7, 8, 9, 10):
                    with self.subTest(fold=fold, varying=vary, nan=nan, case=case):
                        self.run_fixture('vkpcachetest', '--upgrade', self.source, case, env=env)

    def test_missing_corrupt_and_context_mismatch(self):
        env = dict(self.env, TOTK_VK_VARYING_FIX='1')
        for case in (1, 2, 3, 4, 5, 6):
            with self.subTest(case=case):
                self.run_fixture('vkpcachetest', '--upgrade', self.source, case, env=env)

    def test_completion_requires_actual_cache_hit_or_build(self):
        result = self.run_fixture('vkpipetest', '--pso-keylist')
        self.assertIn('0 failed', result.stdout)


if __name__ == '__main__':
    unittest.main()
