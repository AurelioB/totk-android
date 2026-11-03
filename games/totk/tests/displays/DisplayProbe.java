package com.aure.totk.companion;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Host driver for tests/test_android_displays.py: one fake display set per stdin line, one answer line each.
 *    <main pnp|-> [id:usable:sink:pnp ...]   usable 1/0, sink -1 (no product info) / 0 unknown / 1 built-in / 2 direct
 *                                             / 3 transitive, pnp '-' for none
 * answers "<classify> <select>" with classify NONE / BUILT_IN / EXTERNAL and the selected display id (-1 for none). */
public final class DisplayProbe {
    public static void main(String[] args) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        String[] names = {"NONE", "BUILT_IN", "EXTERNAL"};
        for (String line; (line = in.readLine()) != null;) {
            String[] w = line.trim().split("\\s+");
            String main = w[0].equals("-") ? null : w[0];
            List<DisplayClass.Screen> screens = new ArrayList<>();
            for (int i = 1; i < w.length; i++) {
                String[] f = w[i].split(":");
                screens.add(new DisplayClass.Screen(Integer.parseInt(f[0]), f[1].equals("1"), Integer.parseInt(f[2]), f[3].equals("-") ? null : f[3]));
            }
            DisplayClass.Screen[] all = screens.toArray(new DisplayClass.Screen[0]);
            System.out.println(names[DisplayClass.classify(all, main)] + " " + DisplayClass.select(all, main));
        }
    }
}
