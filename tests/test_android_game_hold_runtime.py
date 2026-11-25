"""Exercise production game-hold native/cache policy and Java service/timers.

Only Android framework scheduling/process discovery and unrelated native runtime
facilities are contract doubles. The policy, JNI hooks, marker IO, GameHold and
GameControlService are production code; no game data or Android runtime is used.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
JAVA = GAME / 'android/java/com/aure/totk'


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def command(args):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise AssertionError(f'{args}\n{result.stdout}\n{result.stderr}')
    return result


C_TYPES = r'''
typedef void JNIEnv;
typedef void *jclass;
typedef int jboolean;
typedef long long jlong;
#define JNIEXPORT
#define JNICALL
#define JNI_TRUE 1
#define JNI_FALSE 0
'''
C_PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "android_game_hold.h"
static AndroidGameHold game_hold;
static struct { int paused; } game_pause;
static int background_calls, background[32], audio_calls, input_clears;
static int64_t game_input_after_ns;
static uint64_t clock_ns;
static uint64_t mono_ns(void) { return ++clock_ns; }
static int runtime_android_game_paused(void) { return game_pause.paused; }
static void android_game_pause_set(void *state, int paused) { (void)state; game_pause.paused = paused; }
static void runtime_audio_output_pause(int paused) { (void)paused; audio_calls++; }
static void runtime_input_clear(void) { input_clears++; }
static void (*vk_wb_alias_report)(const char *phase);
static void present_timeline_lifecycle(int window, int paused) { (void)window; (void)paused; }
static void vk_pcache_background(int paused) { assert(background_calls < 32); background[background_calls++] = paused; }
'''
C_MARKER_PRELUDE = r'''
static int g_log_fd = 1;
static char g_log_path[1100];
typedef struct { int unused; } ANativeActivity;
static int shutdown_calls, exit_calls, window_clears;
static void vk_pcache_shutdown(void) { shutdown_calls++; }
static void window_clear(ANativeActivity *activity) { (void)activity; window_clears++; }
static void (*vk_gpuprof_shutdown)(void);
static void test_exit(int status) { assert(!status); exit_calls++; }
#define _exit test_exit
'''
C_TESTS = r'''
static void reset(int enabled) {
    game_hold = (AndroidGameHold){.enabled=enabled};
    game_activity_paused = game_focus_paused = game_pause.paused = 0;
    background_calls = audio_calls = input_clears = 0;
}
static uint64_t pending(void) { return Java_com_aure_totk_GameHold_nativePendingPause(NULL, NULL); }
static void flush(uint64_t generation) { Java_com_aure_totk_GameHold_nativeFlushPause(NULL, NULL, (jlong)generation); }
static void settings(int active) { Java_com_aure_totk_GameHold_nativeSettingsActive(NULL, NULL, active); }
int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[1], "off")) {
        const char *off[] = {NULL, "", "0", "01", "true", "1 ", " 1", "2", "-1", "1\n"};
        assert(android_game_hold_enabled("1"));
        for (unsigned i = 0; i < sizeof off / sizeof *off; i++) {
            reset(android_game_hold_enabled(off[i])); assert(!game_hold.enabled);
            android_game_lifecycle_pause(1);
            assert(background_calls == 1 && background[0] == 1 && audio_calls == 1 && input_clears == 1);
            runtime_android_game_focus(NULL, NULL, 0, 1); android_game_lifecycle_pause(1);
            assert(background_calls == 1); settings(1); settings(0); assert(!pending());
            android_game_lifecycle_pause(0); assert(background_calls == 1);
            runtime_android_game_focus(NULL, NULL, 1, 1);
            assert(background_calls == 2 && background[1] == 0);
        }
    } else if (!strcmp(argv[1], "grace")) {
        reset(1); android_game_lifecycle_pause(1);
        uint64_t first = pending(); assert(first && !background_calls);
        android_game_lifecycle_pause(1); runtime_android_game_focus(NULL, NULL, 0, 1);
        assert(pending() == first && audio_calls == 1);
        settings(1); assert(!pending()); flush(first); assert(!background_calls);
        settings(0); uint64_t released = pending(); assert(released && released != first && !background_calls);
        settings(1); flush(released); assert(!background_calls);
        settings(0); released = pending(); assert(released);
        flush(released); assert(background_calls == 1 && background[0] == 1 && !pending());
        flush(released); settings(1); settings(0); assert(background_calls == 1 && !pending());
        android_game_lifecycle_pause(0); assert(background_calls == 1);
        runtime_android_game_focus(NULL, NULL, 1, 1); assert(background_calls == 2 && background[1] == 0);
    } else if (!strcmp(argv[1], "resume")) {
        reset(1); settings(1); android_game_lifecycle_pause(1); assert(!pending() && !background_calls);
        settings(0); uint64_t old = pending(); assert(old);
        android_game_lifecycle_pause(0); flush(old);
        assert(background_calls == 1 && background[0] == 0 && !pending());
        android_game_lifecycle_pause(1); uint64_t current = pending(); assert(current && current != old);
        flush(old); assert(background_calls == 1); flush(current);
        assert(background_calls == 2 && background[1] == 1);
    } else if (!strcmp(argv[1], "exit")) {
        reset(1); settings(1); android_game_lifecycle_pause(1);
        ANativeActivity activity = {0}; on_destroy(&activity);
        assert(shutdown_calls == 1 && exit_calls == 1 && window_clears == 1 && !background_calls);
    } else if (!strcmp(argv[1], "marker")) {
        char path[1400]; snprintf(path, sizeof path, "%s/state/game-hold", argv[2]);
        FILE *out = fopen(path, "w"); assert(out); fputs("stale\n/old/totk.log\n", out); fclose(out);
        assert(!android_game_hold_marker_reset(argv[2]) && access(path, F_OK));
        reset(0); android_game_hold_marker_publish(); assert(access(path, F_OK));
        reset(1); snprintf(g_log_path, sizeof g_log_path, "%s/custom-game/totk.log", argv[2]);
        android_game_hold_marker_publish();
        FILE *in = fopen(path, "r"); assert(in); int pid; char log[1200];
        assert(fscanf(in, "%d\n%1199[^\n]", &pid, log) == 2); fclose(in);
        assert(pid == getpid() && !strcmp(log, g_log_path));
        assert(!android_game_hold_marker_reset(argv[2]) && access(path, F_OK));
        reset(0); android_game_hold_marker_publish(); assert(access(path, F_OK));
    } else return 2;
    puts("passed"); return 0;
}
'''
JNI_PROBE = r'''
JNIEXPORT void JNICALL Java_com_aure_totk_HoldProbe_reset(JNIEnv *env, jclass cls, jboolean enabled) {
    (void)env; (void)cls;
    game_hold = (AndroidGameHold){.enabled=enabled};
    game_activity_paused = game_focus_paused = game_pause.paused = 0;
    background_calls = audio_calls = input_clears = 0;
}
JNIEXPORT void JNICALL Java_com_aure_totk_HoldProbe_pause(JNIEnv *env, jclass cls, jboolean paused) {
    (void)env; (void)cls; android_game_lifecycle_pause(paused);
}
JNIEXPORT jint JNICALL Java_com_aure_totk_HoldProbe_calls(JNIEnv *env, jclass cls) {
    (void)env; (void)cls; return background_calls;
}
JNIEXPORT jint JNICALL Java_com_aure_totk_HoldProbe_last(JNIEnv *env, jclass cls) {
    (void)env; (void)cls; return background_calls ? background[background_calls - 1] : -1;
}
'''
STUBS = {
    'android/os/Looper.java': 'package android.os; public class Looper { public static Looper getMainLooper() { return new Looper(); } }',
    'android/os/Process.java': 'package android.os; public class Process { public static int myPid() { return 42; } public static int myUid() { return 100; } }',
    'android/os/IBinder.java': 'package android.os; public interface IBinder {}',
    'android/os/Binder.java': 'package android.os; public class Binder implements IBinder {}',
    'android/content/Intent.java': '''package android.content;
public class Intent { private String action; public Intent setAction(String value) { action = value; return this; } public String getAction() { return action; } }''',
    'android/content/Context.java': '''package android.content;
public class Context {
    public static final String ACTIVITY_SERVICE = "activity";
    public static java.io.File files;
    public static final android.app.ActivityManager manager = new android.app.ActivityManager();
    public java.io.File getFilesDir() { return files; }
    public Object getSystemService(String name) { return manager; }
    public String getPackageName() { return "com.aure.totk"; }
    public Context getApplicationContext() { return this; }
}''',
    'android/app/ActivityManager.java': '''package android.app;
public class ActivityManager {
    public static class RunningAppProcessInfo {
        public int pid, uid; public String processName;
        public RunningAppProcessInfo(int pid, int uid, String name) { this.pid = pid; this.uid = uid; processName = name; }
    }
    public final java.util.List<RunningAppProcessInfo> processes = new java.util.ArrayList<>();
    public java.util.List<RunningAppProcessInfo> getRunningAppProcesses() { return processes; }
}''',
    'android/app/Service.java': '''package android.app;
public class Service extends android.content.Context {
    public android.os.IBinder onBind(android.content.Intent intent) { return null; }
    public boolean onUnbind(android.content.Intent intent) { return false; }
    public void onDestroy() {}
}''',
    'android/os/Handler.java': '''package android.os;
public class Handler {
    private static final class Task { final Runnable run; final long at; Task(Runnable r, long a) { run=r; at=a; } }
    private static final java.util.List<Task> tasks = new java.util.ArrayList<>();
    public static long now;
    public Handler(Looper looper) {}
    public boolean postDelayed(Runnable r, long delay) { tasks.add(new Task(r, now+delay)); return true; }
    public void removeCallbacks(Runnable r) { tasks.removeIf(task -> task.run == r); }
    public static int pending() { return tasks.size(); }
    public static Runnable at(long at) { for (Task task : tasks) if (task.at == at) return task.run; throw new AssertionError("missing timer " + at); }
    public static void advance(long ms) {
        long until=now+ms;
        while (true) {
            Task first=null;
            for (Task task : tasks) if (task.at <= until && (first == null || task.at < first.at)) first=task;
            if (first == null) break;
            tasks.remove(first); now=first.at; first.run.run();
        }
        now=until;
    }
}''',
}
JAVA_PROBE = r'''package com.aure.totk;
import android.app.ActivityManager.RunningAppProcessInfo;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import java.io.*;
import java.nio.file.*;
public class HoldProbe {
    static native void reset(boolean enabled);
    static native void pause(boolean paused);
    static native int calls();
    static native int last();
    static void require(boolean value, String why) { if (!value) throw new AssertionError(why); }
    static int count(String text, String value) { return (text.length()-text.replace(value, "").length())/value.length(); }
    static void marker(File log, String pid) throws Exception {
        Files.writeString(new File(Context.files, "state/game-hold").toPath(), pid + "\n" + log + "\n");
    }
    public static void main(String[] args) throws Exception {
        String mode=args[0]; Context.files=new File(args[2]); new File(Context.files,"state").mkdirs();
        Context context=new Context();
        Context.manager.processes.add(new RunningAppProcessInfo(42,100,"com.aure.totk"));
        File log=new File(Context.files,"custom-game/totk.log"); log.getParentFile().mkdirs();
        Intent hold=new Intent().setAction(GameHold.ACTION), ordinary=new Intent();
        GameControlService service=new GameControlService();
        if (mode.equals("empty")) {
            require(!GameHold.enabled(context), "missing marker enabled"); GameHold.log(context,"off");
            service.onBind(hold); service.onUnbind(ordinary); service.onUnbind(hold); service.onDestroy();
            marker(log,"99"); require(!GameHold.enabled(context),"dead PID enabled"); GameHold.log(context,"dead");
            marker(log,"42"); Context.manager.processes.get(0).processName="com.aure.totk:setup";
            require(!GameHold.enabled(context),"wrong process identity enabled");
            Context.manager.processes.get(0).processName="com.aure.totk"; Context.manager.processes.get(0).uid=999;
            require(!GameHold.enabled(context),"wrong UID enabled");
            marker(new File("relative/totk.log"),"42"); require(!GameHold.enabled(context),"relative log enabled");
            marker(log,"invalid"); require(!GameHold.enabled(context),"invalid PID enabled");
            require(!log.exists() && Handler.pending()==0,"empty/off process opened log or scheduled work");
            System.out.println("passed empty"); return; // No native library was loaded in this JVM.
        }
        if (mode.equals("cached-log")) {
            marker(log,"42"); require(GameHold.enabled(context),"live session not validated");
            GameHold.log(context,"binding acquired");
            Context.manager.processes.clear(); Files.delete(new File(Context.files,"state/game-hold").toPath());
            require(!GameHold.enabled(context),"dead session remained enabled");
            GameHold.log(context,"binding released reason=game-exited");
            String text=Files.readString(log.toPath());
            require(text.contains("[TotkGameHold] binding acquired\n") && text.contains("[TotkGameHold] binding released reason=game-exited\n"),"death/release log lost session sink");
            GameHold.endSessionLog(); GameHold.log(context,"off");
            require(Files.readString(log.toPath()).equals(text),"released session leaked writes into off launch");
            System.out.println("passed cached-log"); return;
        }
        System.load(args[1]); reset(!mode.equals("off"));
        if (!mode.equals("off")) marker(log,"42");
        GameHold.initialize();
        if (mode.equals("off")) {
            pause(true); GameHold.paused(context); service.onBind(hold); service.onUnbind(hold);
            require(calls()==1 && last()==1 && Handler.pending()==0 && !log.exists(),"off behavior changed");
            pause(false); GameHold.resumed(); require(calls()==2 && last()==0,"off resume changed");
        } else if (mode.equals("grace")) {
            require(GameHold.enabled(context),"live opt-in not enabled");
            pause(true); GameHold.paused(context); require(calls()==0,"pause flushed before grace");
            Handler.advance(749); require(calls()==0,"grace shorter than 750ms");
            Handler.advance(1); require(calls()==1 && last()==1,"real background flush lost");
            Handler.advance(250); Handler.advance(4000);
            String text=Files.readString(log.toPath());
            require(count(text,"sample=pause ")==1 && count(text,"sample=1s ")==1 && count(text,"sample=5s ")==1,"missing OOM samples");
            require(count(text,"[TotkGameHold]")==3 && text.contains("pid=42 oom_score_adj="),"wrong diagnostics sink/prefix/PID");
            pause(false); GameHold.resumed(); require(calls()==2 && last()==0,"resume did not restore cache foreground");
        } else if (mode.equals("settings")) {
            service.onBind(ordinary); pause(true); GameHold.paused(context);
            Handler.advance(200); service.onBind(hold); Handler.advance(2000);
            require(calls()==0,"Settings acquired in grace still flushed");
            service.onUnbind(ordinary); Handler.advance(1000); require(calls()==0,"ordinary close binding released Settings");
            service.onUnbind(hold); Handler.advance(749); require(calls()==0,"release lacked grace");
            service.onBind(hold); Handler.advance(1000); require(calls()==0,"rotation rebind flushed");
            service.onUnbind(hold); Handler.advance(750); require(calls()==1 && last()==1,"Home/release did not flush");
            service.onDestroy(); Handler.advance(1000); require(calls()==1,"service teardown repeated release flush");
            pause(false); GameHold.resumed();
        } else if (mode.equals("late-settings")) {
            pause(true); GameHold.paused(context); Handler.advance(1000);
            require(calls()==1 && last()==1,"late bind case lacked normal background flush");
            service.onBind(hold); Handler.advance(1000); service.onUnbind(hold); Handler.advance(1000);
            require(calls()==1,"late Settings bind repeated an already requested flush");
            pause(false); GameHold.resumed();
        } else if (mode.equals("resume")) {
            service.onBind(hold); pause(true); GameHold.paused(context);
            Runnable stale1s=Handler.at(1000), stale5s=Handler.at(5000);
            Handler.advance(200); service.onUnbind(hold);
            Runnable staleFlush=Handler.at(950);
            pause(false); GameHold.resumed(); String before=Files.readString(log.toPath());
            staleFlush.run(); stale1s.run(); stale5s.run(); Handler.advance(6000);
            require(calls()==1 && last()==0 && Handler.pending()==0,"stale timer flushed resumed game");
            require(Files.readString(log.toPath()).equals(before),"stale diagnostic ran after resume");
            pause(true); GameHold.paused(context); Runnable destroyed1s=Handler.at(Handler.now+1000);
            GameHold.destroyed(); before=Files.readString(log.toPath()); destroyed1s.run(); Handler.advance(6000);
            require(calls()==1 && Handler.pending()==0 && Files.readString(log.toPath()).equals(before),"destroy left callbacks live");
        } else if (mode.equals("destroy-service")) {
            service.onBind(hold); pause(true); GameHold.paused(context); Handler.advance(1000);
            require(calls()==0,"bound pause flushed"); service.onDestroy(); Handler.advance(750);
            require(calls()==1 && last()==1,"destroyed binding did not restore background flush");
        } else throw new AssertionError(mode);
        GameHold.destroyed(); require(Handler.pending()==0,"pending callbacks after teardown");
        System.out.println("passed " + mode);
    }
}
'''


class GameHoldRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cls.cc:
            raise unittest.SkipTest('host C compiler required')
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', os.environ.get('TMPDIR', ROOT / '.scratch')))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='game-hold-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.source = (GAME / 'runtime/frontend_android.c').read_text()
        cls.pause = section(cls.source, 'static int game_activity_paused, game_focus_paused;', '/* Top-resumed is global;')
        cls.pause += section(cls.source, 'static void android_game_lifecycle_pause(', 'static void on_pause(')
        marker = section(cls.source, 'static char g_game_hold_marker[1200];', '/* The logcat copy')
        destroy = section(cls.source, 'static void on_destroy(', '__attribute__((visibility("default"))) void ANativeActivity_onCreate')
        native = cls.work / 'native.c'
        native.write_text(C_TYPES + C_PRELUDE + cls.pause + C_MARKER_PRELUDE + marker + destroy + C_TESTS)
        cls.native = cls.work / 'native'
        command([cls.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', GAME / 'runtime', native, '-o', cls.native])

    def native_case(self, mode):
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.work) as case:
            (Path(case) / 'state').mkdir()
            self.assertIn('passed', command([self.native, mode, case]).stdout)

    def test_exact_opt_in_and_unchanged_off_cache_calls(self):
        self.native_case('off')

    def test_pause_binding_release_rotation_and_duplicate_generation_guards(self):
        self.native_case('grace')

    def test_resume_invalidates_old_pause_and_release_callbacks(self):
        self.native_case('resume')

    def test_exit_flush_is_not_deferred_by_settings(self):
        self.native_case('exit')
        close = section(self.source, 'int runtime_front_request_close(void)', '/* One stat per 500 ms')
        self.assertIn('vk_pcache_shutdown();', close)

    def test_session_marker_is_reset_and_publishes_actual_pid_and_absolute_log(self):
        self.native_case('marker')
        startup = section(self.source, 'static int start_game(', '/* ---------- input')
        self.assertLess(startup.index('android_game_hold_marker_reset('), startup.index('android_game_lock('))
        self.assertLess(startup.index('env_file(p);'), startup.index('getenv("TOTK_GAME_HOLD")'))
        self.assertLess(startup.index('getenv("TOTK_GAME_HOLD")'), startup.index('android_game_hold_marker_publish();'))

    def test_dynamic_jni_exports_and_native_library_load_order(self):
        exports = (GAME / 'android/libtotk.map').read_text()
        for name in ('nativeEnabled', 'nativeSettingsActive', 'nativePendingPause', 'nativeFlushPause'):
            self.assertIn('Java_com_aure_totk_GameHold_' + name + ';', exports)
            self.assertIn('Java_com_aure_totk_GameHold_' + name + '(', self.pause)
        activity = (JAVA / 'TotkActivity.java').read_text()
        create = section(activity, '    @Override protected void onCreate(', '    private boolean settingsRequested;')
        self.assertLess(create.index('super.onCreate(state);'), create.index('GameHold.initialize();'))
        self.assertLess(create.index('CompanionNative.isEnabled()'), create.index('GameHold.initialize();'))
        pause = section(activity, '    @Override protected void onPause()', '    @Override protected void onStop()')
        self.assertLess(pause.index('super.onPause();'), pause.index('GameHold.paused(this);'))
        self.assertIn('GameHold.updatePause();', section(activity, '    @Override public void onWindowFocusChanged(', '    @Override protected void onPause()'))
        self.assertIn('GameHold.resumed();', section(activity, '    @Override protected void onResume()', '    @Override public void onTopResumedActivityChanged('))
        destroy = section(activity, '    @Override protected void onDestroy()', '\n}')
        self.assertLess(destroy.index('GameHold.destroyed();'), destroy.index('super.onDestroy();'))


@unittest.skipUnless(sys.platform.startswith('linux'), 'host JNI fixture requires Linux')
class GameHoldJava(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc, javac, cls.java = shutil.which('cc'), shutil.which('javac'), shutil.which('java')
        if not cc or not javac or not cls.java:
            raise unittest.SkipTest('host C compiler and JDK required')
        include = Path(javac).resolve().parents[1] / 'include'
        if not (include / 'jni.h').is_file():
            raise unittest.SkipTest('JDK JNI headers required')
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', os.environ.get('TMPDIR', ROOT / '.scratch')))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='game-hold-java-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        source = (GAME / 'runtime/frontend_android.c').read_text()
        pause = section(source, 'static int game_activity_paused, game_focus_paused;', '/* Top-resumed is global;')
        pause += section(source, 'static void android_game_lifecycle_pause(', 'static void on_pause(')
        native = cls.work / 'jni.c'
        native.write_text('#include <jni.h>\n' + C_PRELUDE + pause + JNI_PROBE)
        cls.library = cls.work / 'libholdprobe.so'
        command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared', '-I', include,
                 '-I', include / 'linux', '-I', GAME / 'runtime', native, '-o', cls.library])
        sources = [JAVA / 'GameHold.java', JAVA / 'GameControlService.java']
        for path, content in STUBS.items():
            target = cls.work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
            sources.append(target)
        probe = cls.work / 'com/aure/totk/HoldProbe.java'
        probe.parent.mkdir(parents=True, exist_ok=True)
        probe.write_text(JAVA_PROBE)
        cls.classes = cls.work / 'classes'
        command([javac, '--release', '17', '-d', cls.classes, *sources, probe])

    def probe(self, mode):
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.work) as case:
            result = command([self.java, '-cp', self.classes, 'com.aure.totk.HoldProbe', mode, self.library, case])
            self.assertIn('passed ' + mode, result.stdout)

    def test_empty_service_and_stale_markers_never_load_or_call_native(self):
        self.probe('empty')

    def test_release_after_game_death_keeps_session_log_until_explicit_cleanup(self):
        self.probe('cached-log')

    def test_switch_off_has_immediate_original_cache_calls_and_no_log_work(self):
        self.probe('off')

    def test_bounded_background_grace_and_three_game_process_oom_samples(self):
        self.probe('grace')

    def test_distinct_bindings_release_grace_rotation_and_home(self):
        self.probe('settings')

    def test_binding_after_grace_cannot_undo_or_repeat_requested_flush(self):
        self.probe('late-settings')

    def test_resume_destroy_cancel_timers_even_if_stale_callbacks_run(self):
        self.probe('resume')

    def test_service_destroy_restores_background_flush(self):
        self.probe('destroy-service')


if __name__ == '__main__':
    unittest.main()
