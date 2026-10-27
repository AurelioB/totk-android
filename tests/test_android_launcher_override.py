"""Synthetic-only launcher overrides, compiled resources and Android builder plumbing."""
import ast
import contextlib
import hashlib
import importlib.util
import io
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'games/totk/android/apk')]
import build_apk
from test_android_apk import chunks, manifest_elements, strings

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


android = load('launcher_android', 'games/totk/android/build_android.py')
release = load('launcher_release', 'games/totk/android/build_release.py')


def resource_entries(data, xml=None):
    """Independently resolve every mipmap resource ID/configuration to its packaged file (keys: the mipmap names). The
    'xml' type (backup rules) follows mipmap; its ID -> path pairs go into xml when a dict is given."""
    assert list(chunks(data, 0, len(data))) == [(2, 0, 12, len(data))]
    pool, package_chunk = list(chunks(data, 12, len(data)))
    paths = strings(data, pool[1])
    _, package, header, size = package_chunk
    assert header == 288
    typeoff, _, keyoff, _, typeid = struct.unpack_from('<5I', data, package + 268)
    assert typeid == 0
    types = strings(data, package + typeoff)
    assert types == ['mipmap', 'xml'], types
    keys = strings(data, package + keyoff)
    counts = {}
    for kind, start, _, _ in chunks(data, package + header, package + size):
        if kind == 0x0202:
            tid, _, _, count = struct.unpack_from('<BBHI', data, start + 8)
            counts[tid] = count
    result = {}
    for kind, start, header, _ in chunks(data, package + header, package + size):
        if kind != 0x0201:
            continue
        tid, flags, _, count, entries = struct.unpack_from('<BBHII', data, start + 8)
        base = sum(counts[t] for t in counts if t < tid)
        assert (flags, count) == (0, counts[tid])
        density, = struct.unpack_from('<H', data, start + 34)
        sdk, = struct.unpack_from('<H', data, start + 44)
        for index in range(count):
            off, = struct.unpack_from('<I', data, start + header + index * 4)
            if off == 0xffffffff:
                continue
            entry = start + entries + off
            assert struct.unpack_from('<HHI', data, entry) == (8, 0, base + index)
            value_size, reserved, dtype, value = struct.unpack_from('<HBBI', data, entry + 8)
            assert (value_size, reserved, dtype) == (8, 0, 3)
            if types[tid - 1] == 'mipmap': result[(0x7f010000 + index, density, sdk)] = paths[value]
            elif xml is not None: xml[(0x7f000000 | tid << 16) + index] = (keys[base + index], paths[value])
    return keys[:counts[1]], result


