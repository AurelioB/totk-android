#pragma once
#include <stdint.h>
#if defined(__ANDROID__)
/* Startup names register candidates; only busy game workers and named critical
 * pipeline threads join the opt-in session. No scheduler settings are changed. */
int totk_perf_full(void);
void totk_perf_thread(const char *name);
void totk_perf_render(uint64_t interval_ns);
void totk_perf_frame(uint64_t now_ns, uint64_t period_ns, uint64_t pacing_ns);
struct ANativeWindow;
struct ASurfaceTransaction;
struct ASurfaceControl;
void totk_perf_window(struct ANativeWindow *window);
void totk_perf_surface(struct ASurfaceTransaction *tx, struct ASurfaceControl *surface);
unsigned totk_perf_rate(void);
#else
static inline void totk_perf_thread(const char *name) { (void)name; }
static inline void totk_perf_render(uint64_t ns) { (void)ns; }
static inline int totk_perf_full(void) { return 0; }
static inline void totk_perf_frame(uint64_t now, uint64_t period, uint64_t pacing) { (void)now; (void)period; (void)pacing; }
#endif
