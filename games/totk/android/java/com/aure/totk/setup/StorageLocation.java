package com.aure.totk.setup;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Environment;
import android.os.StatFs;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/** Where the imported game data lives (StoragePaths.Kind): the app's private storage <filesDir>/game (the default; no
 * FUSE in between), the app folder on an SD card (<card>/Android/data/<package>/files), or a folder the player picked
 * on internal shared storage or a card, which needs All files access. Installs made before private storage existed
 * stay in the old internal app folder (getExternalFilesDir) until the one-time verified move offered on Home.
 * Every Java user of <game> goes through gameDir(); the game process gets the same folder as TOTK_DATA_DIR in
 * settings.env, which runtime/frontend_android.c validates (android_storage.h) before it picks its data folder and log. */
public final class StorageLocation {
    private StorageLocation() { }
    private static final String PREFS = "storage", DATA_DIR = "data_dir", DATA_LABEL = "data_dir_label";
    /** LAYOUT: set once the first start of this version has decided where an existing install is (initialChoice). */
    private static final String LAYOUT = "layout", MIGRATE_DECLINED = "migrate_declined", MOVE_FROM = "move_from", MOVE_TO = "move_to";
    /** RomFS part size on FAT volumes (a FAT32 file must stay below 4 GiB): 0xFFFF0000, as the contract fixes it. */
    static final long FAT_PART_BYTES = 4_294_901_760L;
    static final String STORAGE_ROOT = "/storage";

