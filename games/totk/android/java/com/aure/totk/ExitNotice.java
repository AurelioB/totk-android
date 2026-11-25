package com.aure.totk;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.os.Build;
import android.util.Log;
import android.view.KeyEvent;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** Cold-return diagnosis only, not process retention or a save-state facility.
 * Platform/proc/history work runs on a dedicated Java worker, never guest/render threads. */
public final class ExitNotice {
    private static final ExecutorService worker = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "TotkExitNotice"); thread.setDaemon(true); return thread;
    });
    private boolean resumed, checking;
    private long epoch;
    private AlertDialog dialog;

    public void resume(Activity activity, BooleanSupplier ready, Consumer<Boolean> visible) {
        resumed = true;
        if (Build.VERSION.SDK_INT < 30 || checking || dialog != null) return;
        checking = true;
        long request = ++epoch;
        worker.execute(() -> {
            ExitNoticeStore store = new ExitNoticeStore(activity.getFilesDir());
            ExitNoticeStore.Receipt pending = null;
            ExitNoticePolicy.Decision decision = null;
            try {
                ActivityManager manager = activity.getSystemService(ActivityManager.class);
                if (manager != null) {
                    long update = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).lastUpdateTime;
                    List<ExitNoticeStore.Receipt> receipts = store.receipts();
                    boolean checkedPrevious = false;
                    for (int i = receipts.size() - 1; i >= 0; i--) {
                        ExitNoticeStore.Receipt receipt = receipts.get(i);
                        if (alive(manager, activity.getPackageName(), receipt.session)) continue;
                        boolean previous = !checkedPrevious;
                        checkedPrevious = true;
                        long cutoff = i + 1 < receipts.size() ? receipts.get(i + 1).session.startedMs : System.currentTimeMillis();
                        List<ApplicationExitInfo> history;
                        try { history = manager.getHistoricalProcessExitReasons(activity.getPackageName(), receipt.session.pid, 32); }
                        catch (RuntimeException unavailable) { history = java.util.Collections.emptyList(); }
                        ExitNoticePolicy.Exit[] exits = new ExitNoticePolicy.Exit[history.size()];
                        for (int j = 0; j < exits.length; j++) {
                            ApplicationExitInfo exit = history.get(j);
                            exits[j] = new ExitNoticePolicy.Exit(exit.getProcessName(), exit.getPid(), exit.getReason(), exit.getStatus(),
                                    exit.getImportance(), exit.getTimestamp(), exit.getPss(), exit.getRss());
                        }
                        ExitNoticePolicy.Decision classified = ExitNoticePolicy.classify(receipt.session, activity.getPackageName(),
                                update, cutoff, false, receipt.orderly(), false, exits);
                        if (!previous || classified.message == null || !ExitNoticeStore.enabled(new File(receipt.envPath))) {
                            if (store.claim(receipt)) diagnostic(receipt, classified);
                            store.retire(receipt);
                        } else { pending = receipt; decision = classified; }
                    }
                }
            } catch (Exception unavailable) { Log.w("TotkExitNotice", "Previous-session diagnosis unavailable", unavailable); }
            ExitNoticeStore.Receipt receipt = pending;
            ExitNoticePolicy.Decision result = decision;
            activity.runOnUiThread(() -> {
                checking = false;
                if (request != epoch) {
                    if (resumed) resume(activity, ready, visible);
                    return;
                }
                if (!resumed || activity.isFinishing() || activity.isDestroyed()
                        || receipt == null || !ready.getAsBoolean()) return;
                try {
                    if (!store.claim(receipt)) { store.retire(receipt); return; }
                    diagnostic(receipt, result);
                    dialog = new AlertDialog.Builder(activity).setTitle("Previous session").setMessage(result.message)
                            .setPositiveButton("OK", (d, which) -> {}).create();
                    dialog.setCanceledOnTouchOutside(false);
                    dialog.setOnDismissListener(d -> { dialog = null; visible.accept(false); });
                    dialog.setOnKeyListener((d, code, event) -> {
                        if (code != KeyEvent.KEYCODE_BUTTON_A && code != KeyEvent.KEYCODE_BUTTON_B) return false;
                        if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) dialog.dismiss();
                        return true;
                    });
                    visible.accept(true);
                    dialog.show();
                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).requestFocus();
                    store.retire(receipt);
                } catch (IOException | RuntimeException failed) {
                    if (dialog != null) { dialog.dismiss(); dialog = null; }
                    visible.accept(false);
                    Log.w("TotkExitNotice", "Could not present previous-session notice", failed);
                }
            });
        });
    }
    public void pause() {
        resumed = false;
        epoch++;
        if (dialog != null) dialog.dismiss();
    }
    private static boolean alive(ActivityManager manager, String processName, ExitNoticePolicy.Session session) {
        try {
            String stat = Files.readString(new File("/proc/" + session.pid + "/stat").toPath());
            String[] fields = stat.substring(stat.lastIndexOf(')') + 2).trim().split(" +");
            return fields.length > 19 && Long.parseLong(fields[19]) == session.startTicks;
        } catch (IOException | RuntimeException unavailable) {
            // If proc is restricted, a matching same-UID game PID is conservatively still alive.
            List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
            if (processes != null) for (ActivityManager.RunningAppProcessInfo process : processes)
                if (process.pid == session.pid && processName.equals(process.processName)) return true;
            return false;
        }
    }
    private static void diagnostic(ExitNoticeStore.Receipt receipt, ExitNoticePolicy.Decision decision) {
        ExitNoticePolicy.Exit exit = decision.exit;
        String line = "[TotkExitNotice] session=" + receipt.session.key() + " reason=" + (exit == null ? "unavailable" : exit.reason)
                + " importance=" + (exit == null ? "unavailable" : exit.importance)
                + " pss_kib=" + (exit == null ? "unavailable" : exit.pssKiB)
                + " rss_kib=" + (exit == null ? "unavailable" : exit.rssKiB)
                + " timestamp_ms=" + (exit == null ? "unavailable" : exit.timestampMs);
        Log.i("TotkExitNotice", line);
        try (FileOutputStream out = new FileOutputStream(receipt.logPath, true)) {
            out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException | SecurityException unavailable) { /* logcat retains the diagnostic if storage is unavailable. */ }
    }
}
