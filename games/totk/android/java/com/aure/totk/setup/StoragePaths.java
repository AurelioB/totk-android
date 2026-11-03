package com.aure.totk.setup;

import java.util.Locale;
import java.util.Map;

/** The path rules of direct file access, kept free of Android classes so the host tests run exactly this code
 * (tests/test_android_direct_access.py): which kind of game folder a path is, which picked folders may hold the game
 * data, how a picked document becomes a real path (Azahar's NativeLibrary.getNativePath), where an existing install
 * starts out after the update that introduced private storage, and when a setup input is handed over by path. */
final class StoragePaths {
    private StoragePaths() { }

    /** PRIVATE: <filesDir>/game, the app's own storage (no FUSE, not visible to other apps). APP: the app folder
     * <volume>/Android/data/<package>/files (the SD card's, or the internal one older versions used). FOLDER: a folder
     * the player picked on shared storage, usable only while All files access is granted. */
    enum Kind { PRIVATE, APP, FOLDER }

    /** The folder a game-data pick puts the data in when the picked folder is used for other things too. */
    static final String FOLDER_NAME = "TotK Native";
    static final String EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents";
    static final String DOWNLOADS_AUTHORITY = "com.android.providers.downloads.documents";

    static String appSuffix(String pkg) { return "/Android/data/" + pkg + "/files"; }

    static Kind kind(String path, String privateGame, String pkg) {
        String p = trim(path);
        if (p.equals(trim(privateGame))) return Kind.PRIVATE;
        String suffix = appSuffix(pkg);
        if (p.endsWith(suffix) && p.length() > suffix.length()) return Kind.APP;
        return Kind.FOLDER;
    }

    /** The volume part of a path on shared storage: storageRoot/emulated/<user> or storageRoot/<uuid>; null when the
     * path is not below one. */
    static String volumeOf(String path, String storageRoot) {
        String root = trim(storageRoot) + "/";
        if (path == null || !path.startsWith(root)) return null;
        String[] parts = path.substring(root.length()).split("/", -1);
        if (parts[0].isEmpty() || parts[0].equals("self")) return null;
        if (parts[0].equals("emulated")) {
            if (parts.length < 2 || parts[1].isEmpty() || !parts[1].chars().allMatch(Character::isDigit)) return null;
            return root + "emulated/" + parts[1];
        }
        return root + parts[0];
    }

    /** Why a picked folder cannot hold the game data, or null when it can: it must be a plain absolute path below a
     * shared volume (storageRoot/emulated/<user>/... or storageRoot/<uuid>/...), not the volume itself, not inside
     * Android/ (other apps' folders, or our own app folder, which is offered on its own), and not the private folder. */
    static String folderProblem(String path, String storageRoot, String privateFiles) {
        if (path == null || !path.startsWith("/") || path.indexOf('\0') >= 0 || path.indexOf('\n') >= 0 || path.length() >= 900) return "not_a_path";
        String p = trim(path);
        for (String part : p.substring(1).split("/", -1)) if (part.isEmpty() || part.equals(".") || part.equals("..")) return "not_a_path";
        if (privateFiles != null && (p.equals(trim(privateFiles)) || p.startsWith(trim(privateFiles) + "/"))) return "private";
        String volume = volumeOf(p, storageRoot);
        if (volume == null) return "not_shared";
        if (p.equals(volume)) return "volume_root";
        String first = p.substring(volume.length() + 1).split("/", 2)[0];
        if (first.equalsIgnoreCase("Android")) return "android_folder";
        return null;
    }

    /** The folder the data goes into for a picked folder: the pick itself when it is empty, already holds game data or
     * is named FOLDER_NAME; otherwise a FOLDER_NAME folder inside it, so the data never mixes with the player's files. */
    static String folderTarget(String picked, boolean emptyOrGameData) {
        String p = trim(picked);
        if (emptyOrGameData || p.substring(p.lastIndexOf('/') + 1).equals(FOLDER_NAME)) return p;
        return p + "/" + FOLDER_NAME;
    }

    /** A document of the external-storage or downloads provider as a real path (Azahar's getNativePath plus its
     * RemovableStorageHelper), or null when it has none, which means the caller keeps using the Storage Access
     * Framework. docId: DocumentsContract.getDocumentId / getTreeDocumentId. volumes: StorageVolume uuid in upper case ->
     * its directory. The downloads provider's msf:/numeric ids carry no path: FileAccess looks those up in MediaStore
     * (mediaStorePath), never as Download/<display name>, which is wrong for files in subfolders. */
    static String documentPath(String authority, String docId, String primaryRoot, Map<String, String> volumes) {
        if (authority == null || docId == null) return null;
        String path;
        if (authority.equals(EXTERNAL_STORAGE_AUTHORITY)) {
            int colon = docId.indexOf(':');
            if (colon <= 0) return null;
            String volume = docId.substring(0, colon), rel = docId.substring(colon + 1), root;
            if (volume.equals("primary")) root = primaryRoot;
            else if (volume.equals("home")) root = primaryRoot == null ? null : trim(primaryRoot) + "/Documents";   // the Documents root
            else root = volumes == null ? null : volumes.get(volume.toUpperCase(Locale.ROOT));
            if (root == null) return null;
            path = join(root, rel);
        } else if (authority.equals(DOWNLOADS_AUTHORITY) && docId.startsWith("raw:")) path = docId.substring(4);
        else return null;
        return path != null && clean(path) ? path : null;
    }

