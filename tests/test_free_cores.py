"""TOTK_FREE_CORES (games/totk/runtime/threads.c virtual cores): only a value other than "0" frees the cores; "0", an
empty value or no variable leaves the virtual cores on, as documented (it used to test presence, so =0 freed them).
The production lines are compiled out of threads.c into a small driver.
CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
THREADS = ROOT / 'games/totk/runtime/threads.c'


class FreeCores(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('A native C compiler is required')
        source = THREADS.read_text()
        lines = [re.search(r'^static int g_cores_on = -1;$', source, re.M),
                 re.search(r'^static int free_cores_requested\(const char \*value\) \{.*\}$', source, re.M),
                 re.search(r'^static int cores_enabled\(void\) \{.*\}$', source, re.M)]
        if not all(lines):
            raise AssertionError('threads.c: the TOTK_FREE_CORES lines moved')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='free-cores-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        driver = Path(cls.temp.name) / 'driver.c'
        driver.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n' +
                          '\n'.join(m.group(0) for m in lines) +
                          '\nint main(void) { printf("%d\\n", cores_enabled()); return 0; }\n')
        cls.binary = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*cc, '-std=c11', '-Wall', '-Werror', str(driver), '-o', str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def cores(self, value):
        env = {k: v for k, v in os.environ.items() if k != 'TOTK_FREE_CORES'}
        if value is not None:
            env['TOTK_FREE_CORES'] = value
        return subprocess.run([str(self.binary)], env=env, capture_output=True, text=True, timeout=30, check=True).stdout.strip()

    def test_zero_empty_and_unset_change_nothing(self):
        for value in (None, '', '0'):
            self.assertEqual(self.cores(value), '1', value)

    def test_other_values_free_the_cores(self):
        for value in ('1', 'yes', '01'):
            self.assertEqual(self.cores(value), '0', value)


if __name__ == '__main__':
    unittest.main()
