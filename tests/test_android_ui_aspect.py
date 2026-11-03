"""TOTK_ASPECT on Android keeps the HUD's proportions: the HUD quads drawn into the 16:9 canvas are scaled about its
centre (android/gfx/vk_ui_aspect.h) so that, once the presenter shows the canvas at the requested ratio
(vk_present_policy.h), circles stay round and the HUD stays inside the shown picture.

A small C driver runs the real squeeze math and draw decision against the shared canvas tracker (vk_canvas.c) with fake
draws; vk_draw.c is checked to apply it to the viewport and scissor of every draw before native-UI scaling.
CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'

DRIVER = r'''
#include "vk_ui_aspect.h"
#include "vk_present_policy.h"
#include <stdio.h>
#include <string.h>
static vk_canvas_image canvas = {.image=1,.incarnation=1,.storage=0x10000,.size=1280*720*4,.serial=1,.width=1280,.height=720,.format=97};
static vk_canvas_image window = {.image=2,.incarnation=2,.storage=0x1000000,.size=1920*1080*4,.serial=2,.width=1920,.height=1080,.format=37};
static vk_canvas_image other = {.image=3,.incarnation=3,.storage=0x2000000,.size=1280*720*8,.serial=3,.width=1280,.height=720,.format=97};
int main(void) {
    char cmd[32];
    while (scanf("%31s", cmd) == 1) {
        if (!strcmp(cmd, "requested")) {
            char value[64]; if (scanf("%63s", value) != 1) return 2;
            float a = vk_ui_aspect_requested(strcmp(value, "-") ? value : NULL);
            printf("%.6f %d\n", a, vk_ui_aspect_tracks(a));
        } else if (!strcmp(cmd, "init")) {
            int n, f, a; if (scanf("%d %d %d", &n, &f, &a) != 3) return 2;
            vk_canvas_init(n, f, a); printf("%d\n", vk_canvas_on);
        } else if (!strcmp(cmd, "transfer")) {   /* the terminal transfer canvas -> window, emitted */
            vk_canvas_transfer(&canvas, &window, 1); printf("ok\n");
        } else if (!strcmp(cmd, "frame_end")) {
            vk_canvas_frame_end(); printf("ok\n");
        } else if (!strcmp(cmd, "first")) {   /* the first UI draw: canvas_ui == 2 */
            char key[16], which[16]; if (scanf("%15s %15s", which, key) != 2) return 2;
            int r = vk_canvas_first_ui(!strcmp(which, "canvas") ? &canvas : &other, key);
            if (r) vk_canvas_ui_commit(&canvas);
            printf("%d\n", r);
        } else if (!strcmp(cmd, "draw")) {
            /* shown target ui_class count vx vy vw vh sx0 sy0 sx1 sy1 -> on vx vy vw vh sx0 sy0 sx1 sy1 shown_w/h ratio */
            float shown, x, y, w, h; char which[16]; int ui; unsigned count; int x0, y0, x1, y1;
            if (scanf("%f %15s %d %u %f %f %f %f %d %d %d %d", &shown, which, &ui, &count, &x, &y, &w, &h, &x0, &y0, &x1, &y1) != 12) return 2;
            const vk_canvas_image *t = !strcmp(which, "canvas") ? &canvas : &other;
            vk_ui_aspect a = vk_ui_aspect_draw(shown, t, ui, count);
            int on = vk_ui_aspect_on(&a);
            if (on) { vk_ui_aspect_viewport(&a, &x, &y, &w, &h); vk_ui_aspect_rect(&a, &x0, &y0, &x1, &y1); }
            /* As shown: the presenter stretches the canvas to its shown width at the canvas height. */
            double stretch = (double)vk_present_shown_width(t->width, t->height, shown) / t->width;
            printf("%d %.4f %.4f %.4f %.4f %d %d %d %d %.4f\n", on, x, y, w, h, x0, y0, x1, y1, w * stretch / (h < 0 ? -h : h));
        } else return 2;
        fflush(stdout);
    }
    return 0;
}
'''

RING = '0 0 194 194'   # a minimap ring quad's viewport at the canvas origin


class HudAspect(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='ui-aspect-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(GFX), str(source),
                                 str(GFX / 'vk_canvas.c'), '-lm', '-o', str(cls.driver)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_script(self, *lines):
        out = subprocess.run([str(self.driver)], input='\n'.join(lines) + '\n', capture_output=True, text=True,
                             timeout=30, check=True)
        return out.stdout.splitlines()

    def ui_frame(self, *draws, tracker=1):
        """A frame after the canvas is known: the first UI draw begins the UI, then the given draws."""
        out = self.run_script(f'init 0 0 {tracker}', 'transfer', 'frame_end', 'first canvas 7f2113e2',
                              *(f'draw {d}' for d in draws))
        return [self.parse(line) for line in out[4:]], out[3]

    @staticmethod
    def parse(line):
        v = line.split()
        return {'on': int(v[0]), 'vp': tuple(float(x) for x in v[1:5]), 'sc': tuple(int(x) for x in v[5:9]),
                'shown_ratio': float(v[9])}

    def test_requested_ratio_switches_the_tracker_only_off_16_9(self):
        rows = self.run_script('requested -', 'requested 0', 'requested 16:9', 'requested 1920x1080', 'requested 21:9',
                               'requested 4:3', 'requested 2.39', 'requested bogus', 'requested 9')
        tracks = [int(r.split()[1]) for r in rows]
        self.assertEqual(tracks, [0, 0, 0, 0, 1, 1, 1, 0, 0])
        self.assertAlmostEqual(float(rows[4].split()[0]), 21 / 9, places=5)
        self.assertEqual(self.run_script('init 0 0 0', 'init 0 0 1', 'init 1 0 0', 'init 0 1 0'), ['0', '1', '1', '1'])

    def test_wide_ratio_squeezes_hud_quads_horizontally(self):
        (ring, corner), first = self.ui_frame(f'2.333333 canvas 2 6 {RING} 0 0 1280 720',
                                              '2.333333 canvas 1 6 1180 620 100 100 1100 600 1280 720')
        self.assertEqual(first, '1')
        f = 21 / 9 * 9 / 16
        self.assertEqual(ring['on'], 1)
        self.assertAlmostEqual(ring['vp'][2], 194 / f, places=2)   # narrower in the canvas
        self.assertEqual(ring['vp'][3], 194)
        self.assertAlmostEqual(ring['vp'][0], 640 - 640 / f, places=2)   # about the canvas centre
        self.assertAlmostEqual(ring['shown_ratio'], 1.0, delta=0.005)   # round once the presenter stretches it
        self.assertAlmostEqual(corner['vp'][0] + corner['vp'][2], 640 + 640 / f, places=2)   # inside the middle 16:9
        self.assertLessEqual(corner['sc'][0], round(640 + (1100 - 640) / f))
        self.assertGreaterEqual(corner['sc'][2], round(640 + (1280 - 640) / f) - 1)
        self.assertEqual((corner['sc'][1], corner['sc'][3]), (600, 720))

    def test_narrow_ratio_scales_hud_vertically_inside_the_shown_area(self):
        (ring, corner), _ = self.ui_frame(f'1.333333 canvas 2 6 {RING} 0 0 1280 720',
                                          '1.333333 canvas 1 6 0 620 1280 100 0 0 1280 720')
        f = 4 / 3 * 9 / 16
        self.assertEqual(ring['on'], 1)
        self.assertEqual(ring['vp'][2], 194)
        self.assertAlmostEqual(ring['vp'][3], 194 * f, places=2)
        self.assertAlmostEqual(ring['vp'][1], 360 - 360 * f, places=2)
        self.assertAlmostEqual(ring['shown_ratio'], 1.0, delta=0.005)
        x, y, w, h = corner['vp']
        self.assertGreaterEqual(x, 0); self.assertLessEqual(x + w, 1280)
        self.assertGreaterEqual(y, 0); self.assertLessEqual(y + h, 720)
        self.assertAlmostEqual(y + h, 360 + 360 * f, places=2)
        self.assertEqual((corner['sc'][0], corner['sc'][2]), (0, 1280))
        self.assertEqual((corner['sc'][1], corner['sc'][3]), (90, 630))

    def test_16_9_and_unset_change_nothing(self):
        for shown in ('0', '1.777778'):
            (ring,), _ = self.ui_frame(f'{shown} canvas 2 6 {RING} 0 0 1280 720')
            self.assertEqual(ring['on'], 0)
            self.assertEqual(ring['vp'], (0, 0, 194, 194))
            self.assertEqual(ring['sc'], (0, 0, 1280, 720))
        # The present at 16:9 shows the unchanged ring round.
        self.assertAlmostEqual(ring['shown_ratio'], 1.0, delta=0.002)

    def test_only_hud_quads_on_the_canvas_after_the_ui_begins(self):
        draws = (f'2.333333 canvas 0 6 {RING} 0 0 1280 720',     # not a UI vertex shader (inventory 3D scene quad)
                 f'2.333333 canvas 2 3 {RING} 0 0 1280 720',     # full-screen triangle with a UI shader
                 f'2.333333 canvas 1 4 {RING} 0 0 1280 720',     # not a 6-vertex quad
                 f'2.333333 other 2 6 {RING} 0 0 1280 720',      # a same-sized target that is not the canvas
                 f'2.333333 canvas 1 6 {RING} 0 0 1280 720')     # a later-list UI quad on the canvas
        results, _ = self.ui_frame(*draws)
        self.assertEqual([r['on'] for r in results], [0, 0, 0, 0, 1])
        # Before the UI begins this frame (scene work), and before any transfer taught the canvas: unchanged.
        before = self.run_script('init 0 0 1', 'transfer', 'frame_end', f'draw 2.333333 canvas 1 6 {RING} 0 0 1280 720')
        self.assertEqual(self.parse(before[3])['on'], 0)
        unknown = self.run_script('init 0 0 1', 'first canvas 7f2113e2', f'draw 2.333333 canvas 2 6 {RING} 0 0 1280 720')
        self.assertEqual(unknown[1], '0')
        self.assertEqual(self.parse(unknown[2])['on'], 0)
        # The next frame starts outside the UI again.
        nxt = self.run_script('init 0 0 1', 'transfer', 'frame_end', 'first canvas 7f2113e2', 'frame_end',
                              f'draw 2.333333 canvas 1 6 {RING} 0 0 1280 720')
        self.assertEqual(self.parse(nxt[5])['on'], 0)

    def test_tracker_off_never_squeezes(self):
        (ring,), first = self.ui_frame(f'2.333333 canvas 2 6 {RING} 0 0 1280 720', tracker=0)
        self.assertEqual(first, '0')
        self.assertEqual(ring['on'], 0)

    def test_vulkan_backend_applies_it_to_every_draw(self):
        draw = (GFX / 'vk_draw.c').read_text()
        post = (GFX / 'vk_post.c').read_text()
        self.assertIn('vk_ui_aspect_tracks(vk_ui_aspect_requested(getenv("TOTK_ASPECT")))', post)
        self.assertIn('g_ui_aspect = vk_ui_aspect_draw(shown, &target, sh[0]->canvas_ui, count);', draw)
        body = draw[draw.index('static void dynamic_state('):]
        body = body[:body.index('\n}\n')]
        # Squeezed in guest coordinates, before native-UI scaling and before recording / frame-generation capture.
        self.assertLess(body.index('vk_ui_aspect_viewport('), body.index('if (R.native)'))
        self.assertLess(body.index('vk_ui_aspect_rect('), body.index('D.sc = native_rect(D.sc)'))
        self.assertLess(body.index('vk_ui_aspect_viewport('), body.index('g_record_draw->dynamic = D'))
        self.assertLess(body.index('vk_ui_aspect_viewport('), body.index('FGX.record->draw.dynamic = D'))
        self.assertEqual(draw.count('dynamic_state(cb, prim)'), 1)


if __name__ == '__main__':
    unittest.main()
