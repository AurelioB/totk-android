/* Nonblocking present-fence ownership. Callers serialize access; no clocks,
 * Android objects, allocation or waits. Entries own fds and immutable FT,
 * prediction and cadence metadata, never recyclable presenter pointers. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <linux/sync_file.h>
#include "../../runtime/present_predict.h"
#include "vk_present_cadence.h"

enum { VK_PRESENT_FENCE_CAPACITY = 16, VK_PRESENT_FENCE_CHILDREN = 16 };
/* Two immutable future transactions, retired in submission order even when
   callbacks arrive out of order. Callback completion alone never frees a
   PREVIOUS compositor buffer or permits its callback context to be reused. */
enum { VK_PRESENT_FLIGHT_CAPACITY = 2 };
typedef struct { unsigned head, count; } vk_present_flight;
static inline int vk_present_flight_next(const vk_present_flight *p) {
    return p->count < VK_PRESENT_FLIGHT_CAPACITY ?
        (int)((p->head+p->count)%VK_PRESENT_FLIGHT_CAPACITY) : -1;
}
static inline int vk_present_flight_submit(vk_present_flight *p, unsigned index) {
    if (vk_present_flight_next(p) != (int)index) return 0;
    p->count++; return 1;
}
static inline int vk_present_flight_retire(vk_present_flight *p, unsigned index) {
    if (!p->count || index != p->head) return 0;
    p->head=(p->head+1)%VK_PRESENT_FLIGHT_CAPACITY; p->count--; return 1;
}

/* API 31 backpressure is necessary for lossless future-buffer queueing.
 * Explicit diagnostic OFF retains the old async path for matched comparisons;
 * an unavailable runtime symbol instead selects the safe serial fallback. */
enum { VK_PRESENT_BP_SERIAL, VK_PRESENT_BP_DIAGNOSTIC_OFF, VK_PRESENT_BP_REQUEST };
static inline int vk_present_backpressure_policy(int locked, const char *setting,
        int api, int symbol_available) {
    if (!locked || api < 31 || !symbol_available) return VK_PRESENT_BP_SERIAL;
    if (setting && setting[0] == '0' && !setting[1]) return VK_PRESENT_BP_DIAGNOSTIC_OFF;
    return VK_PRESENT_BP_REQUEST;
}
/* Adaptive admission is a default-off experiment. Serial/unset/invalid never
 * requests the bounded future-transaction policy; locked selection is separate. */
static inline int vk_present_adaptive_backpressure_requested(int adaptive,
        const char *selector) {
    return adaptive && selector && !strcmp(selector,"request");
}
static inline int vk_present_flight_defer(int policy, uint64_t refresh_ns,
        uint64_t desired_ns, uint64_t now_ns) {
    return policy != VK_PRESENT_BP_SERIAL && refresh_ns && desired_ns > now_ns;
}
typedef enum {
    VK_PF_SIGNALED, VK_PF_PENDING, VK_PF_NO_FD, VK_PF_IOCTL_ERROR,
    VK_PF_FENCE_ERROR, VK_PF_TOO_MANY, VK_PF_INVALID_TIME,
    VK_PF_EVICTED, VK_PF_TEARDOWN, VK_PF_REASON_COUNT
} vk_present_fence_reason;
typedef struct {
    int fd, used, deferred, error;
    uint64_t frame, apply_ns, generation;
    uint64_t framegen_real; /* exact real budget identity; zero for synthetic/disabled */
    uint64_t source_frame; /* one-based real producer identity, including generated children */
    int generated, interpolated;
    int64_t latch_ns; /* diagnostic only, never display */
    runtime_present_prediction prediction;
    vk_present_cadence_sample cadence; /* immutable metadata; independent of AUTO and FT */
} vk_present_fence_entry;
typedef struct {
    vk_present_fence_entry entry[VK_PRESENT_FENCE_CAPACITY];
    unsigned next, pending;
    uint64_t accepted, deferred, reason[VK_PF_REASON_COUNT];
} vk_present_fences;
typedef vk_present_fence_reason (*vk_present_fence_query)(int fd, uint64_t *display_ns, int *error, void *context);
typedef void (*vk_present_fence_close)(int fd, void *context);
typedef void (*vk_present_fence_result)(const vk_present_fence_entry *entry, vk_present_fence_reason reason, uint64_t display_ns, void *context);

