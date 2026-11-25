package com.aure.totk;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

/** The ordinary close binding and opt-in Settings hold have distinct Intent actions.
 * No game/runtime initialization: a bind racing with game exit may create an otherwise empty process. */
public final class GameControlService extends Service {
    private final IBinder binder = new Binder();
    private boolean settingsBound;

    @Override public IBinder onBind(Intent intent) {
        if (intent != null && GameHold.ACTION.equals(intent.getAction())) {
            settingsBound = true;
            GameHold.settingsActive(this, true);
        }
        return binder;
    }
    @Override public boolean onUnbind(Intent intent) {
        if (intent != null && GameHold.ACTION.equals(intent.getAction())) releaseSettings();
        return false; // A later Settings binding must deliver onBind again, not onRebind.
    }
    private void releaseSettings() {
        if (!settingsBound) return;
        settingsBound = false;
        GameHold.settingsActive(this, false);
    }
    @Override public void onDestroy() {
        releaseSettings();
        super.onDestroy();
    }
}
