package com.aure.totk.companion;

import android.app.ActivityManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ComposeShader;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/** Optional, asynchronous game art generated on the device into <game>/companion/ (never shipped in the APK).
 * No bitmap decode or file reads occur in Canvas drawing. */
public final class AssetStore {
    private final File root;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService decoder = Executors.newSingleThreadExecutor();
    // Counters/snapshot stay per store; one process-wide writer and two bounded delivery channels survive recreation.
    private final long instanceId = CacheLog.ids.incrementAndGet();
    private final long[] cacheSnapshot = new long[4 * 9];
    private final Runnable cacheLogTask = this::writeCacheLog;
    private final AtomicBoolean cacheVisible = new AtomicBoolean();
    private volatile boolean cacheFilled;
    private final BitmapCache images, tiles, overviews, decorImages;
    private final int tileFloor, tileMarginLimit;
    private final int memoryClass;
    private long loggedEvictions;
    private int loggedFills;
    private final Set<String> pending = new HashSet<>(), missing = new HashSet<>();
    private final Runnable changed;
    public volatile JSONObject index = new JSONObject(), text = new JSONObject();
    public volatile JSONArray markers = new JSONArray();
    public volatile Typeface font = Typeface.create("sans-serif", Typeface.NORMAL);
    private volatile boolean closed;
    private volatile Map<String, Decor> decorations = java.util.Collections.emptyMap();
    private volatile Map<String, Integer> palette = java.util.Collections.emptyMap();
    private final Paint decorPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Rect source = new Rect();
    private final RectF destination = new RectF();
    private final Matrix tileMatrix = new Matrix();
    private static final Typeface FALLBACK_FONT = Typeface.create("sans-serif", Typeface.NORMAL);
    private final Paint glyphProbe = new Paint();
    private final android.util.SparseIntArray glyphSupport = new android.util.SparseIntArray();
    private Typeface probedFont;
    private final RectF softBounds = new RectF();
    private final Matrix softMatrix = new Matrix();
    private Decor softPart;
    private Bitmap softImage;
    private Shader softShader;
    private float softUnit;
    private static final int[] EDGE_ALPHA = {Color.TRANSPARENT, Color.WHITE, Color.WHITE, Color.TRANSPARENT};

    private static final class Decor {
        final String path;
        final int[] nine;
        final boolean tile;
        final float aspect;
        final android.graphics.ColorFilter tint;
        Bitmap bitmap;
        BitmapShader shader;
        Decor(JSONObject definition) {
            path = definition.optString("path", "");
            aspect = definition.optInt("h") > 0 ? (float)definition.optInt("w") / definition.optInt("h") : 0;
            JSONArray insets = definition.optJSONArray("nine");
            nine = insets != null && insets.length() == 4 ? new int[]{
                    Math.max(0, insets.optInt(0)), Math.max(0, insets.optInt(1)),
                    Math.max(0, insets.optInt(2)), Math.max(0, insets.optInt(3))} : null;
            tile = definition.optBoolean("tile");
            tint = definition.optBoolean("mask") ? new PorterDuffColorFilter(
                    parseColor(definition.optString("color"), Color.WHITE), PorterDuff.Mode.SRC_IN) : null;
        }
    }

