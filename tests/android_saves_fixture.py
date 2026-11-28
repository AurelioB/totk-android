"""Compile the production SaveMirror/SaveManager with filesystem-backed Android boundary doubles.

The doubles supply document-provider I/O only; selection, copying, verification, export, sync and restore
all execute the production Java. Shared by regressions and the retained historical reproduction.
"""
from pathlib import Path
import subprocess

STUBS = {
    'android/net/Uri.java': '''package android.net;
public final class Uri {
    public final String path; public final boolean children;
    public Uri(String p, boolean c) { path=p; children=c; }
    public static Uri parse(String p) { return new Uri(p, false); }
    public String toString() { return path; }
    public boolean equals(Object o) { return o instanceof Uri && path.equals(((Uri)o).path); }
    public int hashCode() { return path.hashCode(); }
}''',
    'android/database/Cursor.java': '''package android.database;
import java.io.File;
public final class Cursor implements AutoCloseable {
    private final File[] files; private int index=-1;
    public Cursor(File[] f) { files=f; }
    public boolean moveToFirst() { index=0; return files.length>0; }
    public boolean moveToNext() { return ++index<files.length; }
    public String getString(int column) {
        File f=files[index];
        return column==0 ? f.getPath() : column==1 ? f.getName() : f.isDirectory() ? "directory" : "file";
    }
    public long getLong(int column) { return column==3 ? files[index].length() : files[index].lastModified(); }
    public boolean isNull(int column) { return false; }
    public void close() { }
}''',
    'android/content/ContentResolver.java': '''package android.content;
import android.net.Uri; import android.database.Cursor; import java.io.*; import java.util.*;
public final class ContentResolver {
    private final List<UriPermission> grants=new ArrayList<>();
    public Cursor query(Uri uri, String[] columns, String selection, String[] args, String order) {
        File f=new File(uri.path);
        File[] rows=uri.children ? f.listFiles() : f.exists() ? new File[]{f} : new File[0];
        if (rows==null) throw new IllegalStateException("cannot list "+f);
        return new Cursor(rows);
    }
    public InputStream openInputStream(Uri uri) throws IOException { return new FileInputStream(uri.path); }
    public OutputStream openOutputStream(Uri uri, String mode) throws IOException { return new FileOutputStream(uri.path); }
    public void takePersistableUriPermission(Uri uri, int flags) { grants.add(new UriPermission(uri)); }
    public void releasePersistableUriPermission(Uri uri, int flags) { grants.removeIf(p -> p.getUri().equals(uri)); }
    public List<UriPermission> getPersistedUriPermissions() { return grants; }
}''',
    'android/content/UriPermission.java': '''package android.content;
import android.net.Uri;
public final class UriPermission {
    private final Uri uri; public UriPermission(Uri u) { uri=u; }
    public Uri getUri() { return uri; }
    public boolean isReadPermission() { return true; } public boolean isWritePermission() { return true; }
}''',
    'android/content/Intent.java': '''package android.content;
public final class Intent { public static final int FLAG_GRANT_READ_URI_PERMISSION=1, FLAG_GRANT_WRITE_URI_PERMISSION=2; }''',
    'android/content/SharedPreferences.java': '''package android.content;
import java.util.*;
public final class SharedPreferences {
    private final Map<String,Object> data=new HashMap<>();
    public String getString(String k, String fallback) { return (String)data.getOrDefault(k,fallback); }
    public long getLong(String k, long fallback) { return (Long)data.getOrDefault(k,fallback); }
    public Editor edit() { return new Editor(); }
    public final class Editor {
        public Editor putString(String k,String v) { data.put(k,v); return this; }
        public Editor putLong(String k,long v) { data.put(k,v); return this; }
        public Editor remove(String k) { data.remove(k); return this; }
        public boolean commit() { return true; } public void apply() { }
    }
}''',
    'android/content/Context.java': '''package android.content;
import java.io.File; import android.content.pm.PackageManager;
public final class Context {
    public static final int MODE_PRIVATE=0;
    public final boolean direct; private final File files;
    private final ContentResolver resolver=new ContentResolver();
    private final SharedPreferences prefs=new SharedPreferences();
    public Context(File f,boolean d) { files=f; direct=d; files.mkdirs(); }
    public File getFilesDir() { return files; }
    public File getCacheDir() { File cache=new File(files,"cache"); cache.mkdirs(); return cache; }
    public Context getApplicationContext() { return this; }
    public ContentResolver getContentResolver() { return resolver; }
    public SharedPreferences getSharedPreferences(String n,int mode) { return prefs; }
    public String getPackageName() { return "com.aure.totk"; }
    public PackageManager getPackageManager() { return new PackageManager(); }
}''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public final class PackageManager {
    public static final class NameNotFoundException extends Exception { }
    public static final class PackageInfo { public String versionName="host"; }
    public PackageInfo getPackageInfo(String n,int flags) throws NameNotFoundException { return new PackageInfo(); }
}''',
    'android/os/Build.java': 'package android.os; public final class Build { public static final String MODEL="host"; }',
    'android/os/SystemClock.java': '''package android.os;
public final class SystemClock { public static long elapsedRealtime() { return System.nanoTime()/1000000; } }''',
    'android/util/Log.java': '''package android.util;
public final class Log {
    public static int i(String t,String s) { return 0; }
    public static int w(String t,String s) { return 0; }
    public static int w(String t,String s,Throwable e) { return 0; }
}''',
    'android/provider/DocumentsContract.java': '''package android.provider;
import android.net.Uri; import android.content.ContentResolver; import java.io.*;
public final class DocumentsContract {
    public static final class Document {
        public static final String MIME_TYPE_DIR="directory", COLUMN_DOCUMENT_ID="id", COLUMN_DISPLAY_NAME="name",
            COLUMN_MIME_TYPE="mime", COLUMN_SIZE="size", COLUMN_LAST_MODIFIED="mtime";
    }
    public static String getTreeDocumentId(Uri u) { return u.path; }
    public static String getDocumentId(Uri u) { return u.path; }
    public static Uri buildDocumentUriUsingTree(Uri tree,String id) { return Uri.parse(id); }
    public static Uri buildChildDocumentsUriUsingTree(Uri tree,String id) { return new Uri(id,true); }
    public static Uri createDocument(ContentResolver r,Uri parent,String mime,String name) throws IOException {
        File f=new File(parent.path,name);
        // Providers may rename colliding creations: production Folder.create must reject the changed name.
        if (f.exists()) f=new File(parent.path,name+" (1)");
        if (mime.equals(Document.MIME_TYPE_DIR)) { if (!f.mkdir()) throw new IOException("mkdir "+f); }
        else if (!f.createNewFile()) throw new IOException("create "+f);
        return Uri.parse(f.getPath());
    }
    public static boolean deleteDocument(ContentResolver r,Uri u) {
        erase(new File(u.path)); return true;
    }
    private static void erase(File f) { File[] children=f.listFiles(); if(children!=null) for(File c:children) erase(c); f.delete(); }
}''',
    'com/aure/totk/setup/FileAccess.java': '''package com.aure.totk.setup;
import java.io.File; import android.net.Uri; import android.content.Context;
final class FileAccess {
    static File directFolder(Context c,Uri u) { return c.direct ? new File(u.path) : null; }
    static File directFile(Context c,Uri u,long size) { return c.direct ? new File(u.path) : null; }
    static boolean granted() { return false; }
    static String documentPath(Context c,Uri u) { return c.direct ? u.path : null; }
}''',
    'com/aure/totk/setup/InstallState.java': '''package com.aure.totk.setup;
import android.content.Context;
final class InstallState { static GameLock.Result gameState(Context c) { return GameLock.check(c.getFilesDir(),false); } }''',
}

