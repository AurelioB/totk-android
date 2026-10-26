/* Walking-owned, per-real-frame pass census and bounded metadata routing.
   No Vulkan, allocation, clocks, strings, uniform reads or registry traversal.
   Census attachment keys identify the caller's exact image allocation plus its
   mip/layer/aspect, independent of guest objects or recycled image handles.
   Census IDs need not equal guest backing IDs in generic dependency fixtures.
   Census nomination is NOT origin proof: the renderer separately checks live
   scene sampling to the shared pre-UI canvas and committed transfer/present
   identities using current-frame image versions.

   A physical pass ends with the actual committed draw-count delta. Reopening
   the exact same attachment set aggregates into one logical group, regardless
   of format, attachment-zero presence, camera validity or draw count. All
   capacities fail closed but retain the bounded census for diagnostics.

   The renderer's cached walking-owned integer is the only disabled-path read.
   Ordinary enabled draws compare the first/VS/FS/invalidated-context guards.
   Metadata work belongs in callbacks and consumes explicit frame budgets.
   Skipped contexts must be marked incomplete, never promoted to sampling proof. */
#ifndef TOTK_FRAMEGEN_PASSES_H
#define TOTK_FRAMEGEN_PASSES_H

#include "framegen_dependencies.h"
#include <limits.h>

#ifndef FG_PASS_MAX_GROUPS
#define FG_PASS_MAX_GROUPS 512
#endif
#ifndef FG_PASS_MAX_PHYSICAL
#define FG_PASS_MAX_PHYSICAL 2048
#endif
#ifndef FG_PASS_MAX_EVENTS
#define FG_PASS_MAX_EVENTS 8192
#endif
#ifndef FG_PASS_MAX_BINDINGS
#define FG_PASS_MAX_BINDINGS 32768
#endif
#ifndef FG_PASS_MAX_LINKS
#define FG_PASS_MAX_LINKS 512
#endif
#if FG_PASS_MAX_GROUPS < 1 || FG_PASS_MAX_PHYSICAL < 1 || FG_PASS_MAX_EVENTS < 1 || FG_PASS_MAX_BINDINGS < 1 || FG_PASS_MAX_LINKS < 1
#error "invalid framegen pass capacity"
#endif

/* Raster writers are one-based physical orders. Copy/compute producers use a
   tagged nonzero operation ordinal, never a fabricated raster-pass identity. */
#define FG_PASS_WRITER_OPERATION UINT32_C(0x80000000)
enum { FG_PASS_TOP = 5, FG_PASS_COLORS = 8 };
enum {
    FG_PASS_FAILED_GROUPS = 1u << 0, FG_PASS_FAILED_PHYSICAL = 1u << 1,
    FG_PASS_FAILED_EVENTS = 1u << 2, FG_PASS_FAILED_BINDINGS = 1u << 3,
    FG_PASS_FAILED_LINKS = 1u << 4, FG_PASS_FAILED_IDENTITY = 1u << 5,
    FG_PASS_FAILED_OVERFLOW = 1u << 6
};
enum {
    FG_PASS_EVENT_FIRST = 1u << 0, FG_PASS_EVENT_PROGRAM = 1u << 1,
    FG_PASS_EVENT_SAMPLE = 1u << 2
};
typedef struct {
    fg_dep_key key;
    uint32_t nvn_format, vk_format, width, height, samples;
} fg_pass_attachment;
typedef struct {
    fg_pass_attachment color[FG_PASS_COLORS], depth;
    uint32_t width, height;
} fg_pass_key;
typedef struct {
    fg_pass_key key;
    uint64_t draws, depth_writes;
    unsigned physical_passes, first_order, last_order, contexts_complete;
} fg_pass_group;
typedef struct {
    uint64_t draws, depth_writes;
    int group;
} fg_pass_physical;
typedef struct {
    fg_dep_value source;
    unsigned consumer_order;
    int consumer_group;
} fg_pass_sample_link;
typedef struct {
    fg_pass_group group[FG_PASS_MAX_GROUPS];
    unsigned slot[FG_PASS_MAX_GROUPS * 2];
    fg_pass_physical physical[FG_PASS_MAX_PHYSICAL];
    fg_pass_sample_link link[FG_PASS_MAX_LINKS];
    uint64_t epoch, draws;
    unsigned count, physical_passes, link_count, failed;
    unsigned events_used, bindings_used, draw_events, first_events;
    unsigned program_events, sample_events, incomplete_contexts;
    int current_group;
    unsigned open;
} fg_pass_frame;
typedef struct {
    uintptr_t program, fragment;
    unsigned first, changed, sample_contexts, sample_limit;
} fg_pass_guard;
typedef enum {
    FG_PASS_NOMINATED = 0, FG_PASS_NO_CANDIDATE,
    FG_PASS_AMBIGUOUS, FG_PASS_INCOMPLETE
} fg_pass_result;
/* Keep cheap canvas classification live during backoff. One isolated capacity
   failure gets a two-tick retry; consecutive failures retain the once-per32
   bound. Unsupported/load/transition failures keep their original backoff. */
