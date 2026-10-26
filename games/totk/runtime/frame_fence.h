/* Additive frame-end NVN fence evidence; enabled by TOTK_FRAME_TIMELINE=1. */
#pragma once
#include <stdint.h>
#include <stdio.h>

enum { FF_SIGNAL_QUEUE = 1, FF_SIGNAL_ACQUIRE = 2 };
typedef struct {
    uint64_t frame, sync, raw_ticket, waited_ticket, enter_ns, exit_ns;
    uint64_t callsite, guest_thread;
    unsigned group, raw_fenced;
    int satisfied, immediate, translated;
} totk_ff_wait_record;

#if defined(__ANDROID__) || defined(TOTK_FRAME_TIMELINE_TEST)
void totk_ff_signal(uint64_t sync, uint64_t frame, uint64_t ticket, uint64_t now,
                    uint64_t guest_thread, unsigned source, unsigned condition, unsigned flags);
void totk_ff_wait(const totk_ff_wait_record *record);
void totk_ff_ticket_seal(uint64_t frame, uint64_t ticket, uint64_t now);
void totk_ff_ticket_queue(uint64_t frame, uint64_t ticket, uint64_t begin, uint64_t end);
void totk_ff_ticket_gpu(uint64_t frame, uint64_t ticket, uint64_t done);
void totk_ff_ticket_publish(uint64_t ticket, uint64_t now, unsigned post_ops);
void totk_ff_present(uint64_t frame, uint64_t now);
void totk_ff_report(FILE *out, uint64_t first, uint64_t last);

/* Snapshot vocabulary also used by the deterministic host tests. Zero anchors
 * mean unobserved; GPU times are CPU observations, publish is just BEFORE done. */
enum totk_ff_status {
    FF_META_MISSING, FF_META_OK, FF_META_EMPTY, FF_META_STALE,
    FF_META_BUSY, FF_META_CONFLICT, FF_META_TICKET_MISMATCH, FF_META_TRANSLATION_TOKEN
};
enum totk_ff_completion {
    FF_COMPLETE_IMMEDIATE, FF_COMPLETE_GPU_PROGRESS, FF_COMPLETE_CPU_PUBLICATION,
    FF_COMPLETE_TIMEOUT, FF_COMPLETE_UNFENCED, FF_COMPLETE_UNKNOWN, FF_COMPLETE_N
};
typedef struct {
    uint64_t sync, frame, ticket, time_ns, guest_thread;
    unsigned source, condition, flags;
} totk_ff_signal_record;
enum { FF_SEEN_SEAL = 1, FF_SEEN_QUEUE = 2, FF_SEEN_GPU = 4, FF_SEEN_PUBLISH = 8 };
typedef struct {
    uint64_t ticket, frame, seal_ns, queue_begin_ns, queue_end_ns;
    uint64_t gpu_observed_ns, publish_ns;
    unsigned post_ops, seen, conflict;
} totk_ff_ticket_record;
typedef struct {
    totk_ff_wait_record wait;
    totk_ff_signal_record signal;
    totk_ff_ticket_record raw, effective;
    uint64_t game_present_ns, prev_game_present_ns;
    unsigned signal_status, raw_status, effective_status, completion;
} totk_ff_snapshot;
#ifdef TOTK_FRAME_TIMELINE_TEST
/* Reset requires no concurrent users. Snapshot returns 0 for absent/stale/busy
 * frame/index, never another generation's record. */
void totk_ff_test_reset(void);
int totk_ff_test_snapshot(uint64_t frame, unsigned index, totk_ff_snapshot *out);
#endif
#else
#define totk_ff_signal(...) ((void)0)
#define totk_ff_wait(...) ((void)0)
#define totk_ff_ticket_seal(...) ((void)0)
#define totk_ff_ticket_queue(...) ((void)0)
#define totk_ff_ticket_gpu(...) ((void)0)
#define totk_ff_ticket_publish(...) ((void)0)
#define totk_ff_present(...) ((void)0)
#define totk_ff_report(...) ((void)0)
#endif
