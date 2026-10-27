"""Portable service probes; binaries live only in a temporary directory."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'needs Linux cc')
class ServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.probe = Path(cls.temp.name) / 'services'
        subprocess.run([
            'cc', '-std=gnu11', '-D_GNU_SOURCE', '-DTOTK_GAME_V143=1', '-O1',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            str(ROOT / 'games/totk/tests/services/probe.c'), '-o', str(cls.probe),
        ], check=True, capture_output=True, text=True)

    def language(self, tag=None, explicit=None, fallback='C'):
        env = os.environ.copy()
        for key in ('TOTK_LOCALE', 'TOTK_LANG', 'LC_ALL', 'LANG'):
            env.pop(key, None)
        env['LANG'] = fallback
        if tag is not None:
            env['TOTK_LOCALE'] = tag
        if explicit is not None:
            env['TOTK_LANG'] = explicit
        return subprocess.check_output([str(self.probe)], env=env, text=True).strip()

    def test_chinese_script_and_region(self):
        for tag, expected in (
            ('zh-Hant', 'zh-Hant'), ('zh-hant-CN', 'zh-Hant'),
            ('zh-TW', 'zh-Hant'), ('zh-HK', 'zh-Hant'), ('zh-MO', 'zh-Hant'),
            ('zh-Hans-TW', 'zh-Hans'), ('zh-CN', 'zh-Hans'), ('zh', 'zh-Hans'),
            ('zh_Hant_HK', 'zh-Hant'),
        ):
            with self.subTest(tag=tag):
                self.assertEqual(self.language(tag), expected)

    def test_existing_language_policy(self):
        self.assertEqual(self.language('pt-BR'), 'pt-BR')
        self.assertEqual(self.language('pt-PT'), 'pt')
        self.assertEqual(self.language('ja-JP'), 'ja')
        self.assertEqual(self.language('zh-Hant', 'en-US'), 'en-US')
        self.assertEqual(self.language(fallback='zh_TW.UTF-8'), 'zh-Hant')
        self.assertEqual(self.language(), 'en-US')

    def test_allocation_logging(self):
        for setting in (None, '', '0', '1'):
            for result in (0, 0x123):
                with self.subTest(setting=setting, result=result):
                    env = os.environ.copy()
                    env.pop('TOTK_TRACE_ALLOC', None)
                    env.pop('TOTK_TRACE_VAMM', None)
                    if setting is not None:
                        env['TOTK_TRACE_ALLOC'] = setting
                    run = subprocess.run([str(self.probe), str(result)], env=env,
                                         capture_output=True, text=True, check=True)
                    expected = result != 0 or setting == '1'
                    self.assertEqual('AllocateAddressRegion' in run.stderr, expected)
                    self.assertEqual('AllocateMemoryPages' in run.stderr, expected)


if __name__ == '__main__':
    unittest.main()
