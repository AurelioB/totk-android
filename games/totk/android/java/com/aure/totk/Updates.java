package com.aure.totk;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** No automatic GET, redirect, retry, telemetry or body read: only one bounded HEAD on a validated connection. */
public final class Updates {
    private Updates() { }
    public static final class Source {
        public final URI base;
        public final boolean override;
        Source(URI base, boolean override) { this.base = base; this.override = override; }
        public URI latest() { return base.resolve(UpdateRules.PROJECT + "/releases/latest"); }
        public URI asset(String version, boolean checksum) {
            return base.resolve(UpdateRules.PROJECT + "/releases/download/v" + version + "/" + UpdateRules.asset(version) + (checksum ? ".sha256" : ""));
        }
    }
    public static Source source(Context context) throws IOException {
        File override = new File(context.getFilesDir(), "update-source.txt");
        if (!override.exists()) return new Source(URI.create("https://github.com"), false);
        try {
            URI uri = new URI(UpdateRules.read(context.getFilesDir(), "update-source.txt"));
            if ((!"http".equals(uri.getScheme()) && !"https".equals(uri.getScheme())) || uri.getHost() == null
                    || uri.getRawUserInfo() != null || uri.getRawQuery() != null || uri.getRawFragment() != null
                    || !(uri.getRawPath().isEmpty() || uri.getRawPath().equals("/"))) throw new IOException("Invalid private update source");
            return new Source(uri, true);
        } catch (java.net.URISyntaxException malformed) { throw new IOException("Invalid private update source", malformed); }
    }
    public static String version(Context context) {
        try { return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (android.content.pm.PackageManager.NameNotFoundException impossible) { return ""; }
    }
    public static boolean online(Context context) {
        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
        NetworkCapabilities caps = manager == null ? null : manager.getNetworkCapabilities(manager.getActiveNetwork());
        return caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
    }
    public static void check(Context context, Consumer<String> result) {
        Context app = context.getApplicationContext();
        Handler main = new Handler(Looper.getMainLooper());
        AtomicBoolean finished = new AtomicBoolean();
        Transfer transfer = new Transfer();
        // Includes DNS, connect and read time, not merely each socket operation. A late worker never posts a prompt.
        Runnable deadline = () -> { if (finished.compareAndSet(false, true)) { transfer.cancel(); result.accept(null); } };
        main.postDelayed(deadline, 5000);
        Thread worker = new Thread(() -> {
            String answer = null;
            try {
                Source source = source(app);
                answer = UpdateRules.check(UpdateRules.enabled(app.getFilesDir()), online(app), version(app), app.getPackageName(),
                        source.override, source.base, () -> {
                            HttpURLConnection connection = transfer.connect(source.latest(), "HEAD", 2400);
                            try {
                                int status = connection.getResponseCode();
                                return status == 302 ? connection.getHeaderField("Location") : null;
                            } finally { transfer.release(connection); }
                        });
                if (answer != null && !finished.get()) UpdateRules.setPending(app.getFilesDir(), answer);
            } catch (IOException | RuntimeException ignored) { }
            String value = answer;
            main.post(() -> { if (finished.compareAndSet(false, true)) { main.removeCallbacks(deadline); result.accept(value); } });
        }, "update-check");
        worker.setDaemon(true);
        worker.start();
    }
    /** Cancellation closes the current socket, including one waiting for response headers. */
    public static final class Transfer {
        private volatile boolean cancelled;
        private volatile HttpURLConnection active;
        public void cancel() { cancelled = true; HttpURLConnection c = active; if (c != null) c.disconnect(); }
        public void checkCancelled() throws IOException { if (cancelled) throw new IOException("Update cancelled."); }
        HttpURLConnection connect(URI uri, String method, int timeout) throws IOException {
            checkCancelled();
            HttpURLConnection c = (HttpURLConnection) uri.toURL().openConnection();
            active = c;
            c.setInstanceFollowRedirects(false);
            c.setConnectTimeout(timeout); c.setReadTimeout(timeout);
            c.setRequestProperty("User-Agent", "TotK-Native-Updater");
            c.setRequestMethod(method);
            c.setUseCaches(false);
            checkCancelled();
            return c;
        }
        void release(HttpURLConnection c) { c.disconnect(); if (active == c) active = null; }
        public void download(Source source, URI start, File target, long maxBytes, Progress progress) throws IOException {
            URI uri = start;
            for (int redirects = 0; redirects <= 8; redirects++) {
                if (!UpdateRules.allowedDownload(uri, source.base, source.override)) throw new IOException("Unsafe update download address.");
                HttpURLConnection c = connect(uri, "GET", 15000);
                try {
                    int status = c.getResponseCode();
                    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                        String location = c.getHeaderField("Location");
                        if (location == null) throw new IOException("Missing download redirect.");
                        try { uri = uri.resolve(location); } catch (IllegalArgumentException bad) { throw new IOException("Invalid download redirect.", bad); }
                        continue;
                    }
                    if (status != 200) throw new IOException("Update download failed (HTTP " + status + ").");
                    long total = c.getContentLengthLong(), count = 0;
                    if (total > maxBytes) throw new IOException("Update download is too large.");
                    try (InputStream in = c.getInputStream(); FileOutputStream out = new FileOutputStream(target)) {
                        byte[] bytes = new byte[128 * 1024];
                        int n;
                        while ((n = in.read(bytes)) != -1) {
                            checkCancelled(); count += n;
                            if (count > maxBytes) throw new IOException("Update download is too large.");
                            out.write(bytes, 0, n); progress.changed(count, total);
                        }
                        if (total >= 0 && count != total) throw new IOException("The update download was incomplete.");
                    }
                    return;
                } finally { release(c); }
            }
            throw new IOException("Too many update download redirects.");
        }
    }
    public interface Progress { void changed(long bytes, long total); }
    public static String digest(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new java.io.FileInputStream(file)) {
            byte[] bytes = new byte[128 * 1024]; int n;
            while ((n = in.read(bytes)) != -1) digest.update(bytes, 0, n);
        }
        return hex(digest.digest());
    }
    public static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) text.append(Character.forDigit((b >>> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return text.toString();
    }
}
