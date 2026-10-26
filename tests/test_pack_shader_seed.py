"""Tiny host-only fixtures for deterministic seed packs and default packed Android staging.

    python3 -m unittest discover -s tests -p test_pack_shader_seed.py

No game files, SDK, translator, build, or device commands are needed.
"""
import ast
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packer = load('packed_shader_seed', 'games/totk/scripts/pack_shader_seed.py')
android = load('packed_android_build', 'games/totk/android/build_android.py')
reflection = load('refresh_vk_reflect', 'games/totk/android/vk_reflect.py')


def read_pack(path):
    data = path.read_bytes()
    header = struct.unpack_from('<8sIIQQ', data)
    records = [struct.unpack_from('<84sIQQ', data, 32 + index * 104) for index in range(header[3])]
    return data, header, records


class PackedSeedTests(unittest.TestCase):
    def fixture(self, root, reverse=False):
        root.mkdir()
        payloads = {'z.vv': b'variant\tfout:0=float\n', 'A-0.spv': bytes(range(19)), 'variant.vk': b'vk 1\n',
                    'empty.vv': b''}
        for name in sorted(payloads, reverse=reverse):
            (root / name).write_bytes(payloads[name])
        (root / 'seed-id.txt').write_bytes(b'a' * 64 + b'\n')
        (root / 'seed-manifest.json').write_text('{}\n')
        (root / 'old.metal').write_text('not a Vulkan payload')
        (root / 'nested').mkdir()
        (root / 'nested/ignored.spv').write_bytes(b'nested')
        return payloads

    def test_sorted_index_offsets_contents_and_zero_padding(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'seed'
            payloads = self.fixture(root)
            with mock.patch.object(packer, 'COPY_BYTES', 7):
                path = packer.pack_seed(root)
            self.assertEqual(path, root / 'seed.vkpack')
            data, header, records = read_pack(path)
            self.assertEqual(header, (b'TOTKVKP1', 1, 104, len(payloads), len(data)))
            self.assertEqual([row[0].split(b'\0', 1)[0].decode('ascii') for row in records], sorted(payloads))
            end = 32 + 104 * len(payloads)
            for raw_name, reserved, offset, length in records:
                name, padding = raw_name.split(b'\0', 1)
                name = name.decode('ascii')
                self.assertEqual(padding, b'\0' * (83 - len(name)))
                self.assertEqual(reserved, 0)
                self.assertEqual(offset % 8, 0)
                self.assertGreaterEqual(offset, end)
                self.assertEqual(data[end:offset], b'\0' * (offset - end))
                self.assertEqual(length, len(payloads[name]))
                self.assertEqual(data[offset:offset + length], payloads[name])
                self.assertEqual((root / name).read_bytes(), payloads[name])
                end = offset + length
            self.assertEqual(end, len(data))
            self.assertEqual((root / 'seed-id.txt').read_bytes(), b'a' * 64 + b'\n')

    def test_deterministic_across_order_timestamps_and_repacking(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first, second = root / 'first', root / 'second'
            self.fixture(first)
            self.fixture(second, reverse=True)
            for file in second.iterdir():
                os.utime(file, ns=(1_000_000_000, 2_000_000_000))
            second_output = root / 'custom.vkpack'
            a = packer.pack_seed(first).read_bytes()
            self.assertEqual(packer.pack_seed(second, second_output), second_output)
            self.assertEqual(a, second_output.read_bytes())
            self.assertEqual(a, packer.pack_seed(first).read_bytes())
            self.assertEqual(a, packer.pack_seed(second, second_output).read_bytes())

    def test_empty_directory_is_an_empty_pack(self):
        with tempfile.TemporaryDirectory() as folder:
            data, header, records = read_pack(packer.pack_seed(folder))
            self.assertEqual(data, struct.pack('<8sIIQQ', b'TOTKVKP1', 1, 104, 0, 32))
            self.assertEqual(header[3], 0)
            self.assertEqual(records, [])

    def test_name_validation_and_maximum_length(self):
        bad = ['', '.spv', '.vk', '.vv', 'a/b.spv', 'a\\b.vk', 'a b.vv', 'a\n.vk', 'é.spv',
               'a\0.vv', 'a' * 80 + '.spv', 'variant.SPV', 'shader.txt', '..']
        for name in bad:
            with self.subTest(name=name), self.assertRaises(ValueError):
                packer._validate_name(name)
        with tempfile.TemporaryDirectory() as folder:
            name = 'a' * 79 + '.spv'
            (Path(folder) / name).write_bytes(b'maximum')
            _, _, records = read_pack(packer.pack_seed(folder))
            self.assertEqual(records[0][0], name.encode('ascii') + b'\0')

    def test_bad_payload_name_leaves_existing_output_untouched(self):
        for name in ('é.spv', 'bad name.vk', '.vv', 'a' * 80 + '.spv'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / name).write_bytes(b'bad')
                output = root / 'seed.vkpack'
                output.write_bytes(b'old complete pack')
                with self.assertRaises(ValueError):
                    packer.pack_seed(root)
                self.assertEqual(output.read_bytes(), b'old complete pack')
                self.assertEqual(sorted(p.name for p in root.iterdir()), sorted([name, 'seed.vkpack']))

    def test_payload_symlinks_directories_and_fifos_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            outside = root / 'outside.spv'
            outside.write_bytes(b'outside')
            for kind in ('outside-link', 'inside-link', 'dangling-link', 'directory', 'fifo'):
                seed = root / kind
                seed.mkdir()
                payload = seed / 'variant.spv'
                if kind == 'outside-link':
                    payload.symlink_to(outside)
                elif kind == 'inside-link':
                    (seed / 'actual.bin').write_bytes(b'inside')
                    payload.symlink_to('actual.bin')
                elif kind == 'dangling-link':
                    payload.symlink_to('missing')
                elif kind == 'directory':
                    payload.mkdir()
                else:
                    os.mkfifo(payload)
                with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, 'regular file'):
                    packer.pack_seed(seed)
                self.assertFalse((seed / 'seed.vkpack').exists())
            self.assertEqual(outside.read_bytes(), b'outside')

    def test_source_cannot_be_used_as_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            payload = root / 'variant.spv'
            payload.write_bytes(b'source')
            marker = root / 'seed-id.txt'
            marker.write_bytes(b'identity')
            for output in (payload, marker):
                with self.subTest(output=output), self.assertRaisesRegex(ValueError, 'source file'):
                    packer.pack_seed(root, output)
            self.assertEqual(payload.read_bytes(), b'source')
            self.assertEqual(marker.read_bytes(), b'identity')

    def test_output_symlink_is_not_followed_or_replaced(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            outside = root / 'outside'
            outside.write_bytes(b'outside')
            seed = root / 'seed'
            seed.mkdir()
            output = seed / 'seed.vkpack'
            output.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                packer.pack_seed(seed)
            self.assertTrue(output.is_symlink())
            self.assertEqual(outside.read_bytes(), b'outside')

    def test_failed_write_and_replace_preserve_output_and_remove_temporary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'seed'
            self.fixture(root)
            output = root / 'seed.vkpack'
            output.write_bytes(b'previous pack')
            before = sorted(file.name for file in root.iterdir())
            def fail_copy(directory_fd, name, expected, stream):
                stream.write(b'incomplete')
                raise OSError('injected write failure')
            for owner, target, effect in ((packer, '_copy_payload', fail_copy),
                                          (packer.os, 'replace', OSError('injected replace failure'))):
                with self.subTest(target=target), mock.patch.object(owner, target, side_effect=effect):
                    with self.assertRaises(OSError):
                        packer.pack_seed(root)
                self.assertEqual(output.read_bytes(), b'previous pack')
                self.assertEqual(sorted(file.name for file in root.iterdir()), before)
            output.unlink()
            with mock.patch.object(packer, '_copy_payload', side_effect=fail_copy), self.assertRaises(OSError):
                packer.pack_seed(root)
            self.assertFalse(output.exists())
            self.assertEqual(sorted(file.name for file in root.iterdir()), [name for name in before if name != 'seed.vkpack'])

    def test_replacement_happens_once_after_complete_file_is_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'seed'
            self.fixture(root)
            output = root / 'seed.vkpack'
            output.write_bytes(b'old')
            replace = os.replace
            observed = []
            def publish(temporary, destination):
                self.assertEqual(temporary.parent, output.parent)
                self.assertEqual(destination, output)
                self.assertEqual(output.read_bytes(), b'old')
                data, header, _ = read_pack(temporary)
                self.assertEqual(header[4], len(data))
                observed.append(temporary)
                replace(temporary, destination)
            with mock.patch.object(packer.os, 'replace', side_effect=publish):
                packer.pack_seed(root)
            self.assertEqual(len(observed), 1)
            self.assertFalse(observed[0].exists())
            self.assertEqual(output.read_bytes()[:8], b'TOTKVKP1')

    def test_cli_seed_and_custom_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            seed = root / 'seed'
            seed.mkdir()
            (seed / 'variant.vk').write_bytes(b'vk 1\n')
            output = root / 'custom.vkpack'
            with mock.patch('builtins.print') as print_output:
                self.assertEqual(packer.main([str(seed), '--output', str(output)]), 0)
            print_output.assert_called_once_with(output)
            self.assertEqual(read_pack(output)[1][3], 1)
            self.assertFalse((seed / 'seed.vkpack').exists())


class AndroidPackedSeedTests(unittest.TestCase):
    def refresh_fixture(self, root):
        seed, out = root/'prepared', root/'output'
        seed.mkdir(); previous = out/'shaders/v143'; previous.mkdir(parents=True)
        key, rejected = 'a'*64, 'c'*64
        names = [key+'-'+'1'*12, key+'-'+'2'*12]
        for variant in [*names, rejected+'-'+'3'*12]:
            (previous/(variant+'.spv')).write_bytes(struct.pack(
                '<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, 5, 1, 0))
            (previous/(variant+'.vk')).write_text('vk 1\nstage 5\n')
        (previous/(key+'.vv')).write_text(''.join(f'{v}\ttex:6=2d_depth\n' for v in names))
        (previous/(rejected+'.vv')).write_text(f'{rejected}-{"3"*12}\t\n')
        (previous/'seed.vkpack').write_bytes(b'previous pack stays until validated replacement')
        for variant in names:
            (seed/(variant+'.spv')).write_bytes((previous/(variant+'.spv')).read_bytes())
        manifest = dict(schema=1, chain='totk-native-v5-unverified', version='v143', game='totk',
                        translator_sha256='d'*64, source_romfs_sha256='e'*64,
                        preparation_sha256='f'*64, shader_list_sha256='0'*64)
        (seed/'seed-manifest.json').write_text(android.json.dumps(manifest))
        (seed/'shader-keys.json').write_text(android.json.dumps(dict(keys=[key])))
        def convert(source, destination):
            for variant in names:
                (destination/(variant+'.vk')).write_text('vk 1\nstage 5\n')
            (destination/(key+'.vv')).write_text(''.join(f'{v}\ttex:6=2d_depth\n' for v in names))
            return len(names), 0, {}
        return seed, out, previous, names, manifest, convert

    def test_refresh_retains_exact_names_and_publishes_closed_pinned_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            seed, out, previous, names, manifest, convert = self.refresh_fixture(Path(folder))
            with mock.patch.dict(android.sys.modules, vk_reflect=reflection), \
                    mock.patch.object(reflection, 'convert', side_effect=convert), mock.patch.object(android, 'say'):
                android.place_shaders(seed, out, 'v143')
            report = android.json.loads((previous/'seed-stage-manifest.json').read_text())
            self.assertEqual(report['retained_variants'], 2)
            self.assertEqual(report['translator_sha256'], manifest['translator_sha256'])
            self.assertEqual(report['chain'], manifest['chain'])
            self.assertEqual(report['retained_outside_inventory'], ['c'*64+'-'+'3'*12])
            self.assertEqual({p.stem for p in previous.glob('*.spv')}, set(names))
            self.assertFalse((previous/'seed.vkpack').exists())
            for name, digest in report['files'].items():
                self.assertEqual(android.sha256(previous/name), digest)

    def test_refresh_content_collision_keeps_previous_seed_intact(self):
        with tempfile.TemporaryDirectory() as folder:
            seed, out, previous, names, _, convert = self.refresh_fixture(Path(folder))
            (seed/(names[1]+'.spv')).write_bytes(b'different regenerated content')
            with mock.patch.dict(android.sys.modules, vk_reflect=reflection), \
                    mock.patch.object(reflection, 'convert', side_effect=convert), mock.patch.object(android, 'say'):
                with self.assertRaisesRegex(android.BuildError, 'different regenerated content'):
                    android.place_shaders(seed, out, 'v143')
            self.assertEqual((previous/'seed.vkpack').read_bytes(),
                             b'previous pack stays until validated replacement')
            self.assertEqual((previous/(names[1]+'.spv')).read_bytes(), struct.pack(
                '<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, 5, 1, 0))

    def test_build_flag_defaults_to_packed_with_explicit_loose_recovery(self):
        base = ['game.nsp', '-k', 'prod.keys']
        self.assertTrue(android.parse_args(base).packed_shader_seed)
        self.assertFalse(android.parse_args([*base, '--no-packed-shader-seed']).packed_shader_seed)
        self.assertTrue(android.parse_args([*base, '--packed-shader-seed']).packed_shader_seed)
        tree = ast.parse(Path(android.__file__).read_text())
        functions = {node.name: node for node in tree.body if isinstance(node, ast.FunctionDef)}
        for name in ('main', 'runtime_only'):
            guarded = [node for node in ast.walk(functions[name]) if isinstance(node, ast.If)
                       and ast.unparse(node.test) == 'args.packed_shader_seed']
            self.assertEqual(len(guarded), 1, name)
            self.assertEqual(ast.unparse(guarded[0].body[0]), 'pack_shaders(out, version)')

    def test_staged_pack_keeps_payloads_and_existing_marker_verbatim(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            seed = out / 'shaders/v143'
            seed.mkdir(parents=True)
            (seed / 'variant.spv').write_bytes(b'spv')
            marker = seed / 'seed-id.txt'
            marker.write_bytes(b'preserve this namespace\n')
            with mock.patch.object(android, 'say'):
                android.pack_shaders(out, 'v143')
            self.assertEqual((seed / 'variant.spv').read_bytes(), b'spv')
            self.assertEqual(marker.read_bytes(), b'preserve this namespace\n')
            self.assertEqual(read_pack(seed / 'seed.vkpack')[1][3], 1)
            marker.unlink()
            with mock.patch.object(android, 'say'):
                android.pack_shaders(out, 'v143')
            self.assertFalse(marker.exists())

    def test_missing_staged_seed_is_actionable(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(android.BuildError, 'needs a staged seed'):
                android.pack_shaders(Path(folder), 'v143')

    def test_generated_pushes_offer_packed_only_transfer_without_overlay_deletion(self):
        headless = android.PUSH_SH.format(gb=17)
        install = android.INSTALL_SH.format(package=android.APP_PACKAGE)
        self.assertIn('PACKED=1', headless)
        self.assertIn('elif [ "$1" = --loose ]; then PACKED=0; shift; fi', headless)
        self.assertIn('[ -n "$1" ] && DEST="$1"', headless)
        self.assertIn('fresh destination', headless)
        packed_headless = headless.split('for seed in shaders/*; do', 1)[1].split('done', 1)[0]
        packed_install = install.split('if [ "$1" != data-loose ]; then', 1)[1].split('elif [ -d shaders ]; then', 1)[0]
        for script, destination in ((packed_headless, 'DEST'), (packed_install, 'DIR')):
            self.assertIn(f'adb push "$seed/seed.vkpack" "${destination}/$seed/seed.vkpack.tmp"', script)
            self.assertIn(f'adb shell mv "${destination}/$seed/seed.vkpack.tmp" "${destination}/$seed/seed.vkpack"', script)
            self.assertIn(f'adb push --sync "$seed/seed-id.txt" "${destination}/$seed/"', script)
            self.assertNotIn('adb push --sync shaders', script)
            self.assertNotIn('rm ', script)
            self.assertNotIn('find ', script)
        self.assertIn('adb push --sync shaders "$DEST/"', headless)
        self.assertIn('adb push --sync shaders "$DIR/"', install)


if __name__ == '__main__':
    unittest.main()
