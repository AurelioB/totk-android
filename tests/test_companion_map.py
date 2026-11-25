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
    long cacheLogTime, loggedEvictions;
    int loggedFills;
    boolean cacheLogged;
    final ExecutorService cacheLogger = Executors.newSingleThreadExecutor(action -> {
        Thread thread = new Thread(action, "cache-log-probe"); thread.setDaemon(true); return thread;
    });
    final AtomicBoolean cacheLogQueued = new AtomicBoolean();
    final long[] cacheSnapshot = new long[4 * 9];
    final Runnable cacheLogTask = this::writeCacheLog;
    volatile boolean cacheVisible, cacheFilled;
    volatile long nextCacheLogAt;
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
        owner.close();

        CacheProbe assets = new CacheProbe(256);
        CompanionNative.drawThreads.add(Thread.currentThread());
        CompanionNative.monitors = new Object[]{assets.pending, assets.tiles, assets.overviews, assets.decorImages, assets.images};
        CompanionNative.logs.clear();
        assets.logAndDrain(false, 0); assets.logAndDrain(true, 0);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "empty or hidden cache logged");
        assets.bitmap("missing.webp", false); assets.bitmap("missing.webp", false);
        stats(assets.images, "0,0,22020096,0,2,0,0,0,0");
        assets.decoder.drain();
        assets.bitmap("missing.webp", false);
        stats(assets.images, "0,0,22020096,0,3,1,0,0,0");
        assets.bitmap("icons/a.webp", false); assets.decoder.drain();
        assets.logAndDrain(false, 0);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "first hidden fill logged");
        assets.logAndDrain(true, 0);
        String first = "cache v=1 ms=0 reason=first_fill memory_class_mib=256" +
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
        CompanionMapProbe.require(CompanionNative.logs.size() == 1, "fill bypassed ten-second limit");
        assets.logAndDrain(true, 10000);
        CompanionMapProbe.require(CompanionNative.logs.get(1).contains("reason=first_fill"), "coalesced fill lost");
        assets.logAndDrain(true, 20000);
        CompanionMapProbe.require(CompanionNative.logs.get(2).contains("reason=periodic"), "periodic line missing");
        for (int n = 1; n < 30; n++) {
            assets.bitmap("map/Ground/1/" + n + "_0.webp", true); assets.decoder.drain();
        }
        assets.logAndDrain(true, 29999);
        CompanionMapProbe.require(CompanionNative.logs.size() == 3, "evictions bypassed ten-second limit");
        assets.logAndDrain(false, 30000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 3, "hidden eviction logged");
        assets.logAndDrain(true, 30000);
        CompanionMapProbe.require(CompanionNative.logs.get(3).contains("reason=eviction_burst"), "burst lost");
        stats(assets.tiles, "8,22579200,25165824,0,30,30,22,2822400,25401600");
        assets.logAndDrain(true, 0);
        CompanionMapProbe.require(CompanionNative.logs.size() == 4, "backward clock reopened limit");
        assets.close();
        assets.logCaches(true, 50000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 4, "closed store logged");
        CompanionMapProbe.require(assets.tiles.entries == 0 && assets.overviews.entries == 0 &&
                assets.decorImages.entries == 0 && assets.images.entries == 0, "close entry accounting");
        assets.awaitLoggerStop();
        asyncLogging();
    }
    void logAndDrain(boolean visible, long now) throws Exception {
        Clock.now = now;
        logCaches(visible, now);
        cacheLogger.submit(() -> { }).get(10, TimeUnit.SECONDS);
    }
    void awaitLoggerStop() throws Exception {
        CompanionMapProbe.require(cacheLogger.awaitTermination(10, TimeUnit.SECONDS), "logger did not stop");
    }
    static void offThread(Runnable work) throws Exception {
        FutureTask<Void> task = new FutureTask<>(work, null);
        Thread thread = new Thread(task, "draw-or-cache-probe");
        thread.setDaemon(true); thread.start();
        task.get(10, TimeUnit.SECONDS);
    }
    static void asyncLogging() throws Exception {
        CacheProbe assets = new CacheProbe(256);
        CompanionNative.logs.clear();
        CompanionNative.monitors = new Object[]{assets.pending, assets.tiles, assets.overviews, assets.decorImages, assets.images};
        assets.bitmap("icons/a.webp", false); assets.decoder.drain();
        // Admission must return even while another thread owns the cache mutation monitor.
        synchronized (assets.pending) {
            offThread(() -> {
                CompanionNative.drawThreads.add(Thread.currentThread());
                Clock.now = 0; assets.logCaches(true, 0);
            });
            assets.setCacheVisible(false); // queued snapshot must not log after the view/window hides
        }
        assets.cacheLogger.submit(() -> { }).get(10, TimeUnit.SECONDS);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "queued hidden snapshot logged");

        // An unavailable native bridge consumes an attempt interval, but does not lose first-fill attribution.
        CompanionNative.available = false;
        assets.logAndDrain(true, 10000);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "unavailable bridge logged");
        CompanionNative.available = true;
        assets.logAndDrain(true, 19999);
        CompanionMapProbe.require(CompanionNative.logs.isEmpty(), "unavailable attempt bypassed limit");
        assets.logAndDrain(true, 20000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 1 &&
                CompanionNative.logs.get(0).contains("reason=first_fill"), "unavailable bridge lost first fill");

        CompanionNative.entered = new CountDownLatch(1);
        CompanionNative.release = new CountDownLatch(1);
        try {
            Clock.now = 30000; assets.logCaches(true, 30000);
            CompanionMapProbe.require(CompanionNative.entered.await(10, TimeUnit.SECONDS), "sink did not start");
            // Native I/O is stuck. Draw admission and decoder publication still complete; no backlog accumulates.
            offThread(() -> {
                CompanionNative.drawThreads.add(Thread.currentThread());
                for (int n = 0; n < 50; n++) assets.logCaches(true, 100000);
                assets.bitmap("icons/b.webp", false); assets.decoder.drain();
                CompanionMapProbe.require(assets.residentBytes() == 2048, "cache publication waited for sink");
            });
            Clock.now = 100000;
        } finally { CompanionNative.release.countDown(); }
        assets.cacheLogger.submit(() -> { }).get(10, TimeUnit.SECONDS);
        CompanionNative.entered = null; CompanionNative.release = null;
        CompanionMapProbe.require(CompanionNative.logs.size() == 2 &&
                CompanionNative.logs.get(1).contains("images=1,1024,"), "snapshot changed or queued an I/O backlog");
        assets.logAndDrain(true, 109999);
        CompanionMapProbe.require(CompanionNative.logs.size() == 2, "slow sink completion bypassed limit");
        assets.logAndDrain(true, 110000);
        CompanionMapProbe.require(CompanionNative.logs.size() == 3, "slow sink never resumed");

        // Close cancels queued delivery and does not await a native sink that ignores interruption.
        CompanionNative.entered = new CountDownLatch(1);
        CompanionNative.release = new CountDownLatch(1);
        try {
            Clock.now = 120000; assets.logCaches(true, 120000);
            CompanionMapProbe.require(CompanionNative.entered.await(10, TimeUnit.SECONDS), "shutdown sink did not start");
            offThread(assets::close);
        } finally { CompanionNative.release.countDown(); }
        assets.awaitLoggerStop();
        CompanionNative.entered = null; CompanionNative.release = null;
        int count = CompanionNative.logs.size();
        assets.logCaches(true, 200000);
        CompanionMapProbe.require(CompanionNative.logs.size() == count, "closed executor accepted a log");
        CacheProbe canceled = new CacheProbe(256);
        CompanionNative.monitors = new Object[]{canceled.pending, canceled.tiles, canceled.overviews, canceled.decorImages, canceled.images};
        canceled.bitmap("icons/a.webp", false); canceled.decoder.drain();
        synchronized (canceled.pending) {
            Clock.now = 0; canceled.logCaches(true, 0);
            canceled.close();
        }
        canceled.awaitLoggerStop();
        CompanionMapProbe.require(CompanionNative.logs.size() == count, "queued shutdown snapshot logged");
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
            assets.close();
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
            source = (source[:insertion] + '\n' + section(cache, '    private static final class BitmapCache', '    private JSONObject json(')
                      + section(cache, '    public Bitmap bitmap(String path, boolean tile)', '    private void notifyChanged()')
                      + section(cache, '    public void close()', '\n}') + source[insertion:])
            source += section(renderer, '        int cols =', '        JSONObject dynamic =') + MAP_TESTS
            source += section(view, '    private String coordinates()', '    private static String number(') + TEST_MAIN
            # The viewport body uses this exact production decoded-size constant.
            source = source.replace('class MapProbe {', 'class MapProbe {\n' + section(renderer, '    private static final long[] TILE_BYTES', '\n\n'))
            source = source.replace('android.os.SystemClock.elapsedRealtime()', 'Clock.now')
            self.assertIn('assets.logCaches(isShown() && getWindowVisibility() == VISIBLE, android.os.SystemClock.elapsedRealtime());', view)
            self.assertIn('assets.setCacheVisible(isShown() && getWindowVisibility() == VISIBLE);', view)
            self.assertIn('assets.setCacheVisible(isShown() && visibility == VISIBLE);', view)
            snapshot = section(cache, '                // Copy only primitives', '            if (fills == 0')
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
