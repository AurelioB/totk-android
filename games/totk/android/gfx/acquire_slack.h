/* Adaptive acquisition lead, sampled once per game present, never compositor
 * callbacks. Pure policy: the caller supplies monotonic timestamps and target. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "vk_present_slots.h"

enum { ACQUIRE_SLACK_INVALID = -1, ACQUIRE_SLACK_OFF, ACQUIRE_SLACK_ALWAYS, ACQUIRE_SLACK_AUTO,
       ACQUIRE_SLACK_PREDICT, ACQUIRE_SLACK_TWO };
enum { ACQUIRE_SLACK_QUIET_FRAMES = 120 };
#define ACQUIRE_SLACK_LATE_NS 2000000ull

static inline int acquire_slack_parse(const char *mode) {
    if (!mode || !strcmp(mode, "0")) return ACQUIRE_SLACK_OFF;
    if (!strcmp(mode, "1")) return ACQUIRE_SLACK_ALWAYS;
    if (!strcmp(mode, "2")) return ACQUIRE_SLACK_TWO;
    if (!strcmp(mode, "auto")) return ACQUIRE_SLACK_AUTO;
    if (!strcmp(mode, "predict")) return ACQUIRE_SLACK_PREDICT;
    return ACQUIRE_SLACK_INVALID;
}

static inline const char *acquire_slack_two_refusal(const char *slots, const char *buffers,
        const char *framegen) {
    if (!slots || strcmp(slots, "1")) return "requires TOTK_PRESENT_SLOTS=1";
    if (framegen && (!strcmp(framegen, "interp") || !strcmp(framegen, "1") || !strcmp(framegen, "locked") || !strcmp(framegen,"adaptive")))
        return "incompatible with TOTK_FRAMEGEN interpolation/generated frames; real-frame-only reserve required";
    if (vk_present_buffer_count(buffers, "2") < 4)
        return "requires TOTK_PRESENT_BUFFERS>=4; explicit lower buffer override retained";
    return NULL;
}

typedef struct {
    uint64_t last_present_ns, target_ns;
    unsigned lead, quiet_frames, drain_next;
} acquire_slack_policy;
enum { ACQUIRE_SLACK_UNCHANGED, ACQUIRE_SLACK_LATE, ACQUIRE_SLACK_QUIET, ACQUIRE_SLACK_DRAIN };
typedef struct {
    uint64_t interval_ns;
    unsigned reason;
} acquire_slack_observation;

/* A single interval strictly beyond target + 2ms enables the extra frame.
 * Every subsequent late interval renews the recent-late window. Only 120
 * consecutive quiet intervals at the current target remove it. The following
 * interval includes the planned extra-period drain: retain its raw duration,
 * but exempt exactly that interval from late detection to avoid a 120-frame
 * sawtooth. A real hitch coincident with that one drain is not classified.
 * Changing lead never changes acquisition or presentation slot phase. */
static inline acquire_slack_observation acquire_slack_observe(acquire_slack_policy *p,
        uint64_t present_ns, uint64_t target_ns) {
    acquire_slack_observation r = {0};
    uint64_t previous = p->last_present_ns;
    p->last_present_ns = present_ns;
    if (p->target_ns != target_ns) {
        p->target_ns = target_ns;
        p->quiet_frames = 0;
    }
    if (!previous || present_ns <= previous || !target_ns) return r;
    r.interval_ns = present_ns - previous;
    if (p->drain_next) {
        p->drain_next = 0;
        r.reason = ACQUIRE_SLACK_DRAIN;
        return r;
    }
    if (r.interval_ns > target_ns && r.interval_ns - target_ns > ACQUIRE_SLACK_LATE_NS) {
        p->quiet_frames = 0;
        if (!p->lead) { p->lead = 1; r.reason = ACQUIRE_SLACK_LATE; }
    } else if (p->lead && ++p->quiet_frames == ACQUIRE_SLACK_QUIET_FRAMES) {
        p->lead = 0;
        p->quiet_frames = 0;
        p->drain_next = 1;
        r.reason = ACQUIRE_SLACK_QUIET;
    }
    return r;
}
