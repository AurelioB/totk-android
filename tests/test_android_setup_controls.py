"""Host regressions for production setup control/lifecycle methods and the native diagnostic close reader.
Android widgets/services are contract doubles; the exercised method bodies are extracted unchanged from production.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
JAVA = GAME / 'android/java/com/aure/totk/setup'


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


JAVA_PRELUDE = r'''package com.aure.totk.setup;
import java.io.*;
import java.nio.file.*;
import java.util.*;
class View {
    interface OnFocusChangeListener { void onFocusChange(View view, boolean has); }
    interface OnClickListener { void onClick(View view); }
    private OnFocusChangeListener focus;
    private OnClickListener click;
    boolean enabled = true, focused;
    float alpha = 1;
    boolean isEnabled() { return enabled; }
    void setEnabled(boolean value) { enabled = value; }
    void setAlpha(float value) { alpha = value; }
    OnFocusChangeListener getOnFocusChangeListener() { return focus; }
    void setOnFocusChangeListener(OnFocusChangeListener value) { focus = value; }
    void setOnClickListener(OnClickListener value) { click = value; }
    boolean requestFocus() { if (!enabled) return false; focused = true; if (focus != null) focus.onFocusChange(this, true); return true; }
    void performClick() { if (enabled && click != null) click.onClick(this); }
}
class Widgets {
    static final class Row extends View {
        boolean dim;
        boolean dimmed() { return dim; }
        void dimmed(boolean value) { dim = value; setAlpha(value ? .42f : 1); }
    }
}
class SystemClock { static long now; static long uptimeMillis() { return now; } }
class ComponentName {}
interface IBinder {}
interface ServiceConnection {
    void onServiceConnected(ComponentName name, IBinder service);
    void onServiceDisconnected(ComponentName name);
    void onBindingDied(ComponentName name);
    void onNullBinding(ComponentName name);
}
class GameControlService {}
class ExitNotice {
    void resume(SetupActivity activity, java.util.function.BooleanSupplier ready, java.util.function.Consumer<Boolean> visible) {}
    void pause() {}
}
class Intent {
    final Class<?> service;
    String action;
    Intent(Object context, Class<?> service) { this.service = service; }
    Intent setAction(String value) { action = value; return this; }
}
class GameHold {
    static final String ACTION = "com.aure.totk.action.GAME_HOLD";
    static boolean on;
    static int endedSessions;
    static final List<String> lines = new ArrayList<>();
    static boolean enabled(SetupActivity activity) { return on; }
    static void log(SetupActivity activity, String message) { lines.add(message); }
    static void endSessionLog() { endedSessions++; }
}
class TextView { void setText(String text) {} }
class StorageManager {
    int registered, removed;
    void registerStorageVolumeCallback(Object executor, Runnable callback) { registered++; }
    void unregisterStorageVolumeCallback(Runnable callback) { removed++; }
}
class SetupService {
    static final class Status { boolean running; }
    static final Status status = new Status();
    static int listens, unlistens;
    static void listen(SetupActivity activity) { listens++; }
    static void unlisten(SetupActivity activity) { unlistens++; }
}
class Strings {
    static final String SAVE_WAIT_TIMEOUT = "save timeout", CLOSE_GAME_TIMEOUT = "close timeout";
    static String gameLockError(String reason) { return reason; }
}
class BaseActivity {
    protected void onStart() {}
    protected void onResume() {}
    protected void onStop() {}
    protected void onDestroy() {}
}
class Updater { void dispose() {} }
class Worker { void shutdownNow() {} void shutdown() {} }
class Handler {
    final Set<Runnable> pending = new HashSet<>();
    int posted;
    void postDelayed(Runnable value, long delay) { pending.add(value); posted++; }
    void post(Runnable value) { pending.add(value); posted++; }
    void removeCallbacks(Runnable value) { pending.remove(value); }
}
class InstallState {
    static GameLock.Result next = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
    static GameLock.Result gameState(SetupActivity activity) { activity.checks++; return next; }
'''

JAVA_ACTIVITY = r'''
}
public final class SetupActivity extends BaseActivity {
    enum Screen { HOME, SAVES, WELCOME, DATA, STORAGE, SETUP_STORAGE, INSTALL, UPDATE_DOWNLOAD }
    boolean ui, started = true, uiStarted, closingGame, closeSent, noteOk;
    GameLock.Result gameState = InstallState.next;
    final List<View> gameControls = new ArrayList<>();
    Runnable blockedAction;
    String blockedNeed = "", closeId = "probe";
    String note = "";
    TextView gameNotice;
    final ExitNotice exitNotice = new ExitNotice();
    ServiceConnection closeConnection, gameHoldConnection;
    boolean gameHoldFailed, gameHoldOpening, finishing, destroyed;
    static final int BIND_AUTO_CREATE = 1, BIND_IMPORTANT = 64;
    int binds, unbinds, bindFlags;
    boolean bindAccepted = true, unbindThrows;
    RuntimeException bindFailure;
    Runnable duringBind;
    final List<Intent> bindIntents = new ArrayList<>();
    final List<ServiceConnection> bindConnections = new ArrayList<>(), unboundConnections = new ArrayList<>();
    final List<Integer> bindingFlags = new ArrayList<>();
    final Set<ServiceConnection> activeBindings = Collections.newSetFromMap(new IdentityHashMap<>());
    long closeDeadline = 10000, closeStartedAt, gameCheckedAt, cancelArmedAt;
    final Handler main = new Handler();
    final StorageManager storage = new StorageManager();
    final Runnable volumeChanged = () -> {};
    static final long REFRESH_MS = 500;
    Screen screen = Screen.HOME;
    final File files;
    Updater updater;
    final Worker worker = new Worker(), saveWorker = new Worker();
    void completeSettingsMove() {}
    void requestedUpdate() {}
    void offerPendingUpdate() {}
    void updateInstallResult() {}
    void updateDownloadProgress() {}
    int renders, statuses, syncs, checks, notices;
    SetupActivity(File files) throws IOException { this.files = files; Files.createDirectories(new File(files, "state").toPath()); }
    File getFilesDir() { return files; }
    boolean isFinishing() { return finishing; }
    boolean isDestroyed() { return destroyed; }
    void render() { renders++; }
    void updateGameNotice() { notices++; }
    boolean bindService(Intent intent, ServiceConnection connection, int flags) {
        binds++; bindFlags = flags;
        bindIntents.add(intent); bindConnections.add(connection); bindingFlags.add(flags);
        if (bindFailure != null) throw bindFailure;
        if (bindAccepted) activeBindings.add(connection);
        if (duringBind != null) duringBind.run();
        return bindAccepted;
    }
    void unbindService(ServiceConnection connection) {
        unbinds++; unboundConnections.add(connection);
        boolean registered = activeBindings.remove(connection);
        if (unbindThrows || !registered) throw new IllegalArgumentException("connection not registered");
    }
    void onStatus() { statuses++; }
    void syncMirror(boolean manual) { syncs++; }
    void disarmCancel() { cancelArmedAt = 0; }
    Object getMainExecutor() { return this; }
    <T> T getSystemService(Class<T> type) { return type.cast(storage); }
    static void require(boolean condition, String reason) { if (!condition) throw new AssertionError(reason); }
'''

JAVA_TESTS = r'''
    private static ServiceConnection startHold(SetupActivity activity) {
        GameHold.on = true;
        InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
        activity.started = false;
        activity.onStart();
        require(activity.gameHoldConnection != null && activity.binds == 1, "visible GAME did not acquire its hold");
        require(activity.bindFlags == (BIND_AUTO_CREATE | BIND_IMPORTANT), "hold binding lost its exact importance flags");
        require(activity.bindIntents.get(0).service == GameControlService.class
                && GameHold.ACTION.equals(activity.bindIntents.get(0).action), "hold did not use its dedicated service action");
        require(activity.closeConnection == null && !activity.closingGame, "opening Settings started Close");
        return activity.gameHoldConnection;
    }
    private static void lateCallbacks(ServiceConnection connection) {
        connection.onServiceConnected(null, null);
        connection.onServiceDisconnected(null);
        connection.onBindingDied(null);
        connection.onNullBinding(null);
    }
    public static void main(String[] args) throws Exception {
        SetupActivity activity = new SetupActivity(new File(args[1]));
        int[] mutations = {0}, details = {0};
        switch (args[0]) {
            case "focus": {
                InstallState.next = activity.gameState = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                Widgets.Row row = new Widgets.Row();
                row.setOnFocusChangeListener((v, has) -> { if (has) details[0]++; });
                row.setOnClickListener(v -> { throw new AssertionError("original mutation bypassed game gate"); });
                Runnable retry = () -> mutations[0]++;
                activity.gameControl(row, "Changing the driver", retry);
                require(row.isEnabled() && row.requestFocus(), "gated row no longer accepts D-pad focus");
                require(row.dimmed() && details[0] == 1, "gating lost the dimmed look or original detail listener");
                require(activity.blockedAction == retry, "focus did not remember the exact intended operation");
                row.performClick();
                require(mutations[0] == 0 && activity.blockedAction == retry && activity.renders == 1, "activation mutated instead of showing the running-game reason");
                View card = new View();
                activity.gameControl(card, "Moving game data", retry);
                require(card.requestFocus() && card.alpha == .42f, "gated card lost focus or dimming");
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
                row.performClick();
                require(mutations[0] == 1, "cleared game gate did not allow the recorded operation");
                View unavailable = new View(); unavailable.setEnabled(false);
                activity.gameControl(unavailable, "Installing", retry);
                require(!unavailable.isEnabled(), "gating enabled a control disabled for another reason");
                break;
            }
            case "free-control": {
                InstallState.next = activity.gameState = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
                View view = new View();
                boolean[] busy = {true};
                view.setOnClickListener(v -> { details[0]++; if (!busy[0]) mutations[0]++; });
                activity.gameControl(view, "Changing saves", () -> mutations[0]++);
                view.performClick();
                require(details[0] == 1 && mutations[0] == 0, "free control lost its original busy guard");
                busy[0] = false;
                view.performClick();
                require(details[0] == 2 && mutations[0] == 1, "free control lost its original activation listener");
                break;
            }
            case "stale-save": {
                Files.createDirectories(new File(activity.files, "state").toPath());
                Files.writeString(new File(activity.files, "state/save-busy").toPath(), "left by dead game");
                activity.closingGame = true; activity.blockedAction = () -> mutations[0]++;
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
                activity.closePoll.run();
                require(mutations[0] == 1 && !activity.closingGame && activity.note.isEmpty(), "stale save-busy blocked completion after game death");
                require(activity.main.pending.isEmpty(), "completed close kept waiting for the dead game's save");
                break;
            }
            case "live-save": {
                Files.createDirectories(new File(activity.files, "state").toPath());
                Files.writeString(new File(activity.files, "state/save-busy").toPath(), "saving");
                activity.closingGame = true; activity.blockedAction = () -> mutations[0]++;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended marker=absent");
                activity.closePoll.run();
                require(mutations[0] == 0 && activity.closingGame && !activity.closeSent, "close did not wait for a live game's save");
                require(activity.main.pending.contains(activity.closePoll), "live save wait was not scheduled");
                require(!new File(activity.files, "state/close-request").exists(), "close command sent during a live save");
                break;
            }
            case "late-ui": {
                activity.onStart();
                require(activity.started && !activity.uiStarted && activity.main.pending.isEmpty(), "invisible skip-menu path registered UI work");
                activity.showUi();
                require(activity.uiStarted && activity.storage.registered == 1 && SetupService.listens == 1, "late UI did not start its listeners/timer");
                require(activity.main.pending.contains(activity.refresh), "late UI has no refresh callback");
                activity.startUi();
                require(activity.storage.registered == 1 && activity.main.posted == 1, "late UI registered duplicate callbacks");
                SystemClock.now = 1000;
                activity.main.pending.remove(activity.refresh); activity.refresh.run();
                require(activity.checks == 1 && activity.main.pending.size() == 1, "one-second game-state refresh is missing");
                activity.onStop();
                require(!activity.started && !activity.uiStarted && activity.storage.removed == 1 && activity.main.pending.isEmpty(), "stop leaked refresh/listeners");
                int posted = activity.main.posted; activity.refresh.run();
                require(activity.main.posted == posted, "late callback restarted polling after stop");
                activity.onStart();
                require(activity.storage.registered == 2 && activity.main.pending.size() == 1, "resume failed to register exactly one timer");
                break;
            }
            case "bind-save": {
                Files.createDirectories(new File(activity.files, "state").toPath());
                Path saving = new File(activity.files, "state/save-busy").toPath();
                Files.writeString(saving, "saving");
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.closeGame();
                require(activity.binds == 1 && activity.bindFlags == BIND_AUTO_CREATE, "foreground binding missing or over-prioritized");
                activity.closePoll.run();
                require(!activity.closeSent && activity.closeConnection != null, "save was not allowed to progress while bound");
                Files.delete(saving);
                activity.closePoll.run();
                require(activity.closeSent && Files.exists(new File(activity.files, "state/close-request").toPath()), "finished save did not proceed to native close");
                require(activity.unbinds == 0, "binding released before native close finished");
                break;
            }
            case "disconnect": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                ServiceConnection connection = activity.closeConnection;
                connection.onServiceDisconnected(null);
                activity.closePoll.run();
                require(activity.closingGame && mutations[0] == 0, "disconnect bypassed the lock");
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "lock=free");
                connection.onServiceDisconnected(null);
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(!activity.closingGame && activity.unbinds == 1 && mutations[0] == 1, "game death did not release binding and retry");
                connection.onBindingDied(null);
                require(activity.main.pending.isEmpty(), "late death callback restarted a completed close");
                break;
            }
            case "bind-race": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.duringBind = () -> InstallState.next = new GameLock.Result(GameLock.State.FREE, "empty default process, no lock");
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(activity.binds == 1 && activity.unbinds == 1 && mutations[0] == 1, "empty service process was mistaken for GAME");
                require(!new File(activity.files, "state/close-request").exists(), "empty process received a close request");
                break;
            }
            case "bind-failure": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.bindAccepted = false;
                activity.closeGame();
                require(!activity.closingGame && activity.closeConnection == null && activity.unbinds == 1, "refused bind leaked close state");
                require(!activity.note.isEmpty() && activity.main.pending.isEmpty(), "refused bind silently kept polling");
                break;
            }
            case "close-timeout": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.closeGame(); activity.closePoll.run();
                SystemClock.now = activity.closeDeadline;
                activity.closePoll.run();
                require(!activity.closingGame && activity.unbinds == 1 && activity.closeConnection == null, "timeout leaked foreground binding");
                require(!new File(activity.files, "state/close-request").exists() && activity.main.pending.isEmpty(), "timeout left its request/poller behind");
                break;
            }
            case "stop-close": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame(); activity.closePoll.run();
                ServiceConnection old = activity.closeConnection;
                activity.onStop();
                require(!activity.closingGame && activity.closeConnection == null && activity.unbinds == 1, "stop/recreation leaked binding");
                require(!new File(activity.files, "state/close-request").exists() && activity.main.pending.isEmpty(), "stop leaked request/poller");
                old.onServiceConnected(null, null);
                activity.closePoll.run();
                require(activity.main.pending.isEmpty() && mutations[0] == 0, "old activity retried an abandoned action");
                activity.onStart();
                activity.closeGame();
                require(activity.binds == 2 && activity.closeConnection != old, "resumed activity reused old connection");
                old.onServiceDisconnected(null);
                require(activity.closingGame, "old disconnect changed a new close");
                break;
            }
            case "already-free": {
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(activity.binds == 0 && mutations[0] == 1 && !activity.closingGame, "FREE lock spawned a service process");
                break;
            }
            case "hold-opening":
            case "hold-opening-stop": {
                GameHold.on = true;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.started = false; activity.gameHoldOpening = true;
                activity.duringBind = () -> require(!activity.ui && !activity.uiStarted,
                        "cold Settings allocated its UI before acquiring importance");
                activity.updateGameHold();
                ServiceConnection connection = activity.gameHoldConnection;
                require(activity.binds == 1 && connection != null && !activity.started
                        && activity.bindFlags == (BIND_AUTO_CREATE | BIND_IMPORTANT)
                        && GameHold.ACTION.equals(activity.bindIntents.get(0).action),
                        "cold Settings did not bind before onStart");
                if (args[0].equals("hold-opening")) {
                    activity.onStart(); activity.onResume();
                    require(!activity.gameHoldOpening && activity.gameHoldConnection == connection && activity.binds == 1,
                            "onStart duplicated or discarded the cold-opening hold");
                }
                activity.onStop();
                int logs = GameHold.lines.size();
                activity.updateGameHold(); lateCallbacks(connection);
                require(!activity.started && !activity.gameHoldOpening && activity.gameHoldConnection == null
                        && activity.binds == 1 && activity.unbinds == 1 && activity.activeBindings.isEmpty()
                        && GameHold.lines.size() == logs, "stopped cold opening retained or revived its hold");
                break;
            }
            case "hold-opening-bind-false":
            case "hold-opening-bind-throw":
            case "hold-opening-disconnect": {
                GameHold.on = true;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.started = false; activity.gameHoldOpening = true;
                activity.bindAccepted = !args[0].equals("hold-opening-bind-false");
                if (args[0].equals("hold-opening-bind-throw")) activity.bindFailure = new SecurityException("cold bind denied");
                activity.updateGameHold();
                require(activity.binds == 1, "cold failure did not exercise binding");
                ServiceConnection failed = activity.bindConnections.get(0);
                if (args[0].equals("hold-opening-disconnect")) failed.onServiceDisconnected(null);
                require(activity.gameHoldConnection == null && activity.unbinds == 1 && activity.activeBindings.isEmpty(),
                        "failed cold opening retained its binding");
                activity.bindAccepted = true; activity.bindFailure = null;
                int logs = GameHold.lines.size();
                activity.onStart(); activity.onResume(); activity.updateGameHold(); lateCallbacks(failed);
                require(activity.binds == 1 && activity.unbinds == 1 && activity.gameHoldConnection == null
                        && GameHold.lines.size() == logs, "onStart retried a failed cold bind in the same visible lifecycle");
                activity.onStop(); activity.onStart();
                require(activity.binds == 2 && activity.gameHoldConnection != null && activity.gameHoldConnection != failed,
                        "new visible lifecycle could not retry a failed cold opening");
                break;
            }
            case "hold-start": {
                activity.duringBind = () -> require(activity.started && !activity.uiStarted
                        && activity.storage.registered == 0, "hold acquisition ran after UI listeners");
                ServiceConnection connection = startHold(activity);
                require(!activity.ui && activity.main.pending.isEmpty(), "hold forced UI allocation or polling");
                activity.onStart();
                activity.onResume();
                activity.updateGameHold();
                connection.onServiceConnected(null, null);
                activity.showUi();
                activity.onResume();
                require(activity.binds == 1 && activity.gameHoldConnection == connection
                        && activity.activeBindings.size() == 1, "start/resume/connected callbacks duplicated the hold");
                require(!new File(activity.files, "state/close-request").exists(), "hold sent a native close command");
                break;
            }
            case "hold-non-game": {
                GameHold.on = true;
                activity.started = false;
                activity.onStart();
                InstallState.next = new GameLock.Result(GameLock.State.ERROR, "lock probe denied");
                activity.onResume();
                activity.refreshGameState();
                activity.updateGameHold();
                require(activity.binds == 0 && activity.unbinds == 0 && GameHold.lines.isEmpty(),
                        "FREE or ERROR created an empty service process");
                break;
            }
            case "hold-invisible": {
                GameHold.on = true;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.started = false;
                activity.updateGameHold();
                activity.finishing = true;
                activity.onStart();
                activity.finishing = false; activity.destroyed = true;
                activity.onStart();
                require(activity.binds == 0 && activity.unbinds == 0 && GameHold.lines.isEmpty(),
                        "stopped, finishing or destroyed Settings acquired a hold");
                break;
            }
            case "hold-rotation": {
                ServiceConnection old = startHold(activity);
                activity.onStop();
                require(!activity.started && activity.gameHoldConnection == null && activity.unbinds == 1
                        && activity.activeBindings.isEmpty(), "rotation stop leaked its hold");
                int logs = GameHold.lines.size();
                activity.onStop(); activity.onDestroy();
                lateCallbacks(old);
                require(activity.unbinds == 1 && GameHold.lines.size() == logs && activity.main.pending.isEmpty(),
                        "stop/destroy or late callbacks repeated release");
                SetupActivity rotated = new SetupActivity(activity.files);
                rotated.started = false;
                rotated.onStart();
                ServiceConnection next = rotated.gameHoldConnection;
                require(next != null && next != old && rotated.binds == 1, "rotation failed to acquire a new connection");
                lateCallbacks(old);
                require(rotated.gameHoldConnection == next && rotated.unbinds == 0,
                        "old activity callback changed the recreated activity's hold");
                rotated.onStop();
                rotated.onStart();
                require(rotated.binds == 2 && rotated.gameHoldConnection != next, "stop/start reused a released hold");
                lateCallbacks(next);
                require(rotated.gameHoldConnection != null && rotated.unbinds == 1,
                        "stale connection callback released a newer hold");
                break;
            }
            case "hold-finish": {
                ServiceConnection connection = startHold(activity);
                activity.finishing = true;
                activity.updateGameHold();
                require(activity.gameHoldConnection == null && activity.unbinds == 1 && activity.activeBindings.isEmpty(),
                        "finishing Settings retained its hold");
                int logs = GameHold.lines.size();
                activity.onResume(); activity.onStop(); activity.onDestroy();
                lateCallbacks(connection);
                require(activity.binds == 1 && activity.unbinds == 1 && GameHold.lines.size() == logs,
                        "finish lifecycle or late callback revived its hold");
                break;
            }
            case "hold-destroy": {
                ServiceConnection connection = startHold(activity);
                activity.destroyed = true;
                activity.onDestroy();
                int logs = GameHold.lines.size();
                activity.onDestroy(); activity.updateGameHold();
                lateCallbacks(connection);
                require(activity.binds == 1 && activity.unbinds == 1 && activity.gameHoldConnection == null
                        && activity.activeBindings.isEmpty() && GameHold.lines.size() == logs,
                        "destroy leaked, duplicated or revived a hold without onStop");
                break;
            }
            case "hold-disconnect":
            case "hold-binding-died":
            case "hold-null-binding": {
                ServiceConnection connection = startHold(activity);
                connection.onServiceConnected(null, null);
                if (args[0].equals("hold-disconnect")) connection.onServiceDisconnected(null);
                else if (args[0].equals("hold-binding-died")) connection.onBindingDied(null);
                else connection.onNullBinding(null);
                require(activity.gameHoldConnection == null && activity.unbinds == 1 && activity.activeBindings.isEmpty(),
                        "dead/disconnected/null hold was not released immediately");
                int logs = GameHold.lines.size();
                lateCallbacks(connection);
                activity.onResume(); activity.refreshGameState(); activity.updateGameHold();
                require(activity.binds == 1 && activity.unbinds == 1 && GameHold.lines.size() == logs
                        && activity.main.pending.isEmpty(), "failed visible lifecycle restarted the empty game process");
                activity.onStop(); activity.onStart();
                ServiceConnection next = activity.gameHoldConnection;
                require(activity.binds == 2 && next != null && next != connection, "new visible lifecycle did not clear failure");
                lateCallbacks(connection);
                require(activity.gameHoldConnection == next && activity.unbinds == 1,
                        "late failure callback poisoned a newer connection");
                break;
            }
            case "hold-bind-false":
            case "hold-bind-throw": {
                GameHold.on = true;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.bindAccepted = !args[0].equals("hold-bind-false");
                if (args[0].equals("hold-bind-throw")) activity.bindFailure = new SecurityException("bind denied");
                activity.unbindThrows = true;
                activity.started = false;
                activity.onStart();
                require(activity.binds == 1 && activity.unbinds == 1 && activity.gameHoldConnection == null
                        && activity.activeBindings.isEmpty(), "refused/throwing bind leaked its connection");
                require(GameHold.endedSessions == 1, "failed unbind did not clear the cached hold log target");
                ServiceConnection failed = activity.bindConnections.get(0);
                activity.bindAccepted = true; activity.bindFailure = null; activity.unbindThrows = false;
                int logs = GameHold.lines.size();
                lateCallbacks(failed);
                activity.onResume(); activity.refreshGameState(); activity.updateGameHold();
                require(activity.binds == 1 && activity.unbinds == 1 && GameHold.lines.size() == logs && GameHold.endedSessions == 1
                        && activity.main.pending.isEmpty(), "bind failure repeatedly restarted service in the same lifecycle");
                activity.onStop(); activity.onStart();
                require(activity.binds == 2 && activity.gameHoldConnection != null && activity.gameHoldConnection != failed,
                        "bind failure prevented a later visible lifecycle from retrying");
                lateCallbacks(failed);
                require(activity.gameHoldConnection != null && activity.unbinds == 1,
                        "late failed-bind callback released the replacement");
                break;
            }
            case "hold-free-refresh": {
                ServiceConnection old = startHold(activity);
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "game exited");
                activity.refreshGameState();
                require(activity.gameState.free() && activity.gameHoldConnection == null && activity.unbinds == 1
                        && activity.activeBindings.isEmpty(), "FREE refresh did not release the hold");
                int logs = GameHold.lines.size();
                activity.refreshGameState(); lateCallbacks(old);
                require(activity.binds == 1 && activity.unbinds == 1 && GameHold.lines.size() == logs,
                        "FREE refresh or stale callback revived a hold");
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "new running game");
                activity.refreshGameState();
                ServiceConnection next = activity.gameHoldConnection;
                require(activity.binds == 2 && next != null && next != old, "later GAME refresh failed to acquire a fresh hold");
                lateCallbacks(old);
                require(activity.gameHoldConnection == next, "old connection replaced the new game's hold");
                InstallState.next = new GameLock.Result(GameLock.State.ERROR, "probe failed");
                activity.refreshGameState();
                require(activity.gameHoldConnection == null && activity.unbinds == 2 && activity.activeBindings.isEmpty(),
                        "ERROR refresh retained an unproven game hold");
                break;
            }
            case "hold-bind-race": {
                activity.duringBind = () -> InstallState.next = new GameLock.Result(GameLock.State.FREE, "game died during bind");
                GameHold.on = true;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.started = false;
                activity.onStart();
                require(activity.binds == 1, "game/hold race never exercised binding");
                ServiceConnection connection = activity.bindConnections.get(0);
                connection.onServiceConnected(null, null);
                require(activity.binds == 1 && activity.unbinds == 1 && activity.gameHoldConnection == null
                        && activity.activeBindings.isEmpty(), "connected callback retained an empty default process");
                require(!new File(activity.files, "state/close-request").exists() && activity.main.pending.isEmpty(),
                        "hold bind/death race entered the close protocol");
                break;
            }
            case "hold-session-ended": {
                ServiceConnection connection = startHold(activity);
                GameHold.on = false;
                activity.updateGameHold();
                require(activity.gameHoldConnection == null && activity.unbinds == 1 && activity.activeBindings.isEmpty(),
                        "ended hold session retained its existing binding");
                int logs = GameHold.lines.size();
                lateCallbacks(connection);
                activity.onResume(); activity.refreshGameState(); activity.onStop(); activity.onDestroy();
                require(activity.binds == 1 && activity.unbinds == 1 && GameHold.lines.size() == logs,
                        "ended session callback or lifecycle revived its binding");
                break;
            }
            case "hold-close":
            case "hold-close-disconnect": {
                ServiceConnection hold = startHold(activity);
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                ServiceConnection close = activity.closeConnection;
                require(activity.binds == 2 && close != null && close != hold && activity.gameHoldConnection == hold
                        && activity.activeBindings.size() == 2, "Close reused or replaced the visible-menu hold");
                require(activity.bindingFlags.get(1) == BIND_AUTO_CREATE
                        && activity.bindIntents.get(1).action == null
                        && activity.bindIntents.get(1).service == GameControlService.class,
                        "Close inherited hold action or importance flags");
                if (args[0].equals("hold-close-disconnect")) {
                    hold.onServiceDisconnected(null);
                    require(activity.closeConnection == close && activity.closingGame
                            && !activity.unboundConnections.contains(close), "hold death cancelled the independent Close binding");
                }
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                File request = new File(activity.files, "state/close-request");
                require(activity.closeSent && request.exists()
                        && Files.readString(request.toPath()).trim().equals(activity.closeId),
                        "Close stopped using the native request protocol");
                require(mutations[0] == 0 && activity.closeConnection == close,
                        "Close completed before the game lock cleared");
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "game closed");
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(mutations[0] == 1 && !activity.closingGame && activity.closeConnection == null
                        && activity.gameHoldConnection == null && activity.unbinds == 2 && activity.activeBindings.isEmpty(),
                        "completed Close failed to independently release both bindings and retry");
                require(!request.exists() && activity.main.pending.isEmpty(), "completed Close leaked its request or poll");
                int logs = GameHold.lines.size();
                lateCallbacks(hold); lateCallbacks(close);
                require(activity.binds == 2 && activity.unbinds == 2 && mutations[0] == 1
                        && GameHold.lines.size() == logs && activity.main.pending.isEmpty(),
                        "late Close/hold callbacks revived completed work");
                break;
            }
            case "hold-off": {
                require(!GameHold.on, "hold test double must default off");
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.updateGameHold();
                require(activity.checks == 0, "disabled hold probed the game lock");
                activity.onStart(); activity.onResume(); activity.refreshGameState();
                activity.releaseGameHold("disabled");
                activity.onStop(); activity.onDestroy();
                require(activity.binds == 0 && activity.unbinds == 0 && GameHold.lines.isEmpty()
                        && GameHold.endedSessions == 0 && android.util.Log.lines.isEmpty() && activity.main.pending.isEmpty(),
                        "disabled hold performed binding, logging or lifecycle work");
                break;
            }
            case "logging": {
                InstallState.logGameDecision("game decision=GAME branch=lock pid=42 marker=absent");
                require(android.util.Log.lines.size() == 1, "branch log was filtered out with INFO disabled");
                String line = android.util.Log.lines.get(0);
                require(line.contains("TotkSetup") && line.contains("branch=lock") && line.contains("pid=42") && line.contains("marker=absent"), "branch context lost");
                require(line.contains("logInfoEnabled=false") && line.contains("logWarnEnabled=true"), "effective logging levels were not diagnosed");
                break;
            }
        }
        System.out.println("passed " + args[0]);
    }
}
'''

LOG = r'''package android.util;
import java.util.*;
public final class Log {
    public static final int INFO = 4, WARN = 5;
    public static final List<String> lines = new ArrayList<>();
    public static boolean isLoggable(String tag, int level) { return level >= WARN; }
    public static int w(String tag, String text) { if (isLoggable(tag, WARN)) lines.add(tag + ": " + text); return 0; }
    public static int w(String tag, String text, Throwable failed) { return w(tag, text); }
}
'''

INPUT_PRELUDE = r'''#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int close_attempts, keys;
static _Noreturn void runtime_fail(const char *reason) { fprintf(stderr,"%s\n",reason); exit(7); }
static void totk_thread_start(const char *name, void *unused) { (void)name; (void)unused; }
static int runtime_survey_command(const char *line) { (void)line; return 0; }
static void runtime_input_key_synthetic(unsigned code, int down) { if (code == 38) keys += down ? 1 : 10; }
static int runtime_front_request_close(void) { return ++close_attempts == 2; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_anomdump(void) {}
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
static int runtime_hid_feedback_flag(const char *name, int value) { (void)name; (void)value; return 0; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
static void dm_gfx_shot_at(long frame, const char *path) { (void)frame; (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
static void runtime_swkbd_selftest(void) {}
static void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
static void runtime_hle_trace_set(int mode) { (void)mode; }
static void runtime_frame_step_pin(double ms) { (void)ms; }
static void runtime_dump_threads(void) {}
'''


class SetupControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.java, javac, cc = shutil.which('java'), shutil.which('javac'), shutil.which('cc')
        if not cls.java or not javac or not cc:
            raise unittest.SkipTest('host JDK and C compiler required')
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', ROOT / '.scratch/setup-tests')).resolve() / 'controls'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='controls-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.source = source = (JAVA / 'SetupActivity.java').read_text()
        state = (JAVA / 'InstallState.java').read_text()
        methods = section(source, '    private void gameControl(', '    private void gameBanner(')
        game_refresh = section(source, '    private void refreshGameState()', '    /** A foreground binding')
        close = section(source, '    private void closeGame()', '    // ---------- lifecycle')
        start = section(source, '    @Override protected void onStart()', '    @Override protected void onResume()')
        resume = section(source, '    @Override protected void onResume()', '    @Override protected void onStop()')
        stop = section(source, '    @Override protected void onStop()', '    /** A card mounted,')
        refresh = section(source, '    private final Runnable refresh', '    @Override protected void onDestroy()')
        destroy = section(source, '    @Override protected void onDestroy()', '    @Override public void onConfigurationChanged(')
        show = section(source, '    private void showUi()', '    /** A move killed')
        show_tail = show.rsplit('        render();', 1)[1]
        logger = section(state, '    static void logGameDecision(', '    /** Only an acquired game lock')
        probe = cls.work / 'SetupActivity.java'
        probe.write_text(JAVA_PRELUDE + logger + JAVA_ACTIVITY + methods + game_refresh + close
                         + start + resume + stop + refresh + destroy
                         + '    private void showUi() { ui = true; render();' + show_tail + JAVA_TESTS)
        log = cls.work / 'android/util/Log.java'; log.parent.mkdir(parents=True); log.write_text(LOG)
        process = cls.work / 'android/os/Process.java'; process.parent.mkdir(parents=True)
        process.write_text('package android.os; public class Process { public static int myPid() { return 42; } }')
        cls.classes = cls.work / 'classes'
        cls.command([javac, '--release', '17', '-d', cls.classes, probe, log, process,
                     *[JAVA / name for name in ('GameLock.java', 'SaveManager.java', 'Settings.java')]])
        native = cls.work / 'input.c'
        input_source = (GAME / 'runtime/input.c').read_text()
        body = section(input_source, 'static void *input_file(', 'static void input_file_start(')
        native.write_text(INPUT_PRELUDE + body + r'''
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    input_file(argv[1]);
    if (close_attempts != 2 || keys != 11) return 3;
    puts("refused close retained command reader; later close accepted"); return 0;
}
''')
        cls.input = cls.work / 'input'
        cls.command([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', native, '-o', cls.input])

    @staticmethod
    def command(args):
        result = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=120)
        if result.returncode:
            raise AssertionError(f'{args}\n{result.stdout}\n{result.stderr}')
        return result

    def probe(self, mode):
        case = tempfile.mkdtemp(prefix='case-', dir=self.work)
        result = self.command([self.java, '-cp', self.classes, 'com.aure.totk.setup.SetupActivity', mode, case])
        self.assertIn('passed ' + mode, result.stdout)

    def test_gated_rows_and_cards_keep_focus_and_intercept_activation(self):
        self.probe('focus')

    def test_free_controls_retain_original_activation_guards(self):
        self.probe('free-control')

    def test_dead_game_stale_save_marker_does_not_block_pending_action(self):
        self.probe('stale-save')

    def test_live_game_save_still_blocks_close_request(self):
        self.probe('live-save')

    def test_close_binding_lifecycle_and_save_progress(self):
        for mode in ('bind-save', 'disconnect', 'bind-race', 'bind-failure', 'close-timeout', 'stop-close', 'already-free'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_ui_created_after_onstart_registers_one_refresh_and_removes_it(self):
        self.probe('late-ui')

    def test_game_hold_only_acquires_one_visible_game_binding(self):
        for mode in ('hold-opening', 'hold-opening-stop', 'hold-start', 'hold-non-game', 'hold-invisible', 'hold-off'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_game_hold_releases_and_reacquires_across_visible_lifecycles(self):
        for mode in ('hold-rotation', 'hold-finish', 'hold-destroy', 'hold-free-refresh', 'hold-session-ended'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_game_hold_failed_callbacks_and_binds_do_not_restart_in_same_lifecycle(self):
        for mode in ('hold-disconnect', 'hold-binding-died', 'hold-null-binding',
                     'hold-bind-false', 'hold-bind-throw', 'hold-bind-race',
                     'hold-opening-bind-false', 'hold-opening-bind-throw', 'hold-opening-disconnect'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_game_hold_and_close_keep_independent_connections_and_protocols(self):
        for mode in ('hold-close', 'hold-close-disconnect'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_game_hold_lifecycle_integration_precedes_ui_and_keeps_close_flags(self):
        create = section(self.source, '    @Override protected void onCreate(', '    @Override protected void onNewIntent(')
        self.assertLess(create.index('if (returnToRunningGame(getIntent())) return;'), create.index('gameHoldOpening = true;'))
        self.assertLess(create.index('gameHoldOpening = true;'), create.index('updateGameHold();'))
        self.assertLess(create.index('updateGameHold();'), create.index('if (gameError == null'))
        self.assertLess(create.index('updateGameHold();'), create.index('showUi();'))
        start = section(self.source, '    @Override protected void onStart()', '    /** showUi')
        self.assertLess(start.index('started = true;'), start.index('updateGameHold();'))
        self.assertLess(start.index('updateGameHold();'), start.index('startUi();'))
        resume = section(self.source, '    @Override protected void onResume()', '    @Override protected void onStop()')
        self.assertLess(resume.index('updateGameHold();'), resume.index('if (ui)'))
        intent = section(self.source, '    @Override protected void onNewIntent(', '    @Override protected void onStart()')
        self.assertLess(intent.index('setIntent(intent);'), intent.index('updateGameHold();'))
        self.assertLess(intent.index('updateGameHold();'), intent.index('if (moveSettingsToMain(intent))'))
        self.assertIn('private ServiceConnection gameHoldConnection;', self.source)
        self.assertIn('private ServiceConnection closeConnection;', self.source)
        close = section(self.source, '    private void closeGame()', '    private void releaseCloseBinding()')
        self.assertIn('bindService(new Intent(this, GameControlService.class), closeConnection, BIND_AUTO_CREATE)', close)
        self.assertNotIn('GameHold.ACTION', close)
        self.assertNotIn('BIND_IMPORTANT', close)

    def test_branch_warning_survives_info_filter_and_reports_effective_levels(self):
        self.probe('logging')

    def test_refused_diagnostic_close_does_not_end_command_reader(self):
        commands = self.work / 'commands.txt'
        commands.write_text('close\ndown 38\nup 38\nclose\n')
        result = self.command([self.input, commands])
        self.assertIn('refused close retained command reader', result.stdout)
