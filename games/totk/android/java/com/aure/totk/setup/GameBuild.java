package com.aure.totk.setup;

/** Which game this app build runs: what install.json must name and where its files are under the game folder, what
 *  the menu calls the app and the game, and the menu's accent colours.
 *  Tears of the Kingdom 1.4.3 here; another game's build replaces this file (apk/build_dex.py --game). */
final class GameBuild {
    private GameBuild() { }
    static final String GAME = "totk", VERSION = "v143";
    static final long ROMFS_SIZE = 17_052_625_232L;
    static final String APP = "TotK Native", TITLE = "Tears of the Kingdom", VERSION_LABEL = "1.4.3";
    /** Theme.SEL (selection) and Theme.ZONAI (accent), and the hue Theme.tint gives the green-tinted surfaces
     *  (-1: the design's own colours, unchanged). */
    static final int SEL = 0xffd4ea74, ACCENT = 0xff57dfbf, SURFACE_HUE = -1;
}
