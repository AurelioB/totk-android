"""Exercise the publish wrapper with a recording SDK, without compiling or running target code."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PUBLISH = ROOT / 'games/totk/tools/totk_xlate/publish_native.sh'


class AndroidXlatePublishTests(unittest.TestCase):
    def fixture(self, root):
        tools = root / 'games/totk/tools/totk_xlate'
        (tools / 'patches').mkdir(parents=True)
        script = tools / 'publish_native.sh'
        shutil.copy2(PUBLISH, script)
        shutil.copy2(PUBLISH.with_name('check_android_crypto.py'), tools / 'check_android_crypto.py')
        (tools / 'Program.cs').write_text('// recording SDK fixture\n')
        (tools / 'totk_xlate.csproj').write_text('<Project />\n')
        vendor = root / 'vendor/ryujinx/src'
        vendor.mkdir(parents=True)
        (vendor / 'fixture.txt').write_text('original\n')
        (tools / 'patches/0001-fixture.patch').write_text(
            '--- a/src/fixture.txt\n+++ b/src/fixture.txt\n@@ -1 +1 @@\n-original\n+patched\n')
        host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
        ndk = root / 'ndk'
        ndkbin = ndk / f'toolchains/llvm/prebuilt/{host}/bin'
        ndkbin.mkdir(parents=True)
        for name in ('aarch64-linux-android33-clang', 'llvm-objcopy'):
            tool = ndkbin / name
            tool.write_text('#!/bin/sh\nexit 99\n')   # must never be invoked by the recording SDK
            tool.chmod(0o755)
        dotnet = root / 'record-dotnet'
        dotnet.write_text(
            f'#!{sys.executable}\n'
            'import json, os, pathlib, sys\n'
            'args = sys.argv[1:]\n'
            'if args == ["--version"]:\n'
            '    print(os.environ.get("RECORD_SDK_VERSION", "10.0.402"))\n'
            '    sys.exit(0)\n'
            'assert args[0] == "publish"\n'
            'project = pathlib.Path(args[1])\n'
            'targets = project.parent / "Directory.Build.targets"\n'
            'record = {"args": args, "targets": targets.read_text() if targets.exists() else None}\n'
            'pathlib.Path(os.environ["PUBLISH_RECORD"]).write_text(json.dumps(record))\n'
            'out = pathlib.Path(args[args.index("-o") + 1])\n'
            'out.mkdir(parents=True)\n'
            '(out / "totk_xlate").write_bytes(os.environ.get("RECORD_CRYPTO_TOKEN", "recording SDK packaging fixture").encode())\n')
        dotnet.chmod(0o755)
        env = dict(os.environ, PUBLISH_RECORD=str(root / 'publish.json'))
        return script, ndk, ndkbin, dotnet, env

    def test_android_sdk_flags_compiler_alignment_and_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script, ndk, ndkbin, dotnet, env = self.fixture(root)
            out = root / 'staged'
            result = subprocess.run(['sh', str(script), '--android', '--ndk', str(ndk),
                                     '--dotnet', str(dotnet), '--out', str(out)],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('Android translator crypto check: PASS', result.stdout)
            record = json.loads((root / 'publish.json').read_text())
            args = record['args']
            self.assertEqual(args[args.index('-r') + 1], 'linux-bionic-arm64')
            self.assertEqual(args[args.index('--self-contained') + 1], 'true')
            self.assertIn('-p:PublishAot=true', args)
            self.assertIn('-p:PublishAotUsingRuntimePack=true', args)
            self.assertIn('-p:InvariantGlobalization=true', args)
            self.assertIn(f'-p:CppCompilerAndLinker={ndkbin}/aarch64-linux-android33-clang', args)
            self.assertIn('<LinkerArg Include="-Wl,-z,max-page-size=16384"', record['targets'])
            binary = out / 'totk_xlate'
            self.assertEqual(binary.read_bytes(), b'recording SDK packaging fixture')
            self.assertEqual(binary.stat().st_mode & 0o777, 0o755)
            self.assertEqual(binary.with_suffix('.sha256').read_text().strip(),
                             hashlib.sha256(binary.read_bytes()).hexdigest())
            self.assertEqual((root / 'vendor/ryujinx/src/fixture.txt').read_text(), 'original\n')

    def test_android_rejects_native_crypto_before_installing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script, ndk, ndkbin, dotnet, env = self.fixture(root)
            out = root / 'rejected'
            for marker in ('libssl.so', 'libcrypto.so', 'System.Security.Cryptography.Native.OpenSsl', 'CryptoNative_EvpSha256'):
                with self.subTest(marker=marker):
                    result = subprocess.run(['sh', str(script), '--android', '--ndk', str(ndk),
                                             '--dotnet', str(dotnet), '--out', str(out)],
                                            env=dict(env, RECORD_CRYPTO_TOKEN=marker), capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Android translator crypto check: FAIL', result.stderr)
                    self.assertFalse(out.exists(), 'rejected binary must not replace a shipped/staged executable')

    def test_android_prerequisite_failures_are_actionable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script, ndk, ndkbin, dotnet, env = self.fixture(root)
            common = ['sh', str(script), '--android', '--ndk', str(ndk), '--dotnet', str(dotnet)]
            for extra, version, message in ((['--api', '32'], '10.0.402', 'API 33 or newer'),
                                            ([], '9.0.100', 'needs the .NET 10 SDK')):
                with self.subTest(extra=extra, sdk=version):
                    result = subprocess.run([*common, *extra], env=dict(env, RECORD_SDK_VERSION=version),
                                            capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(message, result.stderr)
                    self.assertFalse((root / 'publish.json').exists())
            (ndkbin / 'aarch64-linux-android33-clang').unlink()
            result = subprocess.run(common, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('NDK compiler missing:', result.stderr)
            self.assertFalse((root / 'publish.json').exists())


if __name__ == '__main__':
    unittest.main()
