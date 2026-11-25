package com.aure.totk;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Native-published receipts and a small cross-process notice-once ledger. No Android dependencies. */
public final class ExitNoticeStore {
    public static final class Receipt {
        public final File file;
        public final ExitNoticePolicy.Session session;
        public final String logPath, envPath;
        Receipt(File file, ExitNoticePolicy.Session session, String logPath, String envPath) {
            this.file = file; this.session = session; this.logPath = logPath; this.envPath = envPath;
        }
        public boolean orderly() { return new File(file + ".closed").isFile(); }
    }
    private final File directory;
    public ExitNoticeStore(File files) { directory = new File(files, "state/exit-notice"); }

    public List<Receipt> receipts() throws IOException {
        List<Receipt> result = new ArrayList<>();
        File[] files = directory.listFiles((dir, name) -> name.matches("session-[0-9]+-[0-9]+-[0-9]+"));
        if (files == null) return result;
        for (File file : files) {
            if (file.length() > 4096) continue;
            try (BufferedReader in = Files.newBufferedReader(file.toPath(), StandardCharsets.UTF_8)) {
                if (!"1".equals(in.readLine())) continue;
                int pid = Integer.parseInt(in.readLine());
                long ticks = Long.parseLong(in.readLine()), start = Long.parseLong(in.readLine());
                String log = in.readLine(), env = in.readLine();
                if (pid <= 0 || ticks <= 0 || start <= 0 || log == null || env == null
                        || !new File(log).isAbsolute() || !new File(env).isAbsolute() || in.readLine() != null) continue;
                if (!file.getName().equals("session-" + start + "-" + pid + "-" + ticks)) continue;
                result.add(new Receipt(file, new ExitNoticePolicy.Session(pid, ticks, start), log, env));
            } catch (NumberFormatException | java.nio.file.NoSuchFileException invalid) { /* A racing startup/prune is not an exit. */ }
        }
        result.sort(Comparator.comparingLong(receipt -> receipt.session.startedMs));
        return result;
    }
    /** Commit consumption before showing a window, so two app processes cannot both show this exit.
     * A failed persistence operation must not show an untracked notice. */
    public boolean claim(Receipt receipt) throws IOException { return claim(directory, receipt); }
    private static synchronized boolean claim(File directory, Receipt receipt) throws IOException {
        directory.mkdirs();
        File ledger = new File(directory, "handled");
        // File locks cover :setup vs game; the class monitor covers stores in the same process.
        try (FileChannel channel = FileChannel.open(new File(directory, "lock").toPath(),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE); FileLock lock = channel.lock()) {
            List<String> keys = ledger.isFile() ? Files.readAllLines(ledger.toPath(), StandardCharsets.UTF_8) : new ArrayList<>();
            String key = receipt.session.key();
            if (keys.contains(key)) return false;
            // A second process may have retired the newest normal/silenced session
            // before this one listed files. Never resurrect an older interruption.
            for (String handled : keys) {
                try {
                    if (Long.parseLong(handled.substring(handled.lastIndexOf(':') + 1)) > receipt.session.startedMs) return false;
                } catch (NumberFormatException invalid) { /* Ignore an invalid ledger entry. */ }
            }
            keys.add(key);
            while (keys.size() > 32) keys.remove(0);
            File temporary = new File(directory, "handled.tmp");
            try (FileOutputStream out = new FileOutputStream(temporary)) {
                out.write((String.join("\n", keys) + "\n").getBytes(StandardCharsets.UTF_8));
                out.getFD().sync();
            }
            Files.move(temporary.toPath(), ledger.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            return true;
        }
    }
    public void retire(Receipt receipt) {
        receipt.file.delete();
        new File(receipt.file + ".closed").delete();
        new File(receipt.file + ".background").delete();
    }
    /** Same last-assignment, whitespace/comment/quote rules as totk.env; only exact 0 silences. */
    public static boolean enabled(File env) {
        String value = null;
        if (!env.isFile() || env.length() > 65536) return true;
        try (BufferedReader in = Files.newBufferedReader(env.toPath(), StandardCharsets.UTF_8)) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.replaceFirst("^[ \t]+", "").replaceFirst("[ \r]+$", "");
                if (line.startsWith("export ")) line = line.substring(7);
                if (line.startsWith("#")) continue;
                int equals = line.indexOf('=');
                if (equals < 0 || !"TOTK_EXIT_NOTICE".equals(line.substring(0, equals))) continue;
                value = line.substring(equals + 1);
                if (value.length() >= 2 && (value.charAt(0) == '\'' || value.charAt(0) == '"')
                        && value.charAt(value.length() - 1) == value.charAt(0)) value = value.substring(1, value.length() - 1);
            }
        } catch (IOException | SecurityException unavailable) { return true; }
        return !"0".equals(value);
    }
}