typedef struct { unsigned wait, failed; } fg_pass_probe;
static inline int fg_pass_probe_due(const fg_pass_probe *probe) { return !probe->wait; }
static inline void fg_pass_probe_end(fg_pass_probe *probe, int eligible, int capacity) {
    if (probe->wait) { probe->wait--; return; }
    if (eligible) { probe->failed=0; return; }
    probe->wait=capacity && !probe->failed ? 1 : 31;
    probe->failed=1;
}
/* UI-capable fullscreen shaders also occur in scene/post work. */
static inline int fg_pass_ui_context(int boundary, int ui_shader, int canvas) {
    return boundary && ui_shader && canvas;
}

/* Initialize the frame to zero once. Old arrays need not be cleared each frame:
   only entries below the current counts are ever read. */
static inline void fg_pass_reset(fg_pass_frame *frame) {
    if (++frame->epoch == 0) frame->epoch = 1;
    frame->draws = 0;
    frame->count = frame->physical_passes = frame->link_count = frame->failed = 0;
    frame->events_used = frame->bindings_used = frame->draw_events = 0;
    frame->first_events = frame->program_events = frame->sample_events = 0;
    frame->incomplete_contexts = frame->open = 0;
    memset(frame->slot, 0, sizeof frame->slot);
    frame->current_group = -1;
}
static inline int fg_pass_take_events(fg_pass_frame *frame, unsigned count) {
    if (count > FG_PASS_MAX_EVENTS - frame->events_used) {
        frame->failed |= FG_PASS_FAILED_EVENTS;
        return 0;
    }
    frame->events_used += count;
    return 1;
}
static inline int fg_pass_take_bindings(fg_pass_frame *frame, unsigned count) {
    if (count > FG_PASS_MAX_BINDINGS - frame->bindings_used) {
        frame->failed |= FG_PASS_FAILED_BINDINGS;
        return 0;
    }
    frame->bindings_used += count;
    return 1;
}
/* NVN format is first-observed diagnostic metadata, not physical image/view
   identity: guest enum aliases of the same actual Vulkan view must aggregate. */
static inline int fg_pass_attachment_equal(fg_pass_attachment a, fg_pass_attachment b) {
    if (!a.key.backing || !b.key.backing) return a.key.backing == b.key.backing;
    return fg_dep_key_equal(a.key, b.key) && a.vk_format == b.vk_format &&
        a.width == b.width && a.height == b.height && a.samples == b.samples;
}
static inline int fg_pass_key_equal(const fg_pass_key *a, const fg_pass_key *b) {
    if (a->width != b->width || a->height != b->height ||
        !fg_pass_attachment_equal(a->depth, b->depth)) return 0;
    for (unsigned i = 0; i < FG_PASS_COLORS; i++)
        if (!fg_pass_attachment_equal(a->color[i], b->color[i])) return 0;
    return 1;
}
static inline unsigned fg_pass_color_count(const fg_pass_key *key) {
    unsigned count = 0;
    for (unsigned i = 0; i < FG_PASS_COLORS; i++) count += key->color[i].key.backing != 0;
    return count;
}
/* A cross-frame diagnostic/history key, never a resource identity. Allocation
   addresses, serials, physical order and chunk count deliberately do not enter
   it. Only camera-accepted scene roots may use this structural signature. */
