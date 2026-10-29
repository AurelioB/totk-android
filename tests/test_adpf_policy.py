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
    def test_fake_api(self):
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
                str(ROOT / 'tests/test_adpf_policy.c'), *flags, '-o', str(executable),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('steady-state passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
