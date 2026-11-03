"""TOTK_ASPECT on Android: the canvas holds a wider view squeezed into its own 16:9 shape (runtime/aspect_patch.h), and
the presenter (android/gfx/vk_draw.c) shows it at that ratio with black bars, never stretched over the window.

The geometry is android/gfx/vk_present_policy.h (vk_present_shown_width, vk_present_fit_shown, vk_present_guest_fit),
compiled into a small C driver; the presenter's every fit site is checked to go through it.
CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'

DRIVER = r'''
#include "vk_present_policy.h"
#include <stdio.h>
int main(void) {
    char kind[16]; unsigned w, h, ow, oh; float a;
    while (scanf("%15s %u %u %f %u %u", kind, &w, &h, &a, &ow, &oh) == 6) {
        vk_present_rect r = kind[0] == 'f' ? vk_present_fit_shown(w, h, a, ow, oh)
                                           : vk_present_guest_fit(vk_present_shown_width(w, h, a), h, ow, oh);
        printf("%d %d %u %u\n", r.x, r.y, r.w, r.h);
    }
    return 0;
}
'''


class AspectPresentation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='aspect-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(GFX), str(source), '-o', str(cls.driver)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def fit(self, kind, w, h, aspect, ow, oh):
        out = subprocess.run([str(self.driver)], input=f'{kind} {w} {h} {aspect} {ow} {oh}\n', capture_output=True, text=True,
                             timeout=30, check=True)
        return tuple(int(v) for v in out.stdout.split())

    def test_off_keeps_the_canvas_shape(self):
        self.assertEqual(self.fit('fit', 1280, 720, 0, 1920, 1080), (0, 0, 1920, 1080))
        self.assertEqual(self.fit('fit', 1280, 720, 0, 1280, 960), (0, 120, 1280, 720))
        self.assertEqual(self.fit('guest', 1280, 720, 0, 1920, 1080), (320, 180, 1280, 720))

    def test_wide_ratio_letterboxes_a_16_9_screen(self):
        x, y, w, h = self.fit('fit', 1280, 720, 21 / 9, 1920, 1080)
        self.assertEqual((x, w), (0, 1920))
        self.assertAlmostEqual(w / h, 21 / 9, delta=0.01)
        self.assertEqual(y, (1080 - h) // 2)
        self.assertGreater(y, 0)
        # Docked guest output (no upsampling): the canvas height stays, its shown width is wider.
        x, y, w, h = self.fit('guest', 1280, 720, 21 / 9, 1920, 1080)
        self.assertEqual((w, h), (1680, 720))
        self.assertEqual((x, y), (120, 180))

    def test_narrow_ratio_pillarboxes(self):
        x, y, w, h = self.fit('fit', 1280, 720, 4 / 3, 1920, 1080)
        self.assertEqual((y, h, w), (0, 1080, 1440))
        self.assertEqual(x, 240)

    def test_a_screen_of_that_shape_is_filled(self):
        self.assertEqual(self.fit('fit', 1280, 720, 32 / 9, 3840, 1080), (0, 0, 3840, 1080))

    def test_every_presenter_fit_uses_the_shown_ratio(self):
        draw = (GFX / 'vk_draw.c').read_text()
        self.assertNotRegex(draw, r'fmin\(\(double\)\s*ow\s*/\s*w\b')   # no fit by the canvas's own width
        self.assertEqual(len(re.findall(r'vk_present_fit_shown\(w, ?h, ?pres_shown_aspect\(\)', draw)), 2)
        self.assertEqual(len(re.findall(r'vk_present_shown_width\(', draw)), 3)
        self.assertIn('pres_shown_aspect() != g_guest_aspect', draw)   # key 4 re-fits the cached docked output


if __name__ == '__main__':
    unittest.main()