static inline uint64_t fg_pass_scene_signature(const fg_pass_key *key) {
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t extent[] = {key->width, key->height};
    for (unsigned i = 0; i < 2; i++) hash = (hash ^ extent[i]) * UINT64_C(1099511628211);
    for (unsigned i = 0; i <= FG_PASS_COLORS; i++) {
        const fg_pass_attachment *a = i < FG_PASS_COLORS ? &key->color[i] : &key->depth;
        uint32_t fields[] = {!!a->key.backing, a->vk_format, a->width, a->height, a->samples};
        for (unsigned j = 0; j < 5; j++) hash = (hash ^ fields[j]) * UINT64_C(1099511628211);
    }
    return hash;
}
static inline unsigned fg_pass_key_slot(const fg_pass_key *key) {
    uint64_t hash = ((uint64_t)key->width << 32) ^ key->height;
    for (unsigned i = 0; i <= FG_PASS_COLORS; i++) {
        const fg_pass_attachment *a = i < FG_PASS_COLORS ? &key->color[i] : &key->depth;
        if (!a->key.backing) continue;
        hash = (hash ^ a->key.backing ^ ((uint64_t)a->key.mip << 32) ^ a->key.layer ^
            a->key.aspect ^ ((uint64_t)a->vk_format << 40) ^ ((uint64_t)a->width << 24) ^
            ((uint64_t)a->height << 8) ^ a->samples ^ i) * UINT64_C(0x9e3779b97f4a7c15);
        hash ^= hash >> 29;
    }
    return (unsigned)(hash % (FG_PASS_MAX_GROUPS * 2));
}

/* One metadata event is charged for the pass-level context. Even after a
   metadata budget failure census remains bounded and continues counting. */
static inline unsigned fg_pass_begin_physical(fg_pass_frame *frame) {
    if (frame->open) frame->failed |= FG_PASS_FAILED_IDENTITY;
    (void)fg_pass_take_events(frame, 1);
    frame->current_group = -1;
    frame->open = 1;
    if (frame->physical_passes == UINT_MAX) {
        frame->failed |= FG_PASS_FAILED_OVERFLOW;
        return 0;
    }
    unsigned order = ++frame->physical_passes;
    if (order > FG_PASS_MAX_PHYSICAL) frame->failed |= FG_PASS_FAILED_PHYSICAL;
    else frame->physical[order - 1] = (fg_pass_physical){.group = -1};
    return order;
}

/* An actual bound attachment whose exact image identity is unavailable
   is NOT an absent attachment. Retain an ungrouped physical pass and all draw
   deltas, but never merge it with a legitimate attachment-less raster group. */
static inline int fg_pass_begin_unknown(fg_pass_frame *frame) {
    frame->failed |= FG_PASS_FAILED_IDENTITY;
    (void)fg_pass_begin_physical(frame);
    return -1;
}
static inline int fg_pass_begin(fg_pass_frame *frame, const fg_pass_key *key) {
    unsigned order = fg_pass_begin_physical(frame);
    if (!order) return -1;
    int group = -1;
    unsigned slot = fg_pass_key_slot(key);
    while (frame->slot[slot]) {
        unsigned i = frame->slot[slot] - 1;
        if (fg_pass_key_equal(&frame->group[i].key, key)) { group = (int)i; break; }
        if (++slot == FG_PASS_MAX_GROUPS * 2) slot = 0;
    }
    if (group < 0) {
        if (frame->count == FG_PASS_MAX_GROUPS) frame->failed |= FG_PASS_FAILED_GROUPS;
        else {
            group = (int)frame->count++;
            fg_pass_group *g = &frame->group[group];
            frame->slot[slot] = (unsigned)group + 1;
            g->key = *key;
            g->draws = 0;
            g->physical_passes = g->depth_writes = 0;
            g->contexts_complete = 1;
            g->first_order = order;
        }
    }
    if (group >= 0) {
        fg_pass_group *g = &frame->group[group];
        g->physical_passes++;
        g->last_order = order;
    }
    if (order <= FG_PASS_MAX_PHYSICAL)
        frame->physical[order - 1].group = group;
    frame->current_group = group;
    return group;
}
static inline uint64_t fg_pass_add_draws(fg_pass_frame *frame, uint64_t a, uint64_t b) {
    if (UINT64_MAX - a < b) {
        frame->failed |= FG_PASS_FAILED_OVERFLOW;
        return UINT64_MAX;
    }
    return a + b;
}
static inline void fg_pass_end(fg_pass_frame *frame, int group, uint64_t draws, uint64_t depth_writes) {
    if (!frame->open || group != frame->current_group || group < -1 ||
        (group >= 0 && (unsigned)group >= frame->count)) {
        frame->failed |= FG_PASS_FAILED_IDENTITY;
        return;
    }
    frame->draws = fg_pass_add_draws(frame, frame->draws, draws);
    if (group >= 0) {
        fg_pass_group *g = &frame->group[group];
        g->draws = fg_pass_add_draws(frame, g->draws, draws);
        g->depth_writes = fg_pass_add_draws(frame, g->depth_writes, depth_writes);
    }
    if (frame->physical_passes <= FG_PASS_MAX_PHYSICAL) {
        fg_pass_physical *p = &frame->physical[frame->physical_passes - 1];
        p->draws = draws;
        p->depth_writes = depth_writes;
    }
    frame->open = 0;
    frame->current_group = -1;
}

