"""The game's HD rumble and six-axis sensor on Android, on the host:

- games/totk/runtime/rumble_mix.h: the game's two-band vibration value -> one motor amplitude (gain, clamp, silence
  threshold, stale values), TOTK_RUMBLE_GAIN parsing and the send hysteresis of runtime/feedback_android.c;
- android/java/com/aure/totk/MotionAxes.java on a host JVM: Android sensor samples -> the frame runtime/motion_fusion.c
  takes (acceleration in G pointing down, x right / y up on the screen in every display rotation);
- the JNI and build contract between feedback_android.c, frontend_android.c, companion_jni.c, TotkActivity.java and the
  Makefile, and that the headless stubs no longer stand in for the app.
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
TOTK = ROOT / 'games/totk'
JAVA = TOTK / 'android/java/com/aure/totk'

DRIVER = r'''
#include "rumble_mix.h"
#include <stdio.h>
int main(void) {
    char line[256]; float v[4]; unsigned long long at, now; float gain; char g[64]; int a, s;
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "amp %f %f %f %f %llu %llu %f", &v[0], &v[1], &v[2], &v[3], &at, &now, &gain) == 7)
            printf("%d\n", rumble_amplitude(v, at, now, gain));
        else if (sscanf(line, "gain %63s", g) == 1) printf("%.3f\n", rumble_gain_parse(g[0] == '-' && !g[1] ? NULL : g));
        else if (sscanf(line, "changed %d %d", &a, &s) == 2) printf("%d\n", rumble_changed(a, s));
        else printf("?\n");
    }
    return 0;
}
'''

PROBE = r'''
import com.aure.totk.MotionAxes;
public final class AxesProbe {
    public static void main(String[] a) {
        java.util.Scanner in = new java.util.Scanner(System.in);
        while (in.hasNextInt()) {
            int turns = in.nextInt(); float[] acc = new float[3], gyro = new float[3], out = new float[6];
            for (int i = 0; i < 3; i++) acc[i] = in.nextFloat();
            for (int i = 0; i < 3; i++) gyro[i] = in.nextFloat();
            MotionAxes.toSwitch(acc, gyro, turns, out);
            StringBuilder s = new StringBuilder();
            for (float f : out) s.append(String.format(java.util.Locale.ROOT, "%.4f ", f + 0.0f));
            System.out.println(s.toString().trim());
        }
    }
}
'''

G = 9.80665
MS = 1000000


class RumbleMix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='rumble-mix-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(TOTK / 'runtime'), str(source),
                                 '-o', str(cls.driver), '-lm'], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_driver(self, *lines):
        out = subprocess.run([str(self.driver)], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=30, check=True)
        return out.stdout.split()

    def amp(self, lo, hi, gain=1.0, at=1000 * MS, now=1010 * MS):
        return int(self.run_driver(f'amp {lo} 160 {hi} 320 {at} {now} {gain}')[0])

    def test_two_bands_combine_and_scale(self):
        self.assertEqual(self.amp(0, 0), 0)
        self.assertEqual(self.amp(0.01, 0.01), 0)                 # below 2 %: silence
        self.assertEqual(self.amp(1, 0), 255)
        self.assertEqual(self.amp(0.3, 0.4), round(0.5 * 255))      # sqrt(0.09 + 0.16)
        self.assertEqual(self.amp(0.3, 0.4, gain=0.5), round(0.25 * 255))
        self.assertEqual(self.amp(0.9, 0.9, gain=2), 255)           # clamped
        self.assertEqual(self.amp(1, 1, gain=0), 0)

    def test_stale_or_missing_values_are_silence(self):
        self.assertEqual(self.amp(1, 1, at=0), 0)
        self.assertEqual(self.amp(1, 1, at=1000 * MS, now=1249 * MS), 255)
        self.assertEqual(self.amp(1, 1, at=1000 * MS, now=1250 * MS), 0)
        self.assertEqual(self.amp('nan', 1), 0)

    def test_gain_parsing(self):
        self.assertEqual(self.run_driver('gain -', 'gain 0.25', 'gain 1.0', 'gain 4', 'gain 5', 'gain abc', 'gain 0.5x', 'gain -1'),
                         ['1.000', '0.250', '1.000', '4.000', '1.000', '1.000', '1.000', '1.000'])

    def test_send_hysteresis(self):
        self.assertEqual(self.run_driver('changed 0 0', 'changed 1 0', 'changed 0 1', 'changed 100 107', 'changed 100 108', 'changed 255 0'),
                         ['0', '1', '1', '0', '1', '1'])


class MotionAxesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='motion-axes-')
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        (temp / 'AxesProbe.java').write_text(PROBE)
        subprocess.run([javac, '--release', '17', '-d', str(temp), str(JAVA / 'MotionAxes.java'), str(temp / 'AxesProbe.java')],
                       check=True, capture_output=True, text=True)
        cls.java = java

    def frame(self, turns, acc, gyro):
        line = f'{turns} ' + ' '.join(str(v) for v in (*acc, *gyro)) + '\n'
        out = subprocess.run([self.java, '-cp', self.temp.name, 'AxesProbe'], input=line, capture_output=True, text=True,
                             timeout=60, check=True)
        return [float(v) for v in out.stdout.split()]

    def assertFrame(self, got, want):
        for g, w in zip(got, want):
            self.assertAlmostEqual(g, w, places=3, msg=(got, want))

    def test_lying_face_up_reads_minus_one_g_on_z(self):
        self.assertFrame(self.frame(0, (0, 0, G), (0, 0, 0)), (0, 0, -1, 0, 0, 0))

    def test_held_upright_in_landscape_gravity_points_down_the_screen(self):
        # Natural landscape (handhelds): the top edge is up, the accelerometer pushes +y.
        self.assertFrame(self.frame(0, (0, G, 0), (0, 0, 0))[:3], (0, -1, 0))
        # A portrait phone turned a quarter counterclockwise (ROTATION_90): its right edge (+x) is now the screen's top.
        self.assertFrame(self.frame(1, (G, 0, 0), (0, 0, 0))[:3], (0, -1, 0))
        # ROTATION_270: its left edge (-x) is the top.
        self.assertFrame(self.frame(3, (-G, 0, 0), (0, 0, 0))[:3], (0, -1, 0))
        self.assertFrame(self.frame(2, (0, -G, 0), (0, 0, 0))[:3], (0, -1, 0))

    def test_rotation_turns_with_the_screen_and_keeps_its_sense(self):
        # Turning about the device's own y (its top edge) is turning about the screen's -x at ROTATION_90.
        self.assertFrame(self.frame(1, (0, 0, G), (0, 2, 0)), (0, 0, -1, -2, 0, 0))
        self.assertFrame(self.frame(0, (0, 0, G), (1, 2, 3)), (0, 0, -1, 1, 2, 3))   # no turn: the same axes
        self.assertFrame(self.frame(3, (0, 0, G), (1, 2, 3))[3:], (2, -1, 3))


class FeedbackContract(unittest.TestCase):
    def test_jni_signatures_agree(self):
        activity = (JAVA / 'TotkActivity.java').read_text()
        feedback = (TOTK / 'runtime/feedback_android.c').read_text()
        jni = (TOTK / 'runtime/companion_jni.c').read_text()
        self.assertIn('static native void nativeMotion(float ax, float ay, float az, float gx, float gy, float gz);', activity)
        self.assertIn('{"nativeMotion", "(FFFFFF)V", (void *)runtime_android_motion}', jni)
        self.assertIn('static native void nativeMotionLive(boolean on, String source);', activity)
        self.assertIn('{"nativeMotionLive", "(ZLjava/lang/String;)V", (void *)runtime_android_motion_live}', jni)
        self.assertIn('private static native int nativeFeedbackEnabled();', activity)
        self.assertIn('{"nativeFeedbackEnabled", "()I", (void *)runtime_android_feedback_enabled}', jni)
        self.assertIn('public void rumble(int left, int right)', activity)
        self.assertIn('"rumble", "(II)V"', feedback)
        self.assertIn('public int feedbackHardware()', activity)
        self.assertIn('"feedbackHardware", "()I"', feedback)

    def test_the_app_build_drives_rumble_instead_of_the_headless_stubs(self):
        makefile = (TOTK / 'android/Makefile').read_text()
        self.assertIn('runtime/frontend_android.c runtime/feedback_android.c', makefile)
        headless = (TOTK / 'runtime/frontend_headless.c').read_text()
        guarded = headless[headless.index('#ifndef TOTK_ANDROID_APP\n/* Input'):]
        self.assertLess(guarded.index('void runtime_rumble_set'), guarded.index('#endif'))
        self.assertLess(guarded.index('void runtime_feedback_quiet'), guarded.index('#endif'))
        frontend = (TOTK / 'runtime/frontend_android.c').read_text()
        start = frontend[frontend.index('static int start_game('):]
        self.assertLess(start.index('runtime_android_feedback_hardware('), start.index('pthread_create(&t, &at, game_thread'))
        self.assertTrue(re.search(r'runtime_gamepad_clear\(\); runtime_feedback_quiet\(\);', frontend))

    def test_vibrate_permission(self):
        self.assertIn("'android.permission.VIBRATE'", (TOTK / 'android/apk/build_apk.py').read_text())


if __name__ == '__main__':
    unittest.main()
