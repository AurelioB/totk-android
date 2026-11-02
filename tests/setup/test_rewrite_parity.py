"""Parity of the on-device code stage (android/setup/rewrite) with the Python references on real 1.4.3 modules.

Needs a work folder with the 1.4.3 ExeFS and the Python rewriter's Android output (default
build/TOTK_build/totk in the workspace folder that holds this checkout, override with TOTK_REF_WORK); skipped when it is absent.
Capstone 5.0.9 is fetched and built once by deps.prepare_capstone into TOTK_REWRITE_WORK (default build/setup-rewrite
in games/totk). Outputs go to a temporary folder; nothing is written into the reference work folder."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / 'games/totk'
REWRITE = GAME / 'android/setup/rewrite'
REF = Path(os.environ.get('TOTK_REF_WORK', ROOT.parent / 'build/TOTK_build/totk'))
EXEFS = REF / 'extracted/v143/exefs'
ORACLE = REF / 'rewritten-android/v143'
MODULES = ('main', 'subsdk0', 'sdk')
TSVS = ('fixups', 'gates', 'sites', 'cuts', 'params', 'obs')
PINS = json.loads((GAME / 'assets/expected-v1.4.3.json').read_text())
sys.path.insert(0, str(REWRITE))
from deps import prepare_capstone


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


@unittest.skipUnless(all((EXEFS / m).is_file() and (ORACLE / f'{m}.patched.bin').is_file() for m in MODULES),
                     f'reference work folder {REF} with 1.4.3 ExeFS and rewritten-android/v143 not present')
@unittest.skipUnless(shutil.which('cc') and shutil.which('cmake') and shutil.which('git'), 'needs cc, cmake and git')
class RewriteParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = Path(os.environ.get('TOTK_REWRITE_WORK', GAME / 'build/setup-rewrite'))
        work.mkdir(parents=True, exist_ok=True)
        prefix = prepare_capstone(work, work / 'capstone-host.log', target='host')
        cls.tmp = Path(tempfile.mkdtemp(prefix='totk-rewrite-'))
        cls.cli = cls.tmp / 'rewrite_cli'
        srcs = [REWRITE / f for f in ('rewrite.c', 'nso.c', 'tables.c', 'rw_util.c', 'code.c', 'rewrite_cli.c')]
        subprocess.run(['cc', '-O2', '-std=gnu11', '-Wall', '-Werror', '-I', REWRITE, '-I', prefix / 'include', *srcs,
                        prefix / 'lib/libcapstone.a', '-o', cls.cli], check=True)
        cls.rew, cls.gd, cls.diag = cls.tmp / 'rewritten', cls.tmp / 'game-data', cls.tmp / 'diag'
        for d in (cls.rew, cls.gd, cls.diag):
            d.mkdir()
        cls.code = subprocess.run([cls.cli, 'code', EXEFS, cls.rew, cls.gd, cls.diag], capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_rewritten_modules_match_python_and_pins(self):
        self.assertEqual(self.code.returncode, 0, self.code.stderr)
        for m in MODULES:
            digest = sha256(self.rew / f'{m}.patched.bin')
            self.assertEqual(digest, PINS['android_rewritten_sha256'][f'{m}.patched.bin'], m)
            self.assertEqual(digest, sha256(ORACLE / f'{m}.patched.bin'), m)
            self.assertIn(f'{m}.patched.bin {digest}', self.code.stdout)

    def test_tsvs_match_python(self):
        self.assertEqual(self.code.returncode, 0, self.code.stderr)
        for m in MODULES:
            for t in TSVS:
                self.assertEqual((self.diag / f'{m}.{t}.tsv').read_bytes(), (ORACLE / f'{m}.{t}.tsv').read_bytes(), f'{m}.{t}.tsv')

    def test_tables_match_python_writer_and_pin(self):
        self.assertEqual(self.code.returncode, 0, self.code.stderr)
        py = self.tmp / 'py-game-data'
        subprocess.run([sys.executable, GAME / 'scripts/prepare_runtime.py', '--version', 'v143', '--rewritten', ORACLE,
                        '--report', ORACLE / 'phase12-validation.json', '--out', py, '--exefs', EXEFS],
                       check=True, capture_output=True)
        c_tables = (self.gd / 'runtime-tables.bin').read_bytes()
        self.assertEqual(c_tables, (py / 'runtime-tables.bin').read_bytes())
        self.assertEqual(hashlib.sha256(c_tables).hexdigest(), PINS['android_runtime_tables_sha256'])
        # The tables writer alone, from the Python rewriter's files, gives the same bytes.
        out = self.tmp / 'from-oracle.bin'
        subprocess.run([self.cli, 'tables', ORACLE, EXEFS, out], check=True, capture_output=True)
        self.assertEqual(out.read_bytes(), c_tables)

    def test_foreign_module_is_refused(self):
        r = subprocess.run([self.cli, 'rewrite', EXEFS / 'sdk', self.tmp, 'main'], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('is not the Tears of the Kingdom 1.4.3 module', r.stderr)
        self.assertFalse((self.tmp / 'main.patched.bin').exists())


if __name__ == '__main__':
    unittest.main()
