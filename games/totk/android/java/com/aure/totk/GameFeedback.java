package com.aure.totk;

import android.app.Activity;
import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;

/** The game's HD rumble and six-axis sensor on Android (runtime/feedback_android.c feeds and reads it).
 * Rumble: the first gamepad with vibrators of its own plays it (two motors: left and right side; one: the stronger side),
 * else this device's vibrator. Amplitudes come as 0..255 per side; each is a short one-shot vibration that the native
 * side renews while it lasts, so a stopped game never leaves a motor running. A motor without amplitude control only
 * runs for the stronger values.
 * Motion: the first gamepad with a gyroscope and an accelerometer of its own, else this device's sensors, 200 samples a
 * second, turned into the game's frame by MotionAxes (the device's sensors follow the screen's rotation). Sensors run
 * only while the game is in the foreground. Controllers that come and go are picked up as they appear. */
final class GameFeedback implements SensorEventListener, InputManager.InputDeviceListener, DisplayManager.DisplayListener {
    static final int RUMBLE = 1, MOTION = 2;
    private static final String TAG = "TotkFeedback";
    private static final int ONE_SHOT_MS = 300;     // the native side renews a lasting value every 200 ms
    private static final int SAMPLE_US = 5000;      // 200 samples a second
    private static final int ON_OFF_THRESHOLD = 64; // of 255: a motor without amplitude control runs from here on

    private final Activity activity;
    private final int enabled;
    private final HandlerThread thread = new HandlerThread("totk-feedback");
    private final Handler handler;
    private final InputManager input;
    private final DisplayManager displays;
    private final Object vibratorLock = new Object();
    private Vibrator[] vibrators = new Vibrator[0];   // vibratorLock
    private String vibratorSource = "";              // vibratorLock
    // handler thread only
    private boolean resumed;
    private SensorManager sensors;
    private String sensorSource = "";
    private boolean controllerSensors;
    private final float[] accel = new float[3], frame = new float[6];
    private boolean haveAccel;
    private volatile int quarterTurns;

