/* MIT. State belongs to ONE produced present job, not a reusable image slot.
 * Begin at both NEW-job producer entries. Never begin on a same-job requeue.
 * Generator completion releases GPU/source ownership before compositor handoff;
 * transaction retirement is separate and must not repeat this completion. */
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t capacity_wait_start_ns, capacity_wait_end_ns;
    unsigned capacity_wait_count, retire_async, completion_result;
} vk_present_job;

typedef struct { int accepted, first; } vk_present_job_completion;

static inline void vk_present_job_begin(vk_present_job *job) {
    *job=(vk_present_job){0};
}

static inline vk_present_job_completion vk_present_job_complete(vk_present_job *job,
        unsigned slot, int (*complete)(unsigned)) {
    int first=job->completion_result==0;
    if (first) job->completion_result=complete(slot) ? 2u : 1u;
    return (vk_present_job_completion){job->completion_result==2,first};
}
