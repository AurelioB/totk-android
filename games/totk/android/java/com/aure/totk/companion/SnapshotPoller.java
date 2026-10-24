package com.aure.totk.companion;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import org.json.JSONException;
import org.json.JSONObject;

/** One reader per attached view, with stale-callback rejection across detach/re-attach. Item action requests are
 * submitted and their answers polled on the same thread, never the UI thread. */
public final class SnapshotPoller {
    public interface Listener { void changed(Snapshot snapshot, boolean stale); }
    public final ItemActions actions = new ItemActions();
    private final DataSource source;
    private final Listener listener;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private HandlerThread thread;
    private Handler worker;
    private volatile int generation;
    private final java.util.concurrent.atomic.AtomicBoolean fullRequested = new java.util.concurrent.atomic.AtomicBoolean(true);

    public SnapshotPoller(DataSource source, Listener listener) {
        this.source = source;
        this.listener = listener;
    }
    public void start() {
        if (worker != null || !source.enabled()) return;
        int token = ++generation;
        fullRequested.set(true);
        thread = new HandlerThread("totk-companion");
        thread.start();
        worker = new Handler(thread.getLooper());
        Handler queue = worker;
        queue.post(new Runnable() {
            private Snapshot latest, lastGood;
            private String published;
            private long lastReady, lastFull;
            @Override public void run() {
                if (token != generation) return;
                long now = SystemClock.elapsedRealtime();
                pumpActions(now);
                boolean full = fullRequested.getAndSet(false) || now - lastFull >= 1000;
                try {
                    latest = Snapshot.parse(source.read(full ? 1 : 0), latest, full);
                    if (full) lastFull = now;
                } catch (Exception unavailable) {
                    latest = Snapshot.waiting();
                }
                if (token != generation) return;
                if ("ready".equals(latest.status)) { lastGood = latest; lastReady = now; }
                boolean stale = !"ready".equals(latest.status) && !"unsupported".equals(latest.status)
                        && lastGood != null && now - lastReady <= 30000;
                Snapshot visible = stale ? lastGood : latest;
                actions.observed(latest.actionKey, now);
                String key = visible.fingerprint + (stale ? "#loading" : "#live") + "#" + actions.version();
                if (!key.equals(published)) {
                    published = key;
                    ui.post(() -> { if (token == generation) listener.changed(visible, stale); });
                }
                if (token == generation) queue.postDelayed(this, 125);
            }
        });
    }
    private void pumpActions(long now) {
        ItemActions.Request request = actions.take();
        if (request != null) {
            int code;
            try { code = source.submit(request.action, request.category, request.slot, request.actor, request.partition, request.pouchGen); }
            catch (RuntimeException unavailable) { code = -3; }
            actions.submitted(code, now);
            if (actions.pending() == 0) fullRequested.set(true);
        }
        int seq = actions.pending();
        if (seq <= 0) return;
        String state = null, reason = "";
        try {
            String text = source.outcome(seq);
            if (text != null) {
                JSONObject answer = new JSONObject(text);
                if (answer.optInt("seq") == seq) { state = answer.optString("state", null); reason = answer.optString("reason", ""); }
            }
        } catch (JSONException | RuntimeException unreadable) { /* waits until the bounded answer timeout */ }
        actions.answered(seq, state, reason, now);
        // Identity (pouchGen) is strict: re-read the whole pouch as soon as an outcome is known.
        if (actions.pending() == 0) fullRequested.set(true);
    }
    public void requestFull() { fullRequested.set(true); }
    public void action(int kind, int arg) {
        Handler queue = worker;
        int token = generation;
        if (queue != null) queue.post(() -> { if (token == generation) source.send(kind, arg); });
    }
    public void stop() {
        ++generation;
        if (worker != null) worker.removeCallbacksAndMessages(null);
        if (thread != null) thread.quitSafely();
        thread = null;
        worker = null;
    }
}