class LauncherOverrideTests(unittest.TestCase):
    def fixture(self, root, *, fmt='PNG', transparent=True):
        # Deliberately non-square; RGB in transparent pixels must not bleed into the coloured logo.
        image = Image.new('RGBA', (320, 200), (255, 255, 255, 0) if transparent else 'white')
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 319, 199), outline=(220, 40, 20, 255), width=8)
        draw.rectangle((40, 60, 280, 140), fill=(220, 40, 20, 128 if transparent else 255))
        path = root / ('source.jpg' if fmt == 'JPEG' else 'source.png')
        (image.convert('RGB') if fmt == 'JPEG' else image).save(path, format=fmt)
        return path

    def package(self, root, icon):
        (root / 'libtotk.so').write_bytes(b'synthetic packaging fixture, not an Android executable')
        (root / 'classes.dex').write_bytes(b'dex packaging fixture')
        output = root / 'test.apk'
        build_apk.main(['--dex', str(root / 'classes.dex'), '--lib', str(root / 'libtotk.so'), '--out', str(output),
                        '--key', str(root / 'key.pem'), '--make-key', '--version-code', '1', '--launcher-icon', str(icon)])
        return output

    def test_every_density_layer_and_full_image_safe_circle(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for fmt, transparent in [('PNG', True), ('PNG', False), ('JPEG', False)]:
                with self.subTest(fmt=fmt, transparent=transparent):
                    path = self.fixture(root, fmt=fmt, transparent=transparent)
                    with contextlib.redirect_stdout(io.StringIO()) as log:
                        images, store = build_apk.launcher_override(path)
                    self.assertIn('sha256=' + hashlib.sha256(path.read_bytes()).hexdigest(), log.getvalue())
                    self.assertEqual(len(images), 20)
                    files = {name: data for _, name, data in images}
                    with Image.open(path) as original:
                        rgba = original.convert('RGBA')
                        corners = [rgba.getpixel((x, y))
                                   for x in (0, original.width - 1) for y in (0, original.height - 1)]
                    sampled = tuple(round(sum(c[i] for c in corners) / 4) for i in range(3)) + (255,)
                    background = (11, 20, 22, 255) if transparent else sampled
                    for qualifier, _, size in build_apk.ICON_DENSITIES:
                        for name in ('ic_launcher', 'ic_launcher_round', 'ic_launcher_background', 'ic_launcher_foreground'):
                            image = Image.open(io.BytesIO(files[f'res/mipmap-{qualifier}/{name}.png']))
                            expected = size * 9 // 4 if name.endswith(('background', 'foreground')) else size
                            self.assertEqual(image.size, (expected, expected))
                            self.assertEqual(image.format, 'PNG')
                            if name.endswith('foreground'):
                                bbox = image.getchannel('A').getbbox()
                                assert bbox is not None
                                left, top, right, bottom = bbox
                                self.assertAlmostEqual((right - left) / (bottom - top), 1.6, delta=0.07)
                                # Whole source, including its red border/corners, fits inside the safe circle.
                                radius = expected * 33 / 108
                                for x in (left, right - 1):
                                    for y in (top, bottom - 1):
                                        self.assertLessEqual(math.hypot(x + 0.5 - expected / 2, y + 0.5 - expected / 2), radius)
                                if transparent:
                                    scale = expected * 66 / 108 / math.hypot(*rgba.size)
                                    dimensions = (int(rgba.width * scale), int(rgba.height * scale))
                                    reference = rgba.convert('RGBa').resize(dimensions, Image.Resampling.LANCZOS).convert('RGBA')
                                    canvas = Image.new('RGBA', image.size)
                                    canvas.alpha_composite(reference, ((expected - reference.width) // 2, (expected - reference.height) // 2))
                                    self.assertEqual(image.tobytes(), canvas.tobytes())
                            elif name == 'ic_launcher_round':
                                self.assertEqual(image.getchannel('A').getpixel((0, 0)), 0)
                            else:
                                self.assertEqual(image.getpixel((0, 0)), background)
                    self.assertEqual(Image.open(io.BytesIO(store)).size, (512, 512))

    def test_packaged_manifest_adaptive_xml_and_all_resource_values(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            icon = self.fixture(root)
            with contextlib.redirect_stdout(io.StringIO()):
                output = self.package(root, icon)
            with zipfile.ZipFile(output) as archive:
                app = manifest_elements(archive.read('AndroidManifest.xml'))['application']
                self.assertEqual(app[(build_apk.ANDROID_NS, 'icon')], (1, build_apk.LAUNCHER_ICON))
                self.assertEqual(app[(build_apk.ANDROID_NS, 'roundIcon')], (1, build_apk.LAUNCHER_ROUND))
                self.assertEqual(app[(build_apk.ANDROID_NS, 'label')], (3, 'TotK Native'))
                xml = {}
                keys, values = resource_entries(archive.read('resources.arsc'), xml)
                self.assertEqual(keys, ['ic_launcher', 'ic_launcher_round', 'ic_launcher_background', 'ic_launcher_foreground'])
                self.assertEqual(len(values), 22)
                # The backup rules: the manifest's references resolve to the packaged rule files.
                self.assertEqual(xml, {0x7f020000: ('data_extraction_rules', 'res/xml/data_extraction_rules.xml'),
                                       0x7f020001: ('backup_rules', 'res/xml/backup_rules.xml')})
                self.assertEqual(app[(build_apk.ANDROID_NS, 'dataExtractionRules')], (1, 0x7f020000))
                self.assertEqual(app[(build_apk.ANDROID_NS, 'fullBackupContent')], (1, 0x7f020001))
                self.assertIn('device-transfer', manifest_elements(archive.read('res/xml/data_extraction_rules.xml')))
                for (resource, density, sdk), path in values.items():
                    self.assertIn(path, archive.namelist())
                    if sdk == 26:
                        self.assertEqual(density, 0xfffe)
                        self.assertIn(resource, (build_apk.LAUNCHER_ICON, build_apk.LAUNCHER_ROUND))
                        elements = manifest_elements(archive.read(path))
                        self.assertIn('adaptive-icon', elements)
                        for tag, target in [('background', build_apk.LAUNCHER_BACKGROUND), ('foreground', build_apk.LAUNCHER_FOREGROUND)]:
                            self.assertEqual(elements[tag][(build_apk.ANDROID_NS, 'drawable')], (1, target))
                            for _, dpi, _ in build_apk.ICON_DENSITIES:
                                self.assertIn((target, dpi, 0), values)
                    else:
                        self.assertEqual(sdk, 0)
                        self.assertIn(density, [d for _, d, _ in build_apk.ICON_DENSITIES])
                self.assertNotIn('test-launcher-512.png', archive.namelist())
            with Image.open(root / 'test-launcher-512.png') as store:
                self.assertEqual(store.size, (512, 512))

    def test_android_sdk_parses_compiled_resource_table_and_xml(self):
        sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk'))
        tools = sorted(sdk.glob('build-tools/*/aapt2'))
        if not tools:
            self.skipTest('Android SDK aapt2 unavailable; independent binary parser is covered separately')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with contextlib.redirect_stdout(io.StringIO()):
                output = self.package(root, self.fixture(root))
            resources = subprocess.run([str(tools[-1]), 'dump', 'resources', str(output)], check=True, capture_output=True, text=True).stdout
            for name in ('ic_launcher', 'ic_launcher_round', 'ic_launcher_background', 'ic_launcher_foreground'):
                self.assertIn('mipmap/' + name, resources)
            self.assertIn('(anydpi-v26)', resources)
            for file in ('AndroidManifest.xml', 'res/mipmap-anydpi-v26/ic_launcher.xml', 'res/mipmap-anydpi-v26/ic_launcher_round.xml'):
                xml = subprocess.run([str(tools[-1]), 'dump', 'xmltree', str(output), '--file', file], check=True, capture_output=True, text=True).stdout
                self.assertIn('roundIcon', xml) if file == 'AndroidManifest.xml' else self.assertIn('adaptive-icon', xml)

    def test_invalid_images_fail_before_outputs_and_key(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            invalid = root / 'bad.png'
            invalid.write_bytes(b'not an image')
            gif = root / 'bad.gif'
            Image.new('RGB', (8, 8)).save(gif)
            transparent = root / 'empty.png'
            Image.new('RGBA', (8, 8)).save(transparent)
            truncated = root / 'truncated.png'
            valid = self.fixture(root)
            truncated.write_bytes(valid.read_bytes()[:60])
            for path in (root / 'missing.png', root, invalid, gif, transparent, truncated):
                with self.subTest(path=path), self.assertRaisesRegex(SystemExit, '--launcher-icon:'):
                    self.package(root, path)
                self.assertFalse((root / 'key.pem').exists())
                self.assertFalse((root / 'test.apk').exists())
            with patch.dict(sys.modules, {'PIL': None}), self.assertRaisesRegex(SystemExit, 'requires Pillow'):
                build_apk.launcher_override(valid)

    def test_no_icon_means_no_icon_resources_and_no_sidecar(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'libtotk.so').write_bytes(b'native fixture')
            (root / 'classes.dex').write_bytes(b'dex fixture')
            build_apk.main(['--dex', str(root / 'classes.dex'), '--lib', str(root / 'libtotk.so'), '--out', str(root / 'default.apk'),
                            '--key', str(root / 'key.pem'), '--make-key'])
            with zipfile.ZipFile(root / 'default.apk') as archive:
                # No launcher resources; only the backup rules (always present) and their table.
                self.assertEqual(sorted(n for n in archive.namelist() if n.startswith('res/') or n == 'resources.arsc'),
                                 ['res/xml/backup_rules.xml', 'res/xml/data_extraction_rules.xml', 'resources.arsc'])
                app = manifest_elements(archive.read('AndroidManifest.xml'))['application']
                self.assertNotIn((build_apk.ANDROID_NS, 'icon'), app)
                self.assertEqual(app[(build_apk.ANDROID_NS, 'dataExtractionRules')], (1, 0x7f010000))
            self.assertFalse((root / 'default-launcher-512.png').exists())

    def test_committed_render_is_current_and_packs_without_pillow(self):
        launcher = ROOT / 'games/totk/android/apk/launcher'
        art = ROOT / 'games/totk/android/app-assets/icon/totk-native-1024.png'
        self.assertEqual((launcher / 'source.sha256').read_text().split()[0], hashlib.sha256(art.read_bytes()).hexdigest(),
                         'rerun games/totk/android/apk/render_launcher.py')
        with tempfile.TemporaryDirectory() as folder, patch.dict(sys.modules, {'PIL': None}):
            root = Path(folder)
            (root / 'libtotk.so').write_bytes(b'native fixture')
            (root / 'classes.dex').write_bytes(b'dex fixture')
            with contextlib.redirect_stdout(io.StringIO()):
                build_apk.main(['--dex', str(root / 'classes.dex'), '--lib', str(root / 'libtotk.so'), '--out', str(root / 'a.apk'),
                                '--key', str(root / 'key.pem'), '--make-key', '--launcher-mipmaps', str(launcher)])
            with zipfile.ZipFile(root / 'a.apk') as archive:
                keys, values = resource_entries(archive.read('resources.arsc'))
                self.assertEqual(len(values), 22)
                for qualifier, _, size in build_apk.ICON_DENSITIES:
                    data = archive.read(f'res/mipmap-{qualifier}/ic_launcher.png')
                    self.assertEqual(data, (launcher / f'res/mipmap-{qualifier}/ic_launcher.png').read_bytes())
                    self.assertEqual(struct.unpack('>II', data[16:24]), (size, size))
            self.assertTrue((root / 'a-launcher-512.png').is_file())

    def test_release_and_developer_builds_pack_the_repository_icon(self):
        """No Control-NCA artwork any more: the app's own icon (android/app-assets/icon) goes into every APK."""
        self.assertNotIn('game_launcher', dir(build_apk))
        self.assertNotIn('launcher_icon', vars(android.parse_args(['base.nsp', '-k', 'prod.keys', '--apk'])))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            logs = root / 'logs'; logs.mkdir()
            assets = root / 'stage/assets'; assets.mkdir(parents=True)
            with patch.object(release, 'run') as run, patch.object(release.audit_apk, 'audit', return_value=[]), \
                 patch.object(release, 'audit_entries', return_value=[]), patch.object(release, 'say'), \
                 patch.object(release, 'verify_signatures', return_value=[]):
                release.package(out=root / 'a.apk', dex=root / 'classes.dex', libs=[root / 'libtotk.so'], translator=root / 'x',
                                turnip=None, assets=assets, key=root / 'key.pem', release=True, version_code=1, version_name='1', logs=logs)
            command = run.call_args.args[0]
            self.assertNotIn('--launcher-icon', command)   # no Pillow in the release path
            self.assertEqual(command[command.index('--launcher-mipmaps') + 1], ROOT / 'games/totk/android/apk/launcher')
            self.assertNotIn('--make-key', command)
            self.assertIn('--release', command)


if __name__ == '__main__':
    unittest.main()
