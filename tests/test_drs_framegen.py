"""Production patch guards and bounded controller policy. The guard fixture runs dataless with stand-in site words
(CODE_GUARD_STANDIN); the veneer emulation replays the game's own instructions, so it needs the player's 1.4.3 main NSO
($TOTK_MAIN_NSO) and is skipped without it."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/totk/scripts'
MAIN_NSO = Path(os.environ.get('TOTK_MAIN_NSO', ROOT.parent / 'build/TOTK_build/totk/extracted/v143/exefs/main'))


class DrsFramegen(unittest.TestCase):
    def test_policy(self):
        self.compile_run(ROOT / 'tests/test_drs_framegen.c')

    def test_production_guards(self):
        self.compile_run(SCRIPTS / 'v143_drs_framegen_fixture.c', fixture=True)

    def compile_run(self, source, fixture=False):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'drs-framegen'
            args = [compiler, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror']
            sources = [str(source)]
            if fixture:
                args += ['-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-format', '-DCODE_GUARD_STANDIN=1',
                         '-I' + str(ROOT / 'games/totk/runtime')]
                sources.append(str(ROOT / 'games/totk/android/gfx/host_test_support.c'))
            build = subprocess.run(args + sources + ['-lm', '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS', run.stdout)

    @unittest.skipUnless(importlib.util.find_spec('unicorn') and importlib.util.find_spec('lz4') and MAIN_NSO.is_file(),
                         'Unicorn/lz4 and the 1.4.3 main NSO ($TOTK_MAIN_NSO) required')
    def test_production_veneers(self):
        sys.path.insert(0, str(SCRIPTS))
        try:
            verify = __import__('verify_v143_drs_framegen')
            with tempfile.TemporaryDirectory() as directory:
                image, diag, _ = verify.compile_fixture(Path(directory), verify.native.read_original(MAIN_NSO))
                self.assertEqual(len(verify.operand_checks(image)), 24)
                self.assertEqual(verify.level_checks(diag), 8)
        finally:
            sys.path.remove(str(SCRIPTS))
