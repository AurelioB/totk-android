package com.aure.totk.setup;

/** Breath of the Wild 1.6.0 (apk/build_dex.py --game botw160): replaces java/.../GameBuild.java in that build. */
final class GameBuild {
    private GameBuild() { }
    static final String GAME = "botw", VERSION = "botw160";
    static final long ROMFS_SIZE = 15_131_979_952L;
    static final String APP = "BotW Native", TITLE = "Breath of the Wild", VERSION_LABEL = "1.6.0";
    /** Blue in place of the lime selection and the teal accent, each as bright as the colour it replaces (relative
     *  luminance .75 and .59 against .74 and .59), and blue-slate surfaces (Theme.tint). */
    static final int SEL = 0xffa6ebff, ACCENT = 0xff66d8fa, SURFACE_HUE = 208;
}
