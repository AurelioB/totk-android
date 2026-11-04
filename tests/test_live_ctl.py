"""Live switches (games/totk/runtime/live_ctl.h): the control-file parser line by line, and the host probe
(games/totk/tests/live_ctl/probe.c) that drives the production headers: staging and application at the frame boundary,
generation order, the accessor's cache, a writer / frame thread / readers race, the control thread on a real file, and
every live switch toggled 300 times while render threads (TOTK_PRIO_RENDER by tid), game threads (TOTK_TIMER_SLACK_NS
before their timed waits) and readers run. Both also run under ThreadSanitizer when the compiler has it.

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
TOTK = ROOT / 'games/totk'
FLAGS = ['-D_GNU_SOURCE', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-pthread',
         f'-I{TOTK / "runtime"}']


def compiler():
    cc = shlex.split(os.environ.get('CC', 'cc'))
    return cc if shutil.which(cc[0]) else None


@unittest.skipUnless(compiler(), 'no C compiler')
class LiveCtl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.probe = cls.dir / 'live-ctl'
        subprocess.run([*compiler(), *FLAGS, str(TOTK / 'tests/live_ctl/probe.c'), '-o', str(cls.probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def parse(self, *lines):
        out = subprocess.run([str(self.probe), 'parse'], input=''.join(l + '\n' for l in lines), capture_output=True,
                             text=True, check=True).stdout
        return out.splitlines()

    def test_commands(self):
        self.assertEqual(self.parse('set TOTK_VK_DESC_STOCK=1', '  unset TOTK_PRIO_RENDER  ', 'set TOTK_PRIO_RENDER=-8',
                                    'set TOTK_VK_SPLIT_PASS_DRAWS=+512', 'mark ab arm=B block=2 scene=lookout', '', '# note'),
                         ['ok set TOTK_VK_DESC_STOCK 1 -', 'ok unset TOTK_PRIO_RENDER 0 -', 'ok set TOTK_PRIO_RENDER -8 -',
                          'ok set TOTK_VK_SPLIT_PASS_DRAWS 512 -', 'ok mark - 0 ab arm=B block=2 scene=lookout', 'skip',
                          'skip'])

    def test_split_lrz_safe_commands(self):
        self.assertEqual(self.parse('set TOTK_VK_SPLIT_LRZ_SAFE=0', 'set TOTK_VK_SPLIT_LRZ_SAFE=1',
                                    'unset TOTK_VK_SPLIT_LRZ_SAFE'),
                         ['ok set TOTK_VK_SPLIT_LRZ_SAFE 0 -', 'ok set TOTK_VK_SPLIT_LRZ_SAFE 1 -',
                          'ok unset TOTK_VK_SPLIT_LRZ_SAFE 0 -'])
        self.assertIn('not a decimal in 0..1', self.parse('set TOTK_VK_SPLIT_LRZ_SAFE=2')[0])

    def test_subgroup_fix_is_startup_only(self):
        got = self.parse('set TOTK_VK_SUBGROUP_FIX=1', 'unset TOTK_VK_SUBGROUP_FIX')
        self.assertTrue(all('TOTK_VK_SUBGROUP_FIX is startup-only' in line for line in got), got)
        self.assertTrue(all('set it in totk.env and restart' in line for line in got), got)

    def test_core_wait_event_is_startup_only(self):
        got = self.parse('set TOTK_CORE_WAIT_EVENT=1', 'unset TOTK_CORE_WAIT_EVENT')
        self.assertTrue(all('TOTK_CORE_WAIT_EVENT is startup-only' in line for line in got), got)
        self.assertTrue(all('set it in totk.env and restart' in line for line in got), got)

    def test_position_precision_is_startup_only(self):
        for key in ('TOTK_VK_POS_INVARIANT', 'TOTK_VK_POS_NOCONTRACT'):
            got = self.parse(f'set {key}=1', f'unset {key}')
            self.assertTrue(all(f'{key} is startup-only' in line for line in got), got)
            self.assertTrue(all('set it in totk.env and restart' in line for line in got), got)

    def test_malformed_and_refused(self):
        got = self.parse('set TOTK_VK_DESC_STOCK', 'set TOTK_VK_DESC_STOCK=', 'set TOTK_VK_DESC_STOCK=2',
                         'set TOTK_VK_DESC_STOCK=1x', 'set TOTK_TIMER_SLACK_NS=0', 'set TOTK_PRIO_RENDER=-21',
                         'set totk_vk_desc_stock=1', 'unset', 'mark', 'SET TOTK_VK_DESC_STOCK=1', 'set TOTK_FPS60=1',
                         'set TOTK_IDEAL_CORE_FIX=1', 'unset TOTK_IDEAL_CORE_FIX', 'x' * 300)
        self.assertTrue(all(g.startswith('refused ') for g in got), got)
        self.assertIn('set needs KEY=VALUE', got[0])
        self.assertIn('TOTK_VK_DESC_STOCK=2: not a decimal in 0..1', got[2])
        self.assertIn('not a decimal in 1..50000', got[4])
        self.assertIn('not a decimal in -20..19', got[5])
        self.assertIn('malformed key', got[6])
        self.assertIn('unknown command', got[9])
        self.assertIn('TOTK_FPS60 is not a live switch', got[10])
        self.assertIn('TOTK_IDEAL_CORE_FIX is startup-only', got[11])
        self.assertIn('set it in totk.env and restart', got[12])
        self.assertIn('line longer than 239 bytes', got[13])

    def run_mode(self, binary, mode, env=None):
        work = Path(tempfile.mkdtemp(dir=self.dir))
        r = subprocess.run([str(binary), mode, str(work)], capture_output=True, text=True, timeout=300, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr + (work / 'stderr.txt').read_text(errors='replace')[-4000:])
        self.assertIn('ok: live switch channel', r.stdout)
        return r.stdout

    def tsan_probe(self):
        binary = self.dir / 'live-ctl-tsan'
        if not binary.exists() and subprocess.run([*compiler(), *FLAGS, '-fsanitize=thread', str(TOTK / 'tests/live_ctl/probe.c'),
                                                   '-o', str(binary)], capture_output=True).returncode:
            self.skipTest('the compiler has no -fsanitize=thread')
        return binary

    def test_core(self):
        out = self.run_mode(self.probe, 'core')
        self.assertIn('ok: 400 batches applied', out)
        self.assertIn('ok: control thread', out)

    def test_core_thread_sanitizer(self):
        self.run_mode(self.tsan_probe(), 'core', env=os.environ | {'TSAN_OPTIONS': 'halt_on_error=1 exitcode=66'})

    def test_switches(self):
        # Every whitelisted switch is toggled: the probe's count must match the table in live_ctl.h.
        table = (TOTK / 'runtime/live_ctl.h').read_text()
        table = table[table.index('totk_live_switches[] = {'):]
        count = table[:table.index('};')].count('{"TOTK_')
        self.assertIn(f'ok: 300 batches toggled all {count} live switches', self.run_mode(self.probe, 'switches'))

    def test_switches_thread_sanitizer(self):
        self.run_mode(self.tsan_probe(), 'switches', env=os.environ | {'TSAN_OPTIONS': 'halt_on_error=1 exitcode=66'})


if __name__ == '__main__':
    unittest.main()
