#pragma once
#include <stdint.h>
#include "../../runtime/guest_clock.h"

/* GPU and guest-clock samples describe the same instant (or a bounded bracket).
   Never anchor a production query to its submission's fence completion. */
static inline uint64_t vk_timestamp_ticks(uint64_t query, uint64_t gpu_sample,
                                        uint64_t guest_sample_ns, float period,
                                        uint32_t valid_bits) {
    uint64_t difference = query - gpu_sample;
    if (valid_bits && valid_bits < 64) {
        uint64_t mask = (UINT64_C(1) << valid_bits) - 1;
        difference &= mask;
        if (difference & (UINT64_C(1) << (valid_bits - 1))) difference |= ~mask;
    }
    int64_t delta_ns = (int64_t)((double)(int64_t)difference * period);
    uint64_t ns = delta_ns < 0 ? guest_sample_ns - (uint64_t)-delta_ns : guest_sample_ns + (uint64_t)delta_ns;
    return guest_clock_ns_to_ticks(ns);
}
