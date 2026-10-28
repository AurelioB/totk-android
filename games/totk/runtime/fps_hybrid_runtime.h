/* Hybrid state is latched at acquire. Synchronous replay keeps it immutable
 * through this frame's recording/present; GPU completion uses static selection. */
#pragma once
#include <stdint.h>
#ifdef GPU_VK
int runtime_fps_hybrid_selected(void);
int runtime_fps_hybrid_interpolating(void);
/* valid means a live, unfrozen controller. Invalid/negative completed scores
 * disqualify recovery only: raw present-rate evidence is independent of them. */
unsigned runtime_fps_hybrid_acquire(uint64_t now, uint64_t refresh, unsigned guest,
    int valid, double cpu_score, double gpu_score);
void runtime_fps_hybrid_present(uint64_t now, uint64_t draws, int loading);
#else
static inline int runtime_fps_hybrid_selected(void) { return 0; }
static inline int runtime_fps_hybrid_interpolating(void) { return 0; }
static inline unsigned runtime_fps_hybrid_acquire(uint64_t n,uint64_t r,unsigned g,int v,double c,double p) {
    (void)n; (void)r; (void)v; (void)c; (void)p; return g;
}
static inline void runtime_fps_hybrid_present(uint64_t n,uint64_t d,int e) { (void)n; (void)d; (void)e; }
#endif
