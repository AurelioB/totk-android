package com.aure.totk;

/** Android sensor samples in the frame the game's six-axis sensor uses (runtime/motion_fusion.c, as the Mac's GCMotion):
 * x toward the right, y toward the top edge, z out of the face; acceleration in G with gravity included and pointing
 * down (lying face up reads z = -1); rotation rate in rad/s, right-handed.
 * Android's sensor axes are the same in the device's natural orientation, but its accelerometer reports the push against
 * gravity (face up: z = +9.81 m/s²), so acceleration is negated and scaled. The device's own sensors keep its natural
 * orientation when the screen turns (SENSOR_LANDSCAPE on a phone whose natural orientation is portrait): with the display
 * at quarterTurns (Display.getRotation, counterclockwise) x and y are turned with it, so that x is always the screen's
 * right and y its top. A controller's sensors are in the controller's own frame and are not turned (quarterTurns 0).
 * Plain Java, so the host tests run it (tests/test_android_feedback.py). */
public final class MotionAxes {
    public static final float STANDARD_GRAVITY = 9.80665f;

    private MotionAxes() {}

    /** out[0..2] = acceleration (G), out[3..5] = rotation rate (rad/s), both in the game's frame. */
    public static void toSwitch(float[] accel, float[] gyro, int quarterTurns, float[] out) {
        float ax = -accel[0] / STANDARD_GRAVITY, ay = -accel[1] / STANDARD_GRAVITY, az = -accel[2] / STANDARD_GRAVITY;
        turn(ax, ay, az, quarterTurns, out, 0);
        turn(gyro[0], gyro[1], gyro[2], quarterTurns, out, 3);
    }
    private static void turn(float x, float y, float z, int quarterTurns, float[] out, int at) {
        switch (quarterTurns & 3) {
            case 1: out[at] = -y; out[at + 1] = x; break;
            case 2: out[at] = -x; out[at + 1] = -y; break;
            case 3: out[at] = y; out[at + 1] = -x; break;
            default: out[at] = x; out[at + 1] = y; break;
        }
        out[at + 2] = z;
    }
}
