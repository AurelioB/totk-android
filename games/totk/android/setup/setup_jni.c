/* JNI entry points of com.aure.totk.setup.NativeSetup (android/java/com/aure/totk/setup/NativeSetup.java). run() and
   move() are blocking and called on the setup service's worker thread; progress goes back through its Listener on that
   same thread (setup_progress already throttles to ten updates a second per stage). Strings cross as UTF-16 so file names
   outside the BMP survive; the JSON we return is pure ASCII. */
#include "setup_internal.h"
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>

static atomic_int cancel_flag;

/* UTF-16 Java string -> UTF-8 (unpaired surrogates become U+FFFD). */
static char *utf8_of(JNIEnv *env, jstring s, size_t *len) {
    if (!s) return NULL;
    jsize n = (*env)->GetStringLength(env, s);
    jchar *u = malloc((size_t)n * sizeof *u + 1);
    char *out = malloc((size_t)n * 3 + 1);
    if (!u || !out) { free(u); free(out); return NULL; }
    (*env)->GetStringRegion(env, s, 0, n, u);
    size_t k = 0;
    for (jsize i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00u); i++; }
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) { out[k++] = (char)(0xc0 | c >> 6); out[k++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[k++] = (char)(0xe0 | c >> 12); out[k++] = (char)(0x80 | (c >> 6 & 63)); out[k++] = (char)(0x80 | (c & 63)); }
        else { out[k++] = (char)(0xf0 | c >> 18); out[k++] = (char)(0x80 | (c >> 12 & 63)); out[k++] = (char)(0x80 | (c >> 6 & 63)); out[k++] = (char)(0x80 | (c & 63)); }
    }
    out[k] = 0; free(u); *len = k;
    return out;
}
/* Progress details can carry user file names: pass printable ASCII only, so NewStringUTF never sees invalid input. */
static jstring ascii(JNIEnv *env, const char *s) {
    char t[256]; size_t k = 0;
    for (; s && *s && k < sizeof t - 1; s++) t[k++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
    t[k] = 0;
    return (*env)->NewStringUTF(env, t);
}
typedef struct { JNIEnv *env; jobject listener; jmethodID method; } Listener;
static void on_progress(void *user, const char *stage, uint64_t done, uint64_t total, const char *detail) {
    Listener *l = user;
    if (!l->listener || !l->method) return;
    JNIEnv *env = l->env;
    jstring s = ascii(env, stage), d = ascii(env, detail);
    if (s && d) (*env)->CallVoidMethod(env, l->listener, l->method, s, (jlong)done, (jlong)total, d);
    /* A throwing listener must not unwind through native code; the setup continues. */
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (s) (*env)->DeleteLocalRef(env, s);
    if (d) (*env)->DeleteLocalRef(env, d);
}
static jstring result(JNIEnv *env, char *json) {
    if (!json) return (*env)->NewStringUTF(env, "{\"ok\":false,\"stage\":\"setup\",\"code\":\"internal\",\"message\":\"out of memory\"}");
    if (!strncmp(json, "{\"ok\":false", 11)) __android_log_print(ANDROID_LOG_WARN, "totk-setup", "%s", json);
    jstring s = (*env)->NewStringUTF(env, json);
    free(json);
    return s;
}

static Listener listener_of(JNIEnv *env, jobject listener) {
    Listener l = { env, listener, NULL };
    if (listener) {
        jclass lc = (*env)->GetObjectClass(env, listener);
        l.method = (*env)->GetMethodID(env, lc, "onProgress", "(Ljava/lang/String;JJLjava/lang/String;)V");
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); l.method = NULL; }
        (*env)->DeleteLocalRef(env, lc);
    }
    return l;
}

JNIEXPORT jstring JNICALL Java_com_aure_totk_setup_NativeSetup_run(JNIEnv *env, jclass cls, jstring request, jobject listener) {
    (void)cls;
    size_t len = 0; char *req = utf8_of(env, request, &len);
    if (!req) return result(env, NULL);
    Listener l = listener_of(env, listener);
    atomic_store(&cancel_flag, 0);
    char *json = setup_run_json(req, len, on_progress, &l, &cancel_flag, NULL, NULL);
    free(req);
    return result(env, json);
}
/* {"from","to","privateDir","romfsPartBytes"}: see setup_move_json (move.c). Cancelled by cancel(), like run(). */
JNIEXPORT jstring JNICALL Java_com_aure_totk_setup_NativeSetup_move(JNIEnv *env, jclass cls, jstring request, jobject listener) {
    (void)cls;
    size_t len = 0; char *req = utf8_of(env, request, &len);
    if (!req) return result(env, NULL);
    Listener l = listener_of(env, listener);
    atomic_store(&cancel_flag, 0);
    char *json = setup_move_json(req, len, on_progress, &l, &cancel_flag, NULL, NULL);
    free(req);
    return result(env, json);
}
JNIEXPORT jstring JNICALL Java_com_aure_totk_setup_NativeSetup_removeGameData(JNIEnv *env, jclass cls, jstring dir) {
    (void)cls;
    size_t len = 0; char *d = utf8_of(env, dir, &len);
    if (!d) return result(env, NULL);
    char *json = setup_remove_json(d);
    free(d);
    return result(env, json);
}
JNIEXPORT jstring JNICALL Java_com_aure_totk_setup_NativeSetup_inspect(JNIEnv *env, jclass cls, jstring request) {
    (void)cls;
    size_t len = 0; char *req = utf8_of(env, request, &len);
    if (!req) return result(env, NULL);
    char *json = setup_inspect_json(req, len, NULL);
    free(req);
    return result(env, json);
}
JNIEXPORT void JNICALL Java_com_aure_totk_setup_NativeSetup_cancel(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    atomic_store(&cancel_flag, 1);
}
