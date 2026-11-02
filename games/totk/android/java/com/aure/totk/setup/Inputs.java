package com.aure.totk.setup;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** The user's picked documents, persisted (with their URI grants) so an import resumed after process death reopens them. */
final class Inputs {
    static final String DECRYPTED = "decrypted", CONTAINERS = "containers";
    static final String[] ROLES_DECRYPTED = {"romfs", "exefs"}, ROLES_CONTAINERS = {"base", "update", "keys"};
    /** The single Latin TTF written into all six shared-font slots (the game does not use them in normal play). */
    static final String SHARED_FONT = "NotoSans-Regular.ttf";
    private static final String[] EXEFS_NAMES = {"main", "sdk", "subsdk0"};

    static final class Item {
        final Uri uri; final String name; final long size;
        Item(Uri uri, String name, long size) { this.uri = uri; this.name = name; this.size = size; }
        JSONObject json() throws JSONException { return new JSONObject().put("uri", uri.toString()).put("name", name).put("size", size); }
    }

    private final SharedPreferences prefs;
    private final Context prefsContext;
    String route = "";
    /** role -> items; romfs/exefs may hold several (split parts, ExeFS files), the others exactly one. */
    final java.util.Map<String, List<Item>> items = new java.util.LinkedHashMap<>();
    /** Tree/document URIs whose persisted grants back the items (released when replaced). */
    final java.util.Map<String, List<Uri>> grants = new java.util.HashMap<>();

    private Inputs(Context context) { this.prefsContext = context.getApplicationContext(); this.prefs = prefs(context); }

    static SharedPreferences prefs(Context context) { return context.getSharedPreferences("setup", Context.MODE_PRIVATE); }

    static Inputs load(Context context) {
        Inputs inputs = new Inputs(context);
        try {
            JSONObject root = new JSONObject(inputs.prefs.getString("inputs", "{}"));
            inputs.route = root.optString("route", "");
            JSONObject roles = root.optJSONObject("items");
            if (roles != null) for (java.util.Iterator<String> it = roles.keys(); it.hasNext();) {
                String role = it.next();
                JSONArray list = roles.getJSONArray(role);
                List<Item> parsed = new ArrayList<>();
                for (int i = 0; i < list.length(); i++) {
                    JSONObject o = list.getJSONObject(i);
                    parsed.add(new Item(Uri.parse(o.getString("uri")), o.optString("name"), o.optLong("size", -1)));
                }
                inputs.items.put(role, parsed);
            }
            JSONObject granted = root.optJSONObject("grants");
            if (granted != null) for (java.util.Iterator<String> it = granted.keys(); it.hasNext();) {
                String role = it.next();
                JSONArray list = granted.getJSONArray(role);
                List<Uri> uris = new ArrayList<>();
                for (int i = 0; i < list.length(); i++) uris.add(Uri.parse(list.getString(i)));
                inputs.grants.put(role, uris);
            }
        } catch (JSONException corrupt) { inputs.items.clear(); inputs.grants.clear(); }
        return inputs;
    }
    void save() {
        try {
            JSONObject roles = new JSONObject(), granted = new JSONObject();
            for (java.util.Map.Entry<String, List<Item>> e : items.entrySet()) {
                JSONArray list = new JSONArray();
                for (Item item : e.getValue()) list.put(item.json());
                roles.put(e.getKey(), list);
            }
            for (java.util.Map.Entry<String, List<Uri>> e : grants.entrySet()) {
                JSONArray list = new JSONArray();
                for (Uri uri : e.getValue()) list.put(uri.toString());
                granted.put(e.getKey(), list);
            }
            prefs.edit().putString("inputs", new JSONObject().put("route", route).put("items", roles).put("grants", granted).toString()).apply();
        } catch (JSONException impossible) { throw new IllegalStateException(impossible); }
    }
    String[] roles() { return CONTAINERS.equals(route) ? ROLES_CONTAINERS : ROLES_DECRYPTED; }
    List<Item> get(String role) { List<Item> list = items.get(role); return list == null ? new ArrayList<>() : list; }