static inline void fg_pass_guard_reset(fg_pass_guard *guard, unsigned sample_context_limit) {
    guard->program = guard->fragment = 0;
    guard->first = 1;
    guard->sample_contexts = 0;
    guard->changed = 0;
    guard->sample_limit = sample_context_limit;
}
static inline void fg_pass_guard_invalidate(fg_pass_guard *guard) {
    guard->changed = 1;
}
static inline void fg_pass_skip_context(fg_pass_frame *frame) {
    if (frame->incomplete_contexts != UINT_MAX) frame->incomplete_contexts++;
    if (frame->open && frame->current_group >= 0)
        frame->group[frame->current_group].contexts_complete = 0;
}

/* Shared production/host-benchmark fast guard. No ordinary-draw counter writes,
   work queues, callbacks or per-draw dependency convention. PROGRAM includes
   the first draw, so UI classification is done once per actual VS transition.
   SAMPLE means permission to inspect one bounded reflected-input context; the
   caller still charges individual bindings and marks omitted contexts. */
static inline unsigned fg_pass_draw_event(int active, fg_pass_frame *frame,
                                          fg_pass_guard *guard, uintptr_t program) {
    if (!active) return 0;
    if (!guard->first && !guard->changed && guard->program == program) return 0;
    unsigned event = FG_PASS_EVENT_PROGRAM;
    if (guard->first) event |= FG_PASS_EVENT_FIRST;
    guard->first = 0;
    guard->changed = 0;
    guard->program = program;
    if (!fg_pass_take_events(frame, 1)) {
        fg_pass_skip_context(frame);
        return 0;
    }
    frame->draw_events++;
    frame->program_events++;
    if (event & FG_PASS_EVENT_FIRST) frame->first_events++;
    if (guard->sample_contexts < guard->sample_limit) {
        guard->sample_contexts++;
        frame->sample_events++;
        event |= FG_PASS_EVENT_SAMPLE;
    } else fg_pass_skip_context(frame);
    return event;
}
typedef void (*fg_pass_metadata_fn)(void *context, unsigned event);
static inline unsigned fg_pass_route_draw(int active, fg_pass_frame *frame,
                                          fg_pass_guard *guard, uintptr_t program,
                                          fg_pass_metadata_fn metadata, void *context) {
    unsigned event = fg_pass_draw_event(active, frame, guard, program);
    if (event) metadata(context, event);
    return event;
}
static inline unsigned fg_pass_route_programs(int active, fg_pass_frame *frame, fg_pass_guard *guard,
                                               uintptr_t vertex, uintptr_t fragment,
                                               fg_pass_metadata_fn metadata, void *context) {
    if (!active) return 0;
    if (guard->fragment != fragment) {
        fg_pass_guard_invalidate(guard);
        guard->fragment = fragment;
    }
    return fg_pass_route_draw(1,frame,guard,vertex,metadata,context);
}

/* Diagnostics retain only actual current-frame sampled producer versions.
   Consumers and producers keep physical-pass or tagged operation identities.
   Unknown/history reads are not current-frame links; stale known snapshots or
   invalid writers fail closed. An absent context cannot add a link. */
