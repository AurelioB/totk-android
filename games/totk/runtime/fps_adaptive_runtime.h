/* Exact opt-in only. Synchronous replay ties the latest guest-present stamp to
 * the real frame being submitted; copy it at the Vulkan present boundary. */
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t sim_ns, anchor_ns, epoch;
    int eligible;
} runtime_fps_adaptive_frame;
#ifdef GPU_VK
int runtime_fps_adaptive_selected(void);
/* Adaptive alone chooses host interval1 at60Hz,2 at120Hz. The acquire release
 * limiter additionally caps actual real frames at60. Guest state is untouched. */
unsigned runtime_fps_adaptive_acquire(uint64_t now, uint64_t refresh, unsigned guest);
/* Called once by HLE before synchronous present, with fps_auto_scene inputs.
 * now is observation time, not simulation phase; anchor_ns never changes here. */
runtime_fps_adaptive_frame runtime_fps_adaptive_present(uint64_t now, uint64_t draws, int loading);
runtime_fps_adaptive_frame runtime_fps_adaptive_snapshot(void);
/* HLE consumes the effective post-selector guest producer capture once before
 * runtime_frame_step_update publishes NEXT frame's nominal realtime step. */
void runtime_fps_adaptive_step(uint64_t now, float seconds);
/* Known camera/history cuts invalidate the epoch; two new real frames are
 * required by the presenter before generation can resume. */
void runtime_fps_adaptive_reset(void);
/* Frontend notifications never configure selection: lifecycle may precede env
 * loading. These are weakly referenced by the non-GPU frontend translation unit. */
void runtime_fps_adaptive_lifecycle(int window, int paused);
void runtime_fps_adaptive_pipeline(uint64_t now);
void runtime_fps_adaptive_grace(uint64_t now, uint64_t duration);
#else
static inline int runtime_fps_adaptive_selected(void) { return 0; }
static inline unsigned runtime_fps_adaptive_acquire(uint64_t n, uint64_t r, unsigned g) {
    (void)n; (void)r; return g;
}
static inline runtime_fps_adaptive_frame runtime_fps_adaptive_present(uint64_t n, uint64_t d, int l) {
    (void)n; (void)d; (void)l; return (runtime_fps_adaptive_frame){0};
}
static inline runtime_fps_adaptive_frame runtime_fps_adaptive_snapshot(void) {
    return (runtime_fps_adaptive_frame){0};
}
static inline void runtime_fps_adaptive_step(uint64_t n, float s) { (void)n; (void)s; }
static inline void runtime_fps_adaptive_reset(void) {}
static inline void runtime_fps_adaptive_lifecycle(int w, int p) { (void)w; (void)p; }
static inline void runtime_fps_adaptive_pipeline(uint64_t n) { (void)n; }
static inline void runtime_fps_adaptive_grace(uint64_t n, uint64_t d) { (void)n; (void)d; }
#endif
