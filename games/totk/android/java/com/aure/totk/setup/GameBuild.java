package com.aure.totk.setup;

/** Which game this app build runs: what install.json must name and where its files are under the game folder.
 *  Tears of the Kingdom 1.4.3 here; another game's build replaces this file (apk/build_dex.py --game). */
final class GameBuild {
    private GameBuild() { }
    static final String GAME = "totk", VERSION = "v143";
    static final long ROMFS_SIZE = 17_052_625_232L;
}
