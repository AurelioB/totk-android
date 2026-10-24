/* Opt-in Android critical-path accounting. IDs are one-based game presents;
 * async owners MUST retain the frame ID captured on the walking thread. */
#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

enum { FT_RING = 1024, FT_WALKS = 32, FT_SLOTS = 64 };
/* Walking counters use TLS and publish once per walk. Rare asynchronous resource
 * creations publish with their captured originating frame, never current(). */
#define FT_COUNTERS(X) \
    X(DRAWS, draws) X(PASSES, passes) X(CHUNKS, chunks) \
    X(LAG_VERTEX, lag_vertex) X(LAG_VERTEX_MISS, lag_vertex_miss) \
    X(LAG_INDEX, lag_index) X(LAG_INDEX_MISS, lag_index_miss) \
    X(LAG_UNIFORM, lag_uniform) X(LAG_UNIFORM_MISS, lag_uniform_miss) \
    X(LAG_STORAGE, lag_storage) X(LAG_STORAGE_MISS, lag_storage_miss) \
    X(SNAP_ALLOCS, snap_allocs) X(SNAP_BYTES, snap_bytes) \
    X(HOT_COPIES, hot_copies) X(HOT_BYTES, hot_bytes) \
    X(GUARD_REGS, guard_regs) X(PIPE_CREATES, pipe_creates) X(DESC_CREATES, desc_creates) \
    X(TEX_UPLOADS, tex_uploads) X(TEX_REALIZATIONS, tex_realizations) \
    X(CPU_STEPS, cpu_steps) X(REC_S_NS, rec_s_ns) X(REC_P_NS, rec_p_ns) \
    X(REC_O_NS, rec_o_ns) X(REC_SAMPLES, rec_samples)
#define FT_COUNTER_ENUM(symbol, label) FT_COUNT_##symbol,
enum totk_ft_counter { FT_COUNTERS(FT_COUNTER_ENUM) FT_COUNT_N };
#undef FT_COUNTER_ENUM
typedef struct { uint64_t value[FT_COUNT_N]; } totk_ft_work_counts;
enum { FT_SUBMIT_COPY_POST = 1, FT_SUBMIT_AHB_APPLY = 2 };
typedef struct { uint64_t begin, end, cpu; } totk_ft_walk_record;
typedef struct { uint64_t ticket, seal, queue_begin, queue_end, gpu_done, gpu_ns; } totk_ft_slot_record;
/* Actual connected GetNpadStates state-formation anchors, not Android event
 * times. Sampling IDs are the game's existing Npad sampling numbers. */
typedef struct {
    uint64_t first, last, reads, invalid_reads;
    uint64_t first_sampling, last_sampling, unknown_thread_reads;
    uint32_t first_thread, last_thread;
    unsigned mixed_threads;
} totk_ft_input_reads;
typedef struct {
    totk_ft_input_reads reads;
} totk_ft_input_snapshot;
typedef struct {
    uint64_t frame, wake, ready, wake_addr, wake_waker;
    uint64_t acquire_begin, acquire_end, present, target, handoff, apply, display;
    uint64_t submit;
    uint64_t present_slot, present_ready, present_skipped;
    unsigned submit_source;
    uint64_t walk_ns, walk_cpu_ns, join_ns;
    uint64_t acquire_pacing_ns, acquire_texture_ns;
    /* Raw observations follow the global walking/present window. Main-wake
     * snapshots instead follow the sender across that thread boundary. Neither
     * proves which simulation update consumed input for a particular image. */
    totk_ft_input_reads input_window;
    totk_ft_input_snapshot input_main_wake;
    unsigned input_main_wake_received;
    totk_ft_work_counts work;
    unsigned walks, slots, lost_walks, lost_slots;
    totk_ft_walk_record walk[FT_WALKS];
    totk_ft_slot_record slot[FT_SLOTS];
} totk_ft_record;

#if defined(__ANDROID__) || defined(TOTK_FRAME_TIMELINE_TEST)
extern _Atomic int totk_ft_enabled;
int totk_ft_init(void);
static inline int totk_ft_on(void) {
    int on = atomic_load_explicit(&totk_ft_enabled, memory_order_acquire);
    return on < 0 ? totk_ft_init() : on;
}
static inline uint64_t totk_ft_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t totk_ft_current(void);
extern _Thread_local uint64_t totk_ft_active_frame;
extern _Thread_local totk_ft_work_counts totk_ft_active_work;
/* Until initialization succeeds no thread can have an active work scope. Check the
 * process-wide switch before touching dynamic TLS: Android APP libraries are dlopened. */
