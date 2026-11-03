package com.aure.totk.companion;

/** At most one companion item action at a time. The UI thread taps; the poller thread submits, polls the answer
 * and settles it. Plain Java (no Android types) so request identity and duplicate-tap dropping are host-testable. */
public final class ItemActions {
    /** The native side times out on its own; this only keeps the UI from waiting forever on a lost answer. */
    public static final long ANSWER_TIMEOUT_MS = 15000;
    /** A result stays visible at least this long, then until the inventory or action state next changes. */
    public static final long RESULT_MIN_MS = 1500;

    public enum Phase { IDLE, QUEUED, PENDING, DONE, REFUSED, FAILED }

    /** Names the item by stable identity, as the snapshot showed it; the game thread re-checks all of it. */
    public static final class Request {
        public final String action, category, actor;
        public final int slot, partition;
        public final long pouchGen;
        public Request(String action, String category, int slot, String actor, int partition, long pouchGen) {
            this.action = action; this.category = category; this.slot = slot; this.actor = actor;
            this.partition = partition; this.pouchGen = pouchGen;
        }
        public boolean sameItem(String category, int slot, String actor) {
            return this.category.equals(category) && this.slot == slot && this.actor.equals(actor);
        }
    }

    /** Immutable view for drawing. */
    public static final class State {
        public final Phase phase;
        public final Request request;
        public final String reason;
        State(Phase phase, Request request, String reason) { this.phase = phase; this.request = request; this.reason = reason; }
        public boolean busy() { return phase == Phase.QUEUED || phase == Phase.PENDING; }
    }

    private Phase phase = Phase.IDLE;
    private Request request;
    private String reason = "";
    private boolean taken;
    private int seq, version;
    private long since;
    private String baseline;

    /** UI thread. Returns false, dropping the tap, while another request is queued or pending. */
    public synchronized boolean tap(Request value) {
        if (value == null || phase == Phase.QUEUED || phase == Phase.PENDING) return false;
        request = value; phase = Phase.QUEUED; reason = ""; taken = false; seq = 0; baseline = null; version++;
        return true;
    }

    /** Poller thread: the queued request, handed out exactly once. */
    public synchronized Request take() {
        if (phase != Phase.QUEUED || taken) return null;
        taken = true;
        return request;
    }

    /** Poller thread: the submit return code (seq > 0, or -1 busy, -2 refused now, -3 unsupported, -4 invalid). */
    public synchronized void submitted(int code, long now) {
        if (phase != Phase.QUEUED || !taken) return;
        if (code > 0) { seq = code; phase = Phase.PENDING; since = now; version++; return; }
        settle(code == -3 || code == -4 ? Phase.FAILED : Phase.REFUSED,
                code == -1 ? "busy" : code == -2 ? "notNow" : code == -3 ? "unsupported" : "invalid", now);
    }

    /** Sequence to poll, or 0. */
    public synchronized int pending() { return phase == Phase.PENDING ? seq : 0; }

    /** Poller thread: one answer for seq. A null or "pending" state waits, bounded by ANSWER_TIMEOUT_MS. */
    public synchronized void answered(int answerSeq, String state, String why, long now) {
        if (phase != Phase.PENDING || answerSeq != seq) return;
        String detail = why == null ? "" : why;
        if ("done".equals(state)) settle(Phase.DONE, detail, now);
        else if ("refused".equals(state)) settle(Phase.REFUSED, detail, now);
        else if ("failed".equals(state)) settle(Phase.FAILED, detail, now);
        else if (state != null && !"pending".equals(state)) settle(Phase.FAILED, detail.isEmpty() ? state : detail, now);
        else if (now - since >= ANSWER_TIMEOUT_MS) settle(Phase.FAILED, "noAnswer", now);
    }

    /** Poller thread, every poll: key of the inventory/action state. Clears a shown result once that state changes
     * after RESULT_MIN_MS (changes inside that window are the action's own effect). */
    public synchronized void observed(String key, long now) {
        if (phase != Phase.DONE && phase != Phase.REFUSED && phase != Phase.FAILED) return;
        if (now - since < RESULT_MIN_MS) return;
        if (baseline == null) { baseline = key; return; }
        if (!baseline.equals(key)) { phase = Phase.IDLE; request = null; reason = ""; version++; }
    }

    /** Changes whenever the drawable state changes. */
    public synchronized int version() { return version; }

    public synchronized State state() { return new State(phase, request, reason); }

    private void settle(Phase value, String why, long now) {
        phase = value; reason = why; since = now; baseline = null; version++;
    }
}
