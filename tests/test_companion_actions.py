"""Companion item-action request identity, duplicate-tap dropping and result lifetime on a host JVM.

ItemActions is plain Java; Android drawing and the poller thread are not exercised here.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'


class CompanionActions(unittest.TestCase):
    def test_item_actions(self):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            self.skipTest('JDK 17+ is required for the companion action regression')
        with tempfile.TemporaryDirectory() as temp:
            sources = [TOTK / 'android/java/com/aure/totk/companion/ItemActions.java',
                       TOTK / 'tests/companion/ItemActionsProbe.java']
            compiled = subprocess.run([javac, '--release', '17', '-d', temp, *map(str, sources)],
                                      capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([java, '-ea', '-cp', temp, 'ItemActionsProbe'],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('pass: companion item actions', result.stdout)


if __name__ == '__main__':
    unittest.main()
