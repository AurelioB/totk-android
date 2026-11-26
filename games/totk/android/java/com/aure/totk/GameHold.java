package com.aure.totk;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Session-only Settings hold. Reading the marker never loads the game library,
 * including in an empty service process created by a bind racing with game death. */
public final class GameHold {
    public static final String ACTION = "com.aure.totk.GAME_HOLD";
    private static final long PAUSE_GRACE_MS = 750;
    private static final Handler main = new Handler(Looper.getMainLooper());
    // These fields and all native lifecycle calls are owned by the game UI thread.
    private static boolean nativeReady, sessionEnabled, settingsBound;
    private static long flushGeneration, diagnosticGeneration;
    private static File sessionLogTarget; // Cleared after Setup's final connection-release diagnostic.
    private static Runnable pauseFlush, oom1s, oom5s;

    private static native boolean nativeEnabled();
    private static native void nativeSettingsActive(boolean active);
    private static native long nativePendingPause();
    private static native void nativeFlushPause(long generation);

    /** Only TotkActivity calls this, after NativeActivity and CompanionNative
     * have loaded the runtime and startup has frozen its environment. */
    static void initialize() {
        sessionEnabled = nativeEnabled();
        nativeReady = true;
        if (sessionEnabled) {
            nativeSettingsActive(settingsBound);
            updatePause();
        }
    }

    private static File sessionLog(Context context) {
        try (BufferedReader in = new BufferedReader(new FileReader(new File(context.getFilesDir(), "state/game-hold")))) {
            int pid = Integer.parseInt(in.readLine());
            String path = in.readLine();
            if (pid <= 0 || path == null || !new File(path).isAbsolute()) return null;
            ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            List<ActivityManager.RunningAppProcessInfo> processes = manager == null ? null : manager.getRunningAppProcesses();
            if (processes != null) for (ActivityManager.RunningAppProcessInfo process : processes) {
                if (process.pid == pid && process.uid == Process.myUid() && context.getPackageName().equals(process.processName))
                    return new File(path);
            }
        } catch (IOException | NumberFormatException | SecurityException ignored) { }
        return null;
    }

    /** The actual running session's exact opt-in, not a next-launch settings value. */
    public static boolean enabled(Context context) {
        File target = sessionLog(context);
        if (target != null) sessionLogTarget = target;
        return target != null;
    }
    /** Both processes append to the native session's actual log, including a
     * custom data directory. A previously validated target survives game death
     * until Setup logs its release and calls endSessionLog. */
    public static void log(Context context, String event) {
        File path = sessionLogTarget;
        if (path == null) path = sessionLog(context);
        if (path == null) return;
        sessionLogTarget = path;
        try (FileOutputStream out = new FileOutputStream(path, true)) {
            out.write(("[TotkGameHold] " + event.replace('\n', ' ').replace('\r', ' ') + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException | SecurityException ignored) { }
    }
    /** Setup calls after its final release line, so a dead session can be logged
     * without leaving a stale log target for a later switched-off launch. */
    public static void endSessionLog() { sessionLogTarget = null; }

    /** Service callbacks already run on the UI thread. The stored binding also
     * covers an empty service process in which the game is initialized later. */
    static void settingsActive(Context context, boolean active) {
        settingsBound = active;
        if (!nativeReady || !sessionEnabled) return;
        log(context, "game service Settings=" + active + " pid=" + Process.myPid());
        nativeSettingsActive(active);
        updatePause();
    }

    /** Native pause/focus facets can arrive in either order. Ask for the one
     * pending native generation after each callback rather than scheduling twice. */
    static void updatePause() {
        if (!nativeReady || !sessionEnabled) return;
        long generation = nativePendingPause();
        if (generation == flushGeneration) return;
        if (pauseFlush != null) main.removeCallbacks(pauseFlush);
        pauseFlush = null;
        flushGeneration = generation;
        if (generation == 0) return;
        pauseFlush = () -> {
            if (!nativeReady || !sessionEnabled || flushGeneration != generation) return;
            pauseFlush = null;
            flushGeneration = 0;
            nativeFlushPause(generation);
        };
        // Also debounce unbind/rebind on Settings recreation. Home/external UI
        // still requests the normal cache flush after this <=1s grace.
        main.postDelayed(pauseFlush, PAUSE_GRACE_MS);
    }

    private static void cancelDiagnostics() {
        diagnosticGeneration++;
        if (oom1s != null) main.removeCallbacks(oom1s);
        if (oom5s != null) main.removeCallbacks(oom5s);
        oom1s = oom5s = null;
    }
    private static void sampleOom(Context context, String sample) {
        String value = "unavailable";
        try (BufferedReader in = new BufferedReader(new FileReader("/proc/self/oom_score_adj"))) {
            String line = in.readLine();
            if (line != null) value = line.trim();
        } catch (IOException | SecurityException ignored) { }
        log(context, "game pause sample=" + sample + " pid=" + Process.myPid() + " oom_score_adj=" + value);
    }
    static void paused(Context context) {
        if (!nativeReady || !sessionEnabled) return;
        cancelDiagnostics();
        long generation = diagnosticGeneration;
        Context app = context.getApplicationContext();
        sampleOom(app, "pause");
        oom1s = () -> { if (nativeReady && generation == diagnosticGeneration) sampleOom(app, "1s"); };
        oom5s = () -> { if (nativeReady && generation == diagnosticGeneration) sampleOom(app, "5s"); };
        main.postDelayed(oom1s, 1000);
        main.postDelayed(oom5s, 5000);
        updatePause();
    }
    static void resumed() {
        cancelDiagnostics();
        updatePause();
    }
    static void destroyed() {
        cancelDiagnostics();
        if (pauseFlush != null) main.removeCallbacks(pauseFlush);
        pauseFlush = null;
        flushGeneration = 0;
        nativeReady = sessionEnabled = false;
        endSessionLog();
    }

    private GameHold() { }
}
