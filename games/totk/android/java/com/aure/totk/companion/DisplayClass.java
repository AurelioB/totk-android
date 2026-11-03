package com.aure.totk.companion;

/** Which secondary display the companion may use, decided from the displays present and never from the SoC (the
 * Odin's chip family also ships in single-screen phones). Plain Java without Android types so the host tests can run it
 * with fake displays (tests/test_android_displays.py); PresentationController.secondScreen feeds it the real ones.
 *
 * A presentation display is a built-in panel when its DeviceProductInfo reports CONNECTION_TO_SINK_BUILT_IN (Retroid
 * Pocket Duo), or UNKNOWN with the same manufacturer PnP id as the main panel (AYN Thor reports UNKNOWN for both of its
 * panels). Anything else (HDMI/DisplayPort/USB-C monitors with their own EDID id, Miracast, cast and screen-recording
 * virtual displays, the developer "simulate secondary displays" overlay, all of which have no product info or another
 * id) is external and ignored by default. */
public final class DisplayClass {
    private DisplayClass() { }
    /** classify() results. */
    public static final int NONE = 0, BUILT_IN = 1, EXTERNAL = 2;
    /** DeviceProductInfo.CONNECTION_TO_SINK_* values (API 31), and NO_INFO for a display without product info. */
    public static final int SINK_UNKNOWN = 0, SINK_BUILT_IN = 1, NO_INFO = -1;

    /** One presentation display as the classifier sees it. */
    public static final class Screen {
        final int id, sink;
        final boolean usable;
        final String pnp;
        /** usable = valid, not private and not the display the owner is on; sink = NO_INFO without product info. */
        public Screen(int id, boolean usable, int sink, String pnp) { this.id = id; this.usable = usable; this.sink = sink; this.pnp = pnp; }
    }

    /** Whether this display is a built-in panel of the device. */
    public static boolean builtIn(Screen s, String mainPnp) {
        if (s.sink == SINK_BUILT_IN) return true;
        return s.sink == SINK_UNKNOWN && mainPnp != null && !mainPnp.isEmpty() && mainPnp.equals(s.pnp);
    }
    /** The id of the first usable built-in panel, or -1: the only display the companion presents on. */
    public static int select(Screen[] screens, String mainPnp) {
        for (Screen s : screens) if (s.usable && builtIn(s, mainPnp)) return s.id;
        return -1;
    }
    /** NONE, BUILT_IN (a built-in second panel, preferred over any external one) or EXTERNAL (only other displays). */
    public static int classify(Screen[] screens, String mainPnp) {
        int found = NONE;
        for (Screen s : screens) {
            if (!s.usable) continue;
            if (builtIn(s, mainPnp)) return BUILT_IN;
            found = EXTERNAL;
        }
        return found;
    }
}
