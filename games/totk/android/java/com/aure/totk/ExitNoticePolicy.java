package com.aure.totk;

/** No Android dependencies: platform exit history is evidence, never a save receipt. */
public final class ExitNoticePolicy {
    public static final String LOW_MEMORY_TEXT = "Android closed the previous game session to free memory. Load a saved game; progress since the last save may be lost.";
    public static final String UNEXPECTED_TEXT = "The previous session ended unexpectedly.";
    // ApplicationExitInfo values, including package-change reasons added after API 30.
    static final int EXIT_SELF = 1, LOW_MEMORY = 3, USER_REQUESTED = 10, USER_STOPPED = 11,
            PACKAGE_STATE_CHANGE = 15, PACKAGE_UPDATED = 16;

    public static final class Session {
        public final int pid;
        public final long startTicks, startedMs;
        public Session(int pid, long startTicks, long startedMs) {
            this.pid = pid; this.startTicks = startTicks; this.startedMs = startedMs;
        }
        public String key() { return pid + ":" + startTicks + ":" + startedMs; }
    }
    public static final class Exit {
        public final String process;
        public final int pid, reason, status, importance;
        public final long timestampMs, pssKiB, rssKiB;
        public Exit(String process, int pid, int reason, int status, int importance, long timestampMs, long pssKiB, long rssKiB) {
            this.process = process; this.pid = pid; this.reason = reason; this.status = status; this.importance = importance;
            this.timestampMs = timestampMs; this.pssKiB = pssKiB; this.rssKiB = rssKiB;
        }
    }
    public static final class Decision {
        public final String message;
        public final Exit exit;
        public Decision(String message, Exit exit) { this.message = message; this.exit = exit; }
    }

    /** A live identity, an update, or a consumed session must never become a cold-return warning.
     * The successor's start bounds PID reuse; an absent history only supports "unexpected". */
    public static Decision classify(Session session, String gameProcess, long updateMs, long successorMs,
            boolean alive, boolean orderlyClose, boolean handled, Exit[] history) {
        if (session == null || alive || handled || session.startedMs < updateMs)
            return new Decision(null, null);
        Exit matched = null;
        for (Exit exit : history) {
            if (exit.pid != session.pid || !gameProcess.equals(exit.process)
                    || exit.timestampMs < session.startedMs || exit.timestampMs > successorMs) continue;
            // The first death after this start belongs to this session, not a later reused PID.
            if (matched == null || exit.timestampMs < matched.timestampMs) matched = exit;
        }
        if (orderlyClose) return new Decision(null, matched);
        if (matched == null) return new Decision(UNEXPECTED_TEXT, null);
        switch (matched.reason) {
            case EXIT_SELF:
                return new Decision(matched.status == 0 ? null : UNEXPECTED_TEXT, matched);
            case USER_REQUESTED: case USER_STOPPED:
            case PACKAGE_STATE_CHANGE: case PACKAGE_UPDATED:
                return new Decision(null, matched);
            case LOW_MEMORY:
                return new Decision(LOW_MEMORY_TEXT, matched);
            default:
                return new Decision(UNEXPECTED_TEXT, matched);
        }
    }
    private ExitNoticePolicy() { }
}