    static SharedPreferences prefs(Context context) { return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE); }

    /** The default: private storage, <filesDir>/game. */
    static File privateDir(Context context) { return new File(context.getFilesDir(), "game"); }
    /** The internal app folder (Android/data) that versions before private storage used; null if Android has none. */
    static File legacyDir(Context context) { return context.getExternalFilesDir(null); }

    /** On the first start of this version: an existing install in the old internal app folder stays the game folder (still
     * playable) until it is moved; anything else uses private storage. Runs once; cheap afterwards. */
    private static void init(Context context) {
        SharedPreferences prefs = prefs(context);
        if (prefs.contains(LAYOUT)) return;
        SharedPreferences.Editor edit = prefs.edit().putInt(LAYOUT, 2);
        if (prefs.getString(DATA_DIR, null) == null) {
            File legacy = legacyDir(context);
            String chosen = StoragePaths.initialChoice(legacy != null && holdsGameData(legacy), holdsGameData(privateDir(context)),
                    legacy == null ? null : legacy.getPath());
            if (chosen != null) edit.putString(DATA_DIR, chosen).putString(DATA_LABEL, Strings.INTERNAL_OLD);
        }
        edit.commit();
    }
    /** <game>: the chosen folder, even when its volume is missing right now (callers check available()). */
    public static File gameDir(Context context) {
        init(context);
        String chosen = prefs(context).getString(DATA_DIR, null);
        return chosen == null ? privateDir(context) : new File(chosen);
    }
    static StoragePaths.Kind kind(Context context) { return kind(context, gameDir(context)); }
    static StoragePaths.Kind kind(Context context, File dir) {
        return StoragePaths.kind(dir.getPath(), privateDir(context).getPath(), context.getPackageName());
    }
    /** The chosen folder can be used now: private storage always; an app folder on a mounted volume; a picked folder
     * on a mounted volume while All files access is granted. */
    public static boolean available(Context context) {
        File dir = gameDir(context);
        switch (kind(context, dir)) {
            case PRIVATE: return true;
            case FOLDER: if (!FileAccess.granted()) return false;
            // fall through
            default: return Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(dir)) && dir.isDirectory();
        }
    }
    /** The game data is in a picked folder and All files access was turned off: the fix is to allow it again. */
    static boolean accessLost(Context context) { return kind(context) == StoragePaths.Kind.FOLDER && !FileAccess.granted(); }
    /** The volume name remembered when the folder was chosen (the card can be gone when it is needed). */
    static String label(Context context) {
        String saved = prefs(context).getString(DATA_LABEL, null);
        return saved != null ? saved : Strings.INTERNAL_STORAGE;
    }
    /** settings.env's TOTK_DATA_DIR: always the chosen folder, so the game never has to guess. Private storage is
     * created here (the runtime accepts only an existing folder). */
    static String envValue(Context context) {
        File dir = gameDir(context);
        if (kind(context, dir) == StoragePaths.Kind.PRIVATE) dir.mkdirs();
        return dir.getPath();
    }

    /** Make volume the game folder (after a finished move or before a first install). Private storage clears the pref. */
    static void choose(Context context, Volume volume) {
        SharedPreferences.Editor edit = prefs(context).edit();
        if (volume.kind == StoragePaths.Kind.PRIVATE) edit.remove(DATA_DIR).remove(DATA_LABEL);
        else edit.putString(DATA_DIR, volume.dir.getPath()).putString(DATA_LABEL, volume.name);
        edit.commit();  // the game process reads it from disk at its start
    }
    /** The raw choice (folder, label), for putting it back when switching fails half-way. */
    static String[] saved(Context context) { return new String[]{prefs(context).getString(DATA_DIR, null), prefs(context).getString(DATA_LABEL, null)}; }
    static void restore(Context context, String[] saved) {
        SharedPreferences.Editor edit = prefs(context).edit();
        if (saved[0] == null) edit.remove(DATA_DIR); else edit.putString(DATA_DIR, saved[0]);
        if (saved[1] == null) edit.remove(DATA_LABEL); else edit.putString(DATA_LABEL, saved[1]);
        edit.commit();
    }
    /** Forget a chosen folder that is gone (set up again on another storage): back to private storage. */
    static void reset(Context context) { prefs(context).edit().remove(DATA_DIR).remove(DATA_LABEL).commit(); }

    // ---------- the one-time move to private storage ----------
    static boolean migrationPending(Context context, boolean playable) {
        File legacy = legacyDir(context);
        return StoragePaths.migrationPending(gameDir(context).getPath(), legacy == null ? null : legacy.getPath(), playable,
                prefs(context).getBoolean(MIGRATE_DECLINED, false));
    }
    static void declineMigration(Context context) { prefs(context).edit().putBoolean(MIGRATE_DECLINED, true).commit(); }
    /** The move in progress (or last started) is the one from the old internal app folder to private storage. */
    static boolean migrating(Context context) {
        File legacy = legacyDir(context);
        String from = prefs(context).getString(MOVE_FROM, null), to = prefs(context).getString(MOVE_TO, null);
        return legacy != null && legacy.getPath().equals(from) && privateDir(context).getPath().equals(to);
    }
    /** Both ends of a move, recorded before it starts so an interrupted one can be cleaned up on either side. */
    static void recordMove(Context context, File from, File to) {
        prefs(context).edit().putString(MOVE_FROM, from.getPath()).putString(MOVE_TO, to.getPath()).commit();
    }
    /** Every folder setup may have left game data in: private storage, the app folders, the chosen folder and both
     * ends of the last move. */
    static List<File> knownFolders(Context context) {
        Set<String> paths = new LinkedHashSet<>();
        paths.add(privateDir(context).getPath());
        for (File dir : context.getExternalFilesDirs(null)) if (dir != null) paths.add(dir.getPath());
        paths.add(gameDir(context).getPath());
        for (String key : new String[]{MOVE_FROM, MOVE_TO}) { String p = prefs(context).getString(key, null); if (p != null) paths.add(p); }
        List<File> out = new ArrayList<>();
        for (String p : paths) out.add(new File(p));
        return out;
    }

    /** One place the app can keep game data. internal: on the device's own storage (for the icon and wording). */
    static final class Volume {
        final File dir; final String name; final StoragePaths.Kind kind; final boolean internal, fat; final long free, total;
        Volume(File dir, String name, StoragePaths.Kind kind, boolean internal, boolean fat, long free, long total) {
            this.dir = dir; this.name = name; this.kind = kind; this.internal = internal; this.fat = fat; this.free = free; this.total = total;
        }
        boolean same(File other) { return other != null && dir.getPath().equals(other.getPath()); }
    }
    /** Where the game data can go, the default first: private storage, the SD card app folders, and the old internal app
     * folder or a picked folder while the data is there. Disk work (StatFs, /proc/self/mounts): worker thread. */
    static List<Volume> volumes(Context context) {
        List<Volume> out = new ArrayList<>();
        String mounts = mounts();
        File own = privateDir(context), current = gameDir(context), legacy = legacyDir(context);
        long[] space = space(context.getFilesDir());
        if (space != null) out.add(new Volume(own, Strings.INTERNAL_STORAGE, StoragePaths.Kind.PRIVATE, true, false, space[0], space[1]));
        StorageManager storage = context.getSystemService(StorageManager.class);
        for (File dir : context.getExternalFilesDirs(null)) {
            if (dir == null || !Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(dir))) continue;
            boolean isLegacy = legacy != null && dir.getPath().equals(legacy.getPath());
            if (isLegacy && !dir.getPath().equals(current.getPath())) continue;   // offered only while the data is still there
            dir.mkdirs();
            long[] s = space(dir);
            if (s == null) continue;
            out.add(new Volume(dir, isLegacy ? Strings.INTERNAL_OLD : volumeName(context, storage, dir), StoragePaths.Kind.APP, isLegacy,
                    !isLegacy && fat(mounts, dir.getPath()), s[0], s[1]));
        }
        if (kind(context, current) == StoragePaths.Kind.FOLDER && available(context)) {
            Volume v = folder(context, current);
            if (v != null) out.add(v);
        }
        return out;
    }
    /** A picked folder as a place for the game data (the folder need not exist yet; its volume must). */
    static Volume folder(Context context, File dir) {
        File existing = dir;
        while (existing != null && !existing.isDirectory()) existing = existing.getParentFile();
        long[] s = existing == null ? null : space(existing);
        if (s == null) return null;
        StorageManager storage = context.getSystemService(StorageManager.class);
        StorageVolume volume = storage == null ? null : storage.getStorageVolume(existing);
        boolean internal = volume != null && volume.isPrimary();
        String where = internal ? Strings.INTERNAL_STORAGE : volumeName(context, storage, existing);
        return new Volume(dir, where + " · " + dir.getName(), StoragePaths.Kind.FOLDER, internal, fat(mounts(), dir.getPath()), s[0], s[1]);
    }
    /** Why a picked folder cannot hold the game data (a StoragePaths.folderProblem code, or "access"), or null. */
    static String folderProblem(Context context, File dir) {
        if (!FileAccess.granted()) return "access";
        String problem = StoragePaths.folderProblem(dir.getPath(), STORAGE_ROOT, context.getFilesDir().getPath());
        if (problem != null) return problem;
        if (!Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(dir.getParentFile() == null ? dir : dir.getParentFile()))) return "not_shared";
        return null;
    }
    /** The folder a pick puts the data in (StoragePaths.folderTarget): the pick itself when empty or already ours. */
    static File folderTarget(File picked) {
        String[] names = picked.list();
        boolean usable = names == null || names.length == 0 || holdsGameData(picked);
        return new File(StoragePaths.folderTarget(picked.getPath(), usable));
    }
    /** The place a move goes to, re-checked in the service: one of volumes(), or a valid picked folder. */
    static Volume target(Context context, File dir, String name) {
        for (Volume v : volumes(context)) if (v.same(dir)) return v;
        if (kind(context, dir) != StoragePaths.Kind.FOLDER || folderProblem(context, dir) != null) return null;
        Volume v = folder(context, dir);
        return v == null ? null : new Volume(v.dir, name == null || name.isEmpty() ? v.name : name, v.kind, v.internal, v.fat, v.free, v.total);
    }
    static Volume current(Context context) {
        File dir = gameDir(context);
        for (Volume v : volumes(context)) if (v.same(dir)) return v;
        return null;
    }
    private static String volumeName(Context context, StorageManager storage, File dir) {
        StorageVolume volume = storage == null ? null : storage.getStorageVolume(dir);
        String description = volume == null ? null : volume.getDescription(context);
        return description != null && !description.isEmpty() ? description : Strings.SD_CARD;
    }
    /** {free, total} bytes of the filesystem holding dir, or null when it cannot be asked. */
    private static long[] space(File dir) {
        try { StatFs fs = new StatFs(dir.getPath()); return new long[]{fs.getAvailableBytes(), fs.getTotalBytes()}; }
        catch (IllegalArgumentException unavailable) { return null; }
    }
    /** dir holds something a setup or move made (setup/move.c's game-data entries, install.json or the move's staging
     * folder). A lone driver.so or totk.env does not count. */
    static boolean holdsGameData(File dir) {
        for (String name : new String[]{"install.json", "rewritten", "extracted", "game-data", "shaders", "companion", ".move"})
            if (new File(dir, name).exists()) return true;
        return false;
    }
    /** RomFS part size for the setup/move request: FAT volumes split the 17 GB image (the native side also checks). */
    static long partBytes(Context context, File dir) {
        return fat(mounts(), dir.getPath()) ? FAT_PART_BYTES : 0;
    }
    private static String mounts() {
        try { return new String(Files.readAllBytes(new File("/proc/self/mounts").toPath()), StandardCharsets.UTF_8); }
        catch (IOException | SecurityException unreadable) { return ""; }
    }
    /** /storage/<uuid>/... is served from /mnt/media_rw/<uuid>, whose line in /proc/self/mounts names the filesystem. */
    static boolean fat(String mounts, String path) {
        if (!path.startsWith("/storage/")) return false;
        String rest = path.substring("/storage/".length());
        int slash = rest.indexOf('/');
        String uuid = slash < 0 ? rest : rest.substring(0, slash);
        if (uuid.isEmpty() || uuid.equals("emulated") || uuid.equals("self")) return false;
        for (String line : mounts.split("\n")) {
            String[] f = line.split(" ");
            if (f.length >= 3 && f[1].equals("/mnt/media_rw/" + uuid)) return f[2].equals("vfat") || f[2].equals("msdos");
        }
        return false;
    }
}
