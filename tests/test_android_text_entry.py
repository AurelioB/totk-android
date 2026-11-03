"""The game's software keyboard (nn::swkbd::ShowKeyboard) on Android, on the host:

- the applet glue (games/totk/runtime/swkbd_host.c) compiled with a C driver standing in for the frontend: what the
  dialog is asked (prompt, initial text, length limits, number pad, disabled keys) and what the game gets back through
  the applet's String / result (UTF-16 or UTF-8, the length limit, disabled keys, cancel 0x29f, minimum length);
- the dialog's typing rules (android/java/com/aure/totk/TextRules.java) on a host JVM, against the same cases, so the
  field never offers what the runtime would strip;
- the JNI contract between runtime/frontend_android.c, runtime/companion_jni.c and TotkActivity.java.

The horse-naming request (user-name preset: 9 characters, '@' '%' '\\' disabled) is the case TOTK uses.
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

# One case per line, tab separated: type max min disabled utf8 initial answer ("<cancel>" cancels).
# Output per case, tab separated: result, prompt, initial, max, min, digits, disabled, UTF-16 units of the game's String.
DRIVER = r'''
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *runtime_swkbd_override(const char *name);
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "fail: %s\n", s); exit(3); }
void *runtime_symbol(const char *name) { (void)name; return NULL; }
uint8_t *runtime_module_base(const char *name) { (void)name; return NULL; }
void runtime_thread_init(void) { runtime_fail("probe only"); }
void *runtime_hle_wrap(void *fn) { (void)fn; runtime_fail("probe only"); }
static int away_calls, back_calls;
int runtime_core_away(void) { away_calls++; return 1; }
void runtime_core_back(int *away) { if (*away) back_calls++; }
static const char *answer;
static RuntimeTextRequest seen; static char seen_prompt[512], seen_initial[512];
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    seen = *req; snprintf(seen_prompt, sizeof seen_prompt, "%s", req->prompt); snprintf(seen_initial, sizeof seen_initial, "%s", req->initial);
    if (!strcmp(answer, "<cancel>")) { out[0] = 0; return 0; }
    snprintf(out, out_size, "%s", answer); return 1;
}
static size_t to16(const char *s, uint16_t *o) {   /* the driver's own UTF-8 -> UTF-16 for the initial text */
    const unsigned char *p = (const unsigned char *)s; size_t n = 0;
    while (*p) {
        uint32_t c = *p++; int k = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0; if (k) c &= 0x3f >> k;
        while (k--) c = c << 6 | (*p++ & 0x3f);
        if (c >= 0x10000) { o[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10)); o[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff)); }
        else o[n++] = (uint16_t)c;
    }
    return n;
}
static char *field(char **line) { char *s = *line, *t = strchr(s, '\t'); if (t) { *t = 0; *line = t + 1; } else *line = s + strlen(s); return s; }
int main(void) {
    unsigned (*show)(void *, const void *) = (unsigned (*)(void *, const void *))
        runtime_swkbd_override("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE");
    if (!show) return 2;
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0; char *p = line;
        uint32_t type = (uint32_t)strtoul(field(&p), NULL, 0), max = (uint32_t)strtoul(field(&p), NULL, 0);
        uint32_t min = (uint32_t)strtoul(field(&p), NULL, 0), disabled = (uint32_t)strtoul(field(&p), NULL, 0);
        int utf8 = atoi(field(&p)); const char *initial = field(&p); answer = field(&p);
        uint8_t *arg = calloc(1, 0x500), *work = calloc(1, 0x1000); uint16_t *out = calloc(1, 0x7d4);
        static const uint16_t header[] = {'N','a','m','e',' ','y','o','u','r',' ','h','o','r','s','e',0};
        uint16_t init16[256]; size_t ni = to16(initial, init16); memcpy(work + 0x10, init16, ni * 2);
        uint32_t off = 0x10, len = (uint32_t)ni; uint64_t w = (uint64_t)(uintptr_t)work, ws = 0x1000;
        memcpy(arg + 0x00, &type, 4); memcpy(arg + 0x1c, &disabled, 4); memcpy(arg + 0x24, header, sizeof header);
        memcpy(arg + 0x3ac, &max, 4); memcpy(arg + 0x3b0, &min, 4); arg[0x3bd] = (uint8_t)utf8;
        memcpy(arg + 0x3c0, &off, 4); memcpy(arg + 0x3c4, &len, 4); memcpy(arg + 0x4d0, &w, 8); memcpy(arg + 0x4d8, &ws, 8);
        uint64_t str[2] = {(uint64_t)(uintptr_t)out, 0x7d4};
        memset(&seen, 0, sizeof seen); seen_prompt[0] = seen_initial[0] = 0;
        unsigned r = show(str, arg);
        printf("%#x\t%s\t%s\t%d\t%d\t%d\t%#x\t", r, seen_prompt, seen_initial, seen.max_units, seen.min_units, seen.digits_only, seen.disabled);
        if (utf8) { for (const unsigned char *b = (const unsigned char *)out; *b; b++) printf("%02x", *b); }
        else for (size_t i = 0; out[i]; i++) printf("%s%04x", i ? " " : "", out[i]);
        printf("\n");
        free(arg); free(work); free(out);
    }
    if (away_calls != back_calls) return 4;
    return 0;
}
'''

# Same cases through TextRules: the cleaned answer's UTF-16 units, and whether OK accepts it.
PROBE = r'''
import com.aure.totk.TextRules;
import java.io.*;
import java.nio.charset.StandardCharsets;
public final class RulesProbe {
    public static void main(String[] a) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        PrintStream out = new PrintStream(System.out, true, "UTF-8");
        for (String line; (line = in.readLine()) != null; ) {
            String[] f = line.split("\t", -1);
            TextRules rules = new TextRules(Integer.decode(f[1]), Integer.decode(f[2]), Integer.decode(f[0]) == 1, Integer.decode(f[3]));
            String kept = rules.clean(f[6]);
            StringBuilder units = new StringBuilder();
            for (int i = 0; i < kept.length(); i++) units.append(i > 0 ? " " : "").append(String.format("%04x", (int) kept.charAt(i)));
            out.println(units + "\t" + rules.acceptable(kept) + "\t" + rules.clean(f[5]));
        }
    }
}
'''

HORSE = 0x12c   # '@' '%' '\\' (bits 2, 3, 5) and bit 8, as TOTK's user-name preset sets them
CASES = [
    # type max min disabled utf8 initial answer
    (0, 9, 0, HORSE, 0, '', 'Epona'),
    (0, 9, 0, HORSE, 0, 'Lin@k', 'Ep@ona%\\Storm'),        # disabled keys dropped, then cut to 9 units
    (0, 9, 0, HORSE, 0, 'Old name', '<cancel>'),
    (0, 4, 0, 0, 0, '', 'ab\U0001F40Ec'),                    # a pair that fits whole
    (0, 3, 0, 0, 0, '', 'ab\U0001F40E'),                     # a pair that would be cut in half: dropped
    (1, 6, 0, 0, 0, '12', '12a34 5678'),                     # number pad: digits only
    (0, 8, 3, 0, 0, '', 'ab'),                               # shorter than the minimum: a cancel
    (0, 9, 0, 0, 1, '', 'Agro\u00e9'),                       # UTF-8 output
    (0, 9, 0, 1 << 1 | 1 << 6, 0, '', 'a b 1 c'),            # space and digits disabled
]


def compiler():
    cc = shlex.split(os.environ.get('CC', 'cc'))
    if not cc or not shutil.which(cc[0]):
        raise unittest.SkipTest('A native C compiler is required')
    return cc


def units(text):
    data = text.encode('utf-16-le')
    return ' '.join(f'{int.from_bytes(data[i:i + 2], "little"):04x}' for i in range(0, len(data), 2))


class TextEntryGlue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = compiler()
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='text-entry-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        (temp / 'driver.c').write_text(DRIVER)
        cls.driver = temp / 'driver'
        result = subprocess.run([*cc, '-std=gnu11', '-Wall', '-Wno-format-truncation', '-DTOTK_GAME_V143=1', '-I', str(TOTK / 'runtime'),
                                 '-I', str(TOTK / 'third_party/d2r_gfx/gfx/nvn/include'), str(temp / 'driver.c'),
                                 str(TOTK / 'runtime/swkbd_host.c'), '-o', str(cls.driver)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        env = {k: v for k, v in os.environ.items() if k not in ('TOTK_SWKBD_ANSWER', 'TOTK_PROBE_NEGATIVE')}
        env['TOTK_UI_LANG'] = 'en'
        lines = '\n'.join('\t'.join(str(v) for v in case) for case in CASES) + '\n'
        run = subprocess.run([str(cls.driver)], input=lines.encode(), env=env, capture_output=True, timeout=60)
        if run.returncode:
            raise AssertionError(run.stdout.decode() + run.stderr.decode())
        cls.rows = [line.split('\t') for line in run.stdout.decode().splitlines()]
        cls.java_rows = None
        javac, java = shutil.which('javac'), shutil.which('java')
        if javac and java:
            (temp / 'RulesProbe.java').write_text(PROBE)
            classes = temp / 'classes'
            subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(classes), str(JAVA / 'TextRules.java'),
                            str(temp / 'RulesProbe.java')], check=True, capture_output=True, text=True)
            out = subprocess.run([java, '-cp', str(classes), 'RulesProbe'], input=lines.encode(), capture_output=True,
                                 timeout=60, check=True)
            cls.java_rows = [line.split('\t') for line in out.stdout.decode().splitlines()]

    def row(self, i):
        self.assertEqual(len(self.rows), len(CASES))
        return self.rows[i]

    def test_the_horse_name_request_reaches_the_dialog(self):
        result, prompt, initial, max_units, min_units, digits, disabled, out = self.row(0)
        self.assertEqual((prompt, initial, max_units, min_units, digits, disabled), ('Name your horse', '', '9', '0', '0', hex(HORSE)))
        self.assertEqual((result, out), ('0', units('Epona')))

    def test_initial_text_and_the_game_rules_apply_to_the_answer(self):
        result, _, initial, *_, out = self.row(1)
        self.assertEqual(initial, 'Lin@k')   # the dialog shows it; TextRules filters it as it starts
        self.assertEqual((result, out), ('0', units('EponaStor')))

    def test_cancel_returns_the_applet_cancel_result(self):
        result, *_, out = self.row(2)
        self.assertEqual((result, out), ('0x29f', ''))

    def test_surrogate_pairs_are_never_split(self):
        self.assertEqual(self.row(3)[-1], units('ab\U0001F40E'))
        self.assertEqual(self.row(4)[-1], units('ab'))

    def test_number_pad_and_minimum_length(self):
        number = self.row(5)
        self.assertEqual((number[5], number[-1]), ('1', units('123456')))
        self.assertEqual((self.row(6)[0], self.row(6)[-1]), ('0x29f', ''))

    def test_utf8_output_and_disabled_space_and_digits(self):
        self.assertEqual(bytes.fromhex(self.row(7)[-1]).decode(), 'Agro\u00e9')
        self.assertEqual(self.row(8)[-1], units('abc'))

    def test_dialog_rules_match_the_runtime(self):
        if self.java_rows is None:
            self.skipTest('javac/java not found')
        self.assertEqual(len(self.java_rows), len(CASES))
        for case, row, java in zip(CASES, self.rows, self.java_rows):
            kept, acceptable, _ = java
            if row[0] == '0x29f' and case[-1] == '<cancel>':
                continue
            if case[4]:   # UTF-8 output: compare as text
                self.assertEqual(units(bytes.fromhex(row[-1]).decode()), kept, case)
            elif row[0] == '0':
                self.assertEqual(row[-1], kept, case)
                self.assertEqual(acceptable, 'true', case)
            else:   # the runtime refused it (too short): OK stays disabled in the dialog
                self.assertEqual(acceptable, 'false', case)
        self.assertEqual(self.java_rows[1][2], 'Link')


class TextEntryJniContract(unittest.TestCase):
    def test_native_and_java_signatures_agree(self):
        activity = (JAVA / 'TotkActivity.java').read_text()
        frontend = (TOTK / 'runtime/frontend_android.c').read_text()
        jni = (TOTK / 'runtime/companion_jni.c').read_text()
        self.assertRegex(activity, r'public void showTextInput\(long serial, String prompt, String initial, int maxUnits, int minUnits, '
                                   r'boolean secure,\s+boolean digitsOnly, int disabled\)')
        self.assertIn('"showTextInput", "(JLjava/lang/String;Ljava/lang/String;IIZZI)V"', frontend)
        self.assertIn('private static native void nativeTextInputDone(long serial, byte[] utf8);', activity)
        self.assertIn('{"nativeTextInputDone", "(J[B)V", (void *)runtime_android_text_done}', jni)
        self.assertIn('void runtime_android_text_done(JNIEnv *env, jclass cls, jlong serial, jbyteArray utf8)', frontend)

    def test_android_asks_instead_of_cancelling(self):
        swkbd = (TOTK / 'runtime/swkbd_host.c').read_text()
        self.assertNotIn('not available on this platform', swkbd)
        headless = (TOTK / 'runtime/frontend_headless.c').read_text()
        guarded = headless[headless.index('#ifndef TOTK_ANDROID_APP'):]
        self.assertLess(guarded.index('int runtime_front_text_input'), guarded.index('#endif'))
        frontend = (TOTK / 'runtime/frontend_android.c').read_text()
        self.assertIn('int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {', frontend)
        self.assertTrue(re.search(r'immersive\(a\);\s+text_wake_start\(\);', frontend))

    def test_gamepad_confirms_and_cancels_in_the_game_layout(self):
        entry = (JAVA / 'TextEntry.java').read_text()
        frontend = (TOTK / 'runtime/frontend_android.c').read_text()
        # the same TOTK_FACE_BUTTONS rule as the game: by label unless "position" (A and B swap)
        self.assertIn('by_label = !(e && !strcmp(e, "position"));', frontend)
        self.assertIn('boolean byPosition = "position".equals(Os.getenv("TOTK_FACE_BUTTONS"));', entry)
        self.assertIn('int confirmKey = byPosition ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A;', entry)
        self.assertIn('int cancelKey = byPosition ? KeyEvent.KEYCODE_BUTTON_A : KeyEvent.KEYCODE_BUTTON_B;', entry)
        listener = entry[entry.index('dialog.setOnKeyListener'):entry.index('dialog.setOnDismissListener')]
        self.assertIn('if (code == cancelKey) dialog.cancel();', listener)
        self.assertIn('if (ok.isEnabled()) ok.performClick();', listener)


if __name__ == '__main__':
    unittest.main()
