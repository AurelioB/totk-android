package com.aure.totk.setup;

/** JNI surface of libtotk_setup.so (android/setup/). Loaded only in the ":setup" process, never in the game's. */
public final class NativeSetup {
    static { System.loadLibrary("totk_setup"); }
    private NativeSetup() { }
    public interface Listener { void onProgress(String stage, long done, long total, String detail); }
    /** Blocking (worker thread). Runs/resumes all pending stages. Returns result JSON:
     *  {"ok":true,"install":{...install.json...}} or {"ok":false,"stage":"romfs","code":"hash_mismatch|missing_input|bad_keys|
     *  unsupported_version|no_space|io|cancelled|internal","message":"human readable, no key material"} */
    public static native String run(String requestJson, Listener listener);
    /** Fast identification of picked inputs; writes nothing. Returns JSON {"ok":..,"items":[{"role":"base|update|keys|romfs|exefs",
     *  "ok":true,"what":"TotK base 1.0.0 (XCI)","problem":null}], "route":"decrypted|containers|null","need":[...missing roles]} */
    public static native String inspect(String requestJson);
    public static native void cancel();
    /** Blocking (worker thread). Copies the game data from one app folder to another (an SD card and back):
     *  request {"from":dir,"to":dir,"privateDir":..,"romfsPartBytes":N}; stages into <to>/.move, verifies, renames into
     *  place, install.json last. Progress stage "move". Returns {"ok":true} or {"ok":false,"stage":"move","code":..,
     *  "message":..}; cancel() stops it and leaves the source untouched. Never deletes the source. */
    public static native String move(String requestJson, Listener listener);
    /** Removes the game data under dir (rewritten/ extracted/ game-data/ shaders/ companion/ install.json driver.so
     *  .import .move, nothing else). Same result JSON as move. */
    public static native String removeGameData(String dir);
}
