"""Submix dependency ordering includes active splitter destinations."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "games/totk"


@unittest.skipUnless(sys.platform in ("darwin", "linux"), "native mixer probe needs macOS or Linux")
class AudioRouting(unittest.TestCase):
    def test_splitter_submix_dependencies(self):
        self.check_graphs([])

    def check_graphs(self, defines):
        if sys.platform == "darwin":
            compiler = ["xcrun", "clang"]
            platform_flags = []
            libraries = ["-framework", "AudioToolbox"]
        else:
            native_cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
            if not native_cc:
                self.skipTest("A C compiler is required for the native audio routing regression")
            compiler = [native_cc]
            # Match the Android Makefile's allowances for the device-free Linux backend.
            platform_flags = ["-D_GNU_SOURCE", "-Wno-unused-function", "-Wno-misleading-indentation", "-pthread"]
            libraries = ["-lm"]
        # TemporaryDirectory honors TMPDIR, keeping all compiler output in caller-selected scratch.
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "audio-routing-probe"
            build = subprocess.run(compiler + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(GAME / "runtime"),
            ] + defines + platform_flags + [
                str(GAME / "tests/audio_routing/probe.c"), str(GAME / "runtime/audio_effects.c"),
                str(GAME / "runtime/audio_reverb.c"),
            ] + libraries + ["-o", str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("4 audio routing graphs", result.stdout)
            self.assertIn("4 EOS sample paths", result.stdout)
            clock = Path(folder) / "audio-clock-probe"
            dead_strip = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
            build = subprocess.run(compiler + [
                "-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(GAME / "runtime"),
            ] + defines + platform_flags + [
                str(GAME / "tests/audio_routing/clock_probe.c"),
            ] + dead_strip + libraries + ["-o", str(clock)], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(clock)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("4 EOS metadata paths", result.stdout)


if __name__ == "__main__":
    unittest.main()
