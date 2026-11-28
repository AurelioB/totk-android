package com.aure.totk.setup;

import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Host driver for tests/test_android_saves.py: one command per stdin line (arguments separated by tabs), one answer
 * line per command. "-" stands for "none".
 *    select DIR                      SaveManager.select over DIR's files (symlinks flagged): root/files/total or invalid CODE
 *    zip ZIP STAGING                 SaveManager.extractZip: root/files or invalid CODE
 *    folder DIR STAGING              SaveManager.extractFolder: root/files or invalid CODE
 *    entries COUNT                   select over COUNT synthetic candidates: invalid CODE
 *    export ROOT ZIP                 manifest + writeZip: number of files
 *    manifest FILE                   bounded manifest reader used by save-sync.json: number of files
 *    manifest-heap FILE              bounded reader with 1 ms heap sampling: result, peak_heap and elapsed_ms
 *    property COUNT BYTES KIND FILE EXPECTED
 *                                    select/stream/read logical zero-size files with aggregate UTF-8 path BYTES
 *    property-heap ...               the same admission property with 1 ms heap sampling
 *    utf8                            deterministic UTF-8 length and PathBudget properties, including lone surrogates
 *    path-reader COUNT BYTES KIND FILE
 *                                    independently stream an over-budget fixture and require reader path_bytes
 *    decide LIVE FOLDER LAST         SaveManager.decide over three directories (each scanned; "-" = none)
 *    backup ROOT BACKUPS REASON T    SaveManager.backup: the backup folder name or none
 *    backups BACKUPS                 SaveManager.backups: name:reason:slots ... newest first
 *    replace FILES STAGED REASON T   SaveManager.replace: ok BACKUPNAME|none, or invalid CODE
 *    root FILES                      SaveManager.saveRoot (creates the identity when missing): the folder name
 *    jpeg FILE                       SaveManager.captionJpeg: offset length, or none
 *    captime FILE ZONE               SaveManager.captionTime in time zone ZONE: yyyy-MM-ddTHH:mm:ss, or none
 *    slots ROOT ZONE                 SaveManager.slots: index:time ... (time as above, in ZONE as the default zone)
 *    push TOP SNAPSHOT FAILAFTER     SaveManager.readFolder + push on a plain-directory store without renames; FAILAFTER
 *                                    (-1 = never) makes that write store half of its bytes and fail, like a killed upload
 *    read TOP                        SaveManager.readFolder: gen NAME FILES | incomplete PROBLEM | none
 *    fetch TOP STAGING               SaveManager.fetch with verification: ok FILES or invalid CODE
 *    stable ROOT TARGET BUSY GEN     stableSnapshot with a scripted signal: BUSY and GEN are comma lists of the values
 *                                    returned by successive busy()/generation() calls: ok FILES or unstable
 *    locked FILES                    SaveManager.gameLocked */
