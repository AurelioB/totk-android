package com.aure.totk;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** All platform/history/proc/receipt/diagnostic IO is serialized on the worker.
 * UI callbacks only enqueue, check current lifecycle eligibility, and present/dismiss. */
public final class ExitNotice {
    private static volatile Thread workerThread;
    private static final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "TotkExitNotice"); thread.setDaemon(true); workerThread = thread; return thread;
    });
    private static final Coordinator coordinator = new Coordinator();
    private static volatile Context gameContext;
    private static ExitNoticeStore gameStore; // Worker-owned, survives Activity recreation.
    private static String gameToken;
    private volatile boolean resumed;
    private volatile long epoch;
    private volatile Request current;
    private AlertDialog dialog; // UI-owned.

    /** Before NativeActivity.onCreate; attaching the application context performs no receipt IO. */
    public static void attach(Context context) { gameContext = context.getApplicationContext(); }
    /** Native proves successful game-thread creation before START. PAUSE only enqueues.
     * CLOSE is called by the existing non-UI close poller after native save admission/cache shutdown. */
    public static void nativeEvent(int event, byte[] logBytes, byte[] dataBytes) {
        Context context = gameContext;
        if (Build.VERSION.SDK_INT < 30 || context == null) return;
        if (event == 2 && Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("close receipt cannot wait on the UI thread");
        long elapsed = SystemClock.elapsedRealtime();
        Runnable record = () -> {
            try {
                if (event == 0) {
                    if (gameToken != null) return;
                    String token = ExitNoticePolicy.SUMMARY_PREFIX + UUID.randomUUID();
                    ActivityManager manager = context.getSystemService(ActivityManager.class);
                    if (manager == null) return;
                    manager.setProcessStateSummary(token.getBytes(StandardCharsets.UTF_8));
                    ExitNoticeStore store = new ExitNoticeStore(context.getFilesDir());
                    store.begin(token, Process.myPid(), startTicks(Process.myPid()), boot(), install(context),
                            new String(logBytes, StandardCharsets.UTF_8), new File(new String(dataBytes, StandardCharsets.UTF_8), "totk.env").getPath());
                    gameStore = store; gameToken = token;
                } else if (gameStore != null && gameToken != null) {
                    if (event == 1) gameStore.background(gameToken, elapsed,
                            smallText(new File(context.getFilesDir(), "state/save-generation"), "unavailable", 128));
                    else if (event == 2) gameStore.close(gameToken);
                }
            } catch (Exception unavailable) { Log.w("TotkExitNotice", "Session receipt unavailable", unavailable); }
        };
        if (event != 2) worker.execute(record);
        else if (Thread.currentThread() == workerThread) record.run();
        else {
            try { worker.submit(record).get(); }
            catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
            catch (java.util.concurrent.ExecutionException unavailable) { Log.w("TotkExitNotice", "Close receipt unavailable", unavailable); }
        }
    }
    public void resume(Activity activity, BooleanSupplier ready, Consumer<Boolean> visible) {
        resumed = true;
        if (Build.VERSION.SDK_INT < 30 || dialog != null || activity.isFinishing() || activity.isDestroyed()
                || !ready.getAsBoolean() || current != null && current.epoch == epoch) return;
        Request request = new Request(this, activity, epoch, ready, visible);
        current = request;
        worker.execute(() -> coordinator.add(request));
    }
    public void pause() {
        resumed = false; epoch++;
        if (dialog != null) dialog.dismiss();
    }
    private static final class Request {
        final ExitNotice notice;
        final Activity activity;
        final long epoch;
        final BooleanSupplier ready;
        final Consumer<Boolean> visible;
        Request(ExitNotice notice, Activity activity, long epoch, BooleanSupplier ready, Consumer<Boolean> visible) {
            this.notice = notice; this.activity = activity; this.epoch = epoch; this.ready = ready; this.visible = visible;
        }
        boolean active() { return notice.resumed && notice.epoch == epoch && notice.current == this; }
        void complete() { if (notice.current == this) notice.current = null; }
    }
    private static final class Candidate {
        final ExitNoticeStore.Receipt receipt;
        final ExitNoticePolicy.Decision decision;
        Candidate(ExitNoticeStore.Receipt receipt, ExitNoticePolicy.Decision decision) { this.receipt = receipt; this.decision = decision; }
    }
    /** One history query and in-flight presentation per process; cross-process reservation lives in the store. */
    private static final class Coordinator {
        final ArrayDeque<Request> waiting = new ArrayDeque<>();
        final java.util.LinkedHashSet<String> displayed = new java.util.LinkedHashSet<>();
        ExitNoticeStore store;
        Candidate candidate;
        ExitNoticeStore.Owner owner;
        String reservationId;
        boolean delivering, retryScheduled;
        void add(Request request) { waiting.add(request); drive(); }
        void drive() {
            if (delivering) return;
            while (!waiting.isEmpty() && !waiting.peek().active()) finish(waiting.remove());
            if (waiting.isEmpty()) { candidate = null; store = null; reservationId = null; return; }
            Request request = waiting.peek();
            try {
                if (candidate == null) {
                    Context context = request.activity.getApplicationContext();
                    store = new ExitNoticeStore(context.getFilesDir());
                    candidate = inspect(context, store);
                    if (candidate == null || displayed.contains(candidate.decision.exit.identity())) { finishAll(); return; }
                    owner = new ExitNoticeStore.Owner(boot(), Process.myPid(), startTicks(Process.myPid()));
                    reservationId = UUID.randomUUID().toString();
                }
                ExitNoticeStore.Reservation reservation = store.reserve(candidate.receipt, candidate.decision.exit,
                        reservationId, owner, ExitNotice::ownerAlive);
                if (reservation.status == ExitNoticeStore.Status.GONE) { finishAll(); return; }
                if (reservation.status == ExitNoticeStore.Status.BUSY) {
                    if (!retryScheduled) {
                        retryScheduled = true;
                        worker.schedule(() -> { retryScheduled = false; drive(); }, 50, TimeUnit.MILLISECONDS);
                    }
                    return;
                }
                delivering = true;
                Candidate result = candidate;
                ExitNoticeStore.Claim claim = reservation.claim;
                request.activity.runOnUiThread(() -> present(request, result, claim));
            } catch (Exception unavailable) {
                Log.w("TotkExitNotice", "Previous-session diagnosis unavailable", unavailable);
                finishAll();
            }
        }
        void delivered(Request request, Candidate result, ExitNoticeStore.Claim claim, boolean shown) {
            try {
                if (shown) {
                    displayed.add(claim.identity);
                    while (displayed.size() > 64) displayed.remove(displayed.iterator().next());
                    if (store.shown(claim)) diagnostic(result.receipt, result.decision);
                } else store.release(claim);
            } catch (IOException unavailable) { Log.w("TotkExitNotice", "Notice delivery receipt unavailable", unavailable); }
            delivering = false;
            waiting.remove(request); finish(request);
            if (shown) finishAll();
            else drive();
        }
        void finish(Request request) { request.activity.runOnUiThread(request::complete); }
        void finishAll() {
            while (!waiting.isEmpty()) finish(waiting.remove());
            candidate = null; store = null; reservationId = null;
        }
    }
    private static void present(Request request, Candidate result, ExitNoticeStore.Claim claim) {
        boolean shown = false;
        ExitNotice notice = request.notice;
        try {
            if (request.active() && !request.activity.isFinishing() && !request.activity.isDestroyed() && request.ready.getAsBoolean()) {
                AlertDialog next = new AlertDialog.Builder(request.activity).setTitle("Previous session").setMessage(result.decision.message)
                        .setPositiveButton("OK", (d, which) -> {}).create();
                notice.dialog = next;
                next.setCanceledOnTouchOutside(false);
                next.setOnDismissListener(d -> { if (notice.dialog == next) notice.dialog = null; request.visible.accept(false); });
                next.setOnKeyListener((d, code, event) -> {
                    if (code != KeyEvent.KEYCODE_BUTTON_A && code != KeyEvent.KEYCODE_BUTTON_B) return false;
                    if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) next.dismiss();
                    return true;
                });
                request.visible.accept(true);
                next.show(); shown = true;
                next.getButton(AlertDialog.BUTTON_POSITIVE).requestFocus();
            }
        } catch (RuntimeException failed) {
            if (notice.dialog != null) notice.dialog.dismiss();
            notice.dialog = null;
            request.visible.accept(false);
            // Diagnostic IO, including logcat, stays off the UI thread even on a rejected window.
            worker.execute(() -> Log.w("TotkExitNotice", "Could not present previous-session notice", failed));
        }
        boolean displayed = shown;
        worker.execute(() -> coordinator.delivered(request, result, claim, displayed));
    }
    private static Candidate inspect(Context context, ExitNoticeStore store) throws Exception {
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        if (manager == null) return null;
        List<ApplicationExitInfo> history;
        try { history = manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 0); }
        catch (RuntimeException unavailable) { history = java.util.Collections.emptyList(); }
        ExitNoticePolicy.Exit[] exits = new ExitNoticePolicy.Exit[history.size()];
        for (int i = 0; i < exits.length; i++) {
            ApplicationExitInfo exit = history.get(i);
            byte[] summary = exit.getProcessStateSummary();
            String token = summary == null ? "" : new String(summary, StandardCharsets.UTF_8);
            exits[i] = new ExitNoticePolicy.Exit(exit.getProcessName(), exit.getRealUid(), exit.getPid(), exit.getReason(), exit.getStatus(),
                    exit.getImportance(), exit.getTimestamp(), exit.getPss(), exit.getRss(), token);
        }
        String install = install(context);
        List<ExitNoticeStore.Receipt> receipts = store.receipts();
        for (int i = receipts.size() - 1; i >= 0; i--) {
            ExitNoticeStore.Receipt receipt = receipts.get(i);
            ExitNoticePolicy.Decision decision = ExitNoticePolicy.classify(receipt.session, context.getPackageName(), Process.myUid(), install, receipt.closed, exits);
            if (decision.exit == null && receipt.session.install.equals(install) && alive(manager, context.getPackageName(), receipt.session)) continue;
            if (decision.message != null && ExitNoticeStore.enabled(new File(receipt.envPath))) return new Candidate(receipt, decision);
            if (store.consume(receipt, decision.exit)) diagnostic(receipt, decision);
            return null; // The latest ended/updated session supersedes all older interruptions.
        }
        return null;
    }
    private static String install(Context context) throws Exception {
        PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
        // Equality of package metadata, not ordering against today's device clock.
        return info.firstInstallTime + ":" + info.lastUpdateTime + ":" + info.getLongVersionCode() + ":" + context.getApplicationInfo().sourceDir;
    }
    private static boolean alive(ActivityManager manager, String name, ExitNoticePolicy.Session session) {
        String boot = boot();
        if (!boot.isEmpty() && !session.boot.equals(boot)) return false;
        long ticks = startTicks(session.pid);
        if (ticks > 0 && session.startTicks > 0) return ticks == session.startTicks;
        List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes != null) for (ActivityManager.RunningAppProcessInfo process : processes)
            if (process.pid == session.pid && process.uid == Process.myUid() && name.equals(process.processName)) return true;
        return false;
    }
    private static boolean ownerAlive(ExitNoticeStore.Owner owner) {
        String boot = boot();
        return !boot.isEmpty() && boot.equals(owner.boot) && owner.ticks > 0 && startTicks(owner.pid) == owner.ticks;
    }
    private static String boot() { return smallText(new File("/proc/sys/kernel/random/boot_id"), "", 128); }
    private static long startTicks(int pid) {
        try {
            String stat = smallText(new File("/proc/" + pid + "/stat"), "", 4096);
            String[] fields = stat.substring(stat.lastIndexOf(')') + 2).trim().split(" +");
            return fields.length > 19 ? Long.parseLong(fields[19]) : 0;
        } catch (RuntimeException unavailable) { return 0; }
    }
    private static String smallText(File file, String fallback, int max) {
        try (java.io.BufferedReader in = Files.newBufferedReader(file.toPath(), StandardCharsets.UTF_8)) {
            char[] buffer = new char[max]; int count = in.read(buffer);
            return count > 0 ? new String(buffer, 0, count).trim() : fallback;
        } catch (IOException | SecurityException unavailable) { return fallback; }
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
        } catch (IOException | SecurityException unavailable) { /* logcat retains the worker diagnostic. */ }
    }
}