    /** The setup's companion stage writes this folder (inside <game>, which may be on an SD card); without its
     * index.json the companion stays off. */
    public static File root(Context context) {
        return new File(com.aure.totk.setup.StorageLocation.gameDir(context), "companion");
    }
    public static boolean available(Context context) {
        File root = root(context);
        return root != null && new File(root, "index.json").isFile();
    }
    private InputStream open(String path) throws java.io.IOException {
        File file = new File(root, path);
        // Paths come from index.json; refuse anything that escapes the companion folder.
        if (root == null || !file.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator)) throw new java.io.FileNotFoundException(path);
        return new FileInputStream(file);
    }
    public AssetStore(Context context, Runnable changed) {
        root = root(context);
        this.changed = changed;
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        int memory = manager == null ? 128 : manager.getMemoryClass();
        memoryClass = memory;
        int budget = Math.min(32, Math.max(8, memory / 12)) * 1024 * 1024;
        images = cache(budget);
        tileFloor = Math.max(24 * 1024 * 1024, budget);
        // Android's existing per-app heap class (not guessed physical RAM) limits only the margin above what is on
        // screen; capping the visible working set would bring back the stationary decode/evict loop.
        tileMarginLimit = Math.min(64, Math.max(32, memory / 8)) * 1024 * 1024;
        tiles = cache(tileFloor);
        // A 19,200,000-byte overview used to leave room for only two 840 px tiles in a 24 MiB cache,
        // causing a stationary viewport to decode and evict its visible tiles repeatedly.
        overviews = cache(24 * 1024 * 1024);
        // Full-screen frames must not evict one another or every redraw re-decodes the menu.
        decorImages = cache(48 * 1024 * 1024);
        CacheLog.record(instanceId, false, "constructor");
        decoder.execute(() -> {
            index = json("index.json");
            Map<String, Decor> parts = new HashMap<>();
            JSONObject decor = index.optJSONObject("decor");
            if (decor != null) for (java.util.Iterator<String> keys = decor.keys(); keys.hasNext();) {
                String key = keys.next();
                JSONObject definition = decor.optJSONObject(key);
                if (definition != null) parts.put(key, new Decor(definition));
            }
            decorations = parts;
            Map<String, Integer> colors = new HashMap<>();
            JSONObject declared = index.optJSONObject("palette");
            if (declared != null) for (java.util.Iterator<String> keys = declared.keys(); keys.hasNext();) {
                String key = keys.next();
                try { colors.put(key, parseColor(declared.getString(key), Color.WHITE)); }
                catch (Exception ignored) { }
            }
            palette = colors;
            JSONArray languages = index.optJSONArray("languages");
            String fallback = index.optString("defaultLanguage", "USen");
            String language = language(Locale.getDefault(), languages, fallback);
            JSONObject english = json("text/" + fallback + ".json");
            JSONObject local = language.equals(fallback) ? new JSONObject() : json("text/" + language + ".json");
            for (java.util.Iterator<String> sections = local.keys(); sections.hasNext();) {
                String section = sections.next();
                JSONObject values = local.optJSONObject(section);
                if (values == null) continue;
                JSONObject merged = Snapshot.object(english, section);
                for (java.util.Iterator<String> keys = values.keys(); keys.hasNext();) {
                    String key = keys.next(); Snapshot.put(merged, key, values.opt(key));
                }
                Snapshot.put(english, section, merged);
            }
            text = english;
            String markerPath = index.optString("markers", "map/markers.json");
            try (InputStream in = open(markerPath)) {
                String raw = new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
                if (raw.stripLeading().startsWith("[")) markers = new JSONArray(raw);
                else markers = Snapshot.array(new JSONObject(raw), "markers");
            } catch (Exception ignored) { }
            String face = Snapshot.object(index, "fonts").optString("main", "");
            if (!face.isEmpty()) {
                try { font = Typeface.createFromFile(new File(root, face)); }
                catch (RuntimeException ignored) { }
            }
            notifyChanged();
        });
    }
    /** Periodic/fill/eviction snapshots: one per ten seconds process-wide. Lifecycle-only delivery has one separate
     * ten-second allowance; both channels use this one writer, including across a slow old store's native write.
     * Only primitive lifecycle metadata is retained: 32 events maximum, with an explicit cumulative loss count. */
    private static final class CacheLog {
        static final AtomicLong ids = new AtomicLong();
        static final AtomicInteger visible = new AtomicInteger();
        static final AtomicBoolean queued = new AtomicBoolean();
        static final ExecutorService worker = Executors.newSingleThreadExecutor();
        static volatile long nextSnapshotAt, nextLifecycleAt;
        static volatile boolean eventsPending;
        static final Object events = new Object();
        static final int LIMIT = 32;
        static final long[] eventSeq = new long[LIMIT], eventIds = new long[LIMIT], eventTimes = new long[LIMIT];
        static final boolean[] eventClosed = new boolean[LIMIT];
        static final String[] eventReasons = new String[LIMIT];
        // This copy belongs only to the single writer; no formatting or allocation under either metadata/cache lock.
        static final long[] logIds = new long[LIMIT], logTimes = new long[LIMIT];
        static final boolean[] logClosed = new boolean[LIMIT];
        static final String[] logReasons = new String[LIMIT];
        static int head, count, logCount;
        static long sequence, dropped, logDropped, logThrough;
        static void record(long id, boolean closed, String reason) {
            long now = android.os.SystemClock.elapsedRealtime();
            synchronized (events) {
                if (count == LIMIT) { head = (head + 1) % LIMIT; count--; dropped++; }
                int at = (head + count++) % LIMIT;
                eventSeq[at] = ++sequence; eventIds[at] = id; eventTimes[at] = now;
                eventClosed[at] = closed; eventReasons[at] = reason;
                eventsPending = true;
            }
        }
        static void copyEvents() {
            synchronized (events) {
                logCount = count; logDropped = dropped;
                for (int n = 0; n < count; n++) {
                    int at = (head + n) % LIMIT;
                    logIds[n] = eventIds[at]; logTimes[n] = eventTimes[at];
                    logClosed[n] = eventClosed[at]; logReasons[n] = eventReasons[at];
                    logThrough = eventSeq[at];
                }
            }
        }
        static void appendEvents(StringBuilder line) {
            line.append(" lifecycle=");
            if (logCount == 0) line.append("none");
            for (int n = 0; n < logCount; n++) {
                if (n != 0) line.append(';');
                line.append(logIds[n]).append(',').append(logClosed[n] ? "closed" : "created").append(',')
                        .append(logTimes[n]).append(',').append(logReasons[n]);
            }
            line.append(" lifecycle_dropped=").append(logDropped);
        }
        static void acknowledge() {
            synchronized (events) {
                while (count > 0 && eventSeq[head] <= logThrough) {
                    eventReasons[head] = null; head = (head + 1) % LIMIT; count--;
                }
                eventsPending = count != 0;
            }
        }
        static void releaseCopy() {
            for (int n = 0; n < logCount; n++) logReasons[n] = null;
        }
    }

    /** Mutations and these extra counters use the existing pending monitor. LruCache already counts lookups,
     * allocation bytes and evictions under its own monitor; do not wrap or allocate on the lookup path. */
    private static final class BitmapCache extends LruCache<String, Bitmap> {
        int entries;
        long decodes, largest, highWater;
        boolean filled;
        BitmapCache(int bytes) { super(bytes); }
        @Override protected int sizeOf(String key, Bitmap value) { return value.getAllocationByteCount(); }
        @Override protected void entryRemoved(boolean evicted, String key, Bitmap oldValue, Bitmap newValue) {
            entries--;
        }
        void store(String path, Bitmap image) {
            long before = size(), bytes = image.getAllocationByteCount();
            entries++;
            Bitmap old = put(path, image);
            largest = Math.max(largest, bytes);
            // Include the insertion before LruCache trims, not just the final below-budget size.
            highWater = Math.max(highWater, before + bytes - (old == null ? 0 : old.getAllocationByteCount()));
            filled = true;
        }
        void copy(long[] values, int offset) {
            values[offset] = entries; values[offset + 1] = size(); values[offset + 2] = maxSize();
            values[offset + 3] = hitCount(); values[offset + 4] = missCount(); values[offset + 5] = decodes;
            values[offset + 6] = evictionCount(); values[offset + 7] = largest; values[offset + 8] = highWater;
        }
    }
    private static BitmapCache cache(int bytes) { return new BitmapCache(bytes); }

    /** UI-only admission: no cache/event monitor, formatting or native call on the draw path. */
    void logCaches(boolean visible, long now) {
        setCacheVisible(visible);
        if (!visible || closed || (!cacheFilled && !CacheLog.eventsPending) ||
                (now < CacheLog.nextSnapshotAt && (now < CacheLog.nextLifecycleAt || !CacheLog.eventsPending)) ||
                !CacheLog.queued.compareAndSet(false, true)) return;
        try { CacheLog.worker.execute(cacheLogTask); }
        catch (RejectedExecutionException stopped) { CacheLog.queued.set(false); }
    }
    void setCacheVisible(boolean visible) {
        visible &= !closed;
        if (cacheVisible.get() != visible && cacheVisible.compareAndSet(!visible, visible))
            CacheLog.visible.addAndGet(visible ? 1 : -1);
    }
    private static void appendCache(StringBuilder line, String name, long[] values, int offset) {
        line.append(' ').append(name).append('=').append(values[offset]);
        for (int n = 1; n < 9; n++) line.append(',').append(values[offset + n]);
    }
    private void writeCacheLog() {
        try {
            if (closed || !cacheVisible.get() || CacheLog.visible.get() == 0) return;
            long now = android.os.SystemClock.elapsedRealtime();
            boolean snapshot = now >= CacheLog.nextSnapshotAt;
            if (!snapshot && now < CacheLog.nextLifecycleAt) return;
            int fills;
            synchronized (pending) {
                if (closed) return;
                // Copy only primitives into preallocated storage while cache publication is serialized.
                fills = (tiles.filled ? 1 : 0) | (overviews.filled ? 2 : 0) |
                        (decorImages.filled ? 4 : 0) | (images.filled ? 8 : 0);
                tiles.copy(cacheSnapshot, 0); overviews.copy(cacheSnapshot, 9);
                decorImages.copy(cacheSnapshot, 18); images.copy(cacheSnapshot, 27);
            }
            if (closed || !cacheVisible.get()) return;
            CacheLog.copyEvents();
            if ((!snapshot && CacheLog.logCount == 0) || (fills == 0 && CacheLog.logCount == 0)) return;
            long evictions = cacheSnapshot[6] + cacheSnapshot[15] + cacheSnapshot[24] + cacheSnapshot[33];
            String reason = !snapshot ? "lifecycle" : fills != 0 && fills != loggedFills ? "first_fill" :
                    evictions - loggedEvictions >= 8 ? "eviction_burst" : "periodic";
            StringBuilder line = new StringBuilder(1024).append("cache v=2 ms=").append(now)
                    .append(" instance=").append(instanceId).append(" event=").append(snapshot ? "snapshot" : "lifecycle")
                    .append(" reason=").append(reason).append(" memory_class_mib=").append(memoryClass);
            CacheLog.appendEvents(line);
            appendCache(line, "tiles", cacheSnapshot, 0); appendCache(line, "overview", cacheSnapshot, 9);
            appendCache(line, "decor", cacheSnapshot, 18); appendCache(line, "images", cacheSnapshot, 27);
            if (closed || !cacheVisible.get() || CacheLog.visible.get() == 0) return;
            boolean delivered = CompanionNative.logCover(line.toString());
            // Completion-based process deadlines: slow writes/replacement stores cannot overlap or catch up.
            long next = android.os.SystemClock.elapsedRealtime() + 10000;
            if (snapshot) CacheLog.nextSnapshotAt = next; else CacheLog.nextLifecycleAt = next;
            if (delivered) {
                CacheLog.acknowledge();
                if (snapshot) { loggedFills = fills; loggedEvictions = evictions; }
            }
        } finally { CacheLog.releaseCopy(); CacheLog.queued.set(false); }
    }
    private JSONObject json(String path) {
        try (InputStream in = open(path)) {
            return new JSONObject(new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8));
        } catch (Exception missing) { return new JSONObject(); }
    }
    private static String language(Locale locale, JSONArray options, String fallback) {
        String lang = locale.getLanguage(), country = locale.getCountry();
        String code;
        switch (lang) {
            case "ja": code = "JPja"; break;
            case "fr": code = country.equals("CA") || country.equals("US") ? "USfr" : "EUfr"; break;
            case "es": code = country.equals("US") || country.equals("MX") ? "USes" : "EUes"; break;
            case "de": code = "EUde"; break;
            case "it": code = "EUit"; break;
            case "pt": code = "USpt"; break;
            case "nl": code = "EUnl"; break;
            case "ru": code = "EUru"; break;
            case "ko": code = "KRko"; break;
            case "zh": code = locale.getScript().equals("Hant") || country.equals("TW") || country.equals("HK") ? "TWzh" : "CNzh"; break;
            default: code = country.equals("GB") ? "EUen" : "USen";
        }
        if (options != null) for (int i = 0; i < options.length(); i++) if (code.equals(options.optString(i))) return code;
        return fallback;
    }
    public Typeface typeface(String value) {
        Typeface face = font;
        if (probedFont != face) {
            probedFont = face; glyphProbe.setTypeface(face); glyphSupport.clear();
        }
        for (int offset = 0; offset < value.length();) {
            int codepoint = value.codePointAt(offset);
            offset += Character.charCount(codepoint);
            if (Character.isWhitespace(codepoint)) continue;
            int supported = glyphSupport.get(codepoint);
            if (supported == 0) {
                supported = glyphProbe.hasGlyph(new String(Character.toChars(codepoint))) ? 1 : -1;
                glyphSupport.put(codepoint, supported);
            }
            if (supported < 0) return FALLBACK_FONT;
        }
        return face;
    }
    public String label(String group, String key, String fallback) {
        return Snapshot.object(text, group).optString(key, fallback);
    }
    private String pouchActor(String actor) {
        JSONObject items = text.optJSONObject("items");
        if (items == null || items.has(actor)) return actor;
        String alias = Snapshot.object(index, "iconAliases").optString(actor, actor);
        if (items.has(alias)) return alias;
        // World-space Zonai devices use the corresponding capsule's pouch text/icon, when it exists.
        int suffix = actor.lastIndexOf("_A_");
        if (actor.startsWith("SpObj_") && !actor.contains("_Capsule_") && suffix >= 0) {
            String capsule = actor.substring(0, suffix) + "_Capsule" + actor.substring(suffix);
            if (items.has(capsule)) return capsule;
        }
        return actor;
    }
    public String itemName(String actor) {
        String name = Snapshot.object(Snapshot.object(text, "items"), pouchActor(actor)).optString("name", "");
        // Unknown/modded fuse objects still get readable text, without the internal Obj_/DgnObj_/SpObj_ prefix.
        return name.isEmpty() ? humanize(actor.replaceFirst("^[A-Za-z]*Obj_", "")) : name;
    }
    public String itemDescription(String actor) {
        return Snapshot.object(Snapshot.object(text, "items"), pouchActor(actor)).optString("desc", "");
    }
    public static String humanize(String key) { return key.replace('_', ' ').replaceAll("([a-z])([A-Z])", "$1 $2"); }
    public Bitmap icon(String actor) {
        String canonical = pouchActor(actor);
        String alias = Snapshot.object(index, "iconAliases").optString(canonical, canonical);
        return bitmap("icons/" + alias + ".webp", false);
    }
    public Bitmap ui(String name) {
        String path = Snapshot.object(index, "ui").optString(name, "");
        return path.isEmpty() ? null : bitmap(path, false);
    }
    /** Game colors use CSS RGBA, not Android's eight-digit ARGB spelling. */
    private static int parseColor(String value, int fallback) {
        try {
            if (value.length() == 9 && value.charAt(0) == '#') {
                long rgba = Long.parseLong(value.substring(1), 16);
                return (int)((rgba >>> 8) | ((rgba & 255) << 24));
            }
            return Color.parseColor(value);
        } catch (RuntimeException invalid) { return fallback; }
    }
    public int color(String name, int fallback) {
        Integer value = palette.get(name);
        return value == null ? fallback : value;
    }
    public Bitmap pageIcon(String name) {
        String path = Snapshot.object(index, "pageIcons").optString(name, "");
        return path.isEmpty() ? null : bitmap(path, false);
    }
    public boolean hasDecor(String name) { return decorations.containsKey(name); }
    public float decorAspect(String name, float fallback) {
        Decor part = decorations.get(name);
        return part == null || part.aspect <= 0 ? fallback : part.aspect;
    }

    /** Draw an optional game part. Insets stay crisp; only the center/edges stretch. */
    public boolean decor(Canvas canvas, String name, RectF bounds, float unit) {
        return decor(canvas, name, bounds, unit, 255);
    }
    public boolean decor(Canvas canvas, String name, RectF bounds, float unit, int opacity) {
        Decor part = decorations.get(name);
        if (part == null || part.path.isEmpty() || bounds.isEmpty()) return false;
        Bitmap image = bitmap(part.path, decorImages);
        if (image == null) return false;
        decorPaint.setColor(Color.WHITE);
        decorPaint.setAlpha(opacity);
        decorPaint.setColorFilter(part.tint);
        if (part.tile) {
            if (part.bitmap != image) {
                part.bitmap = image;
                part.shader = new BitmapShader(image, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT);
            }
            tileMatrix.setScale(unit, unit);
            tileMatrix.postTranslate(bounds.left, bounds.top);
            part.shader.setLocalMatrix(tileMatrix);
            decorPaint.setShader(part.shader);
            canvas.drawRect(bounds, decorPaint);
            decorPaint.setShader(null);
        } else if (part.nine != null) {
            int iw = image.getWidth(), ih = image.getHeight();
            int left = Math.min(part.nine[0], iw / 2), top = Math.min(part.nine[1], ih / 2);
            int right = Math.min(part.nine[2], iw / 2), bottom = Math.min(part.nine[3], ih / 2);
            float scale = Math.min(unit, Math.min(bounds.width() / Math.max(1, left + right),
                    bounds.height() / Math.max(1, top + bottom)));
            for (int row = 0; row < 3; row++) for (int col = 0; col < 3; col++) {
                source.set(col == 0 ? 0 : col == 1 ? left : iw - right,
                        row == 0 ? 0 : row == 1 ? top : ih - bottom,
                        col == 0 ? left : col == 1 ? iw - right : iw,
                        row == 0 ? top : row == 1 ? ih - bottom : ih);
                destination.set(col == 0 ? bounds.left : col == 1 ? bounds.left + left * scale : bounds.right - right * scale,
                        row == 0 ? bounds.top : row == 1 ? bounds.top + top * scale : bounds.bottom - bottom * scale,
                        col == 0 ? bounds.left + left * scale : col == 1 ? bounds.right - right * scale : bounds.right,
                        row == 0 ? bounds.top + top * scale : row == 1 ? bounds.bottom - bottom * scale : bounds.bottom);
                if (!source.isEmpty() && !destination.isEmpty()) canvas.drawBitmap(image, source, destination, decorPaint);
            }
        } else canvas.drawBitmap(image, null, bounds, decorPaint);
        decorPaint.setColorFilter(null);
        decorPaint.setAlpha(255);
        return true;
    }
    /** A single item-info backing is visible; cache its texture/edge shaders until layout changes. */
    public boolean softDecor(Canvas canvas, String name, RectF bounds, float unit) {
        Decor part = decorations.get(name);
        if (part == null || part.path.isEmpty() || bounds.isEmpty()) return false;
        if (part.nine != null || part.tile) return decor(canvas, name, bounds, unit);
        Bitmap image = bitmap(part.path, decorImages);
        if (image == null) return false;
        if (softPart != part || softImage != image || !softBounds.equals(bounds) || softUnit != unit) {
            softPart = part; softImage = image; softBounds.set(bounds); softUnit = unit;
            BitmapShader texture = new BitmapShader(image, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
            softMatrix.setScale(bounds.width() / image.getWidth(), bounds.height() / image.getHeight());
            softMatrix.postTranslate(bounds.left, bounds.top); texture.setLocalMatrix(softMatrix);
            float edge = Math.min(24 * unit, Math.min(bounds.width(), bounds.height()) * .15f);
            float horizontal = edge / bounds.width(), vertical = edge / bounds.height();
            Shader x = new LinearGradient(bounds.left, bounds.top, bounds.right, bounds.top, EDGE_ALPHA,
                    new float[]{0, horizontal, 1 - horizontal, 1}, Shader.TileMode.CLAMP);
            Shader y = new LinearGradient(bounds.left, bounds.top, bounds.left, bounds.bottom, EDGE_ALPHA,
                    new float[]{0, vertical, 1 - vertical, 1}, Shader.TileMode.CLAMP);
            Shader edgeMask = new ComposeShader(x, y, PorterDuff.Mode.DST_IN);
            softShader = new ComposeShader(texture, edgeMask, PorterDuff.Mode.DST_IN);
        }
        decorPaint.setColor(Color.WHITE); decorPaint.setColorFilter(part.tint); decorPaint.setShader(softShader);
        canvas.drawRect(bounds, decorPaint);
        decorPaint.setShader(null); decorPaint.setColorFilter(null);
        return true;
    }
    public Bitmap bitmap(String path, boolean tile) {
        return bitmap(path, path.endsWith("/overview.webp") ? overviews : tile ? tiles : path.startsWith("decor/") ? decorImages : images);
    }
    /** A map tile only if it is already decoded; never starts a load. */
    public Bitmap cachedTile(String path) { return closed ? null : tiles.get(path); }
    /** Retain every visible tile plus one more, whatever the memory class; the memory-class limit caps only the extra
        16 MiB margin above that. Oversized decoded images can raise capacity further; budget updates never shrink it. */
    public void tileBudget(long visibleBytes, long tileBytes) {
        long working = visibleBytes + tileBytes, margin = Math.min((long)tileMarginLimit, visibleBytes + 16L * 1024 * 1024);
        int size = (int)Math.min(Integer.MAX_VALUE, Math.max(tileFloor, Math.max(working, margin)));
        synchronized (pending) {
            if (size > tiles.maxSize()) tiles.resize(size);
        }
    }
    private Bitmap bitmap(String path, BitmapCache cache) {
        if (closed) return null;
        Bitmap hit = cache.get(path);
        if (hit != null) return hit;
        synchronized (pending) {
            if (missing.contains(path) || !pending.add(path)) return null;
        }
        decoder.execute(() -> {
            Bitmap result = null;
            try (InputStream in = open(path)) { result = BitmapFactory.decodeStream(in); }
            catch (Exception ignored) { }
            synchronized (pending) {
                pending.remove(path);
                cache.decodes++;
                if (result == null) missing.add(path);
                else if (!closed) {
                    // An image larger than its whole cache would be evicted at once and decoded again on every redraw.
                    if (result.getAllocationByteCount() > cache.maxSize()) cache.resize(result.getAllocationByteCount() + 1024 * 1024);
                    cache.store(path, result);
                    cacheFilled = true;
                }
            }
            if (result != null) notifyChanged();
        });
        return null;
    }
    private void notifyChanged() { main.post(() -> { if (!closed) changed.run(); }); }
    public void close(String reason) {
        synchronized (pending) {
            if (closed) return;
            closed = true;
        }
        setCacheVisible(false);
        CacheLog.record(instanceId, true, reason);
        decoder.shutdownNow();
        synchronized (pending) {
            images.evictAll(); tiles.evictAll(); overviews.evictAll(); decorImages.evictAll();
        }
        decorations = java.util.Collections.emptyMap();
        softPart = null; softImage = null; softShader = null;
    }
}
