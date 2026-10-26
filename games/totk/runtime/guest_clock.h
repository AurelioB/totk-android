#pragma once
#include <stdint.h>

/* The timer gates and all guest-visible GPU clocks must use this provider and
   19.2 MHz conversion. runtime_now_ns currently has CLOCK_MONOTONIC's epoch. */
uint64_t runtime_now_ns(void);
static inline uint64_t guest_clock_ns_to_ticks(uint64_t ns) {
    return (uint64_t)((__uint128_t)ns * 12 / 625);
}
static inline uint64_t guest_clock_ticks_to_ns(uint64_t ticks) {
    return ticks / 12 * 625 + ticks % 12 * 625 / 12;
}
/* Translate a calibrated host sample using a contemporaneous host/guest pair;
   valid even when the guest provider's epoch is hours away from the host's. */
static inline uint64_t guest_clock_rebase_ns(uint64_t ns, uint64_t host_anchor,
                                           uint64_t guest_anchor) {
    return guest_anchor + (ns - host_anchor);
}
