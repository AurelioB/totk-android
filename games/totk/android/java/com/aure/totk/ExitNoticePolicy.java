package com.aure.totk;

/** Pure classification. Platform timestamps identify records; they never order sessions. */
public final class ExitNoticePolicy {
    public static final String LOW_MEMORY_TEXT = "Android closed the previous game session to free memory. Load a saved game; progress since the last save may be lost.";
    public static final String UNEXPECTED_TEXT = "The previous session ended unexpectedly.";
    public static final String SUMMARY_PREFIX = "totk-session-v2:";
    static final int EXIT_SELF = 1, LOW_MEMORY = 3, USER_REQUESTED = 10, USER_STOPPED = 11,
            PACKAGE_STATE_CHANGE = 15, PACKAGE_UPDATED = 16;

    public static final class Session {
        public final long sequence, startTicks;
        public final int pid;
        public final String token, boot, install;
        public Session(long sequence, String token, int pid, long startTicks, String boot, String install) {
            this.sequence = sequence; this.token = token; this.pid = pid; this.startTicks = startTicks;
            this.boot = boot; this.install = install;
        }
        public String key() { return token; }
    }
    public static final class Exit {
        public final String process, token;
        public final int uid, pid, reason, status, importance;
        public final long timestampMs, pssKiB, rssKiB;
        public Exit(String process, int uid, int pid, int reason, int status, int importance,
                long timestampMs, long pssKiB, long rssKiB, String token) {
            this.process = process; this.uid = uid; this.pid = pid; this.reason = reason; this.status = status;
            this.importance = importance; this.timestampMs = timestampMs; this.pssKiB = pssKiB;
            this.rssKiB = rssKiB; this.token = token;
        }
        /** Same platform record across clock/time-zone changes and reboot. PSS/importance are not identity. */
        public String identity() { return process + ":" + uid + ":" + pid + ":" + timestampMs + ":" + token; }
    }
    public static final class Decision {
        public final String message;
        public final Exit exit;
        public Decision(String message, Exit exit) { this.message = message; this.exit = exit; }
    }
    public static Decision classify(Session session, String gameProcess, int uid, String install,
            boolean orderlyClose, Exit[] history) {
        if (session == null || !session.install.equals(install)) return new Decision(null, null);
        Exit matched = null;
        for (Exit exit : history) {
            if (exit.pid != session.pid || exit.uid != uid || !gameProcess.equals(exit.process)
                    || !session.token.equals(exit.token)) continue;
            if (matched != null && !matched.identity().equals(exit.identity())) return new Decision(null, null);
            matched = exit;
        }
        // Rotated/absent/ambiguous history is not evidence of an unexpected or low-memory death.
        if (matched == null || orderlyClose) return new Decision(null, matched);
        switch (matched.reason) {
            case EXIT_SELF:
                return new Decision(matched.status == 0 ? null : UNEXPECTED_TEXT, matched);
            case USER_REQUESTED: case USER_STOPPED: case PACKAGE_STATE_CHANGE: case PACKAGE_UPDATED:
                return new Decision(null, matched);
            case LOW_MEMORY:
                return new Decision(LOW_MEMORY_TEXT, matched);
            default:
                return new Decision(UNEXPECTED_TEXT, matched);
        }
    }
    private ExitNoticePolicy() { }
}
