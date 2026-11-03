package com.aure.totk.setup;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/** Host driver for tests/test_android_direct_access.py: one command per stdin line (arguments separated by tabs), one
 * answer line per command. "-" stands for null.
 *    kind PATH PRIVATE_GAME PACKAGE                StoragePaths.kind
 *    problem PATH STORAGE_ROOT PRIVATE_FILES       StoragePaths.folderProblem: ok or the code
 *    target PICKED USABLE(0/1)                     StoragePaths.folderTarget
 *    doc AUTHORITY ID PRIMARY VOLUMES              StoragePaths.documentPath; VOLUMES "UUID=/dir,UUID=/dir"
 *    media DATA VOLUME_NAME RELATIVE_PATH NAME PRIMARY VOLUMES PICKED   StoragePaths.mediaStorePath; PICKED: the file the
 *                                                  picker's descriptor is open on ("-": it could not be opened)
 *    ids DOCUMENT_ID                               StoragePaths.mediaStoreId
 *    dsize GAME                                    DataSize.get, waiting up to 20 s for its walk: "BYTES WALKS"
 *    dtimeout MS / dinvalidate                     DataSize.timeoutMs / DataSize.invalidate: "ok"
 *    initial LEGACY_HAS PRIVATE_HAS LEGACY         StoragePaths.initialChoice: the path or "private"
 *    pending CURRENT LEGACY PLAYABLE DECLINED      StoragePaths.migrationPending
 *    direct GRANTED PATH READABLE SIZE REPORTED    StoragePaths.useDirectPath
 *    candidates DIR                                SaveManager.candidates: "path size link" sorted, joined by '|'
 *    fspush TOP SNAPSHOT                           readFolder + push through SaveManager.FileStore: the generation
 *    fsread TOP                                    SaveManager.readFolder over a FileStore: gen NAME FILES | incomplete | none
 *    fsfetch TOP STAGING                           SaveManager.fetch (verified) over a FileStore: ok FILES or invalid CODE */
public final class DirectAccessProbe {
    private static String arg(String s) { return s.equals("-") ? null : s; }
    private static boolean flag(String s) { return s.equals("1") || s.equals("true"); }
    private static Map<String, String> volumes(String list) {
        Map<String, String> volumes = new HashMap<>();
        if (!list.equals("-")) for (String pair : list.split(",")) { String[] kv = pair.split("=", 2); volumes.put(kv[0], kv[1]); }
        return volumes;
    }
    /** {st_dev, st_ino} as Os.stat gives them on Android: links followed, regular files only, null when unreadable. */
    private static long[] identity(String path) {
        try {
            Path p = Paths.get(path);
            if (!Files.isRegularFile(p)) return null;
            Map<String, Object> a = Files.readAttributes(p, "unix:dev,ino");
            return new long[]{((Number) a.get("dev")).longValue(), ((Number) a.get("ino")).longValue()};
        } catch (IOException | RuntimeException unreadable) { return null; }
    }

    public static void main(String[] argv) throws IOException {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        for (String line; (line = in.readLine()) != null;) {
            if (line.isEmpty()) continue;
            String[] w = line.split("\t", -1);
            String answer;
            try {
                switch (w[0]) {
                    case "kind": answer = StoragePaths.kind(w[1], w[2], w[3]).name(); break;
                    case "problem": { String p = StoragePaths.folderProblem(arg(w[1]), w[2], arg(w[3])); answer = p == null ? "ok" : p; break; }
                    case "target": answer = StoragePaths.folderTarget(w[1], flag(w[2])); break;
                    case "doc": {
                        String p = StoragePaths.documentPath(arg(w[1]), arg(w[2]), arg(w[3]), volumes(w[4]));
                        answer = p == null ? "-" : p;
                        break;
                    }
                    case "media": {
                        // PICKED: the picked file's path, or its "dev:ino" taken while it could still be reached.
                        String[] devIno = w[7].split(":");
                        long[] picked = w[7].equals("-") ? null : devIno.length == 2 && !w[7].startsWith("/")
                                ? new long[]{Long.parseLong(devIno[0]), Long.parseLong(devIno[1])} : identity(w[7]);
                        String p = StoragePaths.mediaStorePath(arg(w[1]), arg(w[2]), arg(w[3]), arg(w[4]), arg(w[5]), volumes(w[6]),
                                picked, DirectAccessProbe::identity);
                        answer = p == null ? "-" : p;
                        break;
                    }
                    case "ids": answer = String.valueOf(StoragePaths.mediaStoreId(arg(w[1]))); break;
                    case "dsize": {
                        java.util.concurrent.CountDownLatch walked = new java.util.concurrent.CountDownLatch(1);
                        long bytes = DataSize.get(new File(w[1]), walked::countDown);
                        if (bytes == DataSize.MEASURING) {
                            try { walked.await(20, java.util.concurrent.TimeUnit.SECONDS); } catch (InterruptedException e) { throw new IOException(e); }
                            bytes = DataSize.peek();
                        }
                        answer = bytes + " " + DataSize.walks;
                        break;
                    }
                    case "dtimeout": DataSize.timeoutMs = Long.parseLong(w[1]); answer = "ok"; break;
                    case "dinvalidate": DataSize.invalidate(); answer = "ok"; break;
                    case "initial": { String p = StoragePaths.initialChoice(flag(w[1]), flag(w[2]), arg(w[3])); answer = p == null ? "private" : p; break; }
                    case "pending": answer = String.valueOf(StoragePaths.migrationPending(w[1], arg(w[2]), flag(w[3]), flag(w[4]))); break;
                    case "direct": answer = String.valueOf(StoragePaths.useDirectPath(flag(w[1]), arg(w[2]), flag(w[3]), Long.parseLong(w[4]), Long.parseLong(w[5]))); break;
                    case "candidates": {
                        List<String> out = new ArrayList<>();
                        for (SaveManager.Candidate c : SaveManager.candidates(new File(w[1]))) out.add(c.path + " " + c.size + " " + c.link);
                        out.sort(null);
                        answer = String.join("|", out);
                        break;
                    }
                    case "fspush": {
                        SaveManager.FileStore store = new SaveManager.FileStore(new File(w[1]));
                        SaveManager.FolderState state = SaveManager.readFolder(store);
                        SaveManager.Manifest m = SaveManager.manifest(new File(w[2]), null, "probe", "host", 0);
                        answer = SaveManager.push(store, new File(w[2]), m, state);
                        break;
                    }
                    case "fsread": {
                        SaveManager.FolderState s = SaveManager.readFolder(new SaveManager.FileStore(new File(w[1])));
                        answer = s.incomplete ? "incomplete " + s.problem : s.manifest == null ? "none" : "gen " + s.generation + " " + s.manifest.files.size();
                        break;
                    }
                    case "fsfetch": {
                        SaveManager.FileStore store = new SaveManager.FileStore(new File(w[1]));
                        SaveManager.FolderState s = SaveManager.readFolder(store);
                        SaveManager.fetch(store, s, new File(w[2]), true);
                        answer = "ok " + s.manifest.files.size();
                        break;
                    }
                    default: answer = "unknown " + w[0];
                }
            } catch (SaveManager.Invalid invalid) { answer = "invalid " + invalid.code; }
            System.out.println(answer);
            System.out.flush();
        }
    }
}
