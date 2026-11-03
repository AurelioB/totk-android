package com.aure.totk.setup;

import java.io.File;

/** Host probe for CompanionArt (tests/test_android_companion_art.py): "GENERATOR <n>", then per argument folder
 * "<generator> <current|outdated>". */
public final class CompanionArtProbe {
    public static void main(String[] args) {
        System.out.println("GENERATOR " + CompanionArt.GENERATOR);
        for (String dir : args) {
            File f = new File(dir);
            System.out.println(CompanionArt.generator(f) + " " + (CompanionArt.current(f) ? "current" : "outdated"));
        }
    }
}
