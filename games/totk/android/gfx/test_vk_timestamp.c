/* clang -std=gnu11 -Wall -Wextra -Werror test_vk_timestamp.c -o <scratch>/test-vk-timestamp */
#include "vk_timestamp.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint64_t epoch = UINT64_C(4000000000000), gpu = UINT64_C(900000000);
    const uint64_t origin = epoch - UINT64_C(8000000);
    const uint64_t end = gpu + UINT64_C(5000000);
    uint64_t correct = vk_timestamp_ticks(end, gpu, epoch, 1.f, 64);
    assert(correct == (epoch + UINT64_C(5000000)) * 12 / 625);
    assert(correct - origin * 12 / 625 == UINT64_C(13000000) * 12 / 625);
    /* Arbitrary completion delays must never enter conversion. The old first-
       query-to-completion anchor adds every one of these delays permanently. */
    const uint64_t delays[] = {0, 1000000, 17000000, 80000000};
    for (unsigned i = 0; i < sizeof delays / sizeof *delays; i++) {
        uint64_t completion = epoch + UINT64_C(5000000) + delays[i];
        uint64_t legacy = completion * 12 / 625;
        assert(vk_timestamp_ticks(end, gpu, epoch, 1.f, 64) == correct);
        assert(legacy - correct == delays[i] * 12 / 625);
    }
    assert(vk_timestamp_ticks(gpu - 1000, gpu, epoch, 1.f, 64) == (epoch - 1000) * 12 / 625);
    /* Query older/newer than calibration across the valid-bit wrap. */
    assert(vk_timestamp_ticks(0x10, 0xfffffff0, epoch, 1.f, 32) == (epoch + 32) * 12 / 625);
    assert(vk_timestamp_ticks(0xfffffff0, 0x10, epoch, 1.f, 32) == (epoch - 32) * 12 / 625);
    /* Different GPU period/epoch, including Adreno's 19.2 MHz clock. */
    assert(vk_timestamp_ticks(gpu + 1000, gpu, epoch, 2.f, 64) == (epoch + 2000) * 12 / 625);
    uint64_t adreno = vk_timestamp_ticks(gpu + 192000, gpu, epoch, 625.f / 12.f, 64);
    uint64_t expected = (epoch + UINT64_C(10000000)) * 12 / 625;
    assert(adreno >= expected - 1 && adreno <= expected + 1);
    const uint64_t uptime = UINT64_C(224104000000000);
    const uint64_t guest_epoch[] = {uptime + UINT64_C(28800000000000), UINT64_C(3000000000)};
    for (unsigned i = 0; i < sizeof guest_epoch / sizeof *guest_epoch; i++) {
        uint64_t sample = guest_clock_rebase_ns(uptime + 1000000, uptime, guest_epoch[i]);
        uint64_t guest_origin = guest_clock_ns_to_ticks(guest_epoch[i]);
        uint64_t gpu_end = vk_timestamp_ticks(gpu + 2000000, gpu, sample, 1.f, 64);
        assert(gpu_end - guest_origin == guest_clock_ns_to_ticks(3000000));
        assert(guest_clock_ticks_to_ns(gpu_end) == guest_epoch[i] + 3000000);
    }
    puts("timestamp calibration: pass (epoch, delayed completion, signed delta, wrap, period)");
    return 0;
}