    /** Replace a role's picks, keeping the new grants and releasing the ones no other role still uses. */
    void set(ContentResolver resolver, String role, List<Item> picked, List<Uri> granted) {
        List<Uri> old = grants.remove(role);
        items.put(role, picked); grants.put(role, granted);
        if (old != null) for (Uri uri : old) if (!stillGranted(uri)) release(resolver, uri);
        save();
    }
    private boolean stillGranted(Uri uri) {
        for (List<Uri> list : grants.values()) if (list.contains(uri)) return true;
        return false;
    }
    /** Forget every pick (route change, deleted data) and drop their persisted grants. The save folder's grant
     * (SaveMirror) is not a pick and stays. */
    void clear(ContentResolver resolver) {
        String mirror = SaveMirror.folderString(prefsContext);
        for (UriPermission permission : resolver.getPersistedUriPermissions())
            if (!permission.getUri().toString().equals(mirror)) release(resolver, permission.getUri());
        items.clear(); grants.clear(); save();
    }
    private static void release(ContentResolver resolver, Uri uri) {
        try { resolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
        catch (SecurityException notHeld) { }
    }
    static void keep(ContentResolver resolver, Uri uri) {
        try { resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
        catch (SecurityException notPersistable) { }  // still readable for this session
    }

    static Item describe(ContentResolver resolver, Uri uri) {
        String name = uri.getLastPathSegment(); long size = -1;
        try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                if (!cursor.isNull(0)) name = cursor.getString(0);
                if (!cursor.isNull(1)) size = cursor.getLong(1);
            }
        } catch (RuntimeException unavailable) { }
        return new Item(uri, name == null ? "?" : name, size);
    }
    /** Files directly inside a picked folder. For ExeFS keep just main/sdk/subsdk0 when present (dumps also carry npdm/rtld). */
    static List<Item> children(ContentResolver resolver, Uri tree, String role) {
        List<Item> found = new ArrayList<>();
        Uri listing = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = resolver.query(listing, columns, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name == null || name.startsWith(".") || DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(3))) continue;
                found.add(new Item(DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0)), name, cursor.isNull(2) ? -1 : cursor.getLong(2)));
            }
        } catch (RuntimeException unavailable) { }
        if ("exefs".equals(role)) {
            List<Item> modules = new ArrayList<>();
            for (String wanted : EXEFS_NAMES) for (Item item : found) if (item.name.equals(wanted)) modules.add(item);
            if (!modules.isEmpty()) return modules;
        }
        sort(found);
        return found;
    }
    /** Split parts must reach the native side in order: natural name order (part 2 before part 10). */
    static void sort(List<Item> list) { list.sort(Comparator.comparing((Item item) -> item.name, Inputs::natural)); }
    private static int natural(String a, String b) {
        int i = 0, j = 0;
        while (i < a.length() && j < b.length()) {
            char x = a.charAt(i), y = b.charAt(j);
            if (Character.isDigit(x) && Character.isDigit(y)) {
                int si = i, sj = j;
                while (i < a.length() && Character.isDigit(a.charAt(i))) i++;
                while (j < b.length() && Character.isDigit(b.charAt(j))) j++;
                String na = a.substring(si, i).replaceFirst("^0+(?!$)", ""), nb = b.substring(sj, j).replaceFirst("^0+(?!$)", "");
                if (na.length() != nb.length()) return na.length() - nb.length();
                int cmp = na.compareTo(nb);
                if (cmp != 0) return cmp;
            } else {
                int cmp = Character.compare(Character.toLowerCase(x), Character.toLowerCase(y));
                if (cmp != 0) return cmp;
                i++; j++;
            }
        }
        return (a.length() - i) - (b.length() - j);
    }
    static long total(List<Item> list) {
        long sum = 0;
        for (Item item : list) { if (item.size < 0) return -1; sum += item.size; }
        return sum;
    }
    /** Roles still needed by the pending stages with no picks, or with a pick that can no longer be opened (grant revoked,
     * file moved or deleted, card removed): neither by its real path (All files access) nor through the picker. Opens
     * and closes each document; worker thread only. */
    List<String> unreadableRoles(Context context, InstallState install) {
        ContentResolver resolver = context.getContentResolver();
        List<String> lost = new ArrayList<>();
        for (String role : roles()) {
            // Decrypted route: ExeFS feeds only the code stage, RomFS only the copy. Containers feed both.
            if (role.equals("exefs") && install.done("code") || role.equals("romfs") && install.done("romfs")) continue;
            List<Item> list = get(role);
            boolean ok = !list.isEmpty();
            for (Item item : list) {
                if (!ok) break;
                if (FileAccess.directFile(context, item.uri, item.size) != null) continue;
                try (ParcelFileDescriptor fd = resolver.openFileDescriptor(item.uri, "r")) { ok = fd != null; }
                catch (IOException | SecurityException | IllegalArgumentException gone) { ok = false; }
            }
            if (!ok) lost.add(role);
        }
        return lost;
    }

    /** Opened inputs for one native call: descriptors the native side reads with pread and never closes (close() does),
     * or, with All files access, real paths the native side opens and closes itself (Azahar's raw-path branch). */
    static final class Opened implements Closeable {
        final JSONObject request = new JSONObject();
        final List<String> unreadable = new ArrayList<>();
        /** Roles with at least one pick that could not be opened. */
        final java.util.Set<String> lostRoles = new java.util.LinkedHashSet<>();
        /** Inputs handed over by path (for the log). */
        int byPath;
        private final List<Closeable> handles = new ArrayList<>();
        @Override public void close() {
            for (Closeable handle : handles) try { handle.close(); } catch (IOException ignored) { }
            handles.clear();
        }
    }

    /** Build the request JSON (contract: gameDir/privateDir/noBackupDir/nativeLibDir, inputs, fonts, stages).
     * forRun adds what only run() needs (shared font, shader list); inspect() gets the inputs alone. */
    Opened open(Context context, boolean forRun, String[] stages) throws JSONException {
        Opened opened = new Opened();
        JSONObject request = opened.request;
        File game = InstallState.gameDir(context);
        request.put("gameDir", game.getPath())
                .put("privateDir", context.getFilesDir().getPath())
                .put("noBackupDir", context.getNoBackupFilesDir().getPath())
                .put("nativeLibDir", context.getApplicationInfo().nativeLibraryDir)
                .put("romfsPartBytes", StorageLocation.partBytes(context, game));
        JSONObject inputs = new JSONObject();
        ContentResolver resolver = context.getContentResolver();
        for (String role : roles()) {
            JSONArray list = new JSONArray();
            for (Item item : get(role)) {
                // With All files access a pick that has a real path goes by path (no provider in between, and native
                // code may read it in parallel); otherwise, or if the path is not readable, the picker's descriptor.
                File direct = FileAccess.directFile(context, item.uri, item.size);
                if (direct != null) {
                    list.put(new JSONObject().put("path", direct.getPath()).put("size", direct.length()).put("name", item.name));
                    opened.byPath++;
                    continue;
                }
                try {
                    ParcelFileDescriptor fd = resolver.openFileDescriptor(item.uri, "r");
                    if (fd == null) throw new IOException("no descriptor");
                    opened.handles.add(fd);
                    long size = fd.getStatSize();
                    list.put(new JSONObject().put("fd", fd.getFd()).put("size", size >= 0 ? size : item.size).put("name", item.name));
                } catch (IOException | SecurityException | IllegalArgumentException lost) { opened.unreadable.add(item.name); opened.lostRoles.add(role); }
            }
            if (list.length() == 0) continue;
            if (role.equals("romfs") || role.equals("exefs")) inputs.put(role, list);
            else inputs.put(role, list.getJSONObject(0));
        }
        request.put("inputs", inputs);
        if (forRun) {
            request.put("fonts", new JSONArray().put(font(context, opened)));
            // The shader stage plans its variants from the bundled shader list; without it the seed covers the default
            // bind state only, so a missing asset just omits the field.
            try {
                File list = shaderList(context);
                if (list != null) {
                    ParcelFileDescriptor fd = ParcelFileDescriptor.open(list, ParcelFileDescriptor.MODE_READ_ONLY);
                    opened.handles.add(fd);
                    request.put("shaderList", new JSONObject().put("fd", fd.getFd()).put("offset", 0)
                            .put("length", list.length()).put("name", SHADER_LIST));
                }
            } catch (IOException unavailable) { }
        }
        if (stages != null) request.put("stages", new JSONArray(java.util.Arrays.asList(stages)));
        return opened;
    }
    static final String SHADER_LIST = "shader_list.tsv";
    /** The shader list (~79 MB) is a compressed APK asset, so it has no descriptor of its own: expand it once into
     * <nobackup>/shader_list.tsv. A stamp beside it records the app build plus the copy's size and SHA-256; the copy is
     * reused only while all three still match, so an app update or a damaged file triggers a fresh copy.
     * Returns null when the APK has no shader list. Worker thread only. */
    static File shaderList(Context context) throws IOException {
        File dir = context.getNoBackupFilesDir(), out = new File(dir, SHADER_LIST), stamp = new File(dir, SHADER_LIST + ".stamp");
        String build;
        try {
            android.content.pm.PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            build = info.getLongVersionCode() + ":" + info.lastUpdateTime;
        } catch (android.content.pm.PackageManager.NameNotFoundException impossible) { build = "?"; }
        try {
            String[] saved = new String(java.nio.file.Files.readAllBytes(stamp.toPath()), java.nio.charset.StandardCharsets.UTF_8).trim().split(" ");
            if (saved.length == 3 && saved[0].equals(build) && out.length() == Long.parseLong(saved[1])) {
                try (InputStream in = new java.io.FileInputStream(out)) { if (sha256(in, null).equals(saved[2])) return out; }
            }
        } catch (IOException | NumberFormatException noValidCopy) { }
        File temp = new File(dir, SHADER_LIST + ".tmp");
        String digest;
        try (InputStream in = context.getAssets().open(SHADER_LIST); FileOutputStream copy = new FileOutputStream(temp)) {
            digest = sha256(in, copy);
            copy.getFD().sync();
        } catch (java.io.FileNotFoundException absent) {
            temp.delete(); out.delete(); stamp.delete();
            return null;
        } catch (IOException failed) { temp.delete(); throw failed; }
        if (!temp.renameTo(out)) { temp.delete(); throw new IOException("cannot rename " + temp); }
        File stampTemp = new File(dir, SHADER_LIST + ".stamp.tmp");
        java.nio.file.Files.write(stampTemp.toPath(), (build + " " + out.length() + " " + digest).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        if (!stampTemp.renameTo(stamp)) stampTemp.delete();  // without a stamp the next run simply copies again
        return out;
    }
    /** Hex SHA-256 of a stream, copying it to {@code copy} when given. */
    private static String sha256(InputStream in, OutputStream copy) throws IOException {
        java.security.MessageDigest digest;
        try { digest = java.security.MessageDigest.getInstance("SHA-256"); }
        catch (java.security.NoSuchAlgorithmException impossible) { throw new IllegalStateException(impossible); }
        byte[] buffer = new byte[1 << 20];
        for (int n; (n = in.read(buffer)) > 0;) { digest.update(buffer, 0, n); if (copy != null) copy.write(buffer, 0, n); }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b));
        return hex.toString();
    }
    /** The bundled TTF is stored uncompressed in the APK, so its descriptor is the APK plus an offset; if a build ever
     * compresses it, a private copy is used instead. */
    private static JSONObject font(Context context, Opened opened) throws JSONException {
        JSONObject entry = new JSONObject().put("name", SHARED_FONT);
        try {
            AssetFileDescriptor asset = context.getAssets().openFd("fonts/" + SHARED_FONT);
            opened.handles.add(asset);
            return entry.put("fd", asset.getParcelFileDescriptor().getFd()).put("offset", asset.getStartOffset()).put("length", asset.getLength());
        } catch (IOException compressed) {
            File copy = new File(context.getNoBackupFilesDir(), SHARED_FONT);
            try {
                try (InputStream in = context.getAssets().open("fonts/" + SHARED_FONT); OutputStream out = new FileOutputStream(copy)) { in.transferTo(out); }
                ParcelFileDescriptor fd = ParcelFileDescriptor.open(copy, ParcelFileDescriptor.MODE_READ_ONLY);
                opened.handles.add(fd);
                return entry.put("fd", fd.getFd()).put("offset", 0).put("length", copy.length());
            } catch (IOException missing) { opened.unreadable.add(SHARED_FONT); return entry.put("fd", -1).put("offset", 0).put("length", 0); }
        }
    }
}
