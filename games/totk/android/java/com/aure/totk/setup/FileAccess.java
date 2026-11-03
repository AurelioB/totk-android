package com.aure.totk.setup;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Direct file access ("All files access", MANAGE_EXTERNAL_STORAGE), as Azahar's vanilla build does it: the permission
 * check and the Settings screen that grants it, and picked documents turned into real paths. The permission is optional
 * here: every caller keeps the Storage Access Framework path (descriptors from the picker) for when it is not granted
 * or a document has no real path. Native code never checks the permission itself, except the game's start, which
 * re-checks it for a game folder that needs it (runtime/android_storage.h). */
public final class FileAccess {
    private FileAccess() { }

    /** All files access is granted right now (re-checked on every call: it can be revoked at any time). */
    public static boolean granted() {
        try { return Environment.isExternalStorageManager(); }
        catch (RuntimeException unknown) { return false; }
    }
    /** Settings' "All files access" page for this app (Azahar SetupFragment), or the general list on devices without it. */
    static Intent settingsIntent(Context context) {
        return new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.fromParts("package", context.getPackageName(), null));
    }
    static Intent settingsFallback() { return new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION); }

    /** Mounted volumes by StorageVolume uuid in upper case (the volume part of an external-storage document id). */
    static Map<String, String> volumes(Context context) {
        Map<String, String> out = new HashMap<>();
        StorageManager storage = context.getSystemService(StorageManager.class);
        if (storage == null) return out;
        for (StorageVolume v : storage.getStorageVolumes()) {
            File dir = v.getDirectory();
            if (v.getUuid() != null && dir != null) out.put(v.getUuid().toUpperCase(Locale.ROOT), dir.getPath());
        }
        return out;
    }

    /** The real path of a picked document (ACTION_OPEN_DOCUMENT, or a document inside a picked tree), or null. */
    static String documentPath(Context context, Uri uri) {
        String id;
        try { id = DocumentsContract.isDocumentUri(context, uri) ? DocumentsContract.getDocumentId(uri) : null; }
        catch (RuntimeException notDocument) { id = null; }
        return resolve(context, uri, id, true);
    }
    /** The real path of a picked tree (ACTION_OPEN_DOCUMENT_TREE), or null. */
    static String treePath(Context context, Uri tree) {
        String id;
        try { id = DocumentsContract.getTreeDocumentId(tree); }
        catch (RuntimeException notTree) { return null; }
        return resolve(context, tree, id, false);
    }
    private static String resolve(Context context, Uri uri, String id, boolean document) {
        if (uri == null) return null;
        if ("file".equals(uri.getScheme())) return uri.getPath();
        if (id == null) return null;
        String authority = uri.getAuthority();
        File primary = Environment.getExternalStorageDirectory();
        String primaryRoot = primary == null ? null : primary.getPath();
        if (StoragePaths.DOWNLOADS_AUTHORITY.equals(authority) && !id.startsWith("raw:"))
            return document ? downloadsPath(context, uri, id, primaryRoot) : null;   // a downloads folder by id: no path
        return StoragePaths.documentPath(authority, id, primaryRoot, volumes(context));
    }
    /** A downloads-provider document with an msf:<id> or numeric id: the MediaStore.Files row with that very _ID gives
     * the candidate path(s), and one counts only when it is the file the picker opened, by {st_dev, st_ino} against
     * fstat of the picker's own descriptor (StoragePaths.mediaStorePath). Anything else: no path (the descriptor). */
    private static String downloadsPath(Context context, Uri uri, String id, String primaryRoot) {
        long media = StoragePaths.mediaStoreId(id);
        if (media < 0) return null;
        ContentResolver resolver = context.getContentResolver();
        long[] picked = pickedIdentity(resolver, uri);
        if (picked == null) return null;
        String[] columns = {MediaStore.MediaColumns.DATA, MediaStore.MediaColumns.VOLUME_NAME,
                MediaStore.MediaColumns.RELATIVE_PATH, MediaStore.MediaColumns.DISPLAY_NAME};
        try (Cursor c = resolver.query(MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL), columns,
                MediaStore.MediaColumns._ID + "=?", new String[]{Long.toString(media)}, null)) {
            if (c == null || !c.moveToFirst()) return null;
            return StoragePaths.mediaStorePath(string(c, 0), string(c, 1), string(c, 2), string(c, 3), primaryRoot,
                    volumes(context), picked, FileAccess::identity);
        } catch (RuntimeException unavailable) { return null; }
    }
    /** {st_dev, st_ino} of the regular file behind the picker's descriptor for uri, or null. */
    private static long[] pickedIdentity(ContentResolver resolver, Uri uri) {
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "r")) {
            if (fd == null) return null;
            StructStat st = Os.fstat(fd.getFileDescriptor());
            return OsConstants.S_ISREG(st.st_mode) ? new long[]{st.st_dev, st.st_ino} : null;
        } catch (IOException | ErrnoException | RuntimeException unavailable) { return null; }
    }
    /** {st_dev, st_ino} of the regular file at path, or null (StoragePaths.Identity). */
    private static long[] identity(String path) {
        try {
            StructStat st = Os.stat(path);
            return OsConstants.S_ISREG(st.st_mode) ? new long[]{st.st_dev, st.st_ino} : null;
        } catch (ErrnoException | RuntimeException unavailable) { return null; }
    }
    private static String string(Cursor c, int column) { return c.isNull(column) ? null : c.getString(column); }

    /** A picked document as a file this app can open directly right now: the permission is granted, the document has a
     * real path, it can be read and (when the picker reported one) has the reported size. Null means: use the picker's
     * descriptor (StoragePaths.useDirectPath). */
    static File directFile(Context context, Uri uri, long reportedSize) {
        if (!granted()) return null;
        String path = documentPath(context, uri);
        if (path == null) return null;
        File file = new File(path);
        boolean readable = file.isFile() && file.canRead();
        return StoragePaths.useDirectPath(true, path, readable, readable ? file.length() : -1, reportedSize) ? file : null;
    }
    /** A picked tree as a folder this app can use directly right now, or null (use the Storage Access Framework). */
    static File directFolder(Context context, Uri tree) {
        if (tree == null || !granted()) return null;
        String path = treePath(context, tree);
        if (path == null) return null;
        File dir = new File(path);
        return Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(dir)) && dir.isDirectory() ? dir : null;
    }
}
