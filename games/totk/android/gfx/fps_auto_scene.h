/* Conservative observation gate, not an authoritative game loading-state decoder.
 * Uses the port's existing >2000-draw first-world heuristic plus explicit load
 * callbacks when available, scene-relative low draws and real pipeline bursts. */
#pragma once
#include <stdint.h>
#define FPS_AUTO_SCENE_SETTLE_NS 10000000000ull
#define FPS_AUTO_PIPE_WINDOW_NS 1000000000ull
#define FPS_AUTO_PIPE_BURST 8u
enum { FPS_SCENE_READY, FPS_SCENE_BEFORE_WORLD, FPS_SCENE_LOADING,
       FPS_SCENE_LOW_DRAWS, FPS_SCENE_SETTLING };
typedef struct {
    uint64_t last_ns, hold_until_ns, scene_draws, pipeline_start_ns;
    unsigned world_seen, pipeline_count;
} fps_auto_scene;
/* Grace events extend, never shorten, an existing loading/pipeline hold.
 * Saturation keeps a clock near UINT64_MAX from reopening the gate early. */
static inline void fps_auto_scene_grace(fps_auto_scene *s, uint64_t now, uint64_t duration) {
    if (!duration) return;
    uint64_t deadline = now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
    if (deadline > s->hold_until_ns) s->hold_until_ns = deadline;
}
/* settle_ns is the hold after a burst; later creations inside the window keep
 * re-extending it. fps_auto and default adaptive use FPS_AUTO_SCENE_SETTLE_NS. */
static inline int fps_auto_scene_pipeline_settle(fps_auto_scene *s, uint64_t now, uint64_t settle_ns) {
    if (!s->pipeline_count || now < s->pipeline_start_ns || now - s->pipeline_start_ns > FPS_AUTO_PIPE_WINDOW_NS) {
        s->pipeline_start_ns = now; s->pipeline_count = 0;
    }
    if (s->pipeline_count < FPS_AUTO_PIPE_BURST) s->pipeline_count++;
    if (s->pipeline_count < FPS_AUTO_PIPE_BURST) return 0;
    fps_auto_scene_grace(s, now, settle_ns);
    return 1;
}
static inline int fps_auto_scene_pipeline(fps_auto_scene *s, uint64_t now) {
    return fps_auto_scene_pipeline_settle(s, now, FPS_AUTO_SCENE_SETTLE_NS);
}
static inline unsigned fps_auto_scene_observe(fps_auto_scene *s, uint64_t now,
        uint64_t draws, int loading) {
    if (!now || (s->last_ns && now < s->last_ns)) {
        *s = (fps_auto_scene){.last_ns = now};
        return FPS_SCENE_BEFORE_WORLD;
    }
    s->last_ns = now;
    if (!s->world_seen && draws > 2000 && !loading) {
        s->world_seen = 1; s->scene_draws = draws;
        fps_auto_scene_grace(s, now, FPS_AUTO_SCENE_SETTLE_NS);
    }
    if (!s->world_seen) return FPS_SCENE_BEFORE_WORLD;
    uint64_t floor = s->scene_draws / 3;
    if (floor < 512) floor = 512;
    if (loading || draws < floor) {
        fps_auto_scene_grace(s, now, FPS_AUTO_SCENE_SETTLE_NS);
        return loading ? FPS_SCENE_LOADING : FPS_SCENE_LOW_DRAWS;
    }
    if (draws > s->scene_draws) s->scene_draws = draws;
    return now < s->hold_until_ns ? FPS_SCENE_SETTLING : FPS_SCENE_READY;
}
