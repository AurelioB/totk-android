"""Translator input and gather-policy changes must invalidate shader seeds."""
import ast
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


android = load('android_build', 'games/totk/android/build_android.py')
seed = load('gather_seed', 'games/totk/scripts/prepare_shader_seed.py')


class TranslatorIdentityTests(unittest.TestCase):
    def fixture(self, root):
        source = root / 'games/totk'
        tools = source / 'tools/totk_xlate'
        (tools / 'patches').mkdir(parents=True)
        (tools / 'Program.cs').write_text('source')
        (tools / 'publish_native.sh').write_text('publish')
        vendor = root / 'vendor/ryujinx'
        vendor.mkdir(parents=True)
        (vendor / 'Shader.cs').write_text('vendor source')
        return source, tools, vendor

    def test_source_patch_script_and_vendor_invalidate(self):
        with tempfile.TemporaryDirectory() as folder:
            source, tools, vendor = self.fixture(Path(folder))
            previous = android.translator_inputs_hash(source)
            for file in (tools / 'Program.cs', tools / 'patches/0008.patch',
                         tools / 'publish_native.sh', vendor / 'Shader.cs'):
                file.write_text('changed ' + file.name)
                current = android.translator_inputs_hash(source)
                self.assertNotEqual(previous, current)
                previous = current
            (vendor / 'obj').mkdir()
            (vendor / 'obj/Generated.cs').write_text('not an input')
            self.assertEqual(previous, android.translator_inputs_hash(source))

    def test_explicit_translator_needs_no_stamp_or_sdk(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'custom'
            binary.write_bytes(b'custom translator')
            with mock.patch.object(android, 'run') as run:
                self.assertEqual(android.translator_path(SimpleNamespace(translator=binary), Path(folder), None), binary)
                run.assert_not_called()

    def test_shipped_host_prebuilt_needs_no_stamp_or_sdk(self):
        with tempfile.TemporaryDirectory() as folder:
            tree = Path(folder)
            binary = tree / 'prebuilt/linux-x64/totk_xlate'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'shipped translator')
            with mock.patch('platform.machine', return_value='x86_64'), \
                 mock.patch.object(android.shutil, 'which') as which, \
                 mock.patch.object(android, 'run') as run:
                self.assertEqual(android.translator_path(SimpleNamespace(translator=None), tree, tree), binary)
                which.assert_not_called()
                run.assert_not_called()

    def test_shipped_ondevice_prebuilt_is_staged_without_sdk(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            tree, out = root / 'work/totk', root / 'output'
            binary = tree / 'prebuilt/android-arm64/totk_xlate'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'shipped Android translator')
            binary.chmod(0o644)
            with mock.patch.object(android.shutil, 'which') as which, \
                 mock.patch.object(android, 'run') as run:
                staged = android.prepare_ondevice_translator(tree, root / 'ndk', 33, out, root)
                self.assertEqual(staged, out / 'totk_xlate')
                self.assertEqual(staged.read_bytes(), binary.read_bytes())
                self.assertEqual(staged.stat().st_mode & 0o777, 0o755)
                self.assertFalse((binary.parent / 'totk_xlate.inputs.sha256').exists())
                which.assert_not_called()
                run.assert_not_called()

    def test_missing_ondevice_sdk_fails_before_staging(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.fixture(root)
            out = root / 'output'
            with mock.patch.object(android.mac, 'REPO', root), \
                 mock.patch.object(android.shutil, 'which', return_value=None), \
                 mock.patch.object(android, 'run') as run, \
                 self.assertRaisesRegex(android.BuildError, '--ondevice-xlate:.*[.]NET 10 SDK'):
                android.prepare_ondevice_translator(root / 'work/totk', root / 'ndk', 33, out, root)
            self.assertFalse(out.exists())
            run.assert_not_called()

    def test_ondevice_publish_uses_work_output_and_invalidates_local_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, tools, _ = self.fixture(root)
            tree, out, ndk = root / 'work/totk', root / 'output', root / 'ndk'
            binary = tree / 'prebuilt/android-arm64/totk_xlate'
            def publish(command, *args, **kwargs):
                self.assertEqual(command[:2], ['sh', source / 'tools/totk_xlate/publish_native.sh'])
                self.assertIn('--android', command)
                self.assertEqual(command[command.index('--ndk') + 1], ndk)
                self.assertEqual(command[command.index('--out') + 1], binary.parent)
                self.assertEqual(kwargs['cwd'], source)
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b'fresh Android translator')
            with mock.patch.object(android.mac, 'REPO', root), \
                 mock.patch.object(android.shutil, 'which', return_value='/tool'), \
                 mock.patch.object(android, 'run', side_effect=publish) as run:
                staged = android.prepare_ondevice_translator(tree, ndk, 33, out, root)
                self.assertEqual(staged.read_bytes(), b'fresh Android translator')
                self.assertEqual(run.call_args.args[0][run.call_args.args[0].index('--api') + 1], '33')
                android.prepare_ondevice_translator(tree, ndk, 33, out, root)
                self.assertEqual(run.call_count, 1)
                (tools / 'Program.cs').write_text('changed Android service')
                android.prepare_ondevice_translator(tree, ndk, 33, out, root)
                self.assertEqual(run.call_count, 2)
                android.prepare_ondevice_translator(tree, ndk, 34, out, root)
                self.assertEqual(run.call_count, 3)
                binary.write_bytes(b'replaced binary')
                android.prepare_ondevice_translator(tree, ndk, 34, out, root)
                self.assertEqual(run.call_count, 4)
                self.assertFalse((source / 'prebuilt/android-arm64').exists())

    def test_ondevice_flag_is_explicit_and_forwarded_to_both_apk_paths(self):
        common = ['game.nsp', '-k', 'prod.keys']
        self.assertFalse(android.parse_args(common).ondevice_xlate)
        self.assertTrue(android.parse_args([*common, '--ondevice-xlate']).ondevice_xlate)
        tree = ast.parse(Path(android.__file__).read_text())
        functions = {node.name: node for node in tree.body if isinstance(node, ast.FunctionDef)}
        for name in ('main', 'runtime_only'):
            calls = [node for node in ast.walk(functions[name]) if isinstance(node, ast.Call)
                     and isinstance(node.func, ast.Name) and node.func.id == 'build_app']
            self.assertEqual(len(calls), 1)
            self.assertTrue(any(k.arg == 'ondevice_xlate' and ast.unparse(k.value) == 'ondevice_xlate'
                                for k in calls[0].keywords), name)
        self.assertIn('translator=ondevice_xlate', ast.unparse(functions['build_app']))
        # build_app delegates to build_release.app, whose package step hands the translator to build_apk.py.
        release = ast.parse((Path(android.__file__).parent / 'build_release.py').read_text())
        package = next(n for n in release.body if isinstance(n, ast.FunctionDef) and n.name == 'package')
        self.assertIn("'--ondevice-xlate'", ast.unparse(package))
        self.assertIn('TOTK_SHADER_TRANSLATOR="$PWD/totk_xlate"', android.RUN_SH.format(version='v143'))
        self.assertIn('adb push totk_xlate "$DEST/"', android.PUSH_SH.format(gb=17))

    def test_stale_or_replaced_binary_rebuilds_from_source(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, _, _ = self.fixture(root)
            tree = root / 'work/totk'
            def publish(*args, **kwargs):
                fresh = source / 'prebuilt/linux-x64/totk_xlate'
                fresh.parent.mkdir(parents=True, exist_ok=True)
                fresh.write_bytes(b'fresh translator')
                fresh.with_suffix('.sha256').write_text(android.sha256(fresh))
            with mock.patch.object(android.mac, 'REPO', root), \
                 mock.patch('platform.machine', return_value='x86_64'), \
                 mock.patch.object(android.shutil, 'which', return_value='/tool'), \
                 mock.patch.object(android, 'run', side_effect=publish) as run:
                args = SimpleNamespace(translator=None)
                binary = android.translator_path(args, tree, root)
                self.assertEqual(binary.read_bytes(), b'fresh translator')
                self.assertEqual(Path(run.call_args.args[0][1]), source / 'tools/totk_xlate/publish_native.sh')
                android.translator_path(args, tree, root)
                self.assertEqual(run.call_count, 1)
                binary.write_bytes(b'replaced binary')
                android.translator_path(args, tree, root)
                self.assertEqual(run.call_count, 2)

    def test_seed_identity_includes_effective_gather_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'index.json').write_text(json.dumps({'romfs_sha256': 'romfs'}))
            binary = root / 'translator'
            binary.write_bytes(b'binary')
            identities = []
            with mock.patch.object(seed.ShaderList, 'load', return_value=SimpleNamespace(variants=[])), \
                 mock.patch.object(seed, 'reusable_seed', side_effect=lambda _, identity, __: identities.append(identity) or {'shaders': 1}):
                for value in ('0', '1', ''):
                    with mock.patch.dict(seed.os.environ, {'TOTK_GATHER_BIAS': value}):
                        seed.prepare(root, root, binary, binary, 'v143', shader_list=binary)
            self.assertFalse(identities[0]['gather_bias'])
            self.assertTrue(identities[1]['gather_bias'])
            self.assertEqual(identities[1], identities[2])
            self.assertNotEqual(identities[0], identities[1])


if __name__ == '__main__':
    unittest.main()
