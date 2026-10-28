#include "../../runtime/fps_auto_block.h"
#include "../../runtime/fps_auto_runtime.h"
#include <assert.h>
int main(void) {
    fps_auto_block_clock p = {0};
    assert(fps_auto_block_begin(&p, 100));
    assert(fps_auto_block_begin(&p, 110));
    fps_auto_block_end(&p, 130); assert(!p.total_ns && p.depth == 1);
    fps_auto_block_end(&p, 150); assert(p.total_ns == 50 && !p.depth);
    fps_auto_block_end(&p, 160); assert(p.total_ns == 50);
    assert(fps_auto_block_begin(&p, 200)); fps_auto_block_end(&p, 210);
    assert(p.total_ns == 60);
    assert(fps_auto_block_begin(&p, 200)); fps_auto_block_end(&p, 190);
    assert(p.total_ns == 60 && !p.depth);
    p.total_ns = UINT64_MAX - 5;
    assert(fps_auto_block_begin(&p, 0)); fps_auto_block_end(&p, 10);
    assert(p.total_ns == UINT64_MAX);
    p.depth = UINT_MAX;
    assert(!fps_auto_block_begin(&p, 11) && p.depth == UINT_MAX);
    runtime_fps_auto_frame f = {.release_ns = 1000000000ull,
        .present_ns = 1020000000ull, .game_interval_ns = 33333333,
        .pacing_ns = 10000000, .blocked_ns = 10000000, .post_gate_blocked_ns = 5000000};
    uint64_t ready = f.present_ns + 3000000;
    assert(runtime_fps_auto_busy_span(&f) == 13333333);
    assert(runtime_fps_auto_ready_span(&f, ready, 200000, 100000) == 18300000);
    f.post_gate_blocked_ns = 10000000;
    assert(runtime_fps_auto_ready_span(&f, ready, 200000, 100000) == 16633333);
    /* Native resource/GPU tail is retained, never mistaken for kernel idle. */
    ready += 20000000;
    assert(runtime_fps_auto_ready_span(&f, ready, 200000, 100000) == 36633333);
    f.blocked_ns = f.game_interval_ns - f.pacing_ns + 1;
    assert(!runtime_fps_auto_ready_span(&f, ready, 0, 1));
    f.blocked_ns = 10000000; f.post_gate_blocked_ns = f.blocked_ns + 1;
    assert(!runtime_fps_auto_ready_span(&f, ready, 0, 1));
    f.post_gate_blocked_ns = 10000000; f.release_ns = f.present_ns;
    assert(!runtime_fps_auto_ready_span(&f, f.present_ns + 1, 0, 1));
    return 0;
}
