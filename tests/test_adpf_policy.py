"""Compile the production ADPF state machine against an allocation-counted fake API."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AdpfPolicy(unittest.TestCase):
    def compile_run(self, source):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('A native C compiler is required')
        scratch = ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='adpf-policy-', dir=scratch) as directory:
            executable = Path(directory) / 'adpf-policy'
            flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                     for arg in shlex.split(os.environ.get(name, ''))]
            compiled = subprocess.run([
                *compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT), '-I', str(ROOT / 'tests'), '-x', 'c', '-', *flags, '-o', str(executable),
            ], input=source, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_fake_api(self):
        self.assertIn('steady-state passed',
                      self.compile_run((ROOT / 'tests/test_adpf_policy.c').read_text()))

    def test_load_hint_policy(self):
        self.compile_run(r'''
            #include "games/totk/runtime/adpf_policy.h"
            #include <assert.h>
            int main(void) {
                totk_adpf_load s = {0};
                const uint64_t target = 10000000, over = 10500001, half = 500000000;
                assert(!totk_adpf_load_step(&s, 0, over, target));
                assert(!totk_adpf_load_step(&s, 1, over, target));
                assert(totk_adpf_load_step(&s, 2, over, target));
                assert(!totk_adpf_load_step(&s, half + 1, over, target));
                assert(totk_adpf_load_step(&s, half + 2, over, target));
                /* Exact threshold, invalid frames and target reset the streak. */
                assert(!totk_adpf_load_step(&s, half + 3, 10500000, target) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 4, over, target));
                assert(!totk_adpf_load_step(&s, half + 5, 0, target) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 6, 250000000, target) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 7, over, 0) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 8, over, -1) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 9, over, target));
                assert(!totk_adpf_load_step(&s, half + 10, over, target));
                assert(!totk_adpf_load_step(&s, 1, over, target)); /* reversed clock */
                assert(totk_adpf_load_step(&s, 2 * half + 2, over, target));
                assert(!totk_adpf_load_gpu(7000000, target));
                assert(totk_adpf_load_gpu(7000001, target));
                assert(!totk_adpf_load_gpu(0, target));
                assert(!totk_adpf_load_gpu(7000001, 0));
                assert(!totk_adpf_load_gpu(7000001, -1));
                /* First request at timestamp zero still rate-limits subsequent attempts. */
                s = (totk_adpf_load){.over = 2};
                assert(totk_adpf_load_step(&s, 0, over, target));
                assert(!totk_adpf_load_step(&s, half - 1, over, target));
                assert(totk_adpf_load_step(&s, half, over, target));
            }
        ''')


if __name__ == '__main__':
    unittest.main()