    /** Which of the two this device can drive right now (TotkActivity.feedbackHardware, before the game starts). */
    static int hardware(Context context) {
        int have = deviceVibrator(context) != null ? RUMBLE : 0;
        if (hasMotion(context.getSystemService(SensorManager.class))) have |= MOTION;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (!gamepad(device)) continue;
            if (device.getVibratorManager().getVibratorIds().length > 0) have |= RUMBLE;
            if (hasMotion(device.getSensorManager())) have |= MOTION;
        }
        return have;
    }
    private static boolean hasMotion(SensorManager s) {
        return s != null && s.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null && s.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null;
    }
    private static boolean gamepad(InputDevice d) {
        return d != null && !d.isVirtual() && (d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
    }
    private static Vibrator deviceVibrator(Context context) {
        VibratorManager manager = context.getSystemService(VibratorManager.class);
        Vibrator v = manager == null ? null : manager.getDefaultVibrator();
        return v != null && v.hasVibrator() ? v : null;
    }

    /** enabled: RUMBLE | MOTION, what the game was given at start (TotkActivity.nativeFeedbackEnabled). */
    GameFeedback(Activity activity, int enabled) {
        this.activity = activity;
        this.enabled = enabled;
        thread.start();
        handler = new Handler(thread.getLooper());
        input = activity.getSystemService(InputManager.class);
        displays = activity.getSystemService(DisplayManager.class);
        input.registerInputDeviceListener(this, handler);
        displays.registerDisplayListener(this, handler);
        handler.post(this::choose);
    }
    void resume() { handler.post(() -> { resumed = true; choose(); }); }
    void pause() {
        handler.post(() -> { resumed = false; stopSensors(); });
        rumble(0, 0);
    }
    void close() {
        pause();
        input.unregisterInputDeviceListener(this);
        displays.unregisterDisplayListener(this);
        thread.quitSafely();
    }

    /** The native rumble thread: one amplitude 0..255 per side. */
    void rumble(int left, int right) {
        synchronized (vibratorLock) {
            if (vibrators.length == 1) play(vibrators[0], Math.max(left, right));
            else if (vibrators.length >= 2) { play(vibrators[0], left); play(vibrators[1], right); }
        }
    }
    private static void play(Vibrator v, int amplitude) {
        try {
            boolean scaled = v.hasAmplitudeControl();
            if (amplitude <= 0 || (!scaled && amplitude < ON_OFF_THRESHOLD)) { v.cancel(); return; }
            v.vibrate(VibrationEffect.createOneShot(ONE_SHOT_MS, scaled ? Math.min(255, amplitude) : VibrationEffect.DEFAULT_AMPLITUDE));
        } catch (RuntimeException failed) {
            Log.w(TAG, "vibration failed", failed);
        }
    }

    /** Handler thread: the controller or device that serves each feature now. */
    private void choose() {
        Vibrator[] pad = null; String padName = null, sensorName = null; SensorManager padSensors = null;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (!gamepad(device)) continue;
            if (pad == null) {
                VibratorManager manager = device.getVibratorManager();
                int[] ids = manager.getVibratorIds();
                if (ids.length == 1) pad = new Vibrator[]{manager.getVibrator(ids[0])};
                else if (ids.length > 1) pad = new Vibrator[]{manager.getVibrator(ids[0]), manager.getVibrator(ids[1])};
                if (pad != null) padName = device.getName();
            }
            if (padSensors == null && hasMotion(device.getSensorManager())) { padSensors = device.getSensorManager(); sensorName = device.getName(); }
        }
        if ((enabled & RUMBLE) != 0) {
            Vibrator own = pad == null ? deviceVibrator(activity) : null;
            Vibrator[] chosen = pad != null ? pad : own != null ? new Vibrator[]{own} : new Vibrator[0];
            String name = pad != null ? padName + " (" + pad.length + " motor" + (pad.length > 1 ? "s)" : ")") : own != null ? "this device" : "none";
            synchronized (vibratorLock) {
                if (!name.equals(vibratorSource)) {
                    for (Vibrator v : vibrators) v.cancel();
                    vibrators = chosen; vibratorSource = name;
                    Log.i(TAG, "rumble: " + name);
                }
            }
        }
        if ((enabled & MOTION) != 0) {
            Display display = activity.getDisplay();
            quarterTurns = display == null ? 0 : display.getRotation();
            SensorManager source = padSensors != null ? padSensors : activity.getSystemService(SensorManager.class);
            String name = padSensors != null ? sensorName : "this device";
            if (!resumed) stopSensors();
            else if (sensors == null || !name.equals(sensorSource)) { stopSensors(); startSensors(source, padSensors != null, name); }
        }
    }
    private void startSensors(SensorManager source, boolean controller, String name) {
        if (!hasMotion(source)) return;
        haveAccel = false; controllerSensors = controller;
        boolean ok = source.registerListener(this, source.getDefaultSensor(Sensor.TYPE_ACCELEROMETER), SAMPLE_US, handler)
                && source.registerListener(this, source.getDefaultSensor(Sensor.TYPE_GYROSCOPE), SAMPLE_US, handler);
        if (!ok) { source.unregisterListener(this); Log.w(TAG, "motion sensors of " + name + " could not start"); return; }
        sensors = source; sensorSource = name;
        TotkActivity.nativeMotionLive(true, name + (controller ? ", controller frame" : ", screen frame"));
    }
    private void stopSensors() {
        if (sensors == null) return;
        sensors.unregisterListener(this);
        sensors = null; sensorSource = "";
        TotkActivity.nativeMotionLive(false, null);
    }

    @Override public void onSensorChanged(SensorEvent event) {
        int type = event.sensor.getType();
        if (type == Sensor.TYPE_ACCELEROMETER) { System.arraycopy(event.values, 0, accel, 0, 3); haveAccel = true; return; }
        if (type != Sensor.TYPE_GYROSCOPE || !haveAccel || sensors == null) return;
        MotionAxes.toSwitch(accel, event.values, controllerSensors ? 0 : quarterTurns, frame);
        TotkActivity.nativeMotion(frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
    }
    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    @Override public void onInputDeviceAdded(int id) { choose(); }
    @Override public void onInputDeviceRemoved(int id) { choose(); }
    @Override public void onInputDeviceChanged(int id) { choose(); }
    @Override public void onDisplayAdded(int id) {}
    @Override public void onDisplayRemoved(int id) {}
    @Override public void onDisplayChanged(int id) {
        Display display = activity.getDisplay();
        if (display != null && display.getDisplayId() == id) quarterTurns = display.getRotation();
    }
}
