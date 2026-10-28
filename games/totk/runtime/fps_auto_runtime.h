/* Opt-in Android 60/30 admission control; never changes guest window state. */
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t id, generation, release_ns, present_ns, game_interval_ns, pacing_ns;
    uint64_t blocked_at_release_ns, blocked_ns, post_gate_blocked_ns;
    unsigned interval, policy_interval, scene_eligible, blocked_valid;
    uint32_t owner;
} runtime_fps_auto_frame;
/* Intentional acquire holds and disjoint guest-kernel waits are not CPU work.
 * The owner validates same-thread counter continuity before training. */
static inline uint64_t runtime_fps_auto_busy_span(const runtime_fps_auto_frame *f) {
    if (!f || f->pacing_ns > f->game_interval_ns ||
        f->blocked_ns > f->game_interval_ns - f->pacing_ns) return 0;
    return f->game_interval_ns - f->pacing_ns - f->blocked_ns;
}
/* Join pre-acquire CPU work with the independently completed GPU tail. Taking
 * only READY-release would miss work before acquisition and falsely prove60
 * while intentionally paced30. No presenter-dequeue/desired-slot residence is
 * included. Guest-kernel idle is stripped; native GPU/texture/resource waits
 * are retained, and the independently completed GPU tail is never removed. */
static inline uint64_t runtime_fps_auto_ready_span(const runtime_fps_auto_frame *f,
        uint64_t ready, uint64_t preparation, uint64_t allowance) {
    if (!f || !f->release_ns || f->present_ns < f->release_ns ||
        ready < f->present_ns || !f->game_interval_ns || !allowance ||
        f->pacing_ns > f->game_interval_ns ||
        f->blocked_ns > f->game_interval_ns - f->pacing_ns ||
        f->post_gate_blocked_ns > f->blocked_ns ||
        f->post_gate_blocked_ns > ready - f->release_ns) return 0;
    uint64_t span = ready - f->release_ns - f->post_gate_blocked_ns;
    uint64_t joined = runtime_fps_auto_busy_span(f);
    if (ready - f->present_ns > UINT64_MAX - joined) return 0;
    joined += ready - f->present_ns;
    if (joined > span) span = joined;
    if (!span || preparation > UINT64_MAX - span ||
        allowance > UINT64_MAX - span - preparation) return 0;
    return span + preparation + allowance;
}
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
int runtime_android_fps_auto_active(void);
uint64_t runtime_android_fps_auto_blocked_ns(void);
void runtime_android_fps_auto_grace(uint64_t now_ns, uint64_t duration_ns, const char *reason);
/* Called once before pacing an acquisition; transitions only here. */
unsigned runtime_android_fps_auto_acquire(unsigned guest_interval);
/* Publish only after the texture's old async present has consumed its frame.
 * release_ns is captured after pacing but before replay/resource waits. */
void runtime_android_fps_auto_begin(uintptr_t texture, unsigned interval,
    uint64_t release_ns, uint64_t pacing_ns);
void runtime_android_fps_auto_present(uintptr_t texture, uint64_t now_ns);
void runtime_android_fps_auto_pipeline(uint64_t now_ns);
void runtime_android_fps_auto_scene(uintptr_t texture, uint64_t now_ns, uint64_t draws, int game_loading);
void runtime_android_fps_auto_take(uintptr_t texture, runtime_fps_auto_frame *out);
void runtime_android_fps_auto_feedback(const runtime_fps_auto_frame *frame,
    uint64_t ready_ns, uint64_t preparation_ns, uint64_t allowance_ns);
#else
static inline int runtime_android_fps_auto_active(void) { return 0; }
static inline uint64_t runtime_android_fps_auto_blocked_ns(void) { return 0; }
static inline void runtime_android_fps_auto_grace(uint64_t n, uint64_t d, const char *r) { (void)n; (void)d; (void)r; }
static inline unsigned runtime_android_fps_auto_acquire(unsigned i) { return i; }
static inline void runtime_android_fps_auto_begin(uintptr_t t, unsigned i, uint64_t r, uint64_t p) { (void)t; (void)i; (void)r; (void)p; }
static inline void runtime_android_fps_auto_present(uintptr_t t, uint64_t n) { (void)t; (void)n; }
static inline void runtime_android_fps_auto_pipeline(uint64_t n) { (void)n; }
static inline void runtime_android_fps_auto_scene(uintptr_t t, uint64_t n, uint64_t d, int l) { (void)t; (void)n; (void)d; (void)l; }
static inline void runtime_android_fps_auto_take(uintptr_t t, runtime_fps_auto_frame *o) { (void)t; *o = (runtime_fps_auto_frame){0}; }
static inline void runtime_android_fps_auto_feedback(const runtime_fps_auto_frame *f, uint64_t r, uint64_t p, uint64_t a) { (void)f; (void)r; (void)p; (void)a; }
#endif
