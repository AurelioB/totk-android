"""Run production BootProgress and its native snapshot on a host JVM.

Only Android widgets/queues are doubles: no ART, compositor, focus or scanout claim.
The unrelated companion gate deliberately keeps its narrow BootProgress stub.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java'


def _jni_headers(javac):
    """Ask the compiler's JVM, since javac may be a launcher/version-manager shim."""
    try:
        result = subprocess.run([javac, '-J-XshowSettings:properties', '-version'],
                                capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise unittest.SkipTest(f'Cannot query selected javac JVM for JNI headers: {error}') from error
    home = next((line.partition('=')[2].strip()
                 for line in (result.stderr + result.stdout).splitlines()
                 if line.strip().startswith('java.home =')), None)
    if result.returncode or not home:
        raise unittest.SkipTest('Selected javac JVM did not report java.home for JNI headers')
    include = Path(home) / 'include'
    if not all((include / name).is_file() for name in ('jni.h', 'linux/jni_md.h')):
        raise unittest.SkipTest(f'Selected javac JDK lacks JNI headers under {include}')
    return include


class JniHeaderDiscovery(unittest.TestCase):
    def test_version_manager_shim_uses_compilers_java_home(self):
        javac = shutil.which('javac')
        if not javac:
            self.skipTest('requires javac')
        expected = _jni_headers(javac)
        with tempfile.TemporaryDirectory(prefix='jdk-shim-') as directory:
            # Like mise, this executable lives outside the selected JDK.
            shim = Path(directory) / 'javac'
            shim.write_text('#!/bin/sh\nexec "$REAL_JAVAC" "$@"\n')
            shim.chmod(0o755)
            from unittest.mock import patch
            with patch.dict('os.environ', {'REAL_JAVAC': javac}):
                self.assertEqual(_jni_headers(str(shim)), expected)
            self.assertNotEqual(shim.resolve().parent.parent / 'include', expected)

    def test_missing_headers_skip_with_selected_home(self):
        with tempfile.TemporaryDirectory(prefix='jdk-no-headers-') as directory:
            javac = Path(directory) / 'javac'
            javac.write_text('#!/bin/sh\nprintf "    java.home = %s\\n" "$0" >&2\n')
            javac.chmod(0o755)
            with self.assertRaisesRegex(unittest.SkipTest, 'lacks JNI headers under'):
                _jni_headers(str(javac))

    def test_unreported_java_home_skips_clearly(self):
        with tempfile.TemporaryDirectory(prefix='jdk-no-home-') as directory:
            javac = Path(directory) / 'javac'
            javac.write_text('#!/bin/sh\necho "no java.home available" >&2\n')
            javac.chmod(0o755)
            with self.assertRaisesRegex(unittest.SkipTest, 'did not report java.home'):
                _jni_headers(str(javac))

STUBS = {
    'android/R.java': 'package android; public class R { public static class attr { public static int progressBarStyleHorizontal; }}',
    'android/os/Looper.java': 'package android.os; public class Looper { public static Looper getMainLooper() { return new Looper(); }}',
    'android/os/Handler.java': '''package android.os;
public class Handler {
    public static final java.util.List<Runnable> queue = new java.util.ArrayList<>();
    public Handler(Looper looper) {}
    public boolean post(Runnable r) { queue.add(r); return true; }
    public boolean postDelayed(Runnable r, long ms) {
        if (ms != 250) throw new AssertionError("poll period"); return post(r);
    }
    public void removeCallbacks(Runnable r) { queue.removeIf(item -> item == r); }
    public static void step() { queue.remove(0).run(); }
}''',
    'android/app/Activity.java': '''package android.app;
public class Activity {
    public boolean finishing, destroyed;
    public android.view.Window window = new android.view.Window();
    public boolean isFinishing() { return finishing; }
    public boolean isDestroyed() { return destroyed; }
    public android.view.Window getWindow() { return window; }
    public android.content.res.Resources getResources() { return new android.content.res.Resources(); }
}''',
    'android/content/res/Resources.java': '''package android.content.res;
public class Resources { public Metrics getDisplayMetrics() { return new Metrics(); }
public static class Metrics { public float density = 1; }}''',
    'android/content/res/ColorStateList.java': 'package android.content.res; public class ColorStateList { public static ColorStateList valueOf(int c) { return new ColorStateList(); }}',
    'android/graphics/Color.java': 'package android.graphics; public class Color { public static final int TRANSPARENT = 0; }',
    'android/graphics/drawable/ColorDrawable.java': 'package android.graphics.drawable; public class ColorDrawable { public ColorDrawable(int c) {} }',
    'android/graphics/drawable/GradientDrawable.java': '''package android.graphics.drawable;
public class GradientDrawable { public void setColor(int c) {} public void setCornerRadius(float r) {} public void setStroke(int w, int c) {} }''',
    'android/view/Gravity.java': 'package android.view; public class Gravity { public static final int CENTER = 1, CENTER_VERTICAL = 2; }',
    'android/view/Window.java': 'package android.view; public class Window { public View decor = new View(); public View getDecorView() { return decor; }}',
    'android/view/View.java': '''package android.view;
public class View {
    public static final int GONE = 8, VISIBLE = 0, ACCESSIBILITY_LIVE_REGION_POLITE = 1, IMPORTANT_FOR_ACCESSIBILITY_NO = 2;
    public boolean attached = true; public int width = 600, visibility;
    public boolean isAttachedToWindow() { return attached; } public int getWidth() { return width; }
    public void setVisibility(int v) { visibility = v; }
    public void setBackground(Object b) {} public void setPadding(int a, int b, int c, int d) {}
}''',
    'android/view/ViewGroup.java': '''package android.view;
public class ViewGroup extends View { public static class LayoutParams {
public static final int MATCH_PARENT = -1, WRAP_CONTENT = -2;
public LayoutParams(int w, int h) {} }}''',
    'android/widget/LinearLayout.java': '''package android.widget;
public class LinearLayout extends android.view.ViewGroup {
    public static final int VERTICAL = 1;
    public LinearLayout(android.app.Activity a) {} public void setOrientation(int v) {} public void setGravity(int v) {}
    public void addView(android.view.View v, int w, int h) {} public void addView(android.view.View v, LayoutParams p) {}
    public static class LayoutParams extends android.view.ViewGroup.LayoutParams {
        public int topMargin; public LayoutParams(int w, int h) { super(w,h); }
        public LayoutParams(int w, int h, int weight) { super(w,h); } public void setMarginStart(int v) {}
    }
}''',
    'android/widget/TextView.java': '''package android.widget;
public class TextView extends android.view.View {
    public static final java.util.List<TextView> made = new java.util.ArrayList<>();
    private CharSequence text = ""; public TextView(android.app.Activity a) { made.add(this); }
    public CharSequence getText() { return text; } public void setText(CharSequence v) { text = v; }
    public void setTypeface(Object v) {} public void setTextSize(int v) {} public void setTextColor(int v) {}
    public void setAccessibilityLiveRegion(int v) {} public void setImportantForAccessibility(int v) {}
}''',
    'android/widget/ProgressBar.java': '''package android.widget;
public class ProgressBar extends android.view.View {
    public static ProgressBar last; public boolean indeterminate; public int progress;
    public ProgressBar(android.app.Activity a, Object b, int style) { last = this; }
    public void setIndeterminate(boolean v) { indeterminate = v; } public void setProgress(int v) { progress = v; }
    public void setMax(int v) { if (v != 100) throw new AssertionError(); }
    public void setProgressTintList(Object v) {} public void setIndeterminateTintList(Object v) {}
    public void setProgressBackgroundTintList(Object v) {}
}''',
    'android/widget/PopupWindow.java': '''package android.widget;
public class PopupWindow {
    public static final int INPUT_METHOD_NOT_NEEDED = 2;
    public static PopupWindow last; public boolean showing, touchable = true; public final boolean focusable;
    public int width, shows; public android.view.View anchor;
    public PopupWindow(android.view.View v, int w, int h, boolean f) { last = this; focusable = f; }
    public void dismiss() { showing = false; } public boolean isShowing() { return showing; }
    public void setWidth(int w) { width = w; }
    public void showAtLocation(android.view.View a, int g, int x, int y) { showing = true; anchor = a; shows++; }
    public void setBackgroundDrawable(Object v) {} public void setTouchable(boolean v) { touchable = v; }
    public void setInputMethodMode(int v) { if (v != INPUT_METHOD_NOT_NEEDED) throw new AssertionError(); }
    public void setAnimationStyle(int v) {}
}''',
    'com/aure/totk/setup/Theme.java': '''package com.aure.totk.setup;
public class Theme { public static final int DEEP=1, RULE=2, TEXT=3, ZONAI=4, TRACK=5;
public static Object semibold; public static void init(android.app.Activity a) {} }''',
}

PROBE = r'''
import android.app.Activity;
import android.os.Handler;
import android.widget.*;
import com.aure.totk.setup.BootProgress;
public class BootProbe {
    static native String read(long[] counts);
    static native void update(String stage, long done, long total);
    static native void handoff();
    static void require(boolean ok) { if (!ok) throw new AssertionError(); }
    public static void main(String[] args) {
        System.loadLibrary("boot"); java.util.Locale.setDefault(java.util.Locale.US);
        Activity owner = new Activity(); BootProgress ui = new BootProgress(owner, BootProbe::read);
        update("", 0, 0); ui.resume(); Handler.step();
        PopupWindow popup = PopupWindow.last;
        require(popup.showing && !popup.touchable && !popup.focusable);
        require(TextView.made.get(0).getText().toString().equals("Starting game"));
        require(ProgressBar.last.indeterminate && TextView.made.get(1).visibility == 8);
        if (args[0].equals("repeat")) {
            ui.resume(); ui.resume(); require(Handler.queue.size() == 1);
            Handler.step(); require(Handler.queue.size() == 1); return;
        }
        if (args[0].equals("finishing") || args[0].equals("destroyed")) {
            owner.finishing = args[0].equals("finishing"); owner.destroyed = args[0].equals("destroyed");
            Handler.step(); require(!popup.showing && Handler.queue.isEmpty()); return;
        }
        update("Verify RomFS", 1, 4); Handler.step();
        require(!ProgressBar.last.indeterminate && ProgressBar.last.progress == 25);
        require(TextView.made.get(0).getText().toString().equals("Verify RomFS"));
        require(TextView.made.get(1).getText().toString().equals("25%"));
        update("Load shaders", 9, 4); Handler.step(); require(ProgressBar.last.progress == 100);
        update("Create renderer", 0, 0); Handler.step(); require(ProgressBar.last.indeterminate);
        ui.pause(); require(!popup.showing && Handler.queue.isEmpty());
        // Background before first frame, then a new display/decor on resume.
        owner.window = new android.view.Window(); owner.window.decor.width = 300;
        owner.window.decor.attached = false;
        ui.resume(); Handler.step(); require(!popup.showing);
        owner.window.decor.attached = true; Handler.step();
        require(popup.showing && popup.anchor == owner.window.decor && popup.width == 268);
        handoff(); Handler.step(); require(!popup.showing && Handler.queue.isEmpty());
        ui.resume(); require(Handler.queue.isEmpty()); // no resurrection after real handoff
        update("late work", 1, 2); require(read(new long[2]) == null);
        BootProgress recreated = new BootProgress(new Activity(), BootProbe::read);
        recreated.resume(); Handler.step(); require(Handler.queue.isEmpty() && !popup.showing);
    }
}
'''


class AndroidBootProgress(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java, cc = shutil.which('javac'), shutil.which('java'), shutil.which('cc')
        if not javac or not cls.java or not cc:
            raise unittest.SkipTest('requires a host JDK and C compiler')
        include = _jni_headers(javac)
        temp = tempfile.TemporaryDirectory(prefix='boot-progress-')
        cls.addClassCleanup(temp.cleanup)
        cls.directory = Path(temp.name)
        paths = []
        for name, source in {**STUBS, 'BootProbe.java': PROBE}.items():
            path = cls.directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
            paths.append(str(path))
        cls.command([javac, '--release', '17', '-d', str(cls.directory), *paths,
                     str(JAVA / 'com/aure/totk/setup/BootProgress.java')])
        frontend = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        start = frontend.index('static pthread_mutex_t boot_progress_mu')
        end = frontend.index('/* One Looper owns', start)
        source = '#include <jni.h>\n#include <pthread.h>\n#include <stdatomic.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\nstatic _Atomic unsigned presented;\n'
        source += frontend[start:end]
        source += r'''
JNIEXPORT jstring JNICALL Java_BootProbe_read(JNIEnv *e, jclass c, jlongArray a) {
    return runtime_android_boot_progress(e, c, a);
}
JNIEXPORT void JNICALL Java_BootProbe_update(JNIEnv *e, jclass c, jstring s, jlong d, jlong t) {
    (void)c; const char *text = (*e)->GetStringUTFChars(e,s,0);
    android_boot_update(text,d,t); (*e)->ReleaseStringUTFChars(e,s,text);
}
JNIEXPORT void JNICALL Java_BootProbe_handoff(JNIEnv *e, jclass c) {
    (void)e; (void)c; atomic_fetch_add_explicit(&presented,1,memory_order_relaxed);
}
'''
        native = cls.directory / 'boot.c'
        native.write_text(source)
        cls.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-pthread',
                     '-I' + str(include), '-I' + str(include / 'linux'), str(native),
                     '-o', str(cls.directory / 'libboot.so')])

    @classmethod
    def command(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def probe(self, scenario):
        self.command([self.java, '-Djava.library.path=' + str(self.directory), '-cp',
                      str(self.directory), 'BootProbe', scenario])

    def test_stages_pause_display_recreation_and_real_handoff(self):
        self.probe('stages')

    def test_repeated_resume_keeps_one_poll(self):
        self.probe('repeat')

    def test_finishing_owner_stops_and_dismisses(self):
        self.probe('finishing')

    def test_destroyed_owner_stops_and_dismisses(self):
        self.probe('destroyed')


if __name__ == '__main__':
    unittest.main()
