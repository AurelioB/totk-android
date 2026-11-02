"""The on-device importer against the real 1.4.3 files on this machine (skipped when they are absent): the user's prod.keys,
the 1.4.3 update NSP (identification, ExeFS through the bundled hactool, and the whole container-route code stage through
run()), and the index stage read in place from the reference RomFS. No key value is ever printed: outputs are only
checked for not containing them. There is no base 1.0.0 dump here, so the BKTR RomFS merge itself is not exercised.

Paths: TOTK_KEYS, TOTK_UPDATE_NSP, TOTK_REF_WORK (defaults: in the workspace folder that holds this checkout). Run: python -m unittest tests.setup.test_import_real -v"""
import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_import import Base, PINS  # noqa: E402

WORKSPACE = Path(__file__).resolve().parents[3]   # the folder that holds this checkout
KEYS = Path(os.environ.get('TOTK_KEYS', WORKSPACE / 'input/keys/prod.keys'))
UPDATE = Path(os.environ.get('TOTK_UPDATE_NSP', WORKSPACE / 'input/totk-1.4.3.nsp'))
REF = Path(os.environ.get('TOTK_REF_WORK', WORKSPACE / 'build/TOTK_build/totk'))
ROMFS = REF / 'extracted/v143/romfs'


def key_values():
    """Every value in prod.keys (lowercase hex), only for checking that none leaks."""
    values = []
    for line in KEYS.read_text().splitlines():
        body = line.split('#', 1)[0]
        if '=' in body:
            v = body.split('=', 1)[1].strip().lower()
            if len(v) >= 32:
                values.append(v)
    return values


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


class RealBase(Base):
    def no_secret(self, text, extra=()):
        low = text.lower()
        # Never let a failing assertion print the key: report only that something leaked.
        self.assertFalse(any(v in low for v in self.real_keys), 'key material found in output')

    def setUp(self):
        super().setUp()
        self.real_keys = key_values()


@unittest.skipUnless(KEYS.is_file() and UPDATE.is_file(), 'prod.keys or the 1.4.3 update NSP not present')
class Update(RealBase):
    def test_keys(self):
        p, out = self.cli_run('keys', '--keys', KEYS)
        self.assertTrue(out['ok'] and out['header_key'])
        self.assertTrue(out['titlekek'] and out['key_area_key_application'])

    def test_identify_update_and_refuse_it_as_base(self):
        p, out = self.cli_run('identify', UPDATE, '--keys', KEYS, '--update')
        self.assertTrue(out['ok'], out)
        self.assertEqual(out['kind'], 'nsp')
        self.assertEqual(out['program']['title_id'], PINS['title_id'])
        self.assertEqual(out['program']['bktr_section'], 1)
        self.assertIn('0100F2C0115B6800', {n['title_id'] for n in out['ncas']})
        p, out = self.cli_run('identify', UPDATE, '--keys', KEYS)
        self.assertEqual(out['code'], 'unsupported_version')

    def test_merge_refuses_an_update_as_base(self):
        """The merge needs a base 1.0.0 dump (none on this machine); an update passed as base stops before hactool."""
        work = self.dir / 'work'; work.mkdir()
        p, out = self.cli_run('merge', '--base', UPDATE, '--update', UPDATE, '--keys', KEYS, '--out', work / 'romfs', '--work', work)
        self.assertEqual(out['code'], 'unsupported_version', out)
        self.assertFalse((work / 'romfs').exists())
        self.assertFalse((work / 'import-keys').exists())

    def test_exefs_through_hactool_matches_pins(self):
        work = self.dir / 'work'; work.mkdir()
        p, out = self.cli_run('exefs', '--update', UPDATE, '--keys', KEYS, '--out', work / 'exefs', '--work', work)
        self.assertTrue(out['ok'], out)
        for name, want in PINS['exefs_sha256'].items():
            self.assertEqual(sha256(work / 'exefs' / name), want, name)
        self.assertFalse((work / 'import-keys').exists())
        self.assertFalse((work / 'update.nca').exists())
        self.no_secret_files(work)

    def test_container_route_code_stage(self):
        game, nobackup = self.dir / 'game', self.dir / 'nobackup'
        fds = [os.open(UPDATE, os.O_RDONLY), os.open(KEYS, os.O_RDONLY)]
        self.addCleanup(lambda: [os.close(f) for f in fds])
        req = self.dir / 'req.json'
        req.write_text(json.dumps({'gameDir': str(game), 'noBackupDir': str(nobackup), 'nativeLibDir': str(self.cli.parent / 'host'),
                                   'inputs': {'update': {'fd': fds[0], 'size': 1, 'name': UPDATE.name},
                                              'keys': {'fd': fds[1], 'size': 1, 'name': 'prod.keys'}},
                                   'stages': ['code']}))
        p, out = self.cli_run('inspect', '--request', req, pass_fds=fds)
        self.assertEqual([(i['role'], i['ok']) for i in out['items']], [('keys', True), ('update', True)], out)
        self.assertEqual((out['route'], out['need']), ('containers', ['base']))
        p, out = self.cli_run('run', '--request', req, pass_fds=fds)
        self.assertTrue(out['ok'], out)
        code = out['install']['stages']['code']
        self.assertEqual(code['rewritten_sha256'], PINS['android_rewritten_sha256'])
        if 'android_runtime_tables_sha256' in PINS:
            self.assertEqual(code['runtime_tables_sha256'], PINS['android_runtime_tables_sha256'])
        self.assertEqual(out['install']['route'], 'containers')
        for name, want in PINS['android_rewritten_sha256'].items():
            self.assertEqual(sha256(game / 'rewritten/v143' / name), want)
        self.assertTrue((game / 'game-data/v143/runtime-tables.bin').is_file())
        self.assertFalse((game / '.import').exists())
        self.assertFalse((nobackup / 'import-keys').exists())
        self.no_secret_files(self.dir)


@unittest.skipUnless((ROMFS / 'romfs.bin').is_file(), f'reference RomFS {ROMFS} not present')
class Index(Base):
    def test_index_in_place_matches_pin(self):
        out_dir = Path(tempfile.mkdtemp(dir=self.dir))
        p, out = self.cli_run('index', '--romfs', ROMFS / 'romfs.bin', '--out-index', out_dir / 'runtime-index.bin',
                              '--out-config', out_dir / 'romfs-config.txt')
        self.assertEqual(out, {'ok': True, 'sha256': PINS['romfs_index_sha256'], 'parts': 0})
        if (ROMFS / 'runtime-index.bin').is_file():
            self.assertEqual((out_dir / 'runtime-index.bin').read_bytes(), (ROMFS / 'runtime-index.bin').read_bytes())
        self.assertEqual((out_dir / 'romfs-config.txt').read_text(),
                         f"ROMFS_IMAGE_SIZE {PINS['romfs']['size']}\nROMFS_IMAGE_SHA256 {PINS['romfs']['sha256']}\n"
                         f"ROMFS_INDEX_SHA256 {PINS['romfs_index_sha256']}\n")


if __name__ == '__main__':
    unittest.main()