    /** The MediaStore row id (MediaStore.Files _ID) of a downloads-provider document id "msf:<id>" or "<id>", or -1. */
    static long mediaStoreId(String docId) {
        if (docId == null) return -1;
        String s = docId.startsWith("msf:") ? docId.substring(4) : docId;
        if (s.isEmpty() || s.length() > 18 || !s.chars().allMatch(c -> c >= '0' && c <= '9')) return -1;
        return Long.parseLong(s);
    }

    /** A file's identity, {st_dev, st_ino} of a regular file (stat follows links); null when it cannot be stat'ed (gone,
     * access revoked) or is not a regular file. Android: Os.stat; the host test: the JDK's unix attributes. */
    interface Identity { long[] of(String path); }

    /** The real path of a downloads-provider document from the MediaStore row with the document's own id: the row's
     * _data first, then its volume + RELATIVE_PATH + DISPLAY_NAME. A candidate counts only when it is a clean absolute
     * path to the very file the picker opened: its {st_dev, st_ino} equal picked (fstat of the picker's descriptor), so
     * a same-named file of the same size elsewhere, a stale row or a folder never stands in for the pick. Null (also
     * for an unknown picked identity): use the picker's descriptor. volumeName: MediaStore's VOLUME_NAME
     * ("external_primary", or a removable volume's lower-case uuid). */
    static String mediaStorePath(String data, String volumeName, String relativePath, String displayName,
                                 String primaryRoot, Map<String, String> volumes, long[] picked, Identity identity) {
        if (picked == null || picked.length != 2 || identity == null) return null;
        if (sameFile(data, picked, identity)) return data;
        if (volumeName == null || relativePath == null || displayName == null || displayName.isEmpty() || displayName.indexOf('/') >= 0) return null;
        String root = volumeName.equals("external_primary") || volumeName.equals("external") ? primaryRoot
                : volumes == null ? null : volumes.get(volumeName.toUpperCase(Locale.ROOT));
        if (root == null) return null;
        String path = join(join(root, relativePath), displayName);
        return sameFile(path, picked, identity) ? path : null;
    }
    private static boolean sameFile(String path, long[] picked, Identity identity) {
        if (path == null || !clean(path)) return false;
        long[] id = identity.of(path);
        return id != null && id.length == 2 && id[0] == picked[0] && id[1] == picked[1];
    }

    /** First start of a version with private storage: an install found only in the old internal app folder stays where
     * it is (it remains playable and is offered a verified move); everything else starts in private storage. Returns
     * the folder to record, or null for the private default. */
    static String initialChoice(boolean legacyHasData, boolean privateHasData, String legacy) {
        return legacyHasData && !privateHasData && legacy != null ? legacy : null;
    }

    /** Home offers the one-time move to private storage while a playable install is still in the old internal app
     * folder and the player has not said "not now". */
    static boolean migrationPending(String current, String legacy, boolean playable, boolean declined) {
        return playable && !declined && legacy != null && trim(legacy).equals(trim(current));
    }

    /** A setup input goes to native code by path only with All files access, a real path that can be read and the size
     * the picker reported (or no reported size); otherwise as a descriptor from the picker, as without the permission. */
    static boolean useDirectPath(boolean granted, String path, boolean readable, long fileSize, long reported) {
        return granted && path != null && path.startsWith("/") && readable && fileSize >= 0 && (reported < 0 || reported == fileSize);
    }

    private static String join(String root, String rel) {
        String r = trim(root);
        while (rel.startsWith("/")) rel = rel.substring(1);
        rel = trim(rel);
        return rel.isEmpty() ? r : r + "/" + rel;
    }
    /** Absolute, no empty, '.' or '..' component, no NUL. */
    private static boolean clean(String path) {
        if (!path.startsWith("/") || path.indexOf('\0') >= 0) return false;
        String t = trim(path);
        if (t.equals("/")) return false;
        for (String part : t.substring(1).split("/", -1)) if (part.isEmpty() || part.equals(".") || part.equals("..")) return false;
        return true;
    }
    static String trim(String path) {
        if (path == null) return "";
        String p = path;
        while (p.length() > 1 && p.endsWith("/")) p = p.substring(0, p.length() - 1);
        return p;
    }
}