public final class SavesProbe {
    private static final class HeapSample implements AutoCloseable {
        final Runtime runtime = Runtime.getRuntime();
        volatile boolean running = true;
        volatile long peak;
        final Thread thread;
        HeapSample() {
            thread = new Thread(() -> {
                while (running) {
                    sample();
                    try { Thread.sleep(1); } catch (InterruptedException stop) { break; }
                }
            });
            sample(); thread.start();
        }
        void sample() { peak = Math.max(peak, runtime.totalMemory() - runtime.freeMemory()); }
        @Override public void close() throws InterruptedException { running = false; thread.join(); sample(); }
    }
    private static List<SaveManager.Candidate> listing(File dir) throws IOException {
        List<SaveManager.Candidate> out = new ArrayList<>();
        walk(dir, "", out);
        return out;
    }
    private static void walk(File dir, String prefix, List<SaveManager.Candidate> out) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) return;
        Arrays.sort(children);
        for (File child : children) {
            boolean link = Files.isSymbolicLink(child.toPath());
            if (child.isDirectory() && !link) walk(child, prefix + child.getName() + "/", out);
            else out.add(new SaveManager.Candidate(prefix + child.getName(), link ? 0 : child.length(), link, child.lastModified()));
        }
    }
    private static SaveManager.Manifest manifest(String dir) throws IOException {
        return dir.equals("-") ? null : SaveManager.manifest(new File(dir), null, "probe", "host", 0);
    }
    /** A save folder on a plain directory, with no rename at all (like a provider without FLAG_SUPPORTS_RENAME). */
    private static final class DirStore implements SaveManager.Store {
        final File top; int failAfter;
        DirStore(File top, int failAfter) { this.top = top; this.failAfter = failAfter; }
        @Override public List<String> dirs() {
            List<String> out = new ArrayList<>();
            File[] children = top.listFiles();
            if (children != null) for (File f : children) if (f.isDirectory()) out.add(f.getName());
            return out;
        }
        @Override public Map<String, long[]> files(String dir) throws IOException {
            Map<String, long[]> out = new HashMap<>();
            for (SaveManager.Candidate c : listing(new File(top, dir))) out.put(c.path, new long[]{c.size, c.mtime});
            return out;
        }
        @Override public InputStream open(String dir, String path) throws IOException { return new FileInputStream(new File(new File(top, dir), path)); }
        @Override public void createDir(String dir) throws IOException { if (!new File(top, dir).mkdirs()) throw new IOException("exists " + dir); }
        @Override public void write(String dir, String path, File source) throws IOException {
            File target = new File(new File(top, dir), path);
            target.getParentFile().mkdirs();
            if (target.exists()) throw new IOException("exists " + path);
            byte[] bytes = Files.readAllBytes(source.toPath());
            if (failAfter == 0) {
                try (OutputStream out = new FileOutputStream(target)) { out.write(bytes, 0, bytes.length / 2); }
                throw new IOException("killed during " + path);
            }
            if (failAfter > 0) failAfter--;
            try (OutputStream out = new FileOutputStream(target)) { out.write(bytes); }
        }
        @Override public void deleteDir(String dir) { SaveManager.delete(new File(top, dir)); }
    }
    private static Deque<String> sequence(String list) { return new ArrayDeque<>(Arrays.asList(list.split(","))); }
    private static String stamp(long millis, String zone) {
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", java.util.Locale.ROOT);
        f.setTimeZone(java.util.TimeZone.getTimeZone(zone));
        return f.format(new java.util.Date(millis));
    }

    private static final String EMPTY_SHA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    private interface Checked { void run() throws IOException; }
    private static void require(boolean condition, String problem) {
        if (!condition) throw new AssertionError(problem);
    }
    private static SaveManager.Invalid refused(String code, Checked action) throws IOException {
        try { action.run(); }
        catch (SaveManager.Invalid invalid) {
            require(invalid.code.equals(code), "expected " + code + ", got " + invalid.getMessage());
            return invalid;
        }
        throw new AssertionError("expected " + code + " before any output");
    }
    private static final class CountOutput extends OutputStream {
        long bytes;
        @Override public void write(int value) { bytes++; }
        @Override public void write(byte[] value, int offset, int length) { bytes += length; }
    }
    /** A rejected manifest must not even begin a mirror generation or touch its source files. */
    private static final class UntouchedStore implements SaveManager.Store {
        int mutations;
        @Override public List<String> dirs() { return java.util.Collections.emptyList(); }
        @Override public Map<String, long[]> files(String dir) { return java.util.Collections.emptyMap(); }
        @Override public InputStream open(String dir, String path) { throw new AssertionError("opened rejected save"); }
        private void mutated() { mutations++; throw new AssertionError("mutated store before manifest preflight"); }
        @Override public void createDir(String dir) { mutated(); }
        @Override public void write(String dir, String path, File source) { mutated(); }
        @Override public void deleteDir(String dir) { mutated(); }
    }
    /** Exact encoded byte length, with components at most 121 UTF-8 bytes including the unique filename. */
    private static String imagePath(int index, int bytes, String unit, int unitBytes) {
        String suffix = Integer.toString(100_000 + index).substring(1) + ".jpg";
        StringBuilder out = new StringBuilder("album/");
        int remaining = bytes - out.length() - suffix.length(), component = 0;
        require(remaining >= 0, "path byte target is shorter than its filename");
        while (remaining > 0) {
            if (component + Math.min(unitBytes, remaining) > 112) {
                out.append('/'); remaining--; component = 0;
            } else if (remaining >= unitBytes) {
                out.append(unit); remaining -= unitBytes; component += unitBytes;
            } else {
                out.append('x'); remaining--; component++;
            }
        }
        return out.append(suffix).toString();
    }
    private static void selectionMatches(List<SaveManager.Candidate> all, SaveManager.Manifest manifest) throws IOException {
        SaveManager.Selection selected = SaveManager.select(all);
        require(selected.root.isEmpty() && selected.total == 0 && selected.files.size() == manifest.files.size(),
                "selection differs from logical manifest");
        for (int i = 0; i < selected.files.size(); i++)
            require(selected.files.get(i).path.equals(manifest.files.get(i).path), "selection changed a clean path");
    }
    private static String pathProperty(int count, long target, String kind, File output, String expected) throws IOException {
        require(count > 1, "property needs option.sav and at least one image");
        String unit;
        switch (kind) {
            case "ascii": unit = "p"; break;
            case "escape": unit = "\u0001"; break;
            case "two": unit = "\u00e9"; break;
            case "three": unit = "\u4e2d"; break;
            case "four": unit = "\ud83d\ude00"; break;
            case "mixed": unit = "p".repeat(100) + "\u0100"; break;
            default: throw new AssertionError("unknown path property " + kind);
        }
        int unitBytes = unit.getBytes(StandardCharsets.UTF_8).length;
        List<SaveManager.Candidate> all = new ArrayList<>(count);
        SaveManager.Manifest manifest = new SaveManager.Manifest();
        // Exercise bounded non-path metadata and signed-long entry fields as well as the aggregate path maximum.
        manifest.appVersion = "\u0002".repeat(SaveManager.MAX_MANIFEST_STRING);
        manifest.device = "\u0003".repeat(SaveManager.MAX_MANIFEST_STRING);
        manifest.folder = "content://" + "f".repeat(SaveManager.MAX_MANIFEST_STRING - 10);
        long remaining = target - "option.sav".length();
        int each = (int) (remaining / (count - 1)), extra = (int) (remaining % (count - 1));
        long bytes = 0, observed = 0;
        String firstLimit = null;
        for (int i = 0; i < count; i++) {
            int length = i == 0 ? 10 : each + (i <= extra ? 1 : 0);
            String path = i == 0 ? "option.sav" : imagePath(i, length, unit, unitBytes);
            require(SaveManager.utf8Length(path) == length, "generator missed encoded path target");
            bytes += length;
            if (firstLimit == null && i + 1 > SaveManager.MAX_FILES) { firstLimit = "too_many"; observed = i + 1; }
            if (firstLimit == null && bytes > SaveManager.MAX_PATH_BYTES) { firstLimit = "path_bytes"; observed = bytes; }
            all.add(new SaveManager.Candidate(path, 0, false, 0));
            manifest.files.add(new SaveManager.FileEntry(path, 0, EMPTY_SHA, i % 2 == 0 ? Long.MIN_VALUE : Long.MAX_VALUE));
        }
        require(bytes == target, "generator missed aggregate path target");
        if (!expected.equals("ok")) {
            require(expected.equals(firstLimit), "property expected the wrong first admission limit");
            SaveManager.Invalid selection = refused(expected, () -> SaveManager.select(all));
            long limit = expected.equals("path_bytes") ? SaveManager.MAX_PATH_BYTES : SaveManager.MAX_FILES;
            require(selection.getMessage().contains(Long.toString(observed))
                    && selection.getMessage().contains(Long.toString(limit)), "missing observed/limit diagnostic");
            refused(expected, manifest::validate);
            CountOutput stream = new CountOutput(), zip = new CountOutput();
            refused(expected, () -> manifest.write(stream));
            require(stream.bytes == 0, "manifest wrote bytes before rejection");
            refused(expected, () -> manifest.writeAtomic(output));
            require(!output.exists() && output.getParentFile().list().length == 0, "atomic writer left output");
            File missing = new File(output.getParentFile(), "no-snapshot");
            refused(expected, () -> SaveManager.writeZip(missing, manifest, zip));
            require(zip.bytes == 0, "zip wrote bytes before rejection");
            UntouchedStore store = new UntouchedStore();
            SaveManager.FolderState current = SaveManager.readFolder(store);
            refused(expected, () -> SaveManager.push(store, missing, manifest, current));
            require(store.mutations == 0, "push began a rejected generation");
            return "invalid " + expected + " files=" + count + " path_bytes=" + bytes
                    + " observed=" + observed + " limit=" + limit + " preflight=clean output_bytes=0 mutations=0";
        }
        require(firstLimit == null, "admitted property exceeded an admission limit");
        selectionMatches(all, manifest);
        manifest.validate();
        try (OutputStream out = new FileOutputStream(output)) { manifest.write(out); }
        require(output.length() <= SaveManager.MAX_MANIFEST, "admitted manifest exceeded its derived byte budget");
        SaveManager.Manifest decoded = SaveManager.Manifest.read(output);
        require(manifest.sameFiles(decoded), "streaming manifest changed save content");
        require(decoded.format == 1 && decoded.appVersion.equals(manifest.appVersion)
                && decoded.device.equals(manifest.device) && decoded.folder.equals(manifest.folder),
                "streaming manifest changed format or metadata");
        for (int i = 0; i < manifest.files.size(); i++)
            require(manifest.files.get(i).mtime == decoded.files.get(i).mtime, "streaming manifest changed a signed long");
        return "ok files=" + count + " path_bytes=" + bytes + " manifest_bytes=" + output.length()
                + " max_manifest=" + SaveManager.MAX_MANIFEST + " same=true format=1";
    }
    private static String utf8Properties() throws IOException {
        String[] edges = {"", "ascii", "\u007f\u0080\u07ff\u0800\uffff", "\ud83d\ude00", "\ud800", "\udc00",
                "\ud800\ud800\udc00", "\udc00\ud800", "x\ud800y\udc00z"};
        SaveManager.PathBudget budget = new SaveManager.PathBudget();
        long bytes = 0;
        int cases = 0;
        for (String value : edges) {
            int encoded = value.getBytes(StandardCharsets.UTF_8).length;
            require(SaveManager.utf8Length(value) == encoded, "UTF-8 edge length differs from Java encoder");
            budget.add(value); bytes += encoded; cases++;
        }
        java.util.Random random = new java.util.Random(19);
        for (int i = 0; i < 1024; i++) {
            char[] chars = new char[i % 129];
            for (int j = 0; j < chars.length; j++) chars[j] = (char) random.nextInt(1 << 16);
            String value = new String(chars);
            int encoded = value.getBytes(StandardCharsets.UTF_8).length;
            require(SaveManager.utf8Length(value) == encoded, "UTF-8 generated length differs from Java encoder");
            budget.add(value); bytes += encoded; cases++;
        }
        require(budget.files == cases && budget.bytes == bytes, "PathBudget differs from actual Java UTF-8 encoding");
        return "ok cases=" + cases + " files=" + budget.files + " path_bytes=" + budget.bytes;
    }
    private static void rawQuoted(java.io.Writer out, String value) throws IOException {
        out.write('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\') { out.write('\\'); out.write(c); }
            else if (c < 0x20) {
                out.write("\\u00");
                out.write("0123456789abcdef".charAt(c >>> 4));
                out.write("0123456789abcdef".charAt(c & 15));
            } else out.write(c);
        }
        out.write('"');
    }
    /** Independent malformed fixture writer: production serialization must refuse these same paths. */
    private static String overBudgetReader(int count, long target, String kind, File file) throws IOException {
        String unit;
        switch (kind) {
            case "ascii": unit = "p"; break;
            case "escape": unit = "\u0001"; break;
            case "two": unit = "\u00e9"; break;
            case "three": unit = "\u4e2d"; break;
            case "four": unit = "\ud83d\ude00"; break;
            case "mixed": unit = "p".repeat(100) + "\u0100"; break;
            default: throw new AssertionError("unknown reader property " + kind);
        }
        int unitBytes = unit.getBytes(StandardCharsets.UTF_8).length;
        long remaining = target - 10;
        int each = (int) (remaining / (count - 1)), extra = (int) (remaining % (count - 1));
        try {
            try (java.io.Writer out = new java.io.BufferedWriter(new java.io.OutputStreamWriter(
                    new FileOutputStream(file), StandardCharsets.UTF_8))) {
                out.write("{\"format\":1,\"files\":[");
                for (int i = 0; i < count; i++) {
                    out.write(i == 0 ? "{\"path\":" : ",{\"path\":");
                    rawQuoted(out, i == 0 ? "option.sav" : imagePath(i, each + (i <= extra ? 1 : 0), unit, unitBytes));
                    out.write(",\"size\":0,\"sha256\":\"" + EMPTY_SHA + "\",\"mtime\":0}");
                }
                out.write("]}");
            }
            String code = count > SaveManager.MAX_FILES ? "too_many" : "path_bytes";
            refused(code, () -> SaveManager.Manifest.read(file));
            File top = new File(file.getParentFile(), "budget-mirror"), generation = new File(top, "sync-000001");
            if (!generation.mkdirs()) throw new AssertionError("mirror fixture directories");
            try {
                Files.move(file.toPath(), new File(generation, SaveManager.MANIFEST).toPath());
                refused(code, () -> SaveManager.readFolder(new SaveManager.FileStore(top)));
            } finally { SaveManager.delete(top); }
            return "invalid " + code + " reader=" + code + " path_bytes=" + target;
        } finally { Files.deleteIfExists(file.toPath()); }
    }

    public static void main(String[] args) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        for (String line; (line = in.readLine()) != null;) {
            String[] w = line.split("\t");
            String answer;
            try {
                switch (w[0]) {
                    case "select": {
                        SaveManager.Selection s = SaveManager.select(SaveManager.candidates(new File(w[1])));
                        answer = "root=" + s.root + " files=" + s.files.size() + " total=" + s.total + " manifest=" + (s.manifestSource != null);
                        break;
                    }
                    case "zip": {
                        SaveManager.Selection s = SaveManager.extractZip(new File(w[1]), new File(w[2]));
                        answer = "root=" + s.root + " files=" + s.files.size();
                        break;
                    }
                    case "folder-detail":
                    case "folder": {
                        SaveManager.Selection s = SaveManager.extractFolder(new File(w[1]), new File(w[2]));
                        answer = "root=" + s.root + " files=" + s.files.size();
                        break;
                    }
                    case "entries": {
                        List<SaveManager.Candidate> all = new ArrayList<>();
                        for (int n = 0; n < Integer.parseInt(w[1]); n++)
                            all.add(new SaveManager.Candidate(n == 0 ? "option.sav" : "outside/" + n, 0, false, 0));
                        SaveManager.select(all);
                        answer = "accepted";
                        break;
                    }
                    case "export": {
                        SaveManager.Manifest m = SaveManager.manifest(new File(w[1]), null, "probe", "host", 1_700_000_000_000L);
                        try (OutputStream out = new FileOutputStream(w[2])) { SaveManager.writeZip(new File(w[1]), m, out); }
                        answer = String.valueOf(m.files.size());
                        break;
                    }
                    case "manifest": answer = String.valueOf(SaveManager.Manifest.read(new File(w[1])).files.size()); break;
                    case "manifest-heap": {
                        HeapSample heap = new HeapSample();
                        long start = System.nanoTime();
                        try { answer = "ok " + SaveManager.Manifest.read(new File(w[1])).files.size(); }
                        catch (SaveManager.Invalid invalid) { answer = "invalid " + invalid.code; }
                        finally { heap.close(); }
                        answer += " peak_heap=" + heap.peak + " elapsed_ms=" + (System.nanoTime() - start) / 1_000_000;
                        break;
                    }
                    case "property":
                    case "property-heap": {
                        HeapSample heap = w[0].equals("property-heap") ? new HeapSample() : null;
                        long start = System.nanoTime();
                        try {
                            answer = pathProperty(Integer.parseInt(w[1]), Long.parseLong(w[2]), w[3], new File(w[4]), w[5]);
                        } finally { if (heap != null) heap.close(); }
                        if (heap != null) answer += " peak_heap=" + heap.peak + " elapsed_ms=" + (System.nanoTime() - start) / 1_000_000;
                        break;
                    }
                    case "utf8": answer = utf8Properties(); break;
                    case "path-reader":
                        answer = overBudgetReader(Integer.parseInt(w[1]), Long.parseLong(w[2]), w[3], new File(w[4]));
                        break;
                    case "decide": answer = SaveManager.decide(manifest(w[1]), manifest(w[2]), manifest(w[3])).name(); break;
                    case "backup": {
                        SaveManager.Backup b = SaveManager.backup(new File(w[1]), new File(w[2]), w[3], Long.parseLong(w[4]));
                        answer = b == null ? "none" : b.dir.getName();
                        break;
                    }
                    case "backups": {
                        StringBuilder out = new StringBuilder();
                        for (SaveManager.Backup b : SaveManager.backups(new File(w[1])))
                            out.append(out.length() == 0 ? "" : " ").append(b.dir.getName()).append(':').append(b.reason).append(':').append(b.slots);
                        answer = out.toString();
                        break;
                    }
                    case "replace": {
                        SaveManager.Backup b = SaveManager.replace(new File(w[1]), new File(w[2]), w[3], Long.parseLong(w[4]));
                        answer = "ok " + (b == null ? "none" : b.dir.getName());
                        break;
                    }
                    case "root": answer = SaveManager.saveRoot(new File(w[1])).getName(); break;
                    case "jpeg": {
                        int[] j = SaveManager.captionJpeg(Files.readAllBytes(new File(w[1]).toPath()));
                        answer = j == null ? "none" : j[0] + " " + j[1];
                        break;
                    }
                    case "captime": {
                        long t = SaveManager.captionTime(new File(w[1]), java.util.TimeZone.getTimeZone(w[2]));
                        answer = t == 0 ? "none" : stamp(t, w[2]);
                        break;
                    }
                    case "slots": {
                        java.util.TimeZone.setDefault(java.util.TimeZone.getTimeZone(w[2]));
                        StringBuilder out = new StringBuilder();
                        for (SaveManager.Slot s : SaveManager.slots(new File(w[1])))
                            out.append(out.length() == 0 ? "" : " ").append(s.index).append(':').append(stamp(s.time, w[2]));
                        answer = out.toString();
                        break;
                    }
                    case "push": {
                        DirStore store = new DirStore(new File(w[1]), Integer.parseInt(w[3]));
                        SaveManager.FolderState state = SaveManager.readFolder(store);
                        SaveManager.Manifest m = SaveManager.manifest(new File(w[2]), null, "probe", "host", 0);
                        try { answer = SaveManager.push(store, new File(w[2]), m, state); }
                        catch (IOException killed) { answer = "killed"; }
                        break;
                    }
                    case "read": {
                        SaveManager.FolderState s = SaveManager.readFolder(new DirStore(new File(w[1]), -1));
                        answer = s.incomplete ? "incomplete " + s.problem : s.manifest == null ? "none" : "gen " + s.generation + " " + s.manifest.files.size();
                        break;
                    }
                    case "fetch": {
                        DirStore store = new DirStore(new File(w[1]), -1);
                        SaveManager.FolderState s = SaveManager.readFolder(store);
                        SaveManager.fetch(store, s, new File(w[2]), true);
                        answer = "ok " + s.manifest.files.size();
                        break;
                    }
                    case "stable": {
                        Deque<String> busy = sequence(w[3]), gen = sequence(w[4]);
                        SaveManager.Signal signal = new SaveManager.Signal() {
                            @Override public String generation() { return gen.size() > 1 ? gen.poll() : gen.peek(); }
                            @Override public boolean busy() { return Boolean.parseBoolean(busy.size() > 1 ? busy.poll() : busy.peek()); }
                        };
                        SaveManager.Manifest m = SaveManager.stableSnapshot(new File(w[1]), new File(w[2]), signal, "probe", "host", 0);
                        answer = m == null ? "unstable" + (new File(w[2]).exists() ? " leftover" : "") : "ok " + m.files.size();
                        break;
                    }
                    case "game-state": answer = GameLock.check(new File(w[1]), false).state.name(); break;
                    default: answer = "unknown command " + w[0];
                }
            } catch (SaveManager.Invalid invalid) {
                answer = "invalid " + invalid.code + (w[0].endsWith("-detail") ? " " + invalid.getMessage() : "");
            }
            System.out.println(answer);
        }
    }
}
