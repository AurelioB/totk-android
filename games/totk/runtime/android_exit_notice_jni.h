/* Native lifecycle only posts events to the Java receipt worker. No filesystem,
 * wall-clock correlation, locks, waits or diagnostic writes on these UI paths.
 * The global class reference (not an Activity) survives Activity recreation.
 * CLOSE is invoked only by the existing non-UI save-safe close path. */
#ifndef TOTK_ANDROID_EXIT_NOTICE_JNI_H
#define TOTK_ANDROID_EXIT_NOTICE_JNI_H
#include <jni.h>
#include <stdatomic.h>
#include <string.h>

static JavaVM *android_exit_vm;
static jclass android_exit_class;
static jmethodID android_exit_method;
static atomic_int android_exit_bridge_ready;

static inline void android_exit_notice_event(int event) {
    if (!atomic_load_explicit(&android_exit_bridge_ready, memory_order_acquire)) return;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*android_exit_vm)->GetEnv(android_exit_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
#ifdef __ANDROID__
        if ((*android_exit_vm)->AttachCurrentThread(android_exit_vm, &env, NULL) != JNI_OK) return;
#else
        if ((*android_exit_vm)->AttachCurrentThread(android_exit_vm, (void **)&env, NULL) != JNI_OK) return;
#endif
        attached = 1;
    }
    (*env)->CallStaticVoidMethod(env, android_exit_class, android_exit_method, event, NULL, NULL);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*android_exit_vm)->DetachCurrentThread(android_exit_vm);
}
static inline void android_exit_notice_start(const ANativeActivity *activity, const char *log, const char *data) {
    if (activity->sdkVersion < 30 || atomic_load_explicit(&android_exit_bridge_ready, memory_order_relaxed)) return;
    JNIEnv *env = activity->env;
    jclass local = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID method = local ? (*env)->GetStaticMethodID(env, local, "gameSessionEvent", "(I[B[B)V") : NULL;
    jclass global = method ? (*env)->NewGlobalRef(env, local) : NULL;
    jsize log_length = (jsize)strlen(log), data_length = (jsize)strlen(data);
    jbyteArray log_bytes = global ? (*env)->NewByteArray(env, log_length) : NULL;
    jbyteArray data_bytes = global ? (*env)->NewByteArray(env, data_length) : NULL;
    if (log_bytes && data_bytes && !(*env)->ExceptionCheck(env)) {
        (*env)->SetByteArrayRegion(env, log_bytes, 0, log_length, (const jbyte *)log);
        (*env)->SetByteArrayRegion(env, data_bytes, 0, data_length, (const jbyte *)data);
        if (!(*env)->ExceptionCheck(env)) {
            android_exit_vm = activity->vm; android_exit_class = global; android_exit_method = method;
            (*env)->CallStaticVoidMethod(env, global, method, 0, log_bytes, data_bytes);
            if (!(*env)->ExceptionCheck(env)) {
                atomic_store_explicit(&android_exit_bridge_ready, 1, memory_order_release);
                global = NULL; // Process-lifetime class reference, released by native process exit.
            }
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (global) (*env)->DeleteGlobalRef(env, global);
    if (log_bytes) (*env)->DeleteLocalRef(env, log_bytes);
    if (data_bytes) (*env)->DeleteLocalRef(env, data_bytes);
    if (local) (*env)->DeleteLocalRef(env, local);
}
#endif
