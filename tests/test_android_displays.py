"""The second-screen classifier (android/java/.../companion/DisplayClass.java) on a host JVM with fake displays: the
companion and the menu use only a built-in second panel, decided from the displays present, never from the SoC.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
SOURCE = TOTK / 'android/java/com/aure/totk/companion/DisplayClass.java'
PROBE = TOTK / 'tests/displays/DisplayProbe.java'


class DisplayClassTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-displays-')
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', cls.temp.name, str(SOURCE), str(PROBE)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def classify(self, *sets):
        result = subprocess.run([self.java, '-cp', self.temp.name, 'com.aure.totk.companion.DisplayProbe'],
                                input='\n'.join(sets) + '\n', capture_output=True, text=True, timeout=60, check=True)
        return [tuple(line.split()) for line in result.stdout.splitlines()]

    def test_the_tested_handhelds(self):
        # adb dumpsys display, 2026-11-03 (soc-profiles.md): the Duo reports BUILT_IN on its second panel, the Thor
        # UNKNOWN on both panels with the same PnP id, the Odin has no presentation display.
        self.assertEqual(self.classify('QCM 2:1:1:QCM', 'QCM 4:1:0:QCM', 'QCM'),
                         [('BUILT_IN', '2'), ('BUILT_IN', '4'), ('NONE', '-1')])

    def test_external_cast_and_overlay_displays_are_ignored(self):
        self.assertEqual(self.classify(
            'QCM 5:1:2:DEL',     # HDMI/DP monitor: direct connection, its own EDID id
            'QCM 6:1:3:SAM',     # through a dock/hub
            'QCM 7:1:-1:-',      # cast, Miracast, screen recording, developer overlay: no product info
            'QCM 8:1:0:GSM',     # unknown connection with another maker's id
            'QCM 9:1:2:QCM',     # a direct connection is external even with the main panel's id
            '- 4:1:0:QCM',       # the main panel has no product info: an unknown-type display cannot be matched
            'QCM 4:1:0:-'),      # unknown connection without an id
            [('EXTERNAL', '-1')] * 7)

    def test_unusable_displays_never_count(self):
        # usable = valid, not private and not the display the owner is on
        self.assertEqual(self.classify('QCM 2:0:1:QCM', 'QCM 2:0:1:QCM 7:0:-1:-'), [('NONE', '-1')] * 2)

    def test_a_built_in_panel_wins_over_an_external_display(self):
        self.assertEqual(self.classify('QCM 7:1:-1:- 5:1:2:DEL 4:1:0:QCM', 'QCM 2:0:1:QCM 7:1:-1:-', 'QCM 2:1:1:QCM 3:1:1:QCM'),
                         [('BUILT_IN', '4'), ('EXTERNAL', '-1'), ('BUILT_IN', '2')])


if __name__ == '__main__':
    unittest.main()
