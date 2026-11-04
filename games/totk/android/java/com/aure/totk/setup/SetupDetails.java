package com.aure.totk.setup;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.SeekableByteChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

/** Plain-text setup diagnostics. No directory walk: these are the only diagnostic files setup writes.
 * Reads are bounded too, including for a damaged or unexpectedly large log. No Android dependencies (host-tested). */
final class SetupDetails {
    static final int MAX_BYTES = 64 * 1024;
    static final String PRIVACY = "No game files or keys are included; nothing is sent until you choose an app.";
    private static final Pattern HEX = Pattern.compile("[0-9a-fA-F]{32,}");
    private static final Pattern URI = Pattern.compile("[a-zA-Z][a-zA-Z0-9+.-]*://[^\\s\"<>]+");
    private static final Pattern PATH = Pattern.compile("(?<![\\w.])/[^\\s\"<>]+");

    private SetupDetails() { }

    static String extension(String name) {
        int dot = name.lastIndexOf('.');
        String suffix = dot < 0 ? "" : name.substring(dot);
        return suffix.matches("\\.[a-zA-Z0-9]{1,12}") ? suffix.toLowerCase(Locale.ROOT) : "(none)";
    }

    static String build(String metadata, File game, String stage, List<String> excluded) {
        String intro = "TotK Native setup details\n" + PRIVACY + "\n"
                + "Share details before Resume to include the logs.\n\n";
        intro += bounded(scrub(metadata, excluded), 8192) + "\n";
        String[] names = "shaders".equals(stage)
                ? new String[]{"hactool-romfs.log", "hactool-exefs.log", "shaders.log"}
                : new String[]{"hactool-romfs.log", "hactool-exefs.log"};
        // Reserve labels/newlines; every allowed log gets its own head/tail budget, even if another is huge.
        int budget = (MAX_BYTES - bytes(intro) - 512) / names.length;
        StringBuilder report = new StringBuilder(intro);
        File staging = new File(game, ".import");
        for (String name : names) {
            report.append("\n--- .import/").append(name).append(" ---\n");
            report.append(log(staging, name, budget, excluded)).append('\n');
        }
        // Defense in depth: hactool already scrubs >=32-character hex runs before writing its logs.
        return HEX.matcher(report).replaceAll("<hex>");
    }

    private static String log(File staging, String name, int budget, List<String> excluded) {
        File file = new File(staging, name);
        if (Files.isSymbolicLink(staging.toPath()) || Files.isSymbolicLink(file.toPath())) return "[not included: symbolic link]";
        if (!Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return "[missing]";
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return "[not included: not a regular log]";
        try (SeekableByteChannel channel = Files.newByteChannel(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long size = channel.size();
            // UTF-8 replacement characters can expand invalid bytes threefold. Leave room for that and the marker.
            int keep = (budget - 128) / 3;
            if (size <= keep) return bounded(scrub(decode(read(channel, (int) size)), excluded), budget);
            byte[] head = read(channel, keep / 2);
            channel.position(size - (keep - head.length));
            byte[] tail = read(channel, keep - head.length);
            int h = head.length, t = 0;
            // A cut inside a long hex value must not leak its short, otherwise unredacted edge.
            while (h > 0 && hex(head[h - 1])) h--;
            while (t < tail.length && hex(tail[t])) t++;
            String text = decode(java.util.Arrays.copyOf(head, h))
                    + omitted(size - h - (tail.length - t))
                    + decode(java.util.Arrays.copyOfRange(tail, t, tail.length));
            return bounded(scrub(text, excluded), budget);
        } catch (IOException | SecurityException unavailable) {
            return "[unavailable: could not read log]";
        }
    }

    private static byte[] read(SeekableByteChannel channel, int count) throws IOException {
        ByteBuffer buffer = ByteBuffer.allocate(count);
        while (buffer.hasRemaining() && channel.read(buffer) != -1) { }
        return java.util.Arrays.copyOf(buffer.array(), buffer.position());
    }
    private static boolean hex(byte b) {
        return b >= '0' && b <= '9' || b >= 'a' && b <= 'f' || b >= 'A' && b <= 'F';
    }
    private static String decode(byte[] bytes) { return new String(bytes, StandardCharsets.UTF_8); }
    private static int bytes(String text) { return text.getBytes(StandardCharsets.UTF_8).length; }
    private static String omitted(long count) { return "\n[… " + count + " bytes omitted …]\n"; }

    /** All untrusted text, not just logs, is scrubbed. Do not print exception messages (they can contain paths). */
    private static String scrub(String text, List<String> excluded) {
        for (String value : excluded) if (value != null && !value.isEmpty()) text = text.replace(value, "<input>");
        text = URI.matcher(text).replaceAll("<uri>");
        text = PATH.matcher(text).replaceAll("<path>");
        return HEX.matcher(text).replaceAll("<hex>");
    }

    private static String bounded(String text, int limit) {
        byte[] all = text.getBytes(StandardCharsets.UTF_8);
        if (all.length <= limit) return text;
        int keep = (limit - 128) / 3, head = keep / 2, tail = all.length - (keep - head);
        while (head > 0 && hex(all[head - 1])) head--;
        while (tail < all.length && hex(all[tail])) tail++;
        return decode(java.util.Arrays.copyOf(all, head)) + omitted(tail - head)
                + decode(java.util.Arrays.copyOfRange(all, tail, all.length));
    }
}
