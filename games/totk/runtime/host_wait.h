/* MIT. Scoped host-only blocking at an audited HLE boundary. */
#pragma once
#include <stdint.h>

typedef struct runtime_host_wait {
    uint64_t start_ns, acquired_ns, released_at_ns; /* last: runtime clock, only TOTK_POOL_STATS */
    int active, released;
} runtime_host_wait;

/* Call only after a failed try-lock or an observed incomplete GPU ticket at an
 * audited synchronous HLE boundary, before taking ANY host lock. While
 * released, execute no guest code/callbacks. The original virtual core is
 * retained as a resume constraint, not ownership; the guest mask is unchanged.
 * acquired() only timestamps the completed host acquisition, and never parks.
 * end() MUST run after releasing every host lock, before returning to guest.
 * Non-guest/already-away callers and TOTK_VCORE_RELEASE=0 are no-ops (apart
 * from opt-in contention accounting). This switch does not disable existing
 * blocking SVC/NVN handoffs. Tokens must not cross threads or be copied. */
void runtime_host_wait_begin(runtime_host_wait *wait);
void runtime_host_wait_acquired(runtime_host_wait *wait);
void runtime_host_wait_end(runtime_host_wait *wait);
