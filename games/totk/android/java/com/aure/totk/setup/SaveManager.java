package com.aure.totk.setup;

import java.io.ByteArrayOutputStream;
import java.io.BufferedWriter;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Date;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/** The game's save set: the live save root (<private>/user-data/save/<uid>, runtime/fs_host.c), export and import as a
 * zip or folder, local backups, the atomic swap, and the mirror's sync decision. Plain Java without Android types so
 * tests/test_android_saves.py runs it on a host JVM; SaveMirror and SetupActivity are the Android glue. */
final class SaveManager {
    private SaveManager() { }
    static final String TITLE_ID = "0100F2C0115B6000";
    /** Top folder of an export zip and of the mirror folder; the manifest sits inside it next to the save files. */
    static final String TOP = "TotK Native saves", MANIFEST = "totk-native-saves.json";
    /** Six slots with 526 picture-book files each already exceed 3,000 files (#19); 526 is a report, not a capacity.
     * Leave room for fuller picture books and albums. Byte limits still apply to every picture. */
    static final int MAX_FILES = 16_384, KEEP_BACKUPS = 10;
    static final long MAX_TOTAL = 128L << 20;
    /** Game save files are a few MB (progress.sav ~2.3 MB); a large file with another name is not part of a save set. */
    static final long MAX_KNOWN_FILE = 32L << 20, MAX_OTHER_FILE = 1L << 20;
    /** Zip entries anywhere in the archive (outside the save root too): a cap against archives that are not save dumps. */
    static final int MAX_ENTRIES = 65_536;
    /** A set's relative names share 4 MiB of UTF-8, not a per-path character limit. JSON quoting expands each
     * path byte at most sixfold. A file record needs at most 146 other bytes; reserve 160, plus 64 KiB headers. */
    static final long MAX_PATH_BYTES = 4L << 20, MAX_MANIFEST_HEADER = 64L << 10;
    static final int MAX_MANIFEST_ENTRY = 160, MAX_MANIFEST_STRING = 1024;
    static final int MAX_MANIFEST_DEPTH = 16, MAX_MANIFEST_FIELDS = 32, MAX_MANIFEST_NODES = MAX_FILES * 6 + 32;
    static final long MAX_MANIFEST = 6 * MAX_PATH_BYTES + (long) MAX_FILES * MAX_MANIFEST_ENTRY + MAX_MANIFEST_HEADER;
    private static final Pattern SLOT_PROGRESS = Pattern.compile("(?:.*/)?slot_\\d\\d/progress\\.sav");
    private static final Pattern KNOWN = Pattern.compile("(?i).*\\.(sav|dat|jpg|jpeg|bin)");

    // ---------- errors ----------
    /** A save set that must not be imported. code: absolute | traversal | symlink | too_many | path_bytes | too_big |
     * huge_file | no_saves | corrupt | not_zip | game_running. */
    static final class Invalid extends IOException {
        final String code;
        Invalid(String code, String detail) { super(code + (detail == null ? "" : ": " + detail)); this.code = code; }
    }

