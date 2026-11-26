package com.aure.totk;

import java.io.BufferedReader;
import java.io.DataInputStream;
import java.io.DataOutputStream;
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
import java.util.LinkedHashSet;
import java.util.List;
import java.util.function.Predicate;

/** Worker-only, cross-process persistence. No Android dependencies or wall-clock ordering. */
public final class ExitNoticeStore {
    private static final int MAGIC = 0x54454e32, MAX_SESSIONS = 16, MAX_HANDLED = 64;
    public static final class Receipt {
        public final ExitNoticePolicy.Session session;
        public final String logPath, envPath;
        public boolean closed;
        public long backgroundElapsedMs;
        public String committedGeneration = "unavailable";
        Receipt(ExitNoticePolicy.Session session, String logPath, String envPath) {
            this.session = session; this.logPath = logPath; this.envPath = envPath;
        }
    }
    public static final class Owner {
        public final String boot;
        public final int pid;
        public final long ticks;
        public Owner(String boot, int pid, long ticks) { this.boot = boot; this.pid = pid; this.ticks = ticks; }
    }
    public static final class Claim {
        public final long sequence;
        public final String identity, request;
        public final Owner owner;
        Claim(long sequence, String identity, String request, Owner owner) {
            this.sequence = sequence; this.identity = identity; this.request = request; this.owner = owner;
        }
    }
    public enum Status { READY, BUSY, GONE }
    public static final class Reservation {
        public final Status status;
        public final Claim claim;
        Reservation(Status status, Claim claim) { this.status = status; this.claim = claim; }
    }
    private static final class State {
        long nextSequence, retiredThrough;
        final List<Receipt> receipts = new ArrayList<>();
        final LinkedHashSet<String> handled = new LinkedHashSet<>();
        Claim pending;
        boolean changed;
    }
    private interface Operation<T> { T run(State state) throws IOException; }
    private final File directory;
    public ExitNoticeStore(File files) { directory = new File(files, "state/exit-notice"); }

