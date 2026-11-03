/* One vibration amplitude from the game's HD rumble value (runtime/hid_feedback.c VibrationValue {amplitude low,
   frequency low, amplitude high, frequency high}, amplitudes 0..1), for a motor that only has a strength
   (Android's VibrationEffect amplitude 1..255; runtime/feedback_android.c). The two bands combine as on the Mac
   (gamepad_feedback.m intensity_of): sqrt(low^2 + high^2) times the gain, clamped to 1; below 2 % it is silence.
   A value older than RUMBLE_STALE_NS counts as silence, so a paused or frozen game never leaves a motor running. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define RUMBLE_STALE_NS 250000000ull

/* TOTK_RUMBLE_GAIN (launcher: rumble strength): a factor 0..4; unset, empty or malformed = 1. */
static inline float rumble_gain_parse(const char *e) {
    if (!e || !*e) return 1.0f;
    char *end; double g = strtod(e, &end);
    if (end == e || *end || !(g >= 0) || g > 4) return 1.0f;
    return (float)g;
}

/* 0 = off, else 1..255. at_ns = when the game sent the value (0 = never / silenced). */
static inline int rumble_amplitude(const float v[4], uint64_t at_ns, uint64_t now_ns, float gain) {
    if (!at_ns || now_ns - at_ns >= RUMBLE_STALE_NS) return 0;
    float lo = v[0], hi = v[2];
    if (!(lo == lo) || !(hi == hi)) return 0;   /* NaN */
    float a = sqrtf(lo * lo + hi * hi) * gain;
    if (!(a >= 0.02f)) return 0;
    if (a > 1.0f) a = 1.0f;
    int amplitude = (int)lroundf(a * 255.0f);
    return amplitude < 1 ? 1 : amplitude;
}

/* Whether a new amplitude is worth a call to the vibrator (each one is a system call): starting, stopping, or a change of
   at least 8 of 255; smaller steps wait for the periodic renewal. */
static inline int rumble_changed(int amplitude, int sent) {
    if (!amplitude != !sent) return 1;
    return abs(amplitude - sent) >= 8;
}
