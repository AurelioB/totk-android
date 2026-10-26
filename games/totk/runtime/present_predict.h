/* Opt-in deadline prediction. Immutable plans travel by guest texture identity,
 * not thread-local frame guesses. All timestamps are CLOCK_MONOTONIC ns. */
#pragma once
#include <stdint.h>
enum { PRESENT_PREDICT_COPY = 1, PRESENT_PREDICT_AHB = 2 };
typedef struct {
    uint64_t id, generation, slot_ns, deadline_ns, release_target_ns, release_ns;
    uint64_t credit_hold_ns; /* Observed post-target admission-credit residence, not work. */
    uint64_t period_ns, refresh_ns, predicted_cost_ns, allowance_ns, margin_ns, lead_ns;
    unsigned interval, path;
} runtime_present_prediction;
/* Readiness model excludes deliberate admission-credit residence, but retains
 * scheduler wake variation and resource waits included in work_ns. */
static inline uint64_t runtime_present_prediction_ready(const runtime_present_prediction *p,
        uint64_t work_ns) {
    if (!p || !work_ns || p->release_ns < p->release_target_ns ||
        p->credit_hold_ns > p->release_ns - p->release_target_ns ||
        work_ns > UINT64_MAX - p->release_ns) return 0;
    return p->release_ns + work_ns - p->credit_hold_ns;
}
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
int runtime_android_predict_active(void);
int runtime_android_predict_acquire(uintptr_t texture, unsigned interval);
int runtime_android_predict_take(uintptr_t texture, unsigned actual_path, runtime_present_prediction *out);
void runtime_android_predict_feedback(const runtime_present_prediction *plan, unsigned actual_path,
    uint64_t ready_ns, uint64_t work_ns, uint64_t allowance_ns);
void runtime_android_predict_presented(const runtime_present_prediction *plan, uint64_t display_ns);
void runtime_android_predict_cancel(const runtime_present_prediction *plan);
#else
static inline int runtime_android_predict_active(void) { return 0; }
static inline int runtime_android_predict_acquire(uintptr_t t, unsigned i) { (void)t; (void)i; return 0; }
static inline int runtime_android_predict_take(uintptr_t t, unsigned p, runtime_present_prediction *o) {
    (void)t; (void)p; *o = (runtime_present_prediction){0}; return 0;
}
static inline void runtime_android_predict_feedback(const runtime_present_prediction *p, unsigned a,
    uint64_t r, uint64_t w, uint64_t s) { (void)p; (void)a; (void)r; (void)w; (void)s; }
static inline void runtime_android_predict_cancel(const runtime_present_prediction *p) { (void)p; }
static inline void runtime_android_predict_presented(const runtime_present_prediction *p, uint64_t d) { (void)p; (void)d; }
#endif