    public Receipt begin(String token, int pid, long ticks, String boot, String install, String log, String env) throws IOException {
        return locked(state -> {
            if (state.nextSequence == Long.MAX_VALUE) throw new IOException("session sequence exhausted");
            Receipt receipt = new Receipt(new ExitNoticePolicy.Session(++state.nextSequence, token, pid, ticks, boot, install), log, env);
            state.receipts.add(receipt);
            while (state.receipts.size() > MAX_SESSIONS) state.receipts.remove(0);
            state.changed = true;
            return receipt;
        });
    }
    public List<Receipt> receipts() throws IOException { return locked(state -> new ArrayList<>(state.receipts)); }
    public void background(String token, long elapsedMs, String generation) throws IOException {
        locked(state -> {
            for (Receipt receipt : state.receipts) if (receipt.session.token.equals(token)) {
                receipt.backgroundElapsedMs = elapsedMs; receipt.committedGeneration = generation; state.changed = true;
            }
            return null;
        });
    }
    public void close(String token) throws IOException {
        locked(state -> {
            for (Receipt receipt : state.receipts) if (receipt.session.token.equals(token)) { receipt.closed = true; state.changed = true; }
            return null;
        });
    }
    public boolean consume(Receipt receipt, ExitNoticePolicy.Exit exit) throws IOException {
        return locked(state -> consume(state, receipt.session.sequence, exit == null ? null : exit.identity()));
    }
    public Reservation reserve(Receipt receipt, ExitNoticePolicy.Exit exit, String request, Owner owner,
            Predicate<Owner> ownerAlive) throws IOException {
        return locked(state -> {
            String identity = exit.identity();
            if (!present(state, receipt.session.sequence) || state.handled.contains(identity)) return new Reservation(Status.GONE, null);
            if (state.pending != null) {
                if (state.pending.request.equals(request) && state.pending.identity.equals(identity))
                    return new Reservation(Status.READY, state.pending);
                if (state.pending.sequence >= receipt.session.sequence && ownerAlive.test(state.pending.owner))
                    return new Reservation(Status.BUSY, null);
                // Delivery may have happened before its owner died/rebooted. Prefer no repeat in this ambiguous window.
                consume(state, state.pending.sequence, state.pending.identity);
                state.pending = null;
                if (!present(state, receipt.session.sequence)) return new Reservation(Status.GONE, null);
            }
            state.pending = new Claim(receipt.session.sequence, identity, request, owner);
            state.changed = true;
            return new Reservation(Status.READY, state.pending);
        });
    }
    public void release(Claim claim) throws IOException {
        locked(state -> {
            if (sameClaim(state.pending, claim)) { state.pending = null; state.changed = true; }
            return null;
        });
    }
    public boolean shown(Claim claim) throws IOException {
        return locked(state -> {
            if (!sameClaim(state.pending, claim)) return false;
            boolean consumed = consume(state, claim.sequence, claim.identity);
            state.pending = null; state.changed = true;
            return consumed;
        });
    }
    private static boolean sameClaim(Claim a, Claim b) { return a != null && b != null && a.request.equals(b.request) && a.identity.equals(b.identity); }
    private static boolean present(State state, long sequence) {
        if (sequence <= state.retiredThrough) return false;
        for (Receipt receipt : state.receipts) if (receipt.session.sequence == sequence) return true;
        return false;
    }
    private static boolean consume(State state, long sequence, String identity) {
        if (!present(state, sequence)) return false;
        boolean first = identity == null || state.handled.add(identity);
        state.retiredThrough = Math.max(state.retiredThrough, sequence);
        state.receipts.removeIf(receipt -> receipt.session.sequence <= state.retiredThrough);
        while (state.handled.size() > MAX_HANDLED) state.handled.remove(state.handled.iterator().next());
        if (state.pending != null && state.pending.sequence <= state.retiredThrough) state.pending = null;
        state.changed = true;
        return first;
    }
    private <T> T locked(Operation<T> operation) throws IOException { return locked(directory, operation); }
    private static synchronized <T> T locked(File directory, Operation<T> operation) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("cannot create exit-notice directory");
        try (FileChannel channel = FileChannel.open(new File(directory, "lock").toPath(), StandardOpenOption.CREATE, StandardOpenOption.WRITE);
                FileLock lock = channel.lock()) {
            File file = new File(directory, "state-v2");
            State state = read(file);
            T result = operation.run(state);
            if (state.changed) {
                File temporary = new File(directory, "state-v2.tmp");
                try (FileOutputStream out = new FileOutputStream(temporary); DataOutputStream data = new DataOutputStream(out)) {
                    write(data, state); data.flush(); out.getFD().sync();
                }
                Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                // R1 receipts have no platform summary token and cannot safely classify an R2 session.
                File[] legacy = directory.listFiles((dir, name) -> name.matches("session-[0-9]+-[0-9]+-[0-9]+(\\.closed|\\.background)?")
                        || name.equals("handled") || name.equals("handled.tmp"));
                if (legacy != null) for (File old : legacy) old.delete();
            }
            return result;
        }
    }
    private static State read(File file) throws IOException {
        State state = new State();
        if (!file.isFile() || file.length() > 262144) return state;
        try (DataInputStream in = new DataInputStream(Files.newInputStream(file.toPath()))) {
            if (in.readInt() != MAGIC) return new State();
            state.nextSequence = in.readLong(); state.retiredThrough = in.readLong();
            int count = count(in, MAX_SESSIONS);
            for (int i = 0; i < count; i++) {
                ExitNoticePolicy.Session session = new ExitNoticePolicy.Session(in.readLong(), in.readUTF(), in.readInt(), in.readLong(), in.readUTF(), in.readUTF());
                Receipt receipt = new Receipt(session, in.readUTF(), in.readUTF());
                receipt.closed = in.readBoolean(); receipt.backgroundElapsedMs = in.readLong(); receipt.committedGeneration = in.readUTF();
                state.receipts.add(receipt);
            }
            count = count(in, MAX_HANDLED);
            for (int i = 0; i < count; i++) state.handled.add(in.readUTF());
            if (in.readBoolean()) {
                long sequence = in.readLong(); String identity = in.readUTF(), request = in.readUTF();
                state.pending = new Claim(sequence, identity, request, new Owner(in.readUTF(), in.readInt(), in.readLong()));
            }
            if (in.read() != -1 || state.nextSequence < 0 || state.retiredThrough < 0 || state.retiredThrough > state.nextSequence)
                return new State();
            return state;
        } catch (IOException | RuntimeException invalid) {
            // Cleared/corrupt state has no authenticated session receipts. History alone must not replay a warning.
            return new State();
        }
    }
    private static int count(DataInputStream in, int max) throws IOException {
        int count = in.readInt(); if (count < 0 || count > max) throw new IOException("invalid receipt count"); return count;
    }
    private static void write(DataOutputStream out, State state) throws IOException {
        out.writeInt(MAGIC); out.writeLong(state.nextSequence); out.writeLong(state.retiredThrough); out.writeInt(state.receipts.size());
        for (Receipt receipt : state.receipts) {
            ExitNoticePolicy.Session session = receipt.session;
            out.writeLong(session.sequence); out.writeUTF(session.token); out.writeInt(session.pid); out.writeLong(session.startTicks);
            out.writeUTF(session.boot); out.writeUTF(session.install); out.writeUTF(receipt.logPath); out.writeUTF(receipt.envPath);
            out.writeBoolean(receipt.closed); out.writeLong(receipt.backgroundElapsedMs); out.writeUTF(receipt.committedGeneration);
        }
        out.writeInt(state.handled.size()); for (String key : state.handled) out.writeUTF(key);
        out.writeBoolean(state.pending != null);
        if (state.pending != null) {
            Claim claim = state.pending;
            out.writeLong(claim.sequence); out.writeUTF(claim.identity); out.writeUTF(claim.request);
            out.writeUTF(claim.owner.boot); out.writeInt(claim.owner.pid); out.writeLong(claim.owner.ticks);
        }
    }
    /** Same last-assignment/export/quote rules as totk.env; only exact 0 silences. Worker-only. */
    public static boolean enabled(File env) {
        String value = null;
        if (!env.isFile() || env.length() > 65536) return true;
        try (BufferedReader in = Files.newBufferedReader(env.toPath(), StandardCharsets.UTF_8)) {
            String line;
            while ((line = in.readLine()) != null) {
                int first = 0, last = line.length();
                while (first < last && (line.charAt(first) == ' ' || line.charAt(first) == '\t')) first++;
                while (last > first && (line.charAt(last - 1) == ' ' || line.charAt(last - 1) == '\r')) last--;
                line = line.substring(first, last);
                if (line.startsWith("export ")) line = line.substring(7);
                if (!line.startsWith("TOTK_EXIT_NOTICE=")) continue;
                value = line.substring("TOTK_EXIT_NOTICE=".length());
                if (value.length() >= 2 && (value.charAt(0) == '\'' || value.charAt(0) == '"')
                        && value.charAt(value.length() - 1) == value.charAt(0)) value = value.substring(1, value.length() - 1);
            }
        } catch (IOException | SecurityException unavailable) { return true; }
        return !"0".equals(value);
    }
}
