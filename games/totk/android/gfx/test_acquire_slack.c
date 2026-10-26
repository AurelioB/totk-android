/* Deterministic producer model: two guest textures, no Android/GPU or sleeps.
 * This tests synthetic pacing only; production GPU lifetime waits stay separate. */
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_pace.h"
#include <assert.h>
#include <stdio.h>

enum { FRAMES = 1200 };
static void run(int slack, int jitter, uint64_t result[FRAMES]) {
    const uint64_t period = 33333333;
    uint64_t now = 1000000000, available[2] = {0};
    const uint64_t work[] = {26000000, 35000000, 17000000, 26000000};
    nvn_pacer p = {0};
    for (unsigned frame = 0; frame < FRAMES; frame++) {
        unsigned idx = frame % 2;
        if (now < available[idx]) now = available[idx];
        now += jitter ? work[frame % 4] : 26000000;
        result[frame] = now;
        uint64_t slot = now + nvn_pace(&p, now, period);
        assert(slot >= now && slot - now <= (slack ? 2 : 1) * period);
        available[idx] = nvn_pace_availability(slot, period, slack);
    }
}
int main(void) {
    const uint64_t p = 33333333;
    assert(nvn_pace_availability(100, 0, 0) == 0);
    assert(nvn_pace_availability(100, 0, 1) == 0);
    assert(nvn_pace_availability(100, p, 0) == 100 + p);
    assert(nvn_pace_availability(100, p, 1) == 100);
    uint64_t old[FRAMES], slack[FRAMES];
    run(0, 0, old); run(1, 0, slack);
    for (unsigned i = 100; i < FRAMES; i++) {
        assert(old[i] - old[i - 1] == p);
        assert(slack[i] - slack[i - 1] == p);
        assert(old[i] - slack[i] == p); /* one frame of phase lead, not 60 fps */
    }
    run(0, 1, old); run(1, 1, slack);
    assert(old[1199] - old[599] == 600 * p);
    assert(slack[1199] - slack[599] == 600 * p);
    /* A hitch rebases the original pacer; availability slack does not alter it. */
    nvn_pacer pacer = {.target = 1000000000};
    uint64_t late = 1250000000;
    assert(nvn_pace(&pacer, late, p) == 0 && pacer.target == late);
    assert(nvn_pace_availability(pacer.target, p, 1) == late);
    assert(nvn_pace(&pacer, late + 10000000, p) == p - 10000000);
    assert(nvn_pace_availability(pacer.target, p, 1) == late + p);
    /* Synthetic early release is not GPU completion: acquisition still takes
     * max(synthetic availability, the texture's separate GPU lifetime fence). */
    uint64_t synthetic = nvn_pace_availability(2000000000, p, 1), gpu_ready = synthetic + 50000000;
    uint64_t acquire = synthetic > gpu_ready ? synthetic : gpu_ready;
    assert(acquire == gpu_ready);
    puts("acquire slack: two-texture cadence remains 30 fps, one-period phase lead, jitter catch-up and hitch policy passed");
    return 0;
}
