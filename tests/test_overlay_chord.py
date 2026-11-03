"""Minus + R live overlay shortcut (games/totk/runtime/overlay_chord.h), the state machine frontend_android.c runs
for every gamepad key event and pad read. A small C driver replays timed events through the production header.

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

# Each input line: "k <ms> <m|r|o> <press|release|repeat>" (Minus / R / any other button) or "p <ms>" (a pad read).
# Output per line, in the order the frontend applies it: U = release the replayed Minus, D = press the held Minus,
# P = deliver this event, C = cycle overlay; - = nothing.
DRIVER = r'''
#include <stdio.h>
#include <string.h>
#include "overlay_chord.h"
int main(void) {
    overlay_chord c = {0}; char kind, button[16], action[16]; unsigned long long ms;
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        unsigned r;
        if (sscanf(line, " k %llu %15s %15s", &ms, button, action) == 3) {
            unsigned b = !strcmp(button, "m") ? OVERLAY_CHORD_MINUS : !strcmp(button, "r") ? OVERLAY_CHORD_R : OVERLAY_CHORD_OTHER;
            unsigned a = !strcmp(action, "press") ? OVERLAY_CHORD_PRESS : !strcmp(action, "repeat") ? OVERLAY_CHORD_REPEAT :
                         !strcmp(action, "cancel") ? OVERLAY_CHORD_CANCEL : OVERLAY_CHORD_RELEASE;
            r = overlay_chord_key(&c, b, a, ms * 1000000ull);
        } else if (sscanf(line, " %c %llu", &kind, &ms) == 2 && kind == 'p') r = overlay_chord_poll(&c, ms * 1000000ull);
        else return 2;
        printf("%s%s%s%s%s\n", r & OVERLAY_CHORD_MINUS_UP ? "U" : "", r & OVERLAY_CHORD_MINUS_DOWN ? "D" : "",
               r & OVERLAY_CHORD_PASS ? "P" : "", r & OVERLAY_CHORD_CYCLE ? "C" : "", r ? "" : "-");
    }
    return 0;
}
'''


class OverlayChord(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='overlay-chord-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                                 str(ROOT / 'games/totk/runtime'), str(source), '-o', str(cls.driver)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_events(self, *events):
        result = subprocess.run([str(self.driver)], input='\n'.join(events) + '\n', capture_output=True, text=True,
                                timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.split()

    def test_chord_swallows_both_buttons_and_cycles_once(self):
        self.assertEqual(self.run_events('k 1000 m press', 'p 1050', 'k 1100 r press', 'p 1200', 'p 2000',
                                         'k 1300 r release', 'k 1400 m release'),
                         ['-', '-', 'C', '-', '-', '-', '-'])

    def test_r_at_the_window_edge(self):
        # 149 ms after Minus is still the chord; at 150 ms the window has closed and both reach the game.
        self.assertEqual(self.run_events('k 0 m press', 'k 149 r press'), ['-', 'C'])
        self.assertEqual(self.run_events('k 0 m press', 'k 150 r press'), ['-', 'DP'])

    def test_lone_minus_is_delivered_late_by_the_pad_read_and_released_normally(self):
        self.assertEqual(self.run_events('k 0 m press', 'p 100', 'p 149', 'p 150', 'p 300', 'k 900 m release'),
                         ['-', '-', '-', 'D', '-', 'P'])

    def test_minus_held_past_the_window_then_r_is_not_a_chord(self):
        self.assertEqual(self.run_events('k 0 m press', 'p 160', 'k 200 r press', 'k 250 r release', 'k 300 m release'),
                         ['-', 'D', 'P', 'P', 'P'])

    def test_quick_tap_is_replayed_with_its_own_length(self):
        # Released after 60 ms, inside the window: pressed for the game at once and released 60 ms later, so the
        # game's once-per-frame pad read sees it held (not a press that is already over by the next read).
        self.assertEqual(self.run_events('k 0 m press', 'k 60 m release', 'p 100', 'p 120', 'p 200', 'k 220 r press'),
                         ['-', 'D', '-', 'U', '-', 'P'])
        # A tap is over: R during its replay passes; a new Minus ends the replay before it is held back again.
        self.assertEqual(self.run_events('k 0 m press', 'k 50 m release', 'k 70 r press', 'p 100', 'k 110 r release'),
                         ['-', 'D', 'P', 'U', 'P'])
        self.assertEqual(self.run_events('k 0 m press', 'k 40 m release', 'k 60 m press', 'p 300', 'k 310 m release'),
                         ['-', 'D', 'U', 'D', 'P'])

    def test_r_without_minus_and_r_before_minus_pass_through(self):
        self.assertEqual(self.run_events('k 0 r press', 'k 50 r release'), ['P', 'P'])
        # R first: the game already has R, so a following Minus is only held back and then delivered.
        self.assertEqual(self.run_events('k 0 r press', 'k 20 m press', 'p 200', 'k 300 r release', 'k 310 m release'),
                         ['P', '-', 'D', 'P', 'P'])

    def test_repeated_chords_and_r_taps_while_minus_stays_down(self):
        self.assertEqual(self.run_events('k 0 m press', 'k 40 r press', 'k 90 r release', 'k 120 m release',
                                         'k 1000 m press', 'k 1030 r press', 'k 1080 r release', 'k 1500 r press',
                                         'k 1550 r release', 'p 2000', 'k 2100 m release'),
                         ['-', 'C', '-', '-', '-', 'C', '-', 'C', '-', '-', '-'])

    def test_key_repeats_never_start_or_leak_a_chord(self):
        # Android repeats a held key; a repeat of an already-delivered R is not a new chord press.
        self.assertEqual(self.run_events('k 0 r press', 'k 100 m press', 'k 120 r repeat', 'p 300',
                                         'k 400 r release', 'k 450 m release'),
                         ['P', '-', 'P', 'D', 'P', 'P'])
        # Repeats of buttons the chord swallowed stay swallowed.
        self.assertEqual(self.run_events('k 0 m press', 'k 10 r press', 'k 500 m repeat', 'k 520 r repeat',
                                         'k 600 r release', 'k 610 m release'),
                         ['-', 'C', '-', '-', '-', '-'])

    def test_another_button_first_flushes_minus_in_order(self):
        self.assertEqual(self.run_events('k 0 m press', 'k 30 o press', 'k 60 r press', 'k 70 o release',
                                         'k 80 r release', 'k 90 m release'),
                         ['-', 'DP', 'P', 'P', 'P', 'P'])

    def test_an_expired_minus_is_delivered_before_the_next_event(self):
        # No pad read in between (game paused reading): the late Minus still precedes the event that follows.
        self.assertEqual(self.run_events('k 0 m press', 'k 400 o press'), ['-', 'DP'])

    def test_focus_loss_or_disconnect_cancel_never_invents_or_sticks_minus(self):
        # Android's canceled release while Minus is held back: nothing reached the game, nothing is delivered.
        self.assertEqual(self.run_events('k 0 m press', 'k 50 m cancel', 'p 200', 'p 400'), ['-', '-', '-', '-'])
        # Already delivered (held past the window): the cancel releases it.
        self.assertEqual(self.run_events('k 0 m press', 'p 200', 'k 300 m cancel', 'p 400'), ['-', 'D', 'P', '-'])
        # During a tap replay: ends it now.
        self.assertEqual(self.run_events('k 0 m press', 'k 50 m release', 'k 60 m cancel', 'p 200'), ['-', 'D', 'U', '-'])
        # Mid-chord: both swallowed buttons are forgotten, and the next Minus starts a fresh shortcut.
        self.assertEqual(self.run_events('k 0 m press', 'k 20 r press', 'k 100 m cancel', 'k 100 r cancel',
                                         'k 500 m press', 'k 520 r press'), ['-', 'C', '-', '-', '-', 'C'])


if __name__ == '__main__':
    unittest.main()
