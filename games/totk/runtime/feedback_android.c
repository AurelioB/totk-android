/* Rumble and motion in the Android app (2026-11-03) for the game's HD rumble and six-axis sensor (runtime/hid_feedback.c,
   on unless TOTK_RUMBLE=0 / TOTK_GYRO=0; the launcher's Controls & audio set them).
   Rumble: runtime_rumble_set only stores the game's latest value per side (it comes from the game's threads every few
   milliseconds). A thread of our own, attached to the JVM, turns the values into one amplitude per side
   (rumble_mix.h, times TOTK_RUMBLE_GAIN) 100 times a second and hands each change to TotkActivity.rumble(left, right)
   (GameFeedback.java: the controller's own vibrators when it has them, else the device's), renewed while it lasts. A
   value the game has not repeated for 250 ms counts as silence, and focus loss silences it at once; with nothing to
   play the thread sleeps.
   Motion: GameFeedback reads the controller's gyroscope and accelerometer (or the device's own), turns them into the
   Switch's frame (MotionAxes.java) and calls TotkActivity.nativeMotion per gyroscope sample; runtime/motion_fusion.c
   makes the six-axis states from them. TOTK_GYRO_AXES remaps the axes as on the Mac.
   start_game asks GameFeedback first (runtime_android_feedback_hardware): without any vibrator, or without a gyroscope
   and accelerometer, the game is not given that device at all instead of a silent one. */
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include "runtime.h"
#include "rumble_mix.h"
#include <android/native_activity.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

ANativeActivity *runtime_android_activity(void);   /* frontend_android.c */
int runtime_gyro_enabled(void);                     /* hid_feedback.c */

enum { TICK_NS = 10000000, RENEW_NS = 200000000 };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static float g_want[2][4] = {{0, 160, 0, 320}, {0, 160, 0, 320}};   /* g_mu */
static uint64_t g_want_at[2];                                       /* g_mu */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void *rumble_thread(void *arg) {
    (void)arg;
    ANativeActivity *a = runtime_android_activity();
    JNIEnv *env = NULL;
    if (!a || (*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) { fprintf(stderr, "[rumble] no Java thread; rumble off\n"); return NULL; }
    jclass cls = (*env)->GetObjectClass(env, a->clazz);
    jmethodID rumble = cls ? (*env)->GetMethodID(env, cls, "rumble", "(II)V") : NULL;
    if (!rumble || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env); fprintf(stderr, "[rumble] TotkActivity.rumble missing; rumble off\n");
        (*a->vm)->DetachCurrentThread(a->vm); return NULL;
    }
    float gain = rumble_gain_parse(getenv("TOTK_RUMBLE_GAIN"));
    fprintf(stderr, "[rumble] on (gain %.2f)\n", gain);
    int sent[2] = {0, 0}; uint64_t sent_at = 0; int played = 0;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        uint64_t now = runtime_now_ns();
        int amp[2];
        for (int s = 0; s < 2; s++) amp[s] = rumble_amplitude(g_want[s], g_want_at[s], now, gain);
        int changed = rumble_changed(amp[0], sent[0]) || rumble_changed(amp[1], sent[1]), renew = (amp[0] || amp[1]) && now - sent_at >= RENEW_NS;
        if (changed || renew) {
            pthread_mutex_unlock(&g_mu);
            (*env)->CallVoidMethod(env, a->clazz, rumble, (jint)amp[0], (jint)amp[1]);
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
            if (!played && (amp[0] || amp[1])) fprintf(stderr, "[rumble] first vibration: left %d, right %d of 255\n", amp[0], amp[1]);
            played |= amp[0] || amp[1];
            pthread_mutex_lock(&g_mu);
            sent[0] = amp[0]; sent[1] = amp[1]; sent_at = now;
        }
        if (!sent[0] && !sent[1] && !amp[0] && !amp[1]) {
            pthread_cond_wait(&g_cv, &g_mu);   /* nothing to play: only the game's next value can change that */
            continue;
        }
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += TICK_NS; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_cv, &g_mu, &until);
    }
    return NULL;
}
static void rumble_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, rumble_thread, NULL)) fprintf(stderr, "[rumble] thread could not start; rumble off\n");
    else pthread_detach(t);
}
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    if (side > 1) return;
    pthread_once(&g_once, rumble_start);
    pthread_mutex_lock(&g_mu);
    g_want[side][0] = al; g_want[side][1] = fl; g_want[side][2] = ah; g_want[side][3] = fh; g_want_at[side] = runtime_now_ns();
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
void runtime_feedback_quiet(void) {   /* focus lost: stop now, not after the stale time */
    pthread_mutex_lock(&g_mu); g_want_at[0] = g_want_at[1] = 0; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}

/* ---- motion: TotkActivity.nativeMotion / nativeMotionLive (registered in runtime/companion_jni.c) ---- */
void runtime_android_motion(JNIEnv *env, jclass cls, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy, jfloat gz) {
    (void)env; (void)cls;
    static _Atomic int axes_read;
    if (!atomic_exchange(&axes_read, 1)) {
        const char *axes = getenv("TOTK_GYRO_AXES"); int r = runtime_motion_axes(axes);
        if (r < 0) runtime_fail("TOTK_GYRO_AXES must name x, y and z once each, e.g. \"-x,y,-z\"");
        if (r > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
    }
    const double acc[3] = {ax, ay, az}, gyro[3] = {gx, gy, gz};
    runtime_motion_feed(acc, gyro, runtime_now_ns());
}
void runtime_android_motion_live(JNIEnv *env, jclass cls, jboolean on, jstring source) {
    (void)cls;
    const char *name = source ? (*env)->GetStringUTFChars(env, source, NULL) : NULL;
    fprintf(stderr, "[gyro] motion sensors %s%s%s\n", on ? "on (" : "off", on && name ? name : "", on ? ")" : "");
    if (name) (*env)->ReleaseStringUTFChars(env, source, name);
    runtime_motion_live(on ? 1 : 0);
}
/* TotkActivity.nativeFeedbackEnabled: 1 rumble, 2 motion, as the game was told at start. */
jint runtime_android_feedback_enabled(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    return (runtime_rumble_enabled() ? 1 : 0) | (runtime_gyro_enabled() ? 2 : 0);
}

/* start_game, UI thread, before the game thread: which of the two the device can drive now
   (TotkActivity.feedbackHardware: 1 a vibrator, 2 a gyroscope with an accelerometer). A missing one is not offered to
   the game (TOTK_RUMBLE / TOTK_GYRO = 0), so the game never drives a device that does nothing. */
void runtime_android_feedback_hardware(ANativeActivity *a) {
    JNIEnv *env = a->env;
    jclass cls = (*env)->GetObjectClass(env, a->clazz);
    jmethodID probe = (*env)->GetMethodID(env, cls, "feedbackHardware", "()I");
    jint have = 3;
    if (probe && !(*env)->ExceptionCheck(env)) have = (*env)->CallIntMethod(env, a->clazz, probe);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); have = 3; }
    (*env)->DeleteLocalRef(env, cls);
    static const struct { const char *env, *what; int bit; } feature[] = {{"TOTK_RUMBLE", "no vibrator", 1}, {"TOTK_GYRO", "no gyroscope", 2}};
    for (unsigned i = 0; i < 2; i++) {
        const char *e = getenv(feature[i].env); int asked = !e || strcmp(e, "0");
        if (asked && !(have & feature[i].bit)) {
            setenv(feature[i].env, "0", 1);
            fprintf(stderr, "[totk-app] %s: %s on this device, so the game is not given one\n", feature[i].env, feature[i].what);
        }
    }
}
#endif