    /** The same name/count admission for import selection, exported snapshots and parsed mirror/sync manifests. */
    static final class PathBudget {
        int files; long bytes;
        void add(String path) throws Invalid {
            if (++files > MAX_FILES) throw new Invalid("too_many", files + " files (limit " + MAX_FILES + ")");
            bytes += utf8Length(path);
            if (bytes > MAX_PATH_BYTES) throw new Invalid("path_bytes", bytes + " relative-path bytes (limit " + MAX_PATH_BYTES + ")");
        }
    }
    static long utf8Length(String text) {
        long bytes = 0;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c < 0x80) bytes++;
            else if (c < 0x800) bytes += 2;
            else if (Character.isHighSurrogate(c) && i + 1 < text.length() && Character.isLowSurrogate(text.charAt(i + 1))) { bytes += 4; i++; }
            else bytes += Character.isSurrogate(c) ? 1 : 3;
        }
        return bytes;
    }

    // ---------- manifest ----------
    static final class FileEntry {
        final String path; final long size; final String sha256; final long mtime;
        FileEntry(String path, long size, String sha256, long mtime) { this.path = path; this.size = size; this.sha256 = sha256; this.mtime = mtime; }
        boolean sameContent(FileEntry o) { return o != null && path.equals(o.path) && size == o.size && sha256 != null && sha256.equals(o.sha256); }
    }
    /** totk-native-saves.json {format:1,title_id,exported_at,app_version,device,files:[{path,size,sha256,mtime}]}; the
     * private save-sync.json uses the same shape plus folder (the mirror tree URI). */
    static final class Manifest {
        int format = 1;
        String titleId = TITLE_ID, exportedAt = "", appVersion = "", device = "", folder = "";
        final List<FileEntry> files = new ArrayList<>();

        Map<String, FileEntry> byPath() {
            Map<String, FileEntry> map = new HashMap<>();
            for (FileEntry e : files) map.put(e.path, e);
            return map;
        }
        /** Same save content: the same paths with the same sizes and digests (dates and devices do not count). */
        boolean sameFiles(Manifest o) {
            if (o == null || o.files.size() != files.size()) return false;
            Map<String, FileEntry> other = o.byPath();
            for (FileEntry e : files) if (!e.sameContent(other.get(e.path))) return false;
            return true;
        }
        long exportedMillis() { return parseTime(exportedAt); }
        void validate() throws Invalid {
            checkString(titleId); checkString(exportedAt); checkString(appVersion); checkString(device); checkString(folder);
            if (format != 1 || !titleId.isEmpty() && !TITLE_ID.equalsIgnoreCase(titleId)) throw new Invalid("corrupt", "manifest identity");
            PathBudget paths = new PathBudget();
            for (FileEntry e : files) {
                normalize(e.path);
                paths.add(e.path);
                if (!digestHex(e.sha256)) throw new Invalid("corrupt", "manifest digest of " + e.path);
            }
        }
        void write(OutputStream sink) throws IOException { validate(); writeValidated(sink); }
        private void writeValidated(OutputStream sink) throws IOException {
            Writer out = new BufferedWriter(new OutputStreamWriter(sink, StandardCharsets.UTF_8));
            out.write("{\"format\":1");
            field(out, "title_id", titleId); field(out, "exported_at", exportedAt); field(out, "app_version", appVersion);
            field(out, "device", device);
            if (!folder.isEmpty()) field(out, "folder", folder);
            out.write(",\"files\":[");
            for (int i = 0; i < files.size(); i++) {
                FileEntry e = files.get(i);
                out.write(i == 0 ? "\n{\"path\":" : ",\n{\"path\":");
                quoted(out, e.path);
                out.write(",\"size\":"); out.write(Long.toString(e.size));
                out.write(",\"sha256\":"); quoted(out, e.sha256);
                out.write(",\"mtime\":"); out.write(Long.toString(e.mtime)); out.write('}');
            }
            out.write("]}\n"); out.flush();
        }
        void writeAtomic(File target) throws IOException {
            validate();
            writeAtomicValidated(target);
        }
        private void writeAtomicValidated(File target) throws IOException {
            File temp = new File(target.getPath() + ".tmp");
            try {
                try (FileOutputStream out = new FileOutputStream(temp)) { writeValidated(out); out.getFD().sync(); }
                Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (IOException failed) { temp.delete(); throw failed; }
        }
        private static void field(Writer out, String key, String value) throws IOException {
            out.write(",\""); out.write(key); out.write("\":"); quoted(out, value);
        }
        private static void quoted(Writer out, String text) throws IOException {
            final String hex = "0123456789abcdef";
            out.write('"');
            for (int i = 0; i < text.length(); i++) {
                char c = text.charAt(i);
                if (c == '"' || c == '\\') { out.write('\\'); out.write(c); }
                else if (c < 0x20) {
                    out.write("\\u00"); out.write(hex.charAt(c >>> 4)); out.write(hex.charAt(c & 15));
                } else out.write(c);
            }
            out.write('"');
        }
        private static void checkString(String value) throws Invalid {
            if (value.length() > MAX_MANIFEST_STRING) throw new Invalid("corrupt", "manifest header string is too long");
        }
        static boolean digestHex(String digest) {
            if (digest == null || digest.length() != 64) return false;
            for (int i = 0; i < digest.length(); i++) {
                char c = digest.charAt(i);
                if (!(c >= '0' && c <= '9' || c >= 'a' && c <= 'f')) return false;
            }
            return true;
        }
        static Manifest read(InputStream in) throws IOException { return new ManifestReader(in).read(); }
        static Manifest read(File file) throws IOException {
            try (InputStream in = new FileInputStream(file)) { return read(in); }
        }
    }
    /** Streaming file-list reader: ignored extensions share the structural limits but allocate no value trees.
     * Reject duplicate recognized fields as an ambiguous schema, rather than retaining superseded paths or lists;
     * duplicate unknown fields remain valid extensions and still consume the object field budget. */
    static final class ManifestReader {
        private final java.io.BufferedReader in;
        private final PathBudget paths = new PathBudget();
        private final StringBuilder text = new StringBuilder();
        private final char[] number = new char[32];
        private final PathDecoder path = new PathDecoder();
        private int next = -2, nodes;
        private long position;

        ManifestReader(InputStream source) {
            in = new java.io.BufferedReader(new java.io.InputStreamReader(
                    new LimitedInput(source), java.nio.charset.StandardCharsets.UTF_8));
        }

        Manifest read() throws IOException {
            if (value(0) != '{') throw corrupt("manifest is not an object");
            take();
            Manifest manifest = new Manifest();
            manifest.format = 0;
            manifest.titleId = manifest.exportedAt = manifest.appVersion = manifest.device = manifest.folder = "";
            int fields = 0, seen = 0;
            space();
            if (!consume('}')) {
                while (true) {
                    if (++fields > MAX_MANIFEST_FIELDS) throw corrupt("too many object fields");
                    space();
                    String key = string(true, false);
                    space();
                    expect(':');
                    int field = rootField(key);
                    if ((seen & field) != 0) throw corrupt("duplicate manifest field " + key);
                    seen |= field;
                    switch (field) {
                        case 1: manifest.format = (int) numericValue(1, 0); break;
                        case 2: manifest.titleId = stringValue(1); break;
                        case 4: manifest.exportedAt = stringValue(1); break;
                        case 8: manifest.appVersion = stringValue(1); break;
                        case 16: manifest.device = stringValue(1); break;
                        case 32: manifest.folder = stringValue(1); break;
                        case 64: files(1, manifest); break;
                        default: skipValue(1);
                    }
                    space();
                    if (consume('}')) break;
                    expect(',');
                }
            }
            space();
            if (peek() != -1) throw corrupt("trailing data");
            if (manifest.format != 1) throw corrupt("unknown manifest format " + manifest.format);
            if (!manifest.titleId.isEmpty() && !manifest.titleId.equalsIgnoreCase(TITLE_ID))
                throw corrupt("saves of another game (" + manifest.titleId + ")");
            return manifest;
        }

        private static int rootField(String key) {
            switch (key) {
                case "format": return 1;
                case "title_id": return 2;
                case "exported_at": return 4;
                case "app_version": return 8;
                case "device": return 16;
                case "folder": return 32;
                case "files": return 64;
                default: return 0;
            }
        }

        private static int entryField(String key) {
            switch (key) {
                case "path": return 1;
                case "size": return 2;
                case "sha256": return 4;
                case "mtime": return 8;
                default: return 0;
            }
        }

        private void files(int depth, Manifest manifest) throws IOException {
            int c = value(depth);
            if (c == 'n') { literal("null"); return; }
            if (c != '[') throw corrupt("manifest file list");
            take();
            space();
            if (consume(']')) return;
            int entries = 0;
            while (true) {
                if (++entries > MAX_FILES) throw new Invalid("too_many", entries + " files (limit " + MAX_FILES + ")");
                if (value(depth + 1) != '{') throw corrupt("manifest file entry");
                entry(depth + 1, manifest);
                space();
                if (consume(']')) return;
                expect(',');
            }
        }

        /** The opening object's value/node has already been counted. */
        private void entry(int depth, Manifest manifest) throws IOException {
            expect('{');
            String name = null, sha = "";
            long size = -1, mtime = 0;
            int fields = 0, seen = 0;
            space();
            if (!consume('}')) {
                while (true) {
                    if (++fields > MAX_MANIFEST_FIELDS) throw corrupt("too many object fields");
                    space();
                    String key = string(true, false);
                    space();
                    expect(':');
                    int field = entryField(key);
                    if ((seen & field) != 0) throw corrupt("duplicate file field " + key);
                    seen |= field;
                    switch (field) {
                        case 1:
                            int c = value(depth + 1);
                            if (c == '"') name = string(true, true);
                            else { skipStarted(c, depth + 1); name = normalize(""); }
                            break;
                        case 2: size = numericValue(depth + 1, -1); break;
                        case 4: sha = stringValue(depth + 1); break;
                        case 8: mtime = numericValue(depth + 1, 0); break;
                        default: skipValue(depth + 1);
                    }
                    space();
                    if (consume('}')) break;
                    expect(',');
                }
            }
            if (name == null) name = normalize("");
            if (sha.length() != 64) throw corrupt("manifest digest");
            boolean uppercase = false;
            for (int i = 0; i < sha.length(); i++) {
                char c = sha.charAt(i);
                if (c >= 'A' && c <= 'F') uppercase = true;
                else if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
                    throw corrupt("manifest digest");
            }
            if (uppercase) sha = sha.toLowerCase(java.util.Locale.ROOT);
            manifest.files.add(new FileEntry(name, size, sha, mtime));
        }

        private String stringValue(int depth) throws IOException {
            int c = value(depth);
            if (c == '"') return string(true, false);
            skipStarted(c, depth);
            return "";
        }

        private long numericValue(int depth, long fallback) throws IOException {
            int c = value(depth);
            if (numberChar(c)) return number(true);
            skipStarted(c, depth);
            return fallback;
        }

        /** Count every value, including containers and ignored values, before descending. Keys are not values. */
        private int value(int depth) throws IOException {
            if (depth > MAX_MANIFEST_DEPTH || ++nodes > MAX_MANIFEST_NODES)
                throw corrupt("manifest structure limit");
            space();
            int c = peek();
            if (c == -1) throw corrupt("end of text");
            return c;
        }

        private void skipValue(int depth) throws IOException { skipStarted(value(depth), depth); }

        private void skipStarted(int c, int depth) throws IOException {
            switch (c) {
                case '{':
                    take();
                    space();
                    if (consume('}')) return;
                    int fields = 0;
                    while (true) {
                        if (++fields > MAX_MANIFEST_FIELDS) throw corrupt("too many object fields");
                        space();
                        string(false, false);
                        space();
                        expect(':');
                        skipValue(depth + 1);
                        space();
                        if (consume('}')) return;
                        expect(',');
                    }
                case '[':
                    take();
                    space();
                    if (consume(']')) return;
                    int entries = 0;
                    while (true) {
                        if (++entries > MAX_FILES) throw corrupt("too many array entries");
                        skipValue(depth + 1);
                        space();
                        if (consume(']')) return;
                        expect(',');
                    }
                case '"': string(false, false); return;
                case 't': literal("true"); return;
                case 'f': literal("false"); return;
                case 'n': literal("null"); return;
                default: number(false);
            }
        }

        /** Retaining is only needed for recognized values and dispatch keys; skipped strings allocate no payload.
         * Path characters go directly to the normalization accumulator, never through the metadata string cap. */
        private String string(boolean retain, boolean isPath) throws IOException {
            expect('"');
            if (isPath) path.reset();
            else if (retain) text.setLength(0);
            int length = 0;
            while (true) {
                int c = take();
                if (c == -1) throw corrupt("unterminated string");
                if (c == '"') {
                    if (isPath) return path.finish();
                    return retain ? text.toString() : null;
                }
                if (c < 0x20) throw corrupt("control character in string");
                if (c == '\\') {
                    c = take();
                    switch (c) {
                        case '"': case '\\': case '/': break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'u':
                            c = 0;
                            for (int i = 0; i < 4; i++) {
                                int h = take(), digit;
                                if (h >= '0' && h <= '9') digit = h - '0';
                                else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                                else throw corrupt("bad \\u escape");
                                c = c * 16 + digit;
                            }
                            break;
                        default: throw corrupt("bad escape");
                    }
                }
                if (isPath) path.append((char) c);
                else {
                    if (++length > MAX_MANIFEST_STRING) throw corrupt("string is too long");
                    if (retain) text.append((char) c);
                }
            }
        }

        private static boolean numberChar(int c) {
            return c >= '0' && c <= '9' || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E';
        }

        private static boolean digit(char c) { return c >= '0' && c <= '9'; }

        /** Preserve the old Long-or-Double coercion (including bounded exponents and decimal format values).
         * Ignored numbers use the same decimal grammar without allocating a token String or boxed Number. */
        private long number(boolean retain) throws IOException {
            int length = 0;
            while (numberChar(peek())) {
                if (length == number.length) throw corrupt("number is too long");
                number[length++] = (char) take();
            }
            if (length == 0) throw corrupt("unexpected value");
            int i = number[0] == '-' || number[0] == '+' ? 1 : 0;
            int start = i;
            while (i < length && digit(number[i])) i++;
            boolean digits = i > start;
            boolean integer = digits && i == length && length - start <= 18 && number[0] != '+';
            if (i < length && number[i] == '.') {
                start = ++i;
                while (i < length && digit(number[i])) i++;
                digits |= i > start;
            }
            if (!digits) throw corrupt("bad number");
            if (i < length && (number[i] == 'e' || number[i] == 'E')) {
                i++;
                if (i < length && (number[i] == '-' || number[i] == '+')) i++;
                start = i;
                while (i < length && digit(number[i])) i++;
                if (i == start) throw corrupt("bad number");
            }
            if (i != length) throw corrupt("bad number");
            if (!retain) return 0;
            if (integer) {
                long result = 0;
                for (i = number[0] == '-' ? 1 : 0; i < length; i++) result = result * 10 + number[i] - '0';
                return number[0] == '-' ? -result : result;
            }
            try { return (long) Double.parseDouble(new String(number, 0, length)); }
            catch (NumberFormatException bad) { throw corrupt("bad number"); }
        }

        /** Drop empty and dot segments while decoding, so even redundant raw paths only allocate their admitted
         * normalized size. Unpaired surrogates cost one byte, matching Java's UTF-8 replacement byte ('?'). */
        private final class PathDecoder {
            private final StringBuilder out = new StringBuilder();
            private long bytes;
            private int dots;
            private boolean segment, started;

            void reset() {
                out.setLength(0);
                bytes = 0;
                dots = 0;
                segment = started = false;
            }

            void append(char c) throws Invalid {
                if (!started) {
                    if (c == '/' || c == '\\') throw new Invalid("absolute", "manifest path");
                    started = true;
                }
                if (c == '\0') throw new Invalid("corrupt", "NUL in manifest path");
                if (c == '/' || c == '\\') { endSegment(); return; }
                if (!segment) {
                    if (c == '.' && dots < 2) { dots++; return; }
                    if (out.length() != 0) put('/');
                    while (dots > 0) { put('.'); dots--; }
                    segment = true;
                }
                put(c);
            }

            private void endSegment() throws Invalid {
                if (!segment && dots == 2) throw new Invalid("traversal", "manifest path");
                dots = 0;
                segment = false;
            }

            private void put(char c) throws Invalid {
                int length = out.length();
                long extra = Character.isLowSurrogate(c) && length != 0 && Character.isHighSurrogate(out.charAt(length - 1))
                        ? 3 : c < 0x80 ? 1 : c < 0x800 ? 2 : Character.isSurrogate(c) ? 1 : 3;
                long observed = paths.bytes + bytes + extra;
                if (observed > MAX_PATH_BYTES)
                    throw new Invalid("path_bytes", observed + " path bytes (limit " + MAX_PATH_BYTES + ")");
                bytes += extra;
                out.append(c);
            }

            String finish() throws Invalid {
                endSegment();
                String name = normalize(out.toString());
                paths.add(name);
                return name;
            }
        }

        private int peek() throws IOException {
            if (next == -2) next = in.read();
            return next;
        }

        private int take() throws IOException {
            int c = peek();
            next = -2;
            if (c != -1) position++;
            return c;
        }

        private void space() throws IOException {
            int c;
            while ((c = peek()) == ' ' || c == '\t' || c == '\r' || c == '\n') take();
        }

        private boolean consume(int c) throws IOException {
            if (peek() != c) return false;
            take();
            return true;
        }

        private void expect(int c) throws IOException {
            if (!consume(c)) throw corrupt("expected '" + (char) c + "'");
        }

        private void literal(String value) throws IOException {
            for (int i = 0; i < value.length(); i++) if (take() != value.charAt(i)) throw corrupt("bad literal");
        }

        private Invalid corrupt(String message) { return new Invalid("corrupt", "manifest: " + message + " at " + position); }

        /** Bound raw bytes before decoder/reader read-ahead. EOF at the exact limit is allowed; one excess byte
         * is sufficient to reject, and the caller retains ownership of the original stream. */
        private static final class LimitedInput extends InputStream {
            private final InputStream source;
            private long remaining = MAX_MANIFEST;

            LimitedInput(InputStream source) { this.source = source; }

            @Override public int read() throws IOException {
                int c = source.read();
                if (c == -1) return -1;
                if (remaining == 0) throw new Invalid("corrupt", "manifest exceeds " + MAX_MANIFEST + " bytes");
                remaining--;
                return c;
            }

            @Override public int read(byte[] buffer, int offset, int length) throws IOException {
                if (length == 0) return 0;
                if (remaining == 0) return read();
                int count = source.read(buffer, offset, (int) Math.min((long) length, remaining));
                if (count > 0) remaining -= count;
                return count;
            }
        }
    }
    static void quote(StringBuilder out, String s) {
        out.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20) out.append(String.format(Locale.ROOT, "\\u%04x", (int) c)); else out.append(c);
            }
        }
        out.append('"');
    }

    /** A small strict JSON reader (org.json is not on the host test classpath): objects, arrays, strings, numbers,
     * true/false/null. */
    static final class Json {
        private final String s; private int i;
        Json(String s) { this.s = s; }
        Object document() { Object v = value(); space(); if (i != s.length()) throw error("trailing data"); return v; }
        private RuntimeException error(String what) { return new IllegalArgumentException(what + " at " + i); }
        private void space() { while (i < s.length() && " \t\r\n".indexOf(s.charAt(i)) >= 0) i++; }
        private Object value() {
            space();
            if (i >= s.length()) throw error("end of text");
            char c = s.charAt(i);
            if (c == '{') {
                i++; Map<String, Object> map = new LinkedHashMap<>(); space();
                if (peek('}')) return map;
                do { space(); String key = string(); space(); expect(':'); map.put(key, value()); space(); } while (peek(','));
                expect('}'); return map;
            }
            if (c == '[') {
                i++; List<Object> list = new ArrayList<>(); space();
                if (peek(']')) return list;
                do { list.add(value()); space(); } while (peek(','));
                expect(']'); return list;
            }
            if (c == '"') return string();
            if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
            if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
            if (s.startsWith("null", i)) { i += 4; return null; }
            int start = i;
            while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
            String number = s.substring(start, i);
            if (number.isEmpty()) throw error("unexpected '" + c + "'");
            try { return number.matches("-?\\d{1,18}") ? (Object) Long.parseLong(number) : (Object) Double.parseDouble(number); }
            catch (NumberFormatException bad) { throw error("bad number"); }
        }
        private boolean peek(char c) { if (i < s.length() && s.charAt(i) == c) { i++; return true; } return false; }
        private void expect(char c) { if (!peek(c)) throw error("expected '" + c + "'"); }
        private String string() {
            expect('"');
            StringBuilder out = new StringBuilder();
            while (true) {
                if (i >= s.length()) throw error("unterminated string");
                char c = s.charAt(i++);
                if (c == '"') return out.toString();
                if (c < 0x20) throw error("control character in string");
                if (c != '\\') { out.append(c); continue; }
                if (i >= s.length()) throw error("unterminated escape");
                char e = s.charAt(i++);
                switch (e) {
                    case '"': case '\\': case '/': out.append(e); break;
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'n': out.append('\n'); break;
                    case 'r': out.append('\r'); break;
                    case 't': out.append('\t'); break;
                    case 'u':
                        if (i + 4 > s.length()) throw error("short \\u escape");
                        try { out.append((char) Integer.parseInt(s.substring(i, i + 4), 16)); } catch (NumberFormatException bad) { throw error("bad \\u escape"); }
                        i += 4; break;
                    default: throw error("bad escape");
                }
            }
        }
    }

    // ---------- time ----------
    static String isoTime(long millis) {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date(millis));
    }
    static long parseTime(String iso) {
        if (iso == null || iso.isEmpty()) return 0;
        try {
            SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
            format.setTimeZone(TimeZone.getTimeZone("UTC"));
            return format.parse(iso).getTime();
        } catch (java.text.ParseException bad) { return 0; }
    }
    /** Export file name: "TotK Native saves 2026-11-02 1403.zip" (local time). */
    static String exportName(long millis) { return TOP + " " + new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.ROOT).format(new Date(millis)) + ".zip"; }

    // ---------- the live save root ----------
    /** The live save root when the local account exists (listing only: never creates the identity), else null. */
    static File existingRoot(File filesDir) {
        File uid = new File(new File(filesDir, "user-data"), "account.uid");
        if (!uid.isFile() || uid.length() != 16) return null;
        try { return saveRoot(filesDir); } catch (IOException unreadable) { return null; }
    }
    /** <private>/user-data/save/<hex(hi)><hex(lo)>, from user-data/account.uid exactly like runtime/account.c and
     * fs_host.c. A missing identity is created (16 random non-zero bytes, as the runtime would on its first start), so
     * saves imported before the first game start land where the game will look. */
    static File saveRoot(File filesDir) throws IOException {
        File profile = new File(filesDir, "user-data"), uid = new File(profile, "account.uid");
        byte[] id = uid.isFile() ? Files.readAllBytes(uid.toPath()) : null;
        if (id == null || id.length == 0) {
            if (!profile.isDirectory() && !profile.mkdirs()) throw new IOException("cannot create " + profile);
            id = new byte[16];
            java.security.SecureRandom random = new java.security.SecureRandom();
            do random.nextBytes(id); while (allZero(id));
            File temp = new File(profile, "account.uid.tmp");
            try (FileOutputStream out = new FileOutputStream(temp)) { out.write(id); out.getFD().sync(); }
            temp.setReadable(false, false); temp.setReadable(true, true); temp.setWritable(false, false); temp.setWritable(true, true);
            Files.move(temp.toPath(), uid.toPath(), StandardCopyOption.ATOMIC_MOVE);
        }
        if (id.length != 16 || allZero(id)) throw new IOException("corrupt account.uid");
        return new File(new File(profile, "save"), uidName(id));
    }
    private static boolean allZero(byte[] bytes) { for (byte b : bytes) if (b != 0) return false; return true; }
    /** account.uid holds {lo, hi} as two little-endian u64; the folder is "%016llx%016llx" of hi then lo. */
    static String uidName(byte[] id) {
        long lo = 0, hi = 0;
        for (int b = 7; b >= 0; b--) { lo = lo << 8 | (id[b] & 0xff); hi = hi << 8 | (id[8 + b] & 0xff); }
        return String.format(Locale.ROOT, "%016x%016x", hi, lo);
    }

    /** Every regular file under root (symbolic links are skipped), sorted by path, with SHA-256. A digest is reused from
     * cache when the file's size and modification time are unchanged, so a sync check does not rehash the set. */
    static List<FileEntry> scan(File root, Manifest cache) throws IOException {
        Map<String, FileEntry> known = cache == null ? new HashMap<>() : cache.byPath();
        List<String> paths = new ArrayList<>();
        walk(root, "", paths);
        java.util.Collections.sort(paths);
        List<FileEntry> out = new ArrayList<>();
        for (String path : paths) {
            File file = new File(root, path);
            long size = file.length(), mtime = file.lastModified();
            FileEntry k = known.get(path);
            String sha = k != null && k.size == size && k.mtime == mtime && k.sha256 != null ? k.sha256 : sha256(file);
            out.add(new FileEntry(path, size, sha, mtime));
        }
        return out;
    }
    private static void walk(File dir, String prefix, List<String> out) {
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) continue;
            String path = prefix + child.getName();
            if (child.isDirectory()) walk(child, path + "/", out);
            else if (child.isFile()) out.add(path);
        }
    }
    static Manifest manifest(File root, Manifest cache, String appVersion, String device, long now) throws IOException {
        Manifest m = new Manifest();
        m.exportedAt = isoTime(now); m.appVersion = appVersion == null ? "" : appVersion; m.device = device == null ? "" : device;
        if (root.isDirectory()) m.files.addAll(scan(root, cache));
        m.validate();
        return m;
    }
    static String sha256(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) { return sha256(in); }
    }
    static String sha256(InputStream in) throws IOException {
        MessageDigest digest = digest();
        byte[] buffer = new byte[64 * 1024];
        for (int n; (n = in.read(buffer)) > 0;) digest.update(buffer, 0, n);
        return hex(digest.digest());
    }
    static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException impossible) { throw new IllegalStateException(impossible); }
    }
    static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return out.toString();
    }

    // ---------- slots ----------
    static final class Slot {
        /** time: when the game saved this slot (caption.sav's own timestamp, else progress.sav's modification time);
         * mtime: progress.sav's modification time. */
        final int index; final long time, mtime; final File caption;
        Slot(int index, long time, long mtime, File caption) { this.index = index; this.time = time; this.mtime = mtime; this.caption = caption; }
    }
    /** slot_NN folders holding a progress.sav, by slot number. */
    static List<Slot> slots(File root) {
        List<Slot> out = new ArrayList<>();
        File[] children = root == null ? null : root.listFiles();
        if (children == null) return out;
        for (File child : children) {
            String name = child.getName();
            if (!name.matches("slot_\\d\\d") || !child.isDirectory()) continue;
            File progress = new File(child, "progress.sav"), caption = new File(child, "caption.sav");
            if (!progress.isFile()) continue;
            long mtime = progress.lastModified(), saved = caption.isFile() ? captionTime(caption, TimeZone.getDefault()) : 0;
            out.add(new Slot(Integer.parseInt(name.substring(5)), saved > 0 ? saved : mtime, mtime, caption.isFile() ? caption : null));
        }
        out.sort((a, b) -> Integer.compare(a.index, b.index));
        return out;
    }
    static long newest(List<Slot> slots) { long t = 0; for (Slot s : slots) t = Math.max(t, s.time); return t; }
    /** Newest progress.sav modification time (comparable with a save folder's manifest, which has no captions). */
    static long newestModified(List<Slot> slots) { long t = 0; for (Slot s : slots) t = Math.max(t, s.mtime); return t; }

    /** caption.sav is the game's hash/value save table: u32 magic 0x01020304, u32 version, u32 data offset, then from
     * 0x20 up to the data offset 8-byte {u32 key hash, u32 value} entries. The slot's save timestamp is six s32
     * entries (murmur3 key hashes below) holding the device's local wall-clock time when the game saved. */
    private static final int CAPTION_MAGIC = 0x01020304, CAPTION_TABLE = 0x20, CAPTION_TABLE_MAX = 0x10000;
    private static final int[] CAPTION_TIME_KEYS = {0x9811a3f7, 0xdfd840d3, 0xbd46f485, 0x23f3d75e, 0x27853bf7, 0x5ee50bed};   // year, month, day, hour, minute, second
    /** The in-game save time of a caption.sav as epoch milliseconds in zone, or 0 when it cannot be read. */
    static long captionTime(File caption, TimeZone zone) {
        try (RandomAccessFile in = new RandomAccessFile(caption, "r")) {
            if (in.length() < CAPTION_TABLE) return 0;
            in.seek(8);
            int end = Integer.reverseBytes(in.readInt());   // the data offset ends the entry table
            if (end < CAPTION_TABLE || end > CAPTION_TABLE_MAX || end > in.length()) return 0;
            byte[] table = new byte[end];
            in.seek(0);
            in.readFully(table);
            return captionTime(table, end, zone);
        } catch (IOException unreadable) { return 0; }
    }
    static long captionTime(byte[] data, int length, TimeZone zone) {
        if (length < CAPTION_TABLE) return 0;
        java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(data, 0, length).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        if (b.getInt(0) != CAPTION_MAGIC) return 0;
        int end = b.getInt(8);
        if (end < CAPTION_TABLE || end > length) return 0;
        int[] v = new int[CAPTION_TIME_KEYS.length];
        int found = 0;
        for (int p = CAPTION_TABLE; p + 8 <= end; p += 8) {
            int key = b.getInt(p);
            for (int k = 0; k < CAPTION_TIME_KEYS.length; k++)
                if (key == CAPTION_TIME_KEYS[k] && (found & (1 << k)) == 0) { v[k] = b.getInt(p + 4); found |= 1 << k; }
        }
        if (found != (1 << CAPTION_TIME_KEYS.length) - 1) return 0;
        if (v[0] < 2000 || v[0] > 2999 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] < 0 || v[3] > 23
                || v[4] < 0 || v[4] > 59 || v[5] < 0 || v[5] > 59) return 0;
        java.util.Calendar c = java.util.Calendar.getInstance(zone, Locale.ROOT);
        c.clear();
        c.setLenient(false);
        c.set(v[0], v[1] - 1, v[2], v[3], v[4], v[5]);
        try { return c.getTimeInMillis(); } catch (IllegalArgumentException impossible) { return 0; }
    }
    /** The picture inside caption.sav: the game stores a 256 x 144 baseline JPEG after a small header. Returns
     * {offset, length} of the bytes from the SOI marker to the last EOI marker, or null when there is none. */
    static int[] captionJpeg(byte[] data) {
        if (data == null) return null;
        int start = -1;
        for (int i = 0; i + 2 < data.length; i++)
            if ((data[i] & 0xff) == 0xff && (data[i + 1] & 0xff) == 0xd8 && (data[i + 2] & 0xff) == 0xff) { start = i; break; }
        if (start < 0) return null;
        for (int j = data.length - 2; j > start; j--)
            if ((data[j] & 0xff) == 0xff && (data[j + 1] & 0xff) == 0xd9) return new int[]{start, j + 2 - start};
        return null;
    }

    // ---------- import: validation and save-root detection ----------
    /** One file (or symbolic link) of a dump, as listed by a zip's central directory or a picked folder. */
    static final class Candidate {
        final String path; final long size; final boolean link; final long mtime;
        Candidate(String path, long size, boolean link, long mtime) { this.path = path; this.size = size; this.link = link; this.mtime = mtime; }
    }
    /** A validated save set to import: the root prefix inside the dump ("" = top) and its files relative to that root. */
    static final class Selection {
        final String root; final List<Candidate> files = new ArrayList<>(); final Map<String, String> sources = new HashMap<>();
        String manifestSource;
        long total;
        Selection(String root) { this.root = root; }
    }
    /** A dump path as a clean relative path ('/' separators, no "." segments); rejects absolute paths and "..". */
    static String normalize(String raw) throws Invalid {
        if (raw == null || raw.isEmpty()) throw new Invalid("corrupt", "empty name");
        if (raw.charAt(0) == '/' || raw.charAt(0) == '\\'
                || raw.length() > 1 && ((raw.charAt(0) >= 'A' && raw.charAt(0) <= 'Z') || (raw.charAt(0) >= 'a' && raw.charAt(0) <= 'z')) && raw.charAt(1) == ':')
            throw new Invalid("absolute", raw);
        boolean changed = false;
        int start = 0;
        for (int i = 0; i <= raw.length(); i++) {
            char c = i == raw.length() ? '/' : raw.charAt(i);
            if (c == '\0') throw new Invalid("corrupt", raw);
            if (c != '/' && c != '\\') continue;
            int length = i - start;
            if (length == 2 && raw.charAt(start) == '.' && raw.charAt(start + 1) == '.') throw new Invalid("traversal", raw);
            if (length == 0 || length == 1 && raw.charAt(start) == '.' || c == '\\') changed = true;
            start = i + 1;
        }
        if (!changed) return raw;
        StringBuilder out = new StringBuilder();
        start = 0;
        for (int i = 0; i <= raw.length(); i++) {
            if (i < raw.length() && raw.charAt(i) != '/' && raw.charAt(i) != '\\') continue;
            int length = i - start;
            if (length != 0 && !(length == 1 && raw.charAt(start) == '.')) {
                if (out.length() != 0) out.append('/');
                out.append(raw, start, i);
            }
            start = i + 1;
        }
        if (out.length() == 0) throw new Invalid("corrupt", "empty name");
        String normalized = out.toString();
        if (normalized.length() > 1 && ((normalized.charAt(0) >= 'A' && normalized.charAt(0) <= 'Z')
                || (normalized.charAt(0) >= 'a' && normalized.charAt(0) <= 'z')) && normalized.charAt(1) == ':')
            throw new Invalid("absolute", raw);
        return normalized;
    }
    private static boolean junk(String path) {
        String name = path.substring(path.lastIndexOf('/') + 1);
        return path.startsWith("__MACOSX/") || path.contains("/__MACOSX/") || name.equals(".DS_Store") || name.equals("Thumbs.db")
                || name.equals("desktop.ini") || name.startsWith("._");
    }
    /** Root directory prefix of a save set: "" or "dir/sub/". */
    private static String rootOf(String path) {
        if (path.equals("option.sav") || path.endsWith("/option.sav")) return path.substring(0, path.length() - "option.sav".length());
        if (SLOT_PROGRESS.matcher(path).matches()) {
            String slotDir = path.substring(0, path.length() - "/progress.sav".length());
            int cut = slotDir.lastIndexOf('/');
            return cut < 0 ? "" : slotDir.substring(0, cut + 1);
        }
        return null;
    }
    /** Validate a whole dump and pick its save root: the shallowest directory holding option.sav or a
     * slot_NN/progress.sav (when several sit at that depth — e.g. dated backups side by side — the one whose
     * progress.sav is newest). Any absolute path, "..", or symbolic link anywhere rejects the dump; under the root,
     * more than MAX_FILES files, more than MAX_TOTAL bytes, or an oversized file rejects it too. */
    static Selection select(Collection<Candidate> all) throws Invalid {
        if (all.size() > MAX_ENTRIES) throw new Invalid("too_many", all.size() + " entries (limit " + MAX_ENTRIES + ")");
        List<Candidate> clean = new ArrayList<>();
        for (Candidate c : all) {
            String path = normalize(c.path);
            if (c.link) throw new Invalid("symlink", path);
            if (!junk(path)) clean.add(new Candidate(path, c.size, false, c.mtime));
        }
        Map<String, Long> roots = new LinkedHashMap<>();
        for (Candidate c : clean) {
            String root = rootOf(c.path);
            if (root != null) roots.merge(root, c.path.endsWith("progress.sav") ? c.mtime : Long.MIN_VALUE, Math::max);
        }
        if (roots.isEmpty()) throw new Invalid("no_saves", null);
        String best = null;
        for (Map.Entry<String, Long> e : roots.entrySet()) {
            if (best == null) { best = e.getKey(); continue; }
            int depth = depth(e.getKey()), bestDepth = depth(best);
            if (depth < bestDepth || depth == bestDepth && e.getValue() > roots.get(best)) best = e.getKey();
        }
        Selection selection = new Selection(best);
        PathBudget paths = new PathBudget();
        for (Candidate c : clean) {
            if (!c.path.startsWith(best)) continue;
            String relative = c.path.substring(best.length());
            if (relative.equals(MANIFEST)) { selection.manifestSource = c.path; continue; }
            long limit = KNOWN.matcher(relative).matches() ? MAX_KNOWN_FILE : MAX_OTHER_FILE;
            if (c.size > limit) throw new Invalid("huge_file", relative + " (" + c.size + " bytes)");
            if (c.size < 0) throw new Invalid("corrupt", relative + " has no size");
            paths.add(relative);
            selection.total += c.size;
            selection.files.add(new Candidate(relative, c.size, false, c.mtime));
            selection.sources.put(relative, c.path);
            if (selection.total > MAX_TOTAL) throw new Invalid("too_big", selection.total + " bytes");
        }
        return selection;
    }
    private static int depth(String prefix) { int n = 0; for (int i = 0; i < prefix.length(); i++) if (prefix.charAt(i) == '/') n++; return n; }

    /** Copy a stream into out, failing once more than limit bytes arrive (sizes in a dump can lie). */
    static void copyLimited(InputStream in, File out, long limit, long mtime) throws IOException {
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        long written = 0;
        try (FileOutputStream o = new FileOutputStream(out)) {
            byte[] buffer = new byte[64 * 1024];
            for (int n; (n = in.read(buffer)) > 0;) {
                written += n;
                if (written > limit) throw new Invalid("too_big", out.getName() + " is larger than its listed size");
                o.write(buffer, 0, n);
            }
            o.getFD().sync();
        }
        if (mtime > 0) out.setLastModified(mtime);
    }
    /** After staging: when the dump carries our manifest, every file must match it (a damaged or edited export is refused). */
    static void verifyAgainst(File staged, Manifest manifest) throws IOException {
        if (manifest == null) return;
        Map<String, FileEntry> want = manifest.byPath();
        List<FileEntry> got = scan(staged, null);
        if (got.size() != want.size()) throw new Invalid("corrupt", "the files do not match the dump's manifest");
        for (FileEntry e : got) if (!e.sameContent(want.get(e.path))) throw new Invalid("corrupt", e.path + " does not match the dump's manifest");
    }

    // ---------- zip ----------
    /** Zip entry names with the symbolic-link bit from the central directory (java.util.zip does not expose the
     * external attributes): name -> {size, isLink}. Handles ZIP64 end records. */
    static Map<String, long[]> zipDirectory(File zip) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(zip, "r")) {
            long length = f.length();
            int tail = (int) Math.min(length, 65_557);
            byte[] end = new byte[tail];
            f.seek(length - tail); f.readFully(end);
            int eocd = -1;
            for (int i = tail - 22; i >= 0; i--) if (le32(end, i) == 0x06054b50L) { eocd = i; break; }
            if (eocd < 0) throw new Invalid("not_zip", "no end of central directory");
            long count = le16(end, eocd + 10), cdSize = le32(end, eocd + 12), cdOffset = le32(end, eocd + 16);
            if (count == 0xffff || cdSize == 0xffffffffL || cdOffset == 0xffffffffL) {
                if (eocd < 20 || le32(end, eocd - 20) != 0x07064b50L) throw new Invalid("not_zip", "zip64 locator missing");
                long record = le64(end, eocd - 20 + 8);
                byte[] z = new byte[56];
                f.seek(record); f.readFully(z);
                if (le32(z, 0) != 0x06064b50L) throw new Invalid("not_zip", "zip64 end record");
                count = le64(z, 32); cdSize = le64(z, 40); cdOffset = le64(z, 48);
            }
            if (count > MAX_ENTRIES) throw new Invalid("too_many", count + " entries (limit " + MAX_ENTRIES + ")");
            if (cdSize > 16L << 20 || cdOffset + cdSize > length) throw new Invalid("not_zip", "central directory out of range");
            byte[] cd = new byte[(int) cdSize];
            f.seek(cdOffset); f.readFully(cd);
            Map<String, long[]> out = new LinkedHashMap<>();
            int p = 0;
            for (long n = 0; n < count; n++) {
                if (p + 46 > cd.length || le32(cd, p) != 0x02014b50L) throw new Invalid("not_zip", "central directory entry " + n);
                int madeBy = (int) le16(cd, p + 4) >> 8, flags = (int) le16(cd, p + 8);
                long size = le32(cd, p + 24), external = le32(cd, p + 38);
                int nameLength = (int) le16(cd, p + 28), extra = (int) le16(cd, p + 30), comment = (int) le16(cd, p + 32);
                if (p + 46 + nameLength + extra + comment > cd.length) throw new Invalid("not_zip", "central directory entry " + n);
                String name = new String(cd, p + 46, nameLength, (flags & 0x800) != 0 ? StandardCharsets.UTF_8 : StandardCharsets.ISO_8859_1);
                if (size == 0xffffffffL) size = zip64Size(cd, p + 46 + nameLength, extra);
                // Unix hosts (3) keep st_mode in the high half of the external attributes; S_IFLNK = 0120000.
                boolean link = madeBy == 3 && ((external >>> 16) & 0170000) == 0120000;
                out.put(name, new long[]{size, link ? 1 : 0});
                p += 46 + nameLength + extra + comment;
            }
            return out;
        }
    }
    private static long zip64Size(byte[] cd, int at, int length) {
        for (int p = at; p + 4 <= at + length;) {
            int id = (int) le16(cd, p), size = (int) le16(cd, p + 2);
            if (id == 1 && size >= 8) return le64(cd, p + 4);
            p += 4 + size;
        }
        return -1;
    }
    private static long le16(byte[] b, int i) { return (b[i] & 0xff) | (b[i + 1] & 0xff) << 8; }
    private static long le32(byte[] b, int i) { return le16(b, i) | le16(b, i + 2) << 16; }
    private static long le64(byte[] b, int i) { return le32(b, i) | le32(b, i + 4) << 32; }

    /** Validate a zip dump and extract its save root into staging (which must not exist yet). */
    static Selection extractZip(File zip, File staging) throws IOException {
        Map<String, long[]> directory = zipDirectory(zip);
        List<Candidate> candidates = new ArrayList<>();
        try (ZipFile file = new ZipFile(zip)) {
            Map<String, Long> times = new HashMap<>();
            for (Enumeration<? extends ZipEntry> e = file.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                times.put(entry.getName(), entry.getTime());
            }
            for (Map.Entry<String, long[]> e : directory.entrySet()) {
                if (e.getKey().endsWith("/") && e.getValue()[1] == 0) { normalize(e.getKey()); continue; }  // directory entry
                Long time = times.get(e.getKey());
                candidates.add(new Candidate(e.getKey(), e.getValue()[0], e.getValue()[1] != 0, time == null ? 0 : time));
            }
            Selection selection = select(candidates);
            Map<String, String> original = new HashMap<>();
            for (Map.Entry<String, long[]> e : directory.entrySet()) original.put(normalize(e.getKey()), e.getKey());
            if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
            for (Candidate c : selection.files) {
                ZipEntry entry = file.getEntry(original.get(selection.sources.get(c.path)));
                if (entry == null) throw new Invalid("corrupt", c.path);
                try (InputStream in = file.getInputStream(entry)) { copyLimited(in, new File(staging, c.path), c.size, c.mtime); }
            }
            if (selection.manifestSource != null) {
                ZipEntry entry = file.getEntry(original.get(selection.manifestSource));
                try (InputStream in = file.getInputStream(entry)) { verifyAgainst(staging, Manifest.read(in)); }
            }
            return selection;
        } catch (java.util.zip.ZipException bad) {
            throw new Invalid("not_zip", bad.getMessage());
        }
    }
    static String readText(InputStream in, long limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16 * 1024];
        for (int n; (n = in.read(buffer)) > 0;) {
            if (n > limit - out.size()) throw new Invalid("too_big", "manifest");
            out.write(buffer, 0, n);
        }
        return out.toString("UTF-8");
    }
    /** The export: TOP/ with the save root's files and TOP/MANIFEST (written last). root is a consistent snapshot. */
    static void writeZip(File root, Manifest manifest, OutputStream sink) throws IOException {
        manifest.validate();
        try (ZipOutputStream zip = new ZipOutputStream(sink)) {
            for (FileEntry e : manifest.files) {
                ZipEntry entry = new ZipEntry(TOP + "/" + e.path);
                entry.setTime(e.mtime);
                zip.putNextEntry(entry);
                try (InputStream in = new FileInputStream(new File(root, e.path))) { in.transferTo(zip); }
                zip.closeEntry();
            }
            ZipEntry entry = new ZipEntry(TOP + "/" + MANIFEST);
            entry.setTime(manifest.exportedMillis());
            zip.putNextEntry(entry);
            manifest.writeValidated(zip);
            zip.closeEntry();
        }
    }

    // ---------- consistent snapshot of the live root ----------
    /** Copy root into an empty target while the game may be saving: each file's size and modification time are read
     * before and after its copy, and the whole tree is listed again at the end; any change (or an IOException from a
     * file that disappeared) discards the copy and starts over, up to attempts times. Returns the manifest of the copy. */
    static Manifest snapshot(File root, File target, String appVersion, String device, long now, int attempts) throws IOException {
        IOException last = null;
        for (int attempt = 0; attempt < attempts; attempt++) {
            delete(target);
            if (!target.mkdirs()) throw new IOException("cannot create " + target);
            try {
                Map<String, long[]> before = stats(root);
                PathBudget names = new PathBudget();
                for (String path : before.keySet()) { normalize(path); names.add(path); }
                boolean changed = false;
                for (Map.Entry<String, long[]> e : before.entrySet()) {
                    File from = new File(root, e.getKey()), to = new File(target, e.getKey());
                    to.getParentFile().mkdirs();
                    try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) { in.transferTo(out); }
                    if (from.length() != e.getValue()[0] || from.lastModified() != e.getValue()[1] || to.length() != e.getValue()[0]) { changed = true; break; }
                    to.setLastModified(e.getValue()[1]);
                }
                if (!changed && sameStats(before, stats(root))) {
                    Manifest m = manifest(target, null, appVersion, device, now);
                    // keep the live modification times (the cache key for later scans of the live root)
                    List<FileEntry> files = new ArrayList<>();
                    for (FileEntry e : m.files) files.add(new FileEntry(e.path, e.size, e.sha256, before.get(e.path)[1]));
                    m.files.clear(); m.files.addAll(files);
                    return m;
                }
            } catch (Invalid invalid) { delete(target); throw invalid; }
            catch (IOException raced) { last = raced; }
            try { Thread.sleep(300L * (attempt + 1)); } catch (InterruptedException stop) { Thread.currentThread().interrupt(); break; }
        }
        delete(target);
        throw last != null ? last : new IOException("the saves kept changing while they were copied");
    }
    private static Map<String, long[]> stats(File root) {
        List<String> paths = new ArrayList<>();
        walk(root, "", paths);
        java.util.Collections.sort(paths);
        Map<String, long[]> out = new LinkedHashMap<>();
        for (String p : paths) { File f = new File(root, p); out.put(p, new long[]{f.length(), f.lastModified()}); }
        return out;
    }
    private static boolean sameStats(Map<String, long[]> a, Map<String, long[]> b) {
        if (!a.keySet().equals(b.keySet())) return false;
        for (Map.Entry<String, long[]> e : a.entrySet()) if (!java.util.Arrays.equals(e.getValue(), b.get(e.getKey()))) return false;
        return true;
    }

    // ---------- swap ----------
    static File incoming(File root) { return new File(root.getPath() + ".incoming"); }
    private static File ready(File root) { return new File(root.getPath() + ".ready"); }
    private static File old(File root) { return new File(root.getPath() + ".old"); }
    /** Replace the live root with a fully written incoming tree: incoming is renamed to .ready (complete), the live
     * root to .old, .ready to the root, then .old is removed. Every step is a rename in one directory, and recover()
     * finishes or undoes an interrupted swap, so a crash never leaves the game without a complete save set. */
    static void install(File incoming, File root) throws IOException {
        syncTree(incoming);
        File ready = ready(root), old = old(root);
        delete(ready); delete(old);
        Files.move(incoming.toPath(), ready.toPath(), StandardCopyOption.ATOMIC_MOVE);
        if (root.exists()) Files.move(root.toPath(), old.toPath(), StandardCopyOption.ATOMIC_MOVE);
        Files.move(ready.toPath(), root.toPath(), StandardCopyOption.ATOMIC_MOVE);
        syncDir(root.getParentFile());
        delete(old);
    }
    /** Finish or roll back a swap interrupted by process death; drops half-written incoming trees. */
    static void recover(File root) throws IOException {
        File ready = ready(root), old = old(root), incoming = incoming(root);
        delete(incoming);
        if (ready.isDirectory()) {
            if (root.exists()) { delete(old); Files.move(root.toPath(), old.toPath(), StandardCopyOption.ATOMIC_MOVE); }
            Files.move(ready.toPath(), root.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } else if (!root.exists() && old.isDirectory()) Files.move(old.toPath(), root.toPath(), StandardCopyOption.ATOMIC_MOVE);
        delete(old);
    }
    private static void syncTree(File dir) {
        File[] children = dir.listFiles();
        if (children != null) for (File child : children) {
            if (child.isDirectory()) syncTree(child);
            else try (FileChannel channel = FileChannel.open(child.toPath(), StandardOpenOption.READ)) { channel.force(true); } catch (IOException ignored) { }
        }
        syncDir(dir);
    }
    /** fsync a directory where the platform allows opening one (Linux does); best effort elsewhere. */
    private static void syncDir(File dir) {
        if (dir == null) return;
        try (FileChannel channel = FileChannel.open(dir.toPath(), StandardOpenOption.READ)) { channel.force(true); } catch (IOException | RuntimeException ignored) { }
    }
    static void copyTree(File from, File to) throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot create " + to);
        File[] children = from.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) continue;
            File target = new File(to, child.getName());
            if (child.isDirectory()) copyTree(child, target);
            else if (child.isFile()) try (InputStream in = new FileInputStream(child)) { copyLimited(in, target, Long.MAX_VALUE, child.lastModified()); }
        }
    }
    static void delete(File file) {
        File[] children = file.isDirectory() && !Files.isSymbolicLink(file.toPath()) ? file.listFiles() : null;
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }

    // ---------- backups ----------
    static final class Backup {
        final File dir; final String reason; final long created; final int slots;
        Backup(File dir, String reason, long created, int slots) { this.dir = dir; this.reason = reason; this.created = created; this.slots = slots; }
        File saves() { return new File(dir, "save"); }
    }
    /** Copy the live root to <backups>/<yyyyMMdd-HHmmss-SSS>/save with backup.json {reason, created, slots}; the folder
     * is written as .partial and renamed when complete. Keeps the newest KEEP_BACKUPS. Returns null when there is
     * nothing to back up. reason: import | mirror | restore | manual. */
    static Backup backup(File root, File backups, String reason, long now) throws IOException {
        if (root == null || !root.isDirectory() || scanCount(root) == 0) return null;
        String name = new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.ROOT).format(new Date(now));
        File dir = new File(backups, name), partial = new File(backups, name + ".partial");
        for (int n = 1; dir.exists(); n++) { dir = new File(backups, name + "-" + n); partial = new File(backups, dir.getName() + ".partial"); }
        delete(partial);
        copyTree(root, new File(partial, "save"));
        int slots = slots(new File(partial, "save")).size();
        StringBuilder json = new StringBuilder("{\"reason\":");
        quote(json, reason);
        json.append(",\"created\":").append(now).append(",\"slots\":").append(slots).append("}\n");
        Settings.writeAtomic(partial, "backup.json", json.toString());
        syncTree(partial);
        Files.move(partial.toPath(), dir.toPath(), StandardCopyOption.ATOMIC_MOVE);
        rotate(backups, KEEP_BACKUPS);
        return new Backup(dir, reason, now, slots);
    }
    private static int scanCount(File root) { List<String> paths = new ArrayList<>(); walk(root, "", paths); return paths.size(); }
    /** Newest first; leftovers of interrupted backups (.partial) are not listed. */
    static List<Backup> backups(File backups) {
        List<Backup> out = new ArrayList<>();
        File[] dirs = backups.listFiles();
        if (dirs == null) return out;
        for (File dir : dirs) {
            if (!dir.isDirectory() || dir.getName().endsWith(".partial")) continue;
            String reason = "manual"; long created = dir.lastModified(); int slots = -1;
            try (InputStream in = new FileInputStream(new File(dir, "backup.json"))) {
                Object parsed = new Json(readText(in, MAX_MANIFEST_HEADER)).document();
                if (parsed instanceof Map) {
                    Map<?, ?> m = (Map<?, ?>) parsed;
                    if (m.get("reason") instanceof String) reason = (String) m.get("reason");
                    if (m.get("created") instanceof Number) created = ((Number) m.get("created")).longValue();
                    if (m.get("slots") instanceof Number) slots = ((Number) m.get("slots")).intValue();
                }
            } catch (IOException | RuntimeException unreadable) { continue; }
            out.add(new Backup(dir, reason, created, slots));
        }
        out.sort((a, b) -> b.dir.getName().compareTo(a.dir.getName()));
        return out;
    }
    static void rotate(File backups, int keep) {
        List<Backup> list = backups(backups);
        for (int i = keep; i < list.size(); i++) delete(list.get(i).dir);
        File[] dirs = backups.listFiles();
        if (dirs != null) for (File dir : dirs) if (dir.getName().endsWith(".partial")) delete(dir);
    }

    // ---------- import, restore ----------
    static File backupsDir(File filesDir) { return new File(filesDir, "save-backups"); }
    /** Private scratch trees (same filesystem as the live root, so a staged set can be renamed into place). */
    static File staging(File filesDir, String name) { return new File(new File(filesDir, "save-staging"), name); }
    /** Make a staged tree the live save set: refuses while the game runs, backs the live set up first (reason), then
     * moves staged into place with install(). The game lock is held only for the backup copy and the renames. */
    static Backup replace(File filesDir, File staged, String reason, long now) throws IOException {
        File root = saveRoot(filesDir);
        try (GameLock.Hold hold = GameLock.hold(filesDir)) {
            hold.requireFree();
            recover(root);
            Backup backup = backup(root, backupsDir(filesDir), reason, now);
            File incoming = incoming(root);
            delete(incoming);
            if (!root.getParentFile().isDirectory() && !root.getParentFile().mkdirs()) throw new IOException("cannot create " + root.getParentFile());
            Files.move(staged.toPath(), incoming.toPath(), StandardCopyOption.ATOMIC_MOVE);
            install(incoming, root);
            return backup;
        }
    }
    /** Put a backup back (the current set is backed up first, reason restore); the backup itself stays. */
    static Backup restore(File filesDir, Backup backup, long now) throws IOException {
        File staged = staging(filesDir, "restore");
        delete(staged);
        copyTree(backup.saves(), staged);
        try { return replace(filesDir, staged, "restore", now); }
        finally { delete(staged); }
    }

    // ---------- the save folder: one generation folder per sync ----------
    /** The save folder's top directory (TOP) through a provider that may have no atomic rename (Storage Access
     * Framework). Paths are '/'-separated and relative to a generation folder. */
    interface Store {
        /** Names of the folders directly inside TOP. */
        List<String> dirs() throws IOException;
        /** Every file under TOP/dir: relative path -> {size, modification time}. */
        Map<String, long[]> files(String dir) throws IOException;
        InputStream open(String dir, String path) throws IOException;
        void createDir(String dir) throws IOException;
        /** Create TOP/dir/path (a new file; parent folders are created) with source's bytes. */
        void write(String dir, String path, File source) throws IOException;
        void deleteDir(String dir) throws IOException;
    }
    /** The save folder as a real directory (All files access, a picked folder with a real path): plain file I/O instead
     * of the provider. Each file is written to a hidden temporary name, synced and renamed into place, so a file of a
     * generation is either complete or absent; temporary leftovers of a killed upload are never listed. */
    static final class FileStore implements Store {
        private static final String PARTIAL = ".partial";
        final File top;
        FileStore(File top) { this.top = top; }
        private File dir(String dir) throws IOException {
            if (dir.isEmpty() || dir.startsWith("/") || dir.contains("..")) throw new IOException("bad folder name " + dir);
            return new File(top, dir);
        }
        @Override public List<String> dirs() throws IOException {
            File[] children = top.listFiles();
            if (children == null) throw new IOException("cannot list " + top);
            List<String> out = new ArrayList<>();
            for (File c : children) if (c.isDirectory() && !Files.isSymbolicLink(c.toPath())) out.add(c.getName());
            return out;
        }
        @Override public Map<String, long[]> files(String dir) throws IOException {
            Map<String, long[]> out = new HashMap<>();
            list(dir(dir), "", out);
            return out;
        }
        private static void list(File d, String prefix, Map<String, long[]> out) throws IOException {
            File[] children = d.listFiles();
            if (children == null) throw new FileNotFoundException(d.getPath());
            for (File c : children) {
                if (out.size() > MAX_ENTRIES) throw new Invalid("too_many", out.size() + " entries (limit " + MAX_ENTRIES + ")");
                if (Files.isSymbolicLink(c.toPath())) continue;
                if (c.isDirectory()) list(c, prefix + c.getName() + "/", out);
                else if (!(c.getName().startsWith(".") && c.getName().endsWith(PARTIAL))) out.put(prefix + c.getName(), new long[]{c.length(), c.lastModified()});
            }
        }
        @Override public InputStream open(String dir, String path) throws IOException {
            if (path.startsWith("/") || path.contains("..")) throw new IOException("bad file name " + path);
            return new FileInputStream(new File(dir(dir), path));
        }
        @Override public void createDir(String dir) throws IOException {
            File d = dir(dir);
            if (!d.mkdirs() && !d.isDirectory()) throw new IOException("cannot create " + d);
        }
        @Override public void write(String dir, String path, File source) throws IOException {
            if (path.startsWith("/") || path.contains("..")) throw new IOException("bad file name " + path);
            File out = new File(dir(dir), path), parent = out.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            File temp = new File(parent, "." + out.getName() + PARTIAL);
            try (InputStream in = new FileInputStream(source); FileOutputStream o = new FileOutputStream(temp)) {
                in.transferTo(o);
                o.getFD().sync();
            } catch (IOException failed) { temp.delete(); throw failed; }
            Files.move(temp.toPath(), out.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE);
        }
        @Override public void deleteDir(String dir) throws IOException {
            File d = dir(dir);
            delete(d);
            if (d.exists()) throw new IOException("cannot remove " + d);
        }
    }
    /** Every file under base (a picked folder with a real path) as import candidates; symbolic links are listed as
     * links (select() refuses them) and not followed. */
    static List<Candidate> candidates(File base) throws IOException {
        List<Candidate> out = new ArrayList<>();
        collect(base, "", out);
        return out;
    }
    private static void collect(File d, String prefix, List<Candidate> out) throws IOException {
        File[] children = d.listFiles();
        if (children == null) throw new FileNotFoundException(d.getPath());
        for (File c : children) {
            if (out.size() > MAX_ENTRIES) throw new Invalid("too_many", out.size() + " entries (limit " + MAX_ENTRIES + ")");
            String rel = prefix + c.getName();
            if (Files.isSymbolicLink(c.toPath())) out.add(new Candidate(rel, 0, true, 0));
            else if (c.isDirectory()) collect(c, rel + "/", out);
            else out.add(new Candidate(rel, c.length(), false, c.lastModified()));
        }
    }
    /** Validate and stage a picked folder with a real path; file contents are streamed, just like zip extraction. */
    static Selection extractFolder(File base, File staging) throws IOException {
        Selection selection = select(candidates(base));
        delete(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        for (Candidate c : selection.files)
            try (InputStream in = new FileInputStream(new File(base, selection.sources.get(c.path)))) { copyLimited(in, new File(staging, c.path), c.size, c.mtime); }
        if (selection.manifestSource != null)
            try (InputStream in = new FileInputStream(new File(base, selection.manifestSource))) { verifyAgainst(staging, Manifest.read(in)); }
        return selection;
    }
    static final String GENERATION = "sync-";
    private static final Pattern GENERATION_NAME = Pattern.compile("sync-(\\d{1,18})");
    static long generationNumber(String dir) {
        java.util.regex.Matcher m = GENERATION_NAME.matcher(dir);
        return m.matches() ? Long.parseLong(m.group(1)) : -1;
    }
    /** The folder as read: the newest complete generation (its manifest), or incomplete = the newest generation that
     * has a manifest does not match it (a mixed or damaged copy), which must not be used. */
    static final class FolderState {
        String generation; Manifest manifest; boolean incomplete; String problem = "";
        /** The highest generation number present (complete or not); the next push uses one more. */
        long highest;
    }
    /** Each push writes a new sync-NNNNNN folder, its files first and its manifest (every file's size and SHA-256)
     * last, and only then removes the older generations. So a generation without a readable manifest is an upload
     * that never finished: it is skipped and the previous one still stands. A generation whose manifest names files
     * that are missing or have another size is mixed: the folder is reported incomplete and nothing is taken from it. */
    static FolderState readFolder(Store store) throws IOException {
        FolderState s = new FolderState();
        List<String> gens = new ArrayList<>();
        for (String d : store.dirs()) {
            long n = generationNumber(d);
            if (n < 0) continue;
            gens.add(d);
            s.highest = Math.max(s.highest, n);
        }
        gens.sort((a, b) -> Long.compare(generationNumber(b), generationNumber(a)));
        for (String gen : gens) {
            Map<String, long[]> files = store.files(gen);
            if (!files.containsKey(MANIFEST)) continue;
            Manifest m;
            try (InputStream in = store.open(gen, MANIFEST)) { m = Manifest.read(in); }
            catch (Invalid invalid) {
                if (invalid.code.equals("path_bytes") || invalid.code.equals("too_many")) throw invalid;
                continue;   // a malformed manifest write can be an interrupted generation
            }
            for (FileEntry e : m.files) {
                long[] f = files.get(e.path);
                if (f == null || f[0] != e.size) { s.incomplete = true; s.problem = gen + "/" + e.path; return s; }
            }
            s.generation = gen; s.manifest = m;
            return s;
        }
        return s;
    }
    /** Download the current generation into staging (a new directory). verify: every file must match the manifest's
     * size and SHA-256 (Invalid "incomplete" otherwise); the paths pass the same checks as an import. */
    static void fetch(Store store, FolderState s, File staging, boolean verify) throws IOException {
        if (s.manifest == null) throw new Invalid("incomplete", "no complete copy in the folder");
        List<Candidate> candidates = new ArrayList<>();
        for (FileEntry e : s.manifest.files) candidates.add(new Candidate(e.path, e.size, false, e.mtime));
        Selection selection = select(candidates);
        if (!selection.root.isEmpty() || selection.files.size() != s.manifest.files.size()) throw new Invalid("incomplete", "unexpected layout in " + s.generation);
        delete(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        try {
            for (Candidate c : selection.files)
                try (InputStream in = store.open(s.generation, c.path)) { copyLimited(in, new File(staging, c.path), c.size, c.mtime); }
            if (verify) {
                Map<String, FileEntry> want = s.manifest.byPath();
                for (FileEntry got : scan(staging, null))
                    if (!got.sameContent(want.get(got.path))) throw new Invalid("incomplete", s.generation + "/" + got.path + " does not match its manifest");
            }
        } catch (Invalid invalid) {
            delete(staging);
            throw invalid.code.equals("incomplete") ? invalid : new Invalid("incomplete", invalid.getMessage());
        } catch (IOException failed) { delete(staging); throw failed; }
    }
    /** Upload a snapshot as generation highest + 1: files, then the manifest, then remove every other generation
     * (complete, unfinished or mixed). Returns the new generation's name. */
    static String push(Store store, File snapshot, Manifest m, FolderState current) throws IOException {
        m.validate();
        String gen = String.format(Locale.ROOT, GENERATION + "%06d", current.highest + 1);
        store.createDir(gen);
        for (FileEntry e : m.files) store.write(gen, e.path, new File(snapshot, e.path));
        File manifest = new File(snapshot.getParentFile(), snapshot.getName() + "-" + MANIFEST);
        try {
            m.writeAtomicValidated(manifest);
            store.write(gen, MANIFEST, manifest);
        } finally { manifest.delete(); }
        for (String d : store.dirs())
            if (!d.equals(gen) && generationNumber(d) >= 0)
                try { store.deleteDir(d); } catch (IOException kept) { /* harmless: the newest complete generation wins */ }
        return gen;
    }
    /** Slots described by a manifest (no pictures): slot_NN/progress.sav entries and their times. */
    static List<Slot> slots(Manifest m) {
        List<Slot> out = new ArrayList<>();
        if (m == null) return out;
        for (FileEntry e : m.files)
            if (e.path.matches("slot_\\d\\d/progress\\.sav")) out.add(new Slot(Integer.parseInt(e.path.substring(5, 7)), e.mtime, e.mtime, null));
        out.sort((a, b) -> Integer.compare(a.index, b.index));
        return out;
    }

    // ---------- in-game snapshots ----------
    /** The runtime's save signal: state/save-generation (bumped after each completed save) and state/save-busy
     * (present from the first change of a save until its commit has finished). */
    interface Signal { String generation(); boolean busy(); }
    static Signal fileSignal(File filesDir) {
        File state = new File(filesDir, "state");
        return new Signal() {
            @Override public String generation() {
                try { return new String(Files.readAllBytes(new File(state, "save-generation").toPath()), StandardCharsets.US_ASCII).trim(); }
                catch (IOException none) { return ""; }
            }
            @Override public boolean busy() { return new File(state, "save-busy").exists(); }
        };
    }
    /** A snapshot that is known to be one whole save: the generation is recorded and the game must not be saving
     * before the copy, and afterwards the generation must be unchanged and still no save in progress (plus the
     * per-file size/time checks of snapshot()). Returns null when that does not hold: nothing is kept, retry later. */
    static Manifest stableSnapshot(File root, File target, Signal signal, String appVersion, String device, long now) {
        String before = signal.generation();
        if (signal.busy()) return null;
        Manifest m;
        try { m = snapshot(root, target, appVersion, device, now, 1); }
        catch (IOException changed) { delete(target); return null; }
        if (signal.busy() || !before.equals(signal.generation())) { delete(target); return null; }
        return m;
    }

    // ---------- the mirror's decision ----------
    enum Decision { NONE, PUSH, PULL, CONFLICT }
    /** live = the device's saves, folder = the save folder's set (null when it holds none), last = both sides at the
     * last successful sync with this folder (null = never synced with it).
     * In sync, or nothing anywhere: NONE. Empty folder: PUSH. Empty device: PULL (an empty side never overwrites the
     * other, so neither side's saves can be wiped by a sync). First sync with a folder that already holds different
     * saves: CONFLICT. Afterwards whichever side changed since the last sync wins; both changed: CONFLICT. */
    static Decision decide(Manifest live, Manifest folder, Manifest last) {
        boolean liveEmpty = live == null || live.files.isEmpty(), folderEmpty = folder == null || folder.files.isEmpty();
        if (liveEmpty && folderEmpty) return Decision.NONE;
        if (folderEmpty) return Decision.PUSH;
        if (liveEmpty) return Decision.PULL;
        if (live.sameFiles(folder)) return Decision.NONE;
        if (last == null) return Decision.CONFLICT;
        boolean liveChanged = !live.sameFiles(last), folderChanged = !folder.sameFiles(last);
        if (!liveChanged) return folderChanged ? Decision.PULL : Decision.NONE;
        return folderChanged ? Decision.CONFLICT : Decision.PUSH;
    }

}