static inline int fg_pass_record_link(fg_pass_frame *frame, uint64_t current_epoch,
                                      fg_dep_value source, unsigned consumer,
                                      int consumer_group, unsigned latest_raster) {
    if (!current_epoch || source.epoch != current_epoch || !source.version ||
        !(source.writer & ~FG_PASS_WRITER_OPERATION) ||
        (!(source.writer & FG_PASS_WRITER_OPERATION) && source.writer > latest_raster)) {
        frame->failed |= FG_PASS_FAILED_IDENTITY;
        return 0;
    }
    for (unsigned i = 0; i < frame->link_count; i++)
        if (frame->link[i].consumer_order == consumer &&
            frame->link[i].source.epoch == source.epoch &&
            frame->link[i].source.version == source.version) return 1;
    if (frame->link_count == FG_PASS_MAX_LINKS) {
        frame->failed |= FG_PASS_FAILED_LINKS;
        return 0;
    }
    frame->link[frame->link_count++] = (fg_pass_sample_link){
        .source = source, .consumer_order = consumer,
        .consumer_group = consumer_group};
    return 1;
}
static inline int fg_pass_link(fg_pass_frame *frame, uint64_t current_epoch, fg_dep_value source) {
    if (!source.known) return 1;
    if (!frame->open || !frame->physical_passes) {
        frame->failed |= FG_PASS_FAILED_IDENTITY;
        return 0;
    }
    return fg_pass_record_link(frame, current_epoch, source, frame->physical_passes,
                               frame->current_group, frame->physical_passes - 1);
}
static inline int fg_pass_operation_link(fg_pass_frame *frame, uint64_t current_epoch,
                                        unsigned operation_writer, fg_dep_value source) {
    if (!source.known) return 1;
    if (!(operation_writer & FG_PASS_WRITER_OPERATION) ||
        !(operation_writer & ~FG_PASS_WRITER_OPERATION) ||
        ((source.writer & FG_PASS_WRITER_OPERATION) && source.writer >= operation_writer)) {
        frame->failed |= FG_PASS_FAILED_IDENTITY;
        return 0;
    }
    return fg_pass_record_link(frame, current_epoch, source, operation_writer, -1,
                               frame->physical_passes);
}

/* Stable ranking across ALL logical groups, including tiny/format-unsupported
   and attachment-zero-less passes. Ties keep first physical occurrence order. */
static inline unsigned fg_pass_top5(const fg_pass_frame *frame, unsigned out[FG_PASS_TOP]) {
    unsigned count = 0;
    for (unsigned i = 0; i < frame->count; i++) {
        unsigned at = 0;
        while (at < count && frame->group[out[at]].draws >= frame->group[i].draws) at++;
        if (at == FG_PASS_TOP) continue;
        unsigned end = count < FG_PASS_TOP ? count++ : FG_PASS_TOP - 1;
        while (end > at) { out[end] = out[end - 1]; end--; }
        out[at] = i;
    }
    return count;
}

/* First-principles nomination only: unique greatest-draw, depth-writing,
   multiple-color group at the supplied scene render extent. No Metal enum,
   minimum MRT dimension, C0, depth-format or camera-validity gate. Never use
   this nomination without separate complete sampled-ancestry proof. */
static inline fg_pass_result fg_pass_main_candidate(const fg_pass_frame *frame,
                                                   unsigned width, unsigned height,
                                                   unsigned *candidate) {
    if (frame->failed || frame->open) return FG_PASS_INCOMPLETE;
    int best = -1;
    unsigned tied = 0;
    for (unsigned i = 0; i < frame->count; i++) {
        const fg_pass_group *g = &frame->group[i];
        if (!width || !height || g->key.width != width || g->key.height != height ||
            !g->draws || !g->depth_writes || !g->key.depth.key.backing ||
            fg_pass_color_count(&g->key) < 2) continue;
        if (best < 0 || g->draws > frame->group[best].draws) { best = (int)i; tied = 0; }
        else if (g->draws == frame->group[best].draws) tied = 1;
    }
    if (best < 0) return FG_PASS_NO_CANDIDATE;
    if (tied) return FG_PASS_AMBIGUOUS;
    if (candidate) *candidate = (unsigned)best;
    return FG_PASS_NOMINATED;
}

#endif
