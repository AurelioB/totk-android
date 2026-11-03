package com.aure.totk.setup;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayDeque;

/** The size of the imported game data (Home, Game data, the storage screens and the space a move needs). It is walked on
 * a thread of its own, never queued behind setup's worker, and walked again whenever the install changes: another
 * folder, install.json rewritten (every finished stage, rebuild, move or delete does that) or a data part replaced. A
 * walk can never leave the menu at "measuring…": one that fails or runs past timeoutMs reports UNKNOWN (tried again a
 * minute later or when the install changes), and a walk overtaken by a newer one is ignored. Free of Android classes,
 * so the host test runs exactly this code (tests/test_android_direct_access.py). */
final class DataSize {
    private DataSize() { }

    static final long MEASURING = -1, UNKNOWN = -2;
    /** What the data consists of (everything setup writes into the game folder, its staging area excluded). */
    static final String[] PARTS = {"rewritten", "game-data", "extracted", "shaders", "companion"};
    /** How long a walk may take before the size is reported as unknown (the tests lower it). */
    static long timeoutMs = 30_000;
    static final long RETRY_MS = 60_000;

    private static String key;           // the install the value (or the running walk) belongs to
    private static long bytes = MEASURING;
    private static long startedAt, endedAt;
    private static int generation;
    /** Walks started so far (the tests count them). */
    static int walks;

    /** The size of the data in game, or MEASURING / UNKNOWN. Starts a walk when the install changed since the last one
     * (or the last walk failed a while ago); done runs on the walking thread once that walk has a result. */
    static synchronized long get(File game, Runnable done) {
        String now = signature(game);
        long t = millis();
        if (!now.equals(key) || (bytes == UNKNOWN && t - endedAt >= RETRY_MS)) start(game, now, done);
        else if (bytes == MEASURING && t - startedAt > timeoutMs) return UNKNOWN;   // a walk stuck in the file system
        return bytes;
    }
    /** The value as it is now, without starting a walk (MEASURING when none has finished for the current install). */
    static synchronized long peek() { return bytes == MEASURING && millis() - startedAt > timeoutMs ? UNKNOWN : bytes; }
    /** Forget the value: the next get walks again (the data was deleted, moved, rebuilt or its volume changed). */
    static synchronized void invalidate() { key = null; }

    private static void start(File game, String now, Runnable done) {
        key = now; bytes = MEASURING; startedAt = millis(); walks++;
        int mine = ++generation;
        long deadline = startedAt + timeoutMs;
        Thread walker = new Thread(() -> {
            long total;
            try { total = walk(game, deadline); }
            catch (Throwable failed) { total = UNKNOWN; }
            synchronized (DataSize.class) {
                if (mine != generation) return;   // overtaken by a newer walk
                bytes = total; endedAt = millis();
            }
            if (done != null) done.run();
        }, "totk-data-size");
        walker.setDaemon(true);
        walker.setPriority(Thread.MIN_PRIORITY);
        walker.start();
    }

    /** Bytes of regular files below game's data parts, symbolic links not followed; UNKNOWN past the deadline. Entries
     * that cannot be read (gone meanwhile, no permission) count as nothing. */
    static long walk(File game, long deadline) {
        ArrayDeque<File> pending = new ArrayDeque<>();
        for (String part : PARTS) pending.push(new File(game, part));
        long total = 0;
        while (!pending.isEmpty()) {
            if (millis() > deadline) return UNKNOWN;
            File file = pending.pop();
            BasicFileAttributes attributes;
            try { attributes = Files.readAttributes(file.toPath(), BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS); }
            catch (IOException | RuntimeException unreadable) { continue; }
            if (attributes.isRegularFile()) total += attributes.size();
            else if (attributes.isDirectory()) {
                File[] children = file.listFiles();
                if (children != null) for (File child : children) pending.push(child);
            }
        }
        return total;
    }

    /** What changes when the install does: the folder, install.json and each part's own entry. */
    static String signature(File game) {
        StringBuilder s = new StringBuilder(game.getAbsolutePath());
        File install = new File(game, "install.json");
        s.append('|').append(install.lastModified()).append(':').append(install.length());
        for (String part : PARTS) s.append('|').append(new File(game, part).lastModified());
        return s.toString();
    }
    private static long millis() { return System.nanoTime() / 1_000_000L; }
}