static inline vk_present_fence_reason vk_present_fence_decode(const struct sync_file_info *info,
        const struct sync_fence_info *children, unsigned capacity, uint64_t *display_ns) {
    *display_ns = 0;
    if (info->status < 0) return VK_PF_FENCE_ERROR;
    if (!info->status) return VK_PF_PENDING;
    if (info->num_fences > capacity) return VK_PF_TOO_MANY;
    if (info->status != 1 || !info->num_fences) return VK_PF_INVALID_TIME;
    uint64_t latest = 0;
    for (unsigned i = 0; i < info->num_fences; i++) {
        if (children[i].status < 0) return VK_PF_FENCE_ERROR;
        if (!children[i].status) return VK_PF_PENDING;
        if (children[i].status != 1 || !children[i].timestamp_ns || children[i].timestamp_ns > INT64_MAX)
            return VK_PF_INVALID_TIME;
        if (children[i].timestamp_ns > latest) latest = children[i].timestamp_ns;
    }
    *display_ns = latest; /* merged sync file signals at its latest child */
    return VK_PF_SIGNALED;
}
static inline void vk_present_fence_finish(vk_present_fences *f, vk_present_fence_entry *entry,
        vk_present_fence_reason reason, uint64_t display_ns, vk_present_fence_close close_fd,
        vk_present_fence_result result, void *context) {
    vk_present_fence_entry owned = *entry;
    entry->used = 0;
    if (owned.used) f->pending--;
    if (owned.fd >= 0) close_fd(owned.fd, context);
    f->reason[reason]++;
    result(&owned, reason, reason == VK_PF_SIGNALED ? display_ns : 0, context);
}
static inline void vk_present_fences_sweep(vk_present_fences *f, vk_present_fence_query query,
        vk_present_fence_close close_fd, vk_present_fence_result result, void *context) {
    for (unsigned i = 0; i < VK_PRESENT_FENCE_CAPACITY; i++) {
        vk_present_fence_entry *entry = &f->entry[i];
        if (!entry->used) continue;
        uint64_t display_ns = 0;
        vk_present_fence_reason reason = query(entry->fd, &display_ns, &entry->error, context);
        if (reason == VK_PF_PENDING) { f->reason[reason]++; continue; }
        vk_present_fence_finish(f, entry, reason, display_ns, close_fd, result, context);
    }
}
static inline void vk_present_fences_add(vk_present_fences *f, vk_present_fence_entry entry,
        vk_present_fence_query query, vk_present_fence_close close_fd,
        vk_present_fence_result result, void *context) {
    f->accepted++;
    entry.used = 0; entry.deferred = 0; entry.error = 0;
    uint64_t display_ns = 0;
    vk_present_fence_reason reason = entry.fd < 0 ? VK_PF_NO_FD : query(entry.fd, &display_ns, &entry.error, context);
    if (reason != VK_PF_PENDING) {
        vk_present_fence_finish(f, &entry, reason, display_ns, close_fd, result, context);
        return;
    }
    f->reason[reason]++; f->deferred++;
    unsigned index = f->next;
    for (unsigned n = 0; n < VK_PRESENT_FENCE_CAPACITY; n++) {
        unsigned candidate = (f->next + n) % VK_PRESENT_FENCE_CAPACITY;
        if (!f->entry[candidate].used) { index = candidate; break; }
    }
    if (f->entry[index].used)
        vk_present_fence_finish(f, &f->entry[index], VK_PF_EVICTED, 0, close_fd, result, context);
    entry.used = entry.deferred = 1; f->entry[index] = entry; f->pending++;
    f->next = (index + 1) % VK_PRESENT_FENCE_CAPACITY;
}
static inline void vk_present_fences_clear(vk_present_fences *f, vk_present_fence_close close_fd,
        vk_present_fence_result result, void *context) {
    for (unsigned i = 0; i < VK_PRESENT_FENCE_CAPACITY; i++) if (f->entry[i].used)
        vk_present_fence_finish(f, &f->entry[i], VK_PF_TEARDOWN, 0, close_fd, result, context);
}
