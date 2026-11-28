"""Reserved save-set metadata never becomes game content, through the production Android save routes."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from android_saves_fixture import compile_mirror
from test_android_saves import tree

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'totk-native-saves.json'
TOP = 'TotK Native saves'


class SaveMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.java, cls.javac = shutil.which('java'), shutil.which('javac')
        if not cls.java or not cls.javac:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='save-metadata-', dir=os.environ.get('TOTK_SETUP_WORK'))
        cls.base = Path(cls.temp.name)
        cls.classes = compile_mirror(ROOT, cls.base / 'compile', cls.javac)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix='case-', dir=self.base))

    def run_probe(self, command, name, mode='direct', source=None, classes=None):
        args = [self.java, '-Xmx64m', '-cp', str(classes or self.classes), 'com.aure.totk.setup.SaveMetadataProbe',
                command, str(self.work / name), mode]
        if source is not None:
            args.append(str(source))
        result = subprocess.run(args, check=True, capture_output=True, text=True, timeout=30)
        return dict(line.split('=', 1) for line in result.stdout.splitlines())

    def test_stray_metadata_does_not_break_export_sync_backup_or_restore(self):
        for mode in ('direct', 'saf'):
            with self.subTest(mode=mode):
                result = self.run_probe('stray', mode, mode)
                self.assertEqual(result, {'export': 'option.sav', 'sync': 'pushed ', 'fetch': 'option.sav',
                                          'backup': 'option.sav', 'preserved': 'true', 'restore': 'option.sav'})
                with zipfile.ZipFile(self.work / mode / 'export.zip') as archive:
                    self.assertEqual(archive.namelist(), [TOP + '/option.sav', TOP + '/' + MANIFEST])
                    self.assertEqual([e['path'] for e in json.loads(archive.read(TOP + '/' + MANIFEST))['files']],
                                     ['option.sav'])

    def test_other_reserved_names_are_not_save_content(self):
        for mode in ('direct', 'saf'):
            with self.subTest(mode=mode):
                result = self.run_probe('reserved', mode, mode)
                self.assertEqual(result, {'export': 'option.sav', 'sync': 'pushed ', 'fetch': 'option.sav',
                                          'backup': 'option.sav', 'preserved': 'true', 'restore': 'option.sav'})

    def test_zip_and_folder_import_verify_but_never_install_metadata(self):
        self.run_probe('clean', 'source')
        source = self.work / 'source/clean.zip'
        folder = self.work / 'dump' / TOP
        with zipfile.ZipFile(source) as archive:
            archive.extractall(folder.parent)
        # Root envelope/temp files are metadata; same-named nested files are tested as ordinary content below.
        extras = [MANIFEST + '.tmp', '.option.sav.partial', TOP + '/option.sav']
        for relative in extras:
            path = folder / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'not save content')
        archive_path = self.work / 'metadata.zip'
        with zipfile.ZipFile(archive_path, 'w') as archive:
            for path in folder.rglob('*'):
                if path.is_file():
                    archive.write(path, TOP + '/' + path.relative_to(folder).as_posix())
        for mode in ('direct', 'saf'):
            for command, imported in (('stage-folder', folder.parent), ('stage-zip', archive_path)):
                with self.subTest(mode=mode, command=command):
                    answer = self.run_probe(command, mode + command, mode, imported)
                    self.assertEqual(answer, {command: 'option.sav'})
                    live = next((self.work / (mode + command) / 'private/user-data/save').iterdir())
                    self.assertEqual(tree(live), {'option.sav': b'\x01\x02\x03'})
        # The manifest is still used: filtering metadata is not a bypass of file integrity checks.
        (folder / 'option.sav').write_bytes(b'edited')
        for mode in ('direct', 'saf'):
            answer = self.run_probe('stage-folder', mode + '-edited', mode, folder.parent)
            self.assertIn('ERROR Invalid: corrupt:', answer['stage-folder'])

    def test_real_historical_nested_files_and_root_projection_round_trip(self):
        for revision in ('v1.0.1', '5762688'):
            classes = compile_mirror(ROOT, self.work / ('compile-' + revision), self.javac, revision)
            for shape in ('nested', 'root'):
                source_name = revision + '-' + shape
                self.run_probe('historical-' + shape, source_name, classes=classes)
                archive_path = self.work / source_name / 'clean.zip'
                folder = self.work / (source_name + '-folder')
                with zipfile.ZipFile(archive_path) as archive:
                    archive.extractall(folder)
                    metadata = json.loads(archive.read(TOP + '/' + MANIFEST))
                    expected = {name[len(TOP) + 1:]: archive.read(name) for name in archive.namelist()
                                if name != TOP + '/' + MANIFEST}
                if shape == 'root':
                    self.assertEqual(len(metadata['files']), 4, 'real old writer must list the excluded root files')
                    expected = {'option.sav': expected['option.sav']}
                else:
                    self.assertIn('album/' + MANIFEST, expected)
                    self.assertIn('slot_00/backup.json', expected)
                for mode in ('direct', 'saf'):
                    for command, source in (('stage-zip', archive_path), ('stage-folder', folder)):
                        with self.subTest(revision=revision, shape=shape, mode=mode, command=command):
                            target = source_name + '-' + mode + '-' + command
                            answer = self.run_probe(command, target, mode, source)
                            self.assertEqual(answer, {command: ','.join(sorted(expected))})
                            live = next((self.work / target / 'private/user-data/save').iterdir())
                            self.assertEqual(tree(live), expected)
                            with zipfile.ZipFile(self.work / target / 'roundtrip.zip') as archive:
                                actual = {name[len(TOP) + 1:]: archive.read(name) for name in archive.namelist()
                                          if name != TOP + '/' + MANIFEST}
                                self.assertEqual(actual, expected)
                                records = json.loads(archive.read(TOP + '/' + MANIFEST))['files']
                                self.assertEqual({entry['path'] for entry in records}, set(expected))
                if shape == 'nested':
                    # Nested same-named content remains integrity-protected, not merely tolerated.
                    changed = folder / TOP / 'album' / MANIFEST
                    changed.write_bytes(b'edited nested save content')
                    for mode in ('direct', 'saf'):
                        answer = self.run_probe('stage-folder', source_name + '-edited-' + mode, mode, folder)
                        self.assertIn('ERROR Invalid: corrupt:', answer['stage-folder'])

    def test_existing_opaque_stray_is_left_untouched_during_export_sync_backup(self):
        result = self.run_probe('opaque', 'preserved')
        self.assertEqual(result, {'export': 'option.sav', 'sync': 'pushed ', 'fetch': 'option.sav',
                                  'backup': 'option.sav', 'preserved': 'true', 'restore': 'option.sav'})


if __name__ == '__main__':
    unittest.main()
