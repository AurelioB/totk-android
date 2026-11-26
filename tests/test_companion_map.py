"""Native layer/JSON regressions and production Java map policy with deterministic framework doubles.

Set COMPANION_JSON_JAR to a host org.json jar, or use one already in Gradle's cache.
No Android UI, real bitmap allocation, game data or device behavior is simulated as evidence.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
JAVA = TOTK / 'android/java/com/aure/totk/companion'


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


PRELUDE = r'''
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import org.json.*;
import com.aure.totk.companion.Snapshot;

class Bitmap {
    final String path; final int bytes;
    Bitmap(String path, int bytes) { this.path = path; this.bytes = bytes; }
    int getAllocationByteCount() { return bytes; }
}
class ImageStream extends ByteArrayInputStream {
    final Bitmap bitmap;
    ImageStream(Bitmap bitmap) { super(new byte[0]); this.bitmap = bitmap; }
}
class BitmapFactory {
    static Bitmap decodeStream(InputStream in) { return ((ImageStream)in).bitmap; }
}
// Android LruCache's byte-accounted access-order contract, without allocating image pixels.
class LruCache<K,V> {
    final LinkedHashMap<K,V> values = new LinkedHashMap<>(0, .75f, true);
    int max, size, evictions, hits, misses;
    Object resizeLock;
    LruCache(int max) { this.max = max; }
    protected int sizeOf(K key, V value) { return 1; }
    protected void entryRemoved(boolean evicted, K key, V oldValue, V newValue) { }
    synchronized V get(K key) {
        V value = values.get(key);
        if (value == null) misses++; else hits++;
        return value;
    }
    synchronized int size() { return size; }
    synchronized int hitCount() { return hits; }
    synchronized int missCount() { return misses; }
    synchronized int evictionCount() { return evictions; }
    synchronized int maxSize() {
        if (resizeLock != null && !Thread.holdsLock(resizeLock)) throw new AssertionError("resize check outside shared lock");
        return max;
    }
    synchronized void resize(int next) {
        if (resizeLock != null && !Thread.holdsLock(resizeLock)) throw new AssertionError("resize outside shared lock");
        max = next; trim();
    }
    synchronized V put(K key, V value) {
        V old = values.put(key, value);
        if (old != null) size -= sizeOf(key, old);
        size += sizeOf(key, value);
        if (old != null) entryRemoved(false, key, old, value);
        trim();
        return old;
    }
    void trim() {
        while (size > max && !values.isEmpty()) {
            Map.Entry<K,V> first = values.entrySet().iterator().next();
            size -= sizeOf(first.getKey(), first.getValue()); values.remove(first.getKey()); evictions++;
            entryRemoved(true, first.getKey(), first.getValue(), null);
        }
    }
    synchronized void evictAll() {
        for (Map.Entry<K,V> entry : values.entrySet()) {
            evictions++; entryRemoved(true, entry.getKey(), entry.getValue(), null);
        }
        values.clear(); size = 0;
    }
}
class QueueExecutor {
    final ArrayDeque<Runnable> work = new ArrayDeque<>();
    void execute(Runnable action) { work.add(action); }
    void drain() { while (!work.isEmpty()) work.remove().run(); }
    void shutdownNow() { work.clear(); }
}
class Clock { static volatile long now; }
class TestWorkers {
    static ExecutorService create() {
        return Executors.newSingleThreadExecutor(action -> {
            Thread thread = new Thread(action, "cache-log-probe"); thread.setDaemon(true); return thread;
        });
    }
}
class CompanionNative {
    static final List<String> logs = new ArrayList<>();
    static final Set<Thread> drawThreads = ConcurrentHashMap.newKeySet();
    static Object[] monitors = {};
    static volatile CountDownLatch entered, release;
    static boolean available = true;
    static boolean logCover(String message) {
        CompanionMapProbe.require(!drawThreads.contains(Thread.currentThread()), "native sink on draw thread");
        for (Object monitor : monitors)
            CompanionMapProbe.require(!Thread.holdsLock(monitor), "native sink under cache monitor");
        CompanionMapProbe.require(CacheProbe.CacheLog.visible.get() > 0, "native sink while every companion is hidden");
        CountDownLatch start = entered, finish = release;
        if (start != null) start.countDown();
        if (finish != null) {
            boolean done = false;
            while (!done) try {
                done = finish.await(10, TimeUnit.SECONDS);
                CompanionMapProbe.require(done, "test sink was not released");
            } catch (InterruptedException stopped) { /* native file writes need not respond to Java interruption */ }
        }
        if (!available) return false;
        logs.add(message); return true;
    }
}
class CacheProbe {
    final BitmapCache images, tiles, overviews, decorImages;
    final int tileFloor, tileMarginLimit;
    final int memoryClass;
    long loggedEvictions;
    int loggedFills;
    final long instanceId = CacheLog.ids.incrementAndGet();
    final long[] cacheSnapshot = new long[4 * 9];
    final Runnable cacheLogTask = this::writeCacheLog;
    final AtomicBoolean cacheVisible = new AtomicBoolean();
    volatile boolean cacheFilled;
    final Set<String> pending = new HashSet<>(), missing = new HashSet<>();
    final QueueExecutor decoder = new QueueExecutor();
    final Map<String,Integer> decodes = new HashMap<>();
    Map<String,Object> decorations = Collections.emptyMap();
    Object softPart, softImage, softShader;
    volatile boolean closed;
    int changes;
    CacheProbe(int memory) {
        memoryClass = memory;
'''

CACHE_TESTS = r'''
    }
    InputStream open(String path) throws IOException {
        decodes.merge(path, 1, Integer::sum);
        if (path.contains("missing")) throw new FileNotFoundException(path);
        int bytes = path.contains("oversized") ? 80 * 1024 * 1024 : path.endsWith("overview.webp") ? 19200000
                : path.contains("/1/") ? 840 * 840 * 4 : path.contains("/2/") ? 300 * 300 * 4 : 1024;
        return new ImageStream(new Bitmap(path, bytes));
    }
    void notifyChanged() { changes++; }
    int decoded() { return decodes.values().stream().mapToInt(Integer::intValue).sum(); }
    int evicted() { return images.evictions + tiles.evictions + overviews.evictions + decorImages.evictions; }
    int tileMax() { synchronized (pending) { return tiles.maxSize(); } }
    void checkResizeLock() { tiles.resizeLock = pending; }
    int residentBytes() { return images.size() + tiles.size() + overviews.size() + decorImages.size(); }
    static void stats(BitmapCache cache, String expected) {
        long[] values = new long[9];
        cache.copy(values, 0);
        StringBuilder line = new StringBuilder();
        appendCache(line, "test", values, 0);
        CompanionMapProbe.require(line.toString().equals(" test=" + expected), "counter mismatch: " + line);
        int bytes = cache.values.values().stream().mapToInt(Bitmap::getAllocationByteCount).sum();
        CompanionMapProbe.require(cache.entries == cache.values.size() && cache.size() == bytes, "resident sum");
    }
    static void counterSequence() throws Exception {
        CacheProbe owner = new CacheProbe(256);
        // Match production's mutation discipline, including replacement, trim and clear callbacks.
        synchronized (owner.pending) {
            BitmapCache cache = owner.images;
            cache.resize(10);
            stats(cache, "0,0,10,0,0,0,0,0,0");
            cache.get("missing");
            stats(cache, "0,0,10,0,1,0,0,0,0");
            cache.store("a", new Bitmap("a", 4));
            stats(cache, "1,4,10,0,1,0,0,4,4");
            cache.store("b", new Bitmap("b", 6));
            stats(cache, "2,10,10,0,1,0,0,6,10");
            cache.get("a");
            cache.store("c", new Bitmap("c", 5));
            stats(cache, "2,9,10,1,1,0,1,6,15");
            CompanionMapProbe.require(!cache.values.containsKey("b"), "LRU eviction order changed");
            cache.store("a", new Bitmap("a", 2));
            stats(cache, "2,7,10,1,1,0,1,6,15");
            cache.resize(4);
            stats(cache, "1,2,4,1,1,0,2,6,15");
            cache.evictAll();
            stats(cache, "0,0,4,1,1,0,3,6,15");
        }
        owner.close("script");
        resetLog();
        CacheProbe assets = new CacheProbe(256);
        watch(assets);
        assets.logAndDrain(false, 0);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "hidden creation logged");
        assets.bitmap("missing.webp", false); assets.bitmap("missing.webp", false);
        stats(assets.images, "0,0,22020096,0,2,0,0,0,0");
        assets.decoder.drain();
        assets.bitmap("missing.webp", false);
        stats(assets.images, "0,0,22020096,0,3,1,0,0,0");
        assets.bitmap("icons/a.webp", false); assets.decoder.drain();
        assets.logAndDrain(false, 0);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "first hidden fill logged");
        assets.logAndDrain(true, 0);
        String first = "cache v=2 ms=0 instance=" + assets.instanceId +
                " event=snapshot reason=first_fill memory_class_mib=256 lifecycle=" +
                assets.instanceId + ",created,0,constructor lifecycle_dropped=0" +
                " tiles=0,0,25165824,0,0,0,0,0,0 overview=0,0,25165824,0,0,0,0,0,0" +
                " decor=0,0,50331648,0,0,0,0,0,0 images=1,1024,22020096,0,4,2,0,1024,1024";
        CompanionMapProbe.require(CompanionNative.logs.equals(List.of(first)), "unstable log format: " + CompanionNative.logs);
        assets.bitmap("map/Ground/1/0_0.webp", true);
        assets.bitmap("map/Ground/overview.webp", true);
        assets.bitmap("decor/window.webp", false);
        assets.decoder.drain();
        stats(assets.tiles, "1,2822400,25165824,0,1,1,0,2822400,2822400");
        stats(assets.overviews, "1,19200000,25165824,0,1,1,0,19200000,19200000");
        stats(assets.decorImages, "1,1024,50331648,0,1,1,0,1024,1024");
        assets.logAndDrain(true, 9999);
        CompanionMapProbe.require(CompanionNative.logs.size() == 1, "fill bypassed process limit");
        assets.logAndDrain(true, 10000);
        CompanionMapProbe.require(CompanionNative.logs.get(1).contains("reason=first_fill"), "coalesced fill lost");
        assets.logAndDrain(true, 20000);
        CompanionMapProbe.require(CompanionNative.logs.get(2).contains("reason=periodic"), "periodic line missing");
        for (int n = 1; n < 30; n++) {
            assets.bitmap("map/Ground/1/" + n + "_0.webp", true); assets.decoder.drain();
        }
        assets.logAndDrain(true, 29999);
        CompanionMapProbe.require(CompanionNative.logs.size() == 3, "evictions bypassed process limit");
        assets.logAndDrain(false, 30000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 3, "hidden eviction logged");
        assets.logAndDrain(true, 30000);
        CompanionMapProbe.require(CompanionNative.logs.get(3).contains("reason=eviction_burst"), "burst lost");
        stats(assets.tiles, "8,22579200,25165824,0,30,30,22,2822400,25401600");
        assets.logAndDrain(true, 0);
        CompanionMapProbe.require(CompanionNative.logs.size() == 4, "backward clock reopened limit");
        assets.close("script");
        assets.logAndDrain(true, 50000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 4, "closed store logged");
        CompanionMapProbe.require(assets.tiles.entries == 0 && assets.overviews.entries == 0 &&
                assets.decorImages.entries == 0 && assets.images.entries == 0, "close entry accounting");
        processLifetimes();
        asyncLogging();
        boundedHistory();
    }
    static void drainLogs() throws Exception { CacheLog.worker.submit(() -> { }).get(10, TimeUnit.SECONDS); }
    static void resetLog() throws Exception {
        drainLogs();
        CompanionMapProbe.require(CacheLog.visible.get() == 0 && !CacheLog.queued.get(), "live test store escaped");
        synchronized (CacheLog.events) {
            CacheLog.head = CacheLog.count = CacheLog.logCount = 0;
            CacheLog.dropped = CacheLog.logDropped = 0; CacheLog.eventsPending = false;
            Arrays.fill(CacheLog.eventReasons, null);
        }
        CacheLog.nextSnapshotAt = CacheLog.nextLifecycleAt = 0;
        Clock.now = 0; CompanionNative.logs.clear(); CompanionNative.available = true;
        CompanionNative.entered = null; CompanionNative.release = null;
    }
    static void watch(CacheProbe... stores) {
        CompanionNative.drawThreads.add(Thread.currentThread());
        Object[] monitors = new Object[stores.length * 5 + 1];
        monitors[0] = CacheLog.events;
        for (int n = 0; n < stores.length; n++) {
            CacheProbe store = stores[n];
            Object[] own = {store.pending, store.tiles, store.overviews, store.decorImages, store.images};
            System.arraycopy(own, 0, monitors, n * 5 + 1, 5);
        }
        CompanionNative.monitors = monitors;
    }
    void logAndDrain(boolean visible, long now) throws Exception {
        Clock.now = now; logCaches(visible, now); drainLogs();
    }
    static long lines(String event) {
        return CompanionNative.logs.stream().filter(line -> line.contains(" event=" + event + " ")).count();
    }
    static String latest() { return CompanionNative.logs.get(CompanionNative.logs.size() - 1); }
    static void checkRates() {
        for (String kind : new String[]{"snapshot", "lifecycle"}) {
            long previous = -10000;
            for (String line : CompanionNative.logs) if (line.contains(" event=" + kind + " ")) {
                long now = Long.parseLong(line.split(" ms=")[1].split(" ")[0]);
                CompanionMapProbe.require(now - previous >= 10000, "process-wide " + kind + " rate: " + line);
                previous = now;
            }
        }
    }
    static void processLifetimes() throws Exception {
        resetLog();
        CacheProbe first = new CacheProbe(256);
        first.bitmap("icons/a.webp", false); first.decoder.drain();
        watch(first); first.logAndDrain(true, 0);
        Clock.now = 200;
        CacheProbe second = new CacheProbe(256);
        second.bitmap("icons/b.webp", false); second.decoder.drain();
        watch(first, second); first.close("view_detached");
        second.logAndDrain(true, 200);
        CompanionMapProbe.require(second.instanceId > first.instanceId && lines("snapshot") == 1 &&
                lines("lifecycle") == 1, "two stores bypassed process bounds");
        CompanionMapProbe.require(latest().contains("instance=" + second.instanceId + " ") &&
                latest().contains(second.instanceId + ",created,200,constructor") &&
                latest().contains(first.instanceId + ",closed,200,view_detached"), "rapid recreation not attributable");
        Clock.now = 400;
        CacheProbe third = new CacheProbe(256);
        watch(first, second, third); second.close("view_detached"); second.close("duplicate");
        third.logAndDrain(true, 400);
        CompanionMapProbe.require(CompanionNative.logs.size() == 2, "lifecycle allowance was unbounded");
        third.logAndDrain(true, 10000);
        CompanionMapProbe.require(latest().contains(third.instanceId + ",created,400,constructor") &&
                latest().contains(second.instanceId + ",closed,400,view_detached") &&
                !latest().contains("duplicate") && latest().contains("lifecycle_dropped=0"), "coalesced lifecycle lost");
        third.close("sleep");
        int before = CompanionNative.logs.size();
        Clock.now = 20000; CacheProbe hidden = new CacheProbe(256); hidden.close("hidden");
        hidden.logAndDrain(false, 20000);
        CompanionMapProbe.require(CompanionNative.logs.size() == before, "hidden create/close logged");
        Clock.now = 22000; CacheProbe awake = new CacheProbe(256); watch(awake);
        awake.logAndDrain(true, 22000);
        CompanionMapProbe.require(latest().contains(hidden.instanceId + ",created,20000,constructor") &&
                latest().contains(hidden.instanceId + ",closed,20000,hidden") &&
                latest().contains(third.instanceId + ",closed,10000,sleep"), "hidden journal not deferred to wake");
        checkRates(); awake.close("end");
    }
    static void offThread(Runnable work) throws Exception {
        FutureTask<Void> task = new FutureTask<>(work, null);
        Thread thread = new Thread(task, "draw-or-cache-probe");
        thread.setDaemon(true); thread.start(); task.get(10, TimeUnit.SECONDS);
    }
    static void asyncLogging() throws Exception {
        resetLog();
        CacheProbe assets = new CacheProbe(256); watch(assets);
        assets.bitmap("icons/a.webp", false); assets.decoder.drain();
        synchronized (assets.pending) {
            offThread(() -> {
                CompanionNative.drawThreads.add(Thread.currentThread());
                Clock.now = 0; assets.logCaches(true, 0);
            });
            assets.setCacheVisible(false);
        }
        drainLogs();
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "queued hidden snapshot logged");
        CompanionNative.available = false;
        assets.logAndDrain(true, 10000); // snapshot attempt
        assets.logAndDrain(true, 10001); // separate lifecycle allowance, also unavailable
        CompanionNative.available = true;
        assets.logAndDrain(true, 19999);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "unavailable attempts bypassed both limits");
        assets.logAndDrain(true, 20000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 1 && latest().contains("reason=first_fill") &&
                latest().contains(assets.instanceId + ",created,0,constructor"), "unavailable bridge lost attribution");
        CompanionNative.entered = new CountDownLatch(1); CompanionNative.release = new CountDownLatch(1);
        CacheProbe replacement;
        try {
            Clock.now = 30000; assets.logCaches(true, 30000);
            CompanionMapProbe.require(CompanionNative.entered.await(10, TimeUnit.SECONDS), "sink did not start");
            Clock.now = 30200; replacement = new CacheProbe(256); watch(assets, replacement);
            offThread(() -> {
                CompanionNative.drawThreads.add(Thread.currentThread());
                assets.bitmap("icons/b.webp", false); assets.decoder.drain();
                CompanionMapProbe.require(assets.residentBytes() == 2048, "cache publication waited for sink");
                assets.close("view_detached"); // does not interrupt or shut down the shared writer
                replacement.bitmap("icons/c.webp", false); replacement.decoder.drain();
                for (int n = 0; n < 50; n++) replacement.logCaches(true, 100000);
            });
            Clock.now = 100000;
        } finally { CompanionNative.release.countDown(); }
        drainLogs(); CompanionNative.entered = null; CompanionNative.release = null;
        CompanionMapProbe.require(CompanionNative.logs.size() == 2 &&
                latest().contains("instance=" + assets.instanceId + " ") &&
                latest().contains("images=1,1024,"), "old snapshot changed or queued a backlog");
        replacement.logAndDrain(true, 100000);
        CompanionMapProbe.require(lines("snapshot") == 2 && lines("lifecycle") == 1 &&
                latest().contains(assets.instanceId + ",closed,30200,view_detached"), "shared writer lost replacement");
        replacement.logAndDrain(true, 109999);
        CompanionMapProbe.require(CompanionNative.logs.size() == 3, "slow completion reopened a channel");
        replacement.logAndDrain(true, 110000);
        CompanionMapProbe.require(lines("snapshot") == 3 && latest().contains("reason=first_fill"), "new store never resumed");
        checkRates(); replacement.close("end");
        int count = CompanionNative.logs.size();
        Clock.now = 120000; CacheProbe canceled = new CacheProbe(256); watch(canceled);
        canceled.bitmap("icons/a.webp", false); canceled.decoder.drain();
        synchronized (canceled.pending) {
            canceled.logCaches(true, 120000); canceled.close("queued_shutdown");
        }
        drainLogs();
        CompanionMapProbe.require(CompanionNative.logs.size() == count, "queued shutdown snapshot logged");
    }
    static void boundedHistory() throws Exception {
        resetLog();
        for (int n = 0; n < 33; n++) new CacheProbe(256).close("hidden");
        CompanionMapProbe.require(CacheLog.count == 32 && CacheLog.dropped == 34 &&
                CompanionNative.logs.isEmpty(), "hidden history was unbounded or logged");
        Clock.now = 10000; CacheProbe visible = new CacheProbe(256); watch(visible);
        visible.logAndDrain(true, 10000);
        CompanionMapProbe.require(latest().contains("lifecycle_dropped=35"), "history overflow was concealed");
        CompanionMapProbe.require(latest().split(" lifecycle=")[1].split(" lifecycle_dropped=")[0].split(";").length == 32,
                "lifecycle batch size was not bounded");
        visible.close("end");
    }
}
class RectF {
    float left, top, right, bottom;
    RectF(float width, float height) { right = width; bottom = height; }
    void set(float a, float b, float c, float d) { left = a; top = b; right = c; bottom = d; }
}
class Canvas {
    final Set<String> drawn = new HashSet<>();
    void drawBitmap(Bitmap image, Object source, RectF tile, Object paint) { drawn.add(image.path); }
}
class MapProbe {
    final CacheProbe assets;
    final RectF bounds, tile = new RectF(0, 0);
    final Canvas canvas = new Canvas();
    final Object paint = new Object();
    final JSONObject map = new JSONObject();
    double minX = -6000, maxX = 6000, minZ = -5000, maxZ = 5000, centerX, centerZ, anchorX = Double.NaN, anchorZ;
    float cx, cy, pixelsPerWorld;
    String layer = "Ground";
    MapProbe(CacheProbe assets) { this(assets, 1280, 960); }
    MapProbe(CacheProbe assets, float width, float height) {
        this.assets = assets; bounds = new RectF(width, height); cx = width / 2; cy = height / 2;
    }
    /** Pin the viewport's top-left world point so it straddles tile boundaries at any zoom. */
    MapProbe at(double left, double top) { anchorX = left; anchorZ = top; return this; }
    float x(double v) { return cx + (float)(v - centerX) * pixelsPerWorld; }
    float y(double v) { return cy + (float)(v - centerZ) * pixelsPerWorld; }
    void draw(float pixels) {
        pixelsPerWorld = pixels / 1000;
        if (!Double.isNaN(anchorX)) { centerX = anchorX + cx / pixelsPerWorld; centerZ = anchorZ + cy / pixelsPerWorld; }
        canvas.drawn.clear();
        assets.bitmap("map/" + layer + "/overview.webp", true);
'''

MAP_TESTS = r'''
    }
    void settle(float pixels) { settle(pixels, 8); }
    void settle(float pixels, int tiles) {
        for (int n = 0; n < 4; n++) { draw(pixels); assets.decoder.drain(); }
        int decoded = assets.decoded(), evicted = assets.evicted();
        for (int n = 0; n < 20; n++) { draw(pixels); assets.decoder.drain(); }
        CompanionMapProbe.require(assets.decoded() == decoded && assets.evicted() == evicted,
                "stationary viewport kept decoding/evicting at " + pixels + " on " + layer);
        CompanionMapProbe.require(canvas.drawn.size() == tiles,
                "stationary viewport drew " + canvas.drawn.size() + " of " + tiles + " tiles");
        for (String name : canvas.drawn)
            CompanionMapProbe.require(assets.cachedTile(name) != null, "visible tile not retained: " + name);
    }
}
public class CompanionMapProbe {
    Snapshot data;
    static void require(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
'''

TEST_MAIN = r'''
    public static void main(String[] args) throws Exception {
        CacheProbe.counterSequence();
        CompanionMapProbe probe = new CompanionMapProbe();
        String[] expected = {"1234, 1234, 0000", "-1234, -1234, 0000", "0001, 0001, -0107", "8000, 8000, 7894"};
        List<String> lines = Files.readAllLines(Path.of(args[0]));
        require(lines.size() == expected.length, "native roundtrip fixtures missing");
        for (int n = 0; n < expected.length; n++) {
            probe.data = Snapshot.parse(lines.get(n), null, true);
            require(probe.coordinates().equals(expected[n]), "coordinate " + n + ": " + probe.coordinates());
        }
        probe.data = Snapshot.waiting(); require(probe.coordinates().equals("Position unavailable"), "missing position");
        for (int memory : new int[]{96, 128, 192, 256, 384, 512, 1024}) {
            CacheProbe assets = new CacheProbe(memory);
            // Both the UI budget and decoder's oversized growth must hold the very same lock at check and resize.
            assets.checkResizeLock();
            require(assets.tileMarginLimit == Math.min(64, Math.max(32, memory / 8)) * 1024 * 1024, "class margin limit");
            MapProbe map = new MapProbe(assets);
            map.draw(501); require(assets.decoded() == 0, "draw decoded synchronously");
            require(!assets.pending.isEmpty(), "cold miss did not queue");
            map.settle(501); // 4 x 2 detailed tiles plus the separate overview, larger than the old cache.
            map.draw(500);
            require(map.canvas.drawn.stream().allMatch(p -> p.contains("/1/")), "LOD miss did not use cached other LOD");
            map.settle(500);
            map.settle(501);
            for (String layer : new String[]{"Sky", "UnderGround", "Ground"}) {
                map.layer = layer; map.settle(501);
                require(map.canvas.drawn.stream().allMatch(p -> p.startsWith("map/" + layer + "/")), "wrong-layer fallback");
            }
            assets.bitmap("missing.webp", false); assets.decoder.drain();
            assets.bitmap("missing.webp", false); assets.decoder.drain();
            require(assets.decodes.get("missing.webp") == 1, "missing image retried");
            for (String name : new String[]{"oversized.webp", "decor/oversized.webp", "map/Sky/oversized/overview.webp", "map/Sky/1/oversized.webp"}) {
                assets.bitmap(name, name.startsWith("map/")); assets.decoder.drain();
                require(assets.bitmap(name, name.startsWith("map/")) != null, "oversized image immediately evicted");
            }
            int grown = assets.tileMax(); assets.tileBudget(12L * 840 * 840 * 4, 840L * 840 * 4);
            require(assets.tileMax() == grown && grown == 81 * 1024 * 1024, "budget shrank oversized exception");
            assets.close("policy_test");
            require(assets.residentBytes() == 0, "close did not clear every cache");
            new MapProbe(new CacheProbe(memory)).settle(501);
            // Panels whose map area straddles 4 x 3 (1167 x 757 map area of a 1240 x 1080 panel) and 5 x 3
            // (1920-wide) LOD-1 tiles just above the 500 px switch: 12 or 15 tiles exceed a 32 MiB class limit.
            for (float pixels : new float[]{500.1f, 501}) {
                CacheProbe four = new CacheProbe(memory), five = new CacheProbe(memory);
                new MapProbe(four, 1167, 757).at(-1200, -400).settle(pixels, 12);
                new MapProbe(five, 1920, 800).at(-1200, -400).settle(pixels, 15);
                require(four.tileMax() >= 13 * 840 * 840 * 4 && five.tileMax() >= 16 * 840 * 840 * 4,
                        "visible tiles plus one were capped by memory class " + memory);
            }
        }
        System.out.println("pass: companion map native JSON roundtrip and cache policy");
    }
}
'''


class CompanionMapTests(unittest.TestCase):
    def test_native_layer_json_and_java_cache_policy(self):
        cc, javac, java = shutil.which('cc'), shutil.which('javac'), shutil.which('java')
        if not cc or not javac or not java:
            self.skipTest('C compiler and JDK 17+ required')
        jars = ([Path(os.environ['COMPANION_JSON_JAR'])] if os.environ.get('COMPANION_JSON_JAR') else
                sorted((Path.home() / '.gradle/caches/modules-2/files-2.1/org.json/json').glob('*/*/json-*.jar')))
        if not jars:
            self.skipTest('set COMPANION_JSON_JAR to a host org.json jar')
        with tempfile.TemporaryDirectory(prefix='companion-map-') as directory:
            work = Path(directory)
            native = self.command(['make', '-C', TOTK / 'android', 'companiontest', 'TARGET=linux', f'CC={cc}', f'OUT={work}'])
            self.assertIn('built', native.stdout)
            result = self.command([work / 'companiontest'])
            fixtures = work / 'coordinates.jsonl'
            fixtures.write_text('\n'.join(line.split(': ', 1)[1] for line in result.stdout.splitlines()
                                          if line.startswith('coordinate-roundtrip: ')) + '\n')
            cache = (JAVA / 'AssetStore.java').read_text()
            renderer = (JAVA / 'MapRenderer.java').read_text()
            view = (JAVA / 'CompanionView.java').read_text()
            source = PRELUDE + section(cache, '        int budget =', '        decoder.execute(() -> {') + CACHE_TESTS
            # Insert the production methods before closing CacheProbe; no policy formula is duplicated in the harness.
            insertion = source.index('\n}\nclass RectF')
            source = (source[:insertion] + '\n' + section(cache, '    private static final class CacheLog', '    private JSONObject json(')
                      + section(cache, '    public Bitmap bitmap(String path, boolean tile)', '    private void notifyChanged()')
                      + section(cache, '    public void close(String reason)', '\n}') + source[insertion:])
            source += section(renderer, '        int cols =', '        JSONObject dynamic =') + MAP_TESTS
            source += section(view, '    private String coordinates()', '    private static String number(') + TEST_MAIN
            # The viewport body uses this exact production decoded-size constant.
            source = source.replace('class MapProbe {', 'class MapProbe {\n' + section(renderer, '    private static final long[] TILE_BYTES', '\n\n'))
            source = source.replace('android.os.SystemClock.elapsedRealtime()', 'Clock.now')
            source = source.replace('Executors.newSingleThreadExecutor()', 'TestWorkers.create()')
            # The harness exposes the nested writer only for the logger stub's visibility assertion.
            source = source.replace('private static final class CacheLog', 'static final class CacheLog')
            self.assertIn('assets.logCaches(isShown() && getWindowVisibility() == VISIBLE, android.os.SystemClock.elapsedRealtime());', view)
            self.assertIn('assets.setCacheVisible(isShown() && getWindowVisibility() == VISIBLE);', view)
            self.assertIn('assets.setCacheVisible(isShown() && visibility == VISIBLE);', view)
            snapshot = section(cache, '                // Copy only primitives', '            if (closed || !cacheVisible.get()) return;')
            self.assertNotIn('new ', snapshot)
            self.assertNotIn('append', snapshot)
            self.assertNotIn('logCover', snapshot)
            probe = work / 'CompanionMapProbe.java'
            probe.write_text(source)
            self.command([javac, '--release', '17', '-cp', jars[-1], '-d', work, JAVA / 'Snapshot.java', probe])
            ran = self.command([java, '-ea', '-cp', os.pathsep.join(map(str, (work, jars[-1]))), 'CompanionMapProbe', fixtures])
            self.assertIn('pass: companion map native JSON roundtrip and cache policy', ran.stdout)

    @staticmethod
    def command(args):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result


if __name__ == '__main__':
    unittest.main()