PROBE = r'''package com.aure.totk.setup;
import android.content.Context;
import android.net.Uri;
import java.io.*;
import java.nio.file.*;

public final class SaveMetadataProbe {
    private interface Work { String run() throws Exception; }
    private static void report(String name, Work work) {
        try { System.out.println(name+"="+work.run()); }
        catch(Exception e) { System.out.println(name+"=ERROR "+e.getClass().getSimpleName()+": "+e.getMessage()); }
    }
    private static String content(File dir) throws IOException {
        try (java.util.stream.Stream<Path> paths=Files.walk(dir.toPath())) {
            return paths.filter(Files::isRegularFile).map(p -> dir.toPath().relativize(p).toString())
                .sorted().collect(java.util.stream.Collectors.joining(","));
        }
    }
    public static void main(String[] args) throws Exception {
        String command=args[0]; File base=new File(args[1]); base.mkdirs();
        boolean direct=args.length<3 || args[2].equals("direct");
        Context context=new Context(new File(base,"private"),direct);
        if (command.equals("stage-folder") || command.equals("stage-zip")) {
            File source=new File(args[3]), staged=new File(base,"staged");
            report(command, () -> {
                if(command.equals("stage-folder")) SaveMirror.stageFolder(context,Uri.parse(source.getPath()),staged);
                else SaveMirror.stageZip(context,Uri.parse(source.getPath()),staged);
                SaveManager.replace(context.getFilesDir(),staged,"import",1700000000000L);
                File live=SaveManager.saveRoot(context.getFilesDir());
                SaveManager.Backup backup=SaveManager.backup(live,SaveManager.backupsDir(context.getFilesDir()),"manual",1700000001000L);
                SaveManager.restore(context.getFilesDir(),backup,1700000002000L);
                if(!content(live).equals(content(backup.saves()))) throw new AssertionError("backup/restore changed content");
                try(OutputStream out=new FileOutputStream(new File(base,"roundtrip.zip"))) { SaveMirror.export(context,out); }
                return content(SaveManager.saveRoot(context.getFilesDir()));
            });
            return;
        }
        File root=SaveManager.saveRoot(context.getFilesDir()); root.mkdirs();
        Files.write(new File(root,"option.sav").toPath(),new byte[]{1,2,3});
        if(command.equals("historical-nested")) {
            for(String name:new String[]{"slot_00/progress.sav","album/"+SaveManager.MANIFEST,"slot_00/backup.json",
                    "backup.json","album/"+SaveManager.MANIFEST+".tmp","slot_00/"+SaveManager.TOP+"/note.txt"}) {
                File f=new File(root,name); f.getParentFile().mkdirs(); Files.writeString(f.toPath(),"ordinary "+name);
            }
        }
        if(command.equals("historical-root")) {
            for(String name:new String[]{SaveManager.MANIFEST+".tmp",".option.sav.partial",SaveManager.TOP+"/note.txt"}) {
                File f=new File(root,name); f.getParentFile().mkdirs(); Files.writeString(f.toPath(),"old envelope "+name);
            }
        }
        SaveManager.Manifest clean=SaveManager.manifest(root,null,"host","host",1700000000000L);
        File cleanZip=new File(base,"clean.zip");
        try(OutputStream out=new FileOutputStream(cleanZip)) { SaveManager.writeZip(root,clean,out); }
        if(command.equals("clean") || command.startsWith("historical-")) { System.out.println("clean="+cleanZip); return; }
        // A genuine production manifest, deliberately placed among the game's files as found on the device.
        try(java.util.zip.ZipFile zip=new java.util.zip.ZipFile(cleanZip)) {
            Files.copy(zip.getInputStream(zip.getEntry(SaveManager.TOP+"/"+SaveManager.MANIFEST)),
                new File(root,SaveManager.MANIFEST).toPath());
        }
        if(command.equals("opaque")) Files.writeString(new File(root,SaveManager.MANIFEST).toPath(),"possibly user data");
        byte[] originalManifest=Files.readAllBytes(new File(root,SaveManager.MANIFEST).toPath());
        if(command.equals("reserved")) {
            for(String name:new String[]{SaveManager.MANIFEST+".tmp",".option.sav.partial",SaveManager.TOP+"/option.sav"}) {
                File f=new File(root,name); f.getParentFile().mkdirs(); Files.writeString(f.toPath(),"not save content");
            }
        }
        report("export", () -> {
            try(OutputStream out=new FileOutputStream(new File(base,"export.zip"))) { SaveMirror.export(context,out); }
            File staged=new File(base,"zip-import"); SaveManager.extractZip(new File(base,"export.zip"),staged);
            return content(staged);
        });
        File mirror=new File(base,"mirror"); mirror.mkdirs();
        SaveMirror.setFolder(context,Uri.parse(mirror.getPath()));
        report("sync", () -> { SaveMirror.Result r=SaveMirror.sync(context,true); return r.action+" "+r.error; });
        report("fetch", () -> {
            SaveManager.Store store=new SaveManager.FileStore(new File(mirror,SaveManager.TOP));
            SaveManager.FolderState state=SaveManager.readFolder(store);
            File fetched=new File(base,"fetched"); SaveManager.fetch(store,state,fetched,true); return content(fetched);
        });
        report("backup", () -> {
            SaveManager.Backup b=SaveManager.backup(root,SaveManager.backupsDir(context.getFilesDir()),"manual",1700000000000L);
            return content(b.saves());
        });
        report("preserved", () -> Boolean.toString(java.util.Arrays.equals(originalManifest,
            Files.readAllBytes(new File(root,SaveManager.MANIFEST).toPath()))));
        report("restore", () -> {
            // Restore a historical contaminated backup, independent of how the current backup writer filters it.
            File dir=new File(base,"historical-backup"), saves=new File(dir,"save"); saves.mkdirs();
            Files.copy(new File(root,"option.sav").toPath(),new File(saves,"option.sav").toPath());
            Files.copy(new File(root,SaveManager.MANIFEST).toPath(),new File(saves,SaveManager.MANIFEST).toPath());
            SaveManager.restore(context.getFilesDir(),new SaveManager.Backup(dir,"manual",0,0),1700000001000L);
            return content(root);
        });
    }
}
'''


def compile_mirror(root, work, javac, revision=None):
    """Use actual checked-out or git-show sources, with no historical production-code transformations."""
    java = 'games/totk/android/java/com/aure/totk/setup'
    sources = []
    for name in ('SaveManager.java', 'SaveMirror.java', 'Settings.java', 'GameLock.java'):
        if revision is None:
            sources.append(Path(root) / java / name)
        else:
            source = work / 'production' / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(subprocess.run(['git', 'show', f'{revision}:{java}/{name}'], cwd=root,
                                             check=True, capture_output=True).stdout)
            sources.append(source)
    for name, text in {**STUBS, 'com/aure/totk/setup/SaveMetadataProbe.java': PROBE}.items():
        source = work / 'boundary' / name
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(text, encoding='utf-8')
        sources.append(source)
    classes = work / 'classes'
    subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(classes), *map(str, sources)],
                   check=True, capture_output=True, text=True)
    return classes