static inline uint64_t totk_ft_work_frame(void) {
    if (__builtin_expect(atomic_load_explicit(&totk_ft_enabled, memory_order_relaxed) <= 0, 1)) return 0;
    return totk_ft_active_frame;
}
static inline void totk_ft_count(enum totk_ft_counter counter, uint64_t n) {
    if (__builtin_expect(atomic_load_explicit(&totk_ft_enabled, memory_order_relaxed) <= 0, 1)) return;
    if (__builtin_expect(totk_ft_active_frame != 0, 0)) totk_ft_active_work.value[counter] += n;
}
void totk_ft_work_begin(uint64_t frame);
void totk_ft_work_end(void);
void totk_ft_count_frame(uint64_t frame, enum totk_ft_counter counter, uint64_t n);
void runtime_frame_timeline_walking(void);
/* Called after state formation; frame is captured before releasing inputLock.
 * Per-sender TLS groups are consumed at main->walking WaitForAddress wake.
 * An empty wake is no-read evidence, never reuse of an earlier input sample. */
void totk_ft_input_read(uint64_t frame, uint64_t now, uint32_t guest_thread, uint64_t sampling);
void totk_ft_input_take(totk_ft_input_snapshot *out);
void totk_ft_wake(uint64_t frame, uint64_t now, uint64_t ready, uint64_t addr, uint64_t waker,
                  const totk_ft_input_snapshot *input);
void totk_ft_walk(uint64_t frame, uint64_t begin, uint64_t end, uint64_t cpu_ns);
void totk_ft_walk_wait(uint64_t frame, uint64_t ns);
void totk_ft_acquire(uint64_t frame, uint64_t begin, uint64_t end);
/* Optional additive split; callers retain the existing total acquire span. */
void totk_ft_acquire_wait(uint64_t frame, uint64_t pacing_ns, uint64_t texture_ns);
void totk_ft_slot_seal(uint64_t frame, uint64_t ticket, uint64_t now);
void totk_ft_slot_queue(uint64_t frame, uint64_t ticket, uint64_t begin, uint64_t end);
void totk_ft_slot_gpu(uint64_t frame, uint64_t ticket, uint64_t done, uint64_t gpu_ns);
void totk_ft_handoff(uint64_t frame, uint64_t now);
void totk_ft_compositor(uint64_t frame, uint64_t apply, uint64_t display);
/* Immutable real-picture slot plus independently published final GPU ticket
 * readiness; never substitute a delayed presenter-dequeue timestamp. */
void totk_ft_present_slot(uint64_t frame, uint64_t desired, uint64_t skipped);
void totk_ft_present_ready(uint64_t frame, uint64_t ready);
/* Successful buffer-post / AHB transaction submission only; unlike apply,
 * this is never a delayed copy-path acknowledgement. Retain captured frame. */
void totk_ft_submission(uint64_t frame, uint64_t now, unsigned source);
/* Walking owner calls after this frame's present handoff, advancing the ID.
 * Reporting is delayed 30 frames to observe asynchronous completion. */
void totk_ft_present(uint64_t frame, uint64_t target, uint64_t now);
#ifdef TOTK_FRAME_TIMELINE_TEST
int totk_ft_snapshot(uint64_t frame, totk_ft_record *out);
#include <stdio.h>
void totk_ft_test_report(FILE *out, uint64_t first, uint64_t last);
#endif
#else
static inline int totk_ft_on(void) { return 0; }
static inline uint64_t totk_ft_now(void) { return 0; }
static inline uint64_t totk_ft_current(void) { return 0; }
static inline void runtime_frame_timeline_walking(void) {}
static inline uint64_t totk_ft_work_frame(void) { return 0; }
#define totk_ft_count(...) ((void)0)
#define totk_ft_work_begin(...) ((void)0)
#define totk_ft_work_end(...) ((void)0)
#define totk_ft_count_frame(...) ((void)0)
#define totk_ft_acquire_wait(...) ((void)0)
#define totk_ft_input_read(...) ((void)0)
#define totk_ft_input_take(...) ((void)0)
#define totk_ft_wake(...) ((void)0)
#define totk_ft_walk(...) ((void)0)
#define totk_ft_walk_wait(...) ((void)0)
#define totk_ft_acquire(...) ((void)0)
#define totk_ft_slot_seal(...) ((void)0)
#define totk_ft_slot_queue(...) ((void)0)
#define totk_ft_slot_gpu(...) ((void)0)
#define totk_ft_handoff(...) ((void)0)
#define totk_ft_compositor(...) ((void)0)
#define totk_ft_present_slot(...) ((void)0)
#define totk_ft_present_ready(...) ((void)0)
#define totk_ft_submission(...) ((void)0)
#define totk_ft_present(...) ((void)0)
#endif
