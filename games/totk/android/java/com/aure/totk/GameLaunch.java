package com.aure.totk;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.Intent;
import android.os.Bundle;
import android.view.Display;
import com.aure.totk.companion.DisplayClass;
import com.aure.totk.companion.PresentationController;

/** Route only dual-panel devices to their main screen; keep Android's normal task/recents behavior otherwise. */
public final class GameLaunch {
    private GameLaunch() { }

    public static void start(Activity owner) {
        Intent game = new Intent(owner, TotkActivity.class);
        Bundle options = null;
        if (PresentationController.deviceSecondScreen(owner) == DisplayClass.BUILT_IN) {
            // Android 16's singleTask search can skip a task on the other display and create a second NativeActivity.
            // Move the existing task first, preserving its menu/stack; only then deliver the normal game intent.
            game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            options = ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle();
            ActivityManager manager = owner.getSystemService(ActivityManager.class);
            if (manager == null) throw new IllegalStateException("Activity manager unavailable");
            for (ActivityManager.AppTask task : manager.getAppTasks()) {
                ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                if (game.getComponent().equals(info.baseActivity) || game.getComponent().equals(info.topActivity)) {
                    manager.moveTaskToFront(info.taskId, 0, options);
                    break;
                }
            }
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
