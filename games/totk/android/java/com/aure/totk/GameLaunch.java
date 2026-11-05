package com.aure.totk;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.os.Bundle;
import android.view.Display;
import com.aure.totk.companion.DisplayClass;
import com.aure.totk.companion.PresentationController;

/** Route only dual-panel devices to their main screen; keep Android's normal task/recents behavior otherwise. */
public final class GameLaunch {
    private GameLaunch() { }

    public static void start(Activity owner, boolean cold) {
        Intent game = new Intent(owner, TotkActivity.class);
        Bundle options = null;
        if (PresentationController.deviceSecondScreen(owner) == DisplayClass.BUILT_IN) {
            game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            // A cold start must make the GAME the task's base Intent, not retain the finished menu's launcher
            // Intent. Android 16 can otherwise create another singleTask game instead of finding it across displays.
            // Never clear a task that already contains a game: its native runtime is process-global.
            if (cold) game.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK);
            options = ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle();
        }
        owner.startActivity(game, options);
    }

    /** A launcher may bring an existing task to its own display without going through SetupActivity again. */
    static boolean onMainScreen(Activity owner) {
        Display current = owner.getDisplay();
        return current == null || current.getDisplayId() == Display.DEFAULT_DISPLAY
                || PresentationController.deviceSecondScreen(owner) != DisplayClass.BUILT_IN;
    }
}
