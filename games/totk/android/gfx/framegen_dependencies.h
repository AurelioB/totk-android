/* Per-real-frame resource provenance. No Vulkan, guest-memory or allocation
   dependency. Read values are immutable snapshots, not pointers into the table.

   The caller supplies exact allocation identities: the Vulkan integration uses
   the shared canvas module's image incarnation, not a recyclable handle or
   guest object serial. Compatible image aliases share that identity; mip/layer/
   aspect remain separate. Overlapping CPU/buffer writes are invalidated; GPU
   writes to a different image incarnation do not modify this image's content.
   Snapshot every sampled input (and an attachment's old value when LOAD/blending/partial
   writes actually preserve it) BEFORE publishing any of that draw's outputs.
   Full clears/full replacements do not implicitly retain the old attachment.

   Candidate camera validity is supplied after fg_math_camera. A downstream
   world-composition camera intersects eligible and unresolved origins; raw
   origins and tags remain available for diagnostics. Invalid/revoked roots
   remain unresolved until a camera explicitly excludes them. Do not filter
   using the UI camera: snapshot before the first UI write. Candidate depth
   revocation is explicit, since consuming/reusing G-buffer colors is legitimate.

   Log source[].epoch/version/writer -> output.epoch/version/writer plus masks
   when diagnostics are requested to reconstruct the actual sampled chain. No
   diagnostic graph or per-draw allocation is retained by this helper. */
#ifndef TOTK_FRAMEGEN_DEPENDENCIES_H
#define TOTK_FRAMEGEN_DEPENDENCIES_H

#include <stdint.h>
#include <string.h>

#ifndef FG_DEP_MAX_RESOURCES
#define FG_DEP_MAX_RESOURCES 2048
#endif
#ifndef FG_DEP_MAX_SOURCES
#define FG_DEP_MAX_SOURCES 64
#endif
#ifndef FG_DEP_MAX_CANDIDATES
#define FG_DEP_MAX_CANDIDATES 64
#endif
#if FG_DEP_MAX_RESOURCES < 1 || FG_DEP_MAX_SOURCES < 1 || FG_DEP_MAX_CANDIDATES < 1 || FG_DEP_MAX_CANDIDATES > 64
#error "invalid framegen dependency capacity"
#endif

typedef uint64_t fg_dep_mask;
typedef struct {
    uint64_t backing;
    uint32_t mip, layer, aspect;
} fg_dep_key;
typedef struct {
    fg_dep_mask origins, eligible, unresolved;
    uint64_t tags, version, epoch;
    uint32_t writer;
    unsigned known, tainted;
} fg_dep_value;
typedef struct {
    fg_dep_key key;
    fg_dep_value value;
} fg_dep_resource;
typedef struct {
    fg_dep_resource resource[FG_DEP_MAX_RESOURCES];
    unsigned used[FG_DEP_MAX_RESOURCES]; /* occupied slots in this epoch, no full-table range scans */
    uint64_t epoch, next_version;
    fg_dep_mask valid_candidates, revoked_candidates;
    unsigned count, candidate_count, failed;
} fg_dep_frame;
typedef struct {
    fg_dep_value value;
    fg_dep_value source[FG_DEP_MAX_SOURCES];
    unsigned count;
} fg_dep_sources;
typedef enum {
    FG_DEP_SELECTED = 0, FG_DEP_NO_ORIGIN, FG_DEP_NO_ELIGIBLE,
    FG_DEP_AMBIGUOUS, FG_DEP_TAINTED, FG_DEP_STALE, FG_DEP_FAILED
} fg_dep_result;
enum {
    FG_DEP_TAINT_UNKNOWN=1u, FG_DEP_TAINT_IDENTITY=2u, FG_DEP_TAINT_VIEW=4u,
    FG_DEP_TAINT_STORAGE=8u, FG_DEP_TAINT_CLEAR=16u, FG_DEP_TAINT_MAPPING=32u
};

static inline const char *fg_dep_reason(fg_dep_result result) {
    switch (result) {
        case FG_DEP_SELECTED: return "selected sampled dependency";
        case FG_DEP_NO_ORIGIN: return "no current-frame G-buffer dependency";
        case FG_DEP_NO_ELIGIBLE: return "no valid matching live G-buffer camera";
        case FG_DEP_AMBIGUOUS: return "ambiguous sampled G-buffer dependencies";
        case FG_DEP_TAINTED: return "unknown resource dependency";
        case FG_DEP_STALE: return "stale resource dependency";
        case FG_DEP_FAILED: return "dependency tracking capacity or identity failure";
    }
    return "invalid dependency result";
}

/* State must be zero-initialized before its first reset. Epoch-stamped slots
   avoid clearing/copying the table each frame. No previous-frame edge survives. */
static inline void fg_dep_reset(fg_dep_frame *frame) {
    if (++frame->epoch == 0) {
        memset(frame->resource, 0, sizeof frame->resource);
        frame->epoch = 1;
    }
    frame->next_version = 0;
    frame->valid_candidates = frame->revoked_candidates = 0;
    frame->count = frame->candidate_count = frame->failed = 0;
}

static inline void fg_dep_fail(fg_dep_frame *frame) {
    frame->failed = 1;
}

static inline int fg_dep_key_equal(fg_dep_key a, fg_dep_key b) {
    return a.backing == b.backing && a.mip == b.mip &&
        a.layer == b.layer && a.aspect == b.aspect;
}

static inline unsigned fg_dep_key_slot(fg_dep_key key) {
    uint64_t hash = key.backing ^ ((uint64_t)key.mip << 32) ^ key.layer;
    hash ^= (uint64_t)key.aspect * UINT64_C(0x9e3779b97f4a7c15);
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31;
    return (unsigned)(hash % FG_DEP_MAX_RESOURCES);
}

/* A zero backing is not an identity. Only writes allocate table entries, so
   static textures and absent/history reads cannot exhaust the resource table. */
static inline fg_dep_resource *fg_dep_find(fg_dep_frame *frame, fg_dep_key key, int create) {
    if (!key.backing || !frame->epoch) {
        fg_dep_fail(frame);
        return NULL;
    }
    unsigned slot = fg_dep_key_slot(key);
    for (unsigned probe = 0; probe < FG_DEP_MAX_RESOURCES; probe++) {
        fg_dep_resource *resource = &frame->resource[slot];
        if (resource->value.epoch != frame->epoch) {
            if (!create) return NULL;
            resource->key = key;
            resource->value = (fg_dep_value){0};
            resource->value.epoch = frame->epoch;
            frame->used[frame->count++] = slot;
            return resource;
        }
        if (fg_dep_key_equal(resource->key, key)) return resource;
        if (++slot == FG_DEP_MAX_RESOURCES) slot = 0;
    }
    if (create) fg_dep_fail(frame);
    return NULL;
}

static inline fg_dep_value fg_dep_read(fg_dep_frame *frame, fg_dep_key key) {
    fg_dep_resource *resource = fg_dep_find(frame, key, 0);
    return resource ? resource->value : (fg_dep_value){0};
}

/* Invalid cameras still receive an origin bit, but never eligibility. */
static inline fg_dep_mask fg_dep_candidate(fg_dep_frame *frame, int camera_valid) {
    if (!frame->epoch || frame->candidate_count == FG_DEP_MAX_CANDIDATES) {
        fg_dep_fail(frame);
        return 0;
    }
    fg_dep_mask bit = UINT64_C(1) << frame->candidate_count++;
    if (camera_valid) frame->valid_candidates |= bit;
    return bit;
}

static inline void fg_dep_candidate_invalidate(fg_dep_frame *frame, fg_dep_mask candidates) {
    frame->revoked_candidates |= candidates;
}

static inline void fg_dep_sources_init(fg_dep_sources *sources) {
    sources->value = (fg_dep_value){0};
    sources->count = 0;
}

static inline void fg_dep_sources_add(fg_dep_frame *frame, fg_dep_sources *sources,
                                      fg_dep_value value) {
    if (sources->value.known && sources->value.epoch != frame->epoch) {
        fg_dep_sources_init(sources);
        sources->value.tainted = 1;
    }
    /* Unproduced sampled textures are not evidence of a current-frame edge.
       A retained old snapshot, unlike an ordinary absent read, is a misuse. */
    if (!value.known) return;
    if (value.epoch != frame->epoch || !value.version) {
        sources->value.tainted = 1;
        return;
    }
    for (unsigned i = 0; i < sources->count; i++)
        if (sources->source[i].version == value.version) return;
    if (sources->count == FG_DEP_MAX_SOURCES) {
        sources->value.tainted = 1;
        fg_dep_fail(frame);
        return;
    }
    sources->source[sources->count++] = value;
    sources->value.origins |= value.origins;
    sources->value.eligible |= value.eligible;
    sources->value.unresolved |= value.unresolved;
    sources->value.tags |= value.tags;
    sources->value.tainted |= value.tainted;
    sources->value.known = 1;
    sources->value.epoch = frame->epoch;
}

/* Charge the cheaper complete traversal: exact subresource hash lookups for a
   small view, occupied current-frame versions for a large array/volume. This
   needs no second index or allocation and does not omit unproduced reads. */
static inline unsigned fg_dep_sample_work(const fg_dep_frame *frame, unsigned levels, unsigned layers) {
    if (levels == 1 && layers == 1) return 1;
    if (!layers || levels > frame->count / layers) return frame->count;
    return levels * layers;
}
/* Returns the same bounded work reserved by fg_dep_sample_work. */
static inline unsigned fg_dep_sample_view(fg_dep_frame *frame, fg_dep_sources *sources,
                                          uint64_t backing, unsigned mip, unsigned levels,
                                          unsigned layer, unsigned layers, unsigned aspect) {
    if (!backing || !frame->epoch || !levels || !layers) {
        fg_dep_fail(frame); sources->value.tainted |= FG_DEP_TAINT_UNKNOWN; return 0;
    }
    if (levels == 1 && layers == 1) {
        fg_dep_sources_add(frame,sources,fg_dep_read(frame,(fg_dep_key){backing,mip,layer,aspect}));
        return 1;
    }
    unsigned work=fg_dep_sample_work(frame,levels,layers);
    if (levels <= frame->count / layers) {
        for (unsigned m=0;m<levels;m++) for (unsigned l=0;l<layers;l++) {
            /* Preserve range semantics even for a caller's overflowing end. */
            if (m > UINT32_MAX-mip || l > UINT32_MAX-layer) continue;
            fg_dep_sources_add(frame,sources,
                fg_dep_read(frame,(fg_dep_key){backing,mip+m,layer+l,aspect}));
        }
        return work;
    }
    for (unsigned i=0;i<frame->count;i++) {
        const fg_dep_resource *r=&frame->resource[frame->used[i]];
        if (r->key.backing == backing && r->key.aspect == aspect &&
            r->key.mip >= mip && r->key.mip-mip < levels &&
            r->key.layer >= layer && r->key.layer-layer < layers)
            fg_dep_sources_add(frame,sources,r->value);
    }
    return frame->count;
}

/* sources may be NULL for a known overwrite/clear. A camera match mask is
   authoritative only when camera_present is true; absent camera preserves the
   eligibility established upstream. Never restore eligibility from raw origins. */
static inline fg_dep_value fg_dep_write(fg_dep_frame *frame, fg_dep_key key,
                                       const fg_dep_sources *sources, uint32_t writer,
                                       fg_dep_mask seed, int camera_present,
                                       fg_dep_mask camera_matches, uint64_t tags) {
    fg_dep_value value = sources ? sources->value : (fg_dep_value){0};
    fg_dep_mask allocated = frame->candidate_count == 64 ? UINT64_MAX :
        (UINT64_C(1) << frame->candidate_count) - 1;
    if (seed & ~allocated) fg_dep_fail(frame);
    if (value.known && value.epoch != frame->epoch) {
        value = (fg_dep_value){0};
        value.tainted = 1;
    }
    value.origins |= seed;
    value.unresolved |= seed;
    value.eligible |= seed & frame->valid_candidates;
    value.eligible &= frame->valid_candidates & ~frame->revoked_candidates;
    if (camera_present) {
        value.eligible &= camera_matches;
        value.unresolved &= camera_matches;
    }
    value.tags |= tags;
    value.writer = writer;
    value.epoch = frame->epoch;
    value.known = 1;
    value.version = ++frame->next_version;
    if (!value.version) {
        value.tainted = 1;
        fg_dep_fail(frame);
    }
    fg_dep_resource *resource = fg_dep_find(frame, key, 1);
    if (resource) resource->value = value;
    else value.tainted = 1;
    return value;
}

static inline fg_dep_value fg_dep_clear(fg_dep_frame *frame, fg_dep_key key, uint32_t writer) {
    return fg_dep_write(frame, key, NULL, writer, 0, 0, 0, 0);
}

/* A deterministic regional clear introduces no new sampled dependency and
   conservatively retains the previous whole-subresource ancestry for pixels
   outside its rectangle. Absent current-frame provenance is rootless history,
   as for an ordinary sampled texture; it is NOT an explicit unknown write.
   A tracked unknown/stale prior version must still taint the result. */
static inline fg_dep_value fg_dep_partial_clear(fg_dep_frame *frame, fg_dep_key key, uint32_t writer) {
    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    fg_dep_sources_add(frame,&sources,fg_dep_read(frame,key));
    return fg_dep_write(frame,key,&sources,writer,0,0,0,0);
}

/* Unknown writes kill old proof; subsequent reads propagate uncertainty. This
   is distinct from a full clear, whose replacement content is known rootless. */
static inline fg_dep_value fg_dep_invalidate(fg_dep_frame *frame, fg_dep_key key, uint32_t writer) {
    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    sources.value.tainted = 1;
    return fg_dep_write(frame, key, &sources, writer, 0, 0, 0, 0);
}

static inline void fg_dep_invalidate_backing(fg_dep_frame *frame, uint64_t backing,
                                            uint32_t writer) {
    for (unsigned i = 0; i < FG_DEP_MAX_RESOURCES; i++) {
        fg_dep_resource *resource = &frame->resource[i];
        if (resource->value.epoch == frame->epoch && resource->key.backing == backing)
            (void)fg_dep_invalidate(frame, resource->key, writer);
    }
}

/* Exact/full copies replace the destination. Partial copies additionally keep
   its old value. Both source and destination snapshots precede the write, even
   for self/alias copies. Unknown copy extents must use invalidate instead. */
static inline fg_dep_value fg_dep_copy(fg_dep_frame *frame, fg_dep_key destination,
                                      fg_dep_key source, uint32_t writer, int preserve_destination) {
    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    fg_dep_sources_add(frame, &sources, fg_dep_read(frame, source));
    if (preserve_destination)
        fg_dep_sources_add(frame, &sources, fg_dep_read(frame, destination));
    return fg_dep_write(frame, destination, &sources, writer, 0, 0, 0, 0);
}

/* Does not require global candidate uniqueness. Only actual sampled ancestors
   with valid, live cameras that survived downstream camera filters may select.
   An invalid/revoked extra root is not implicitly excluded: only a downstream
   camera can resolve it away. candidate_index is changed only on success.
   Revoke a candidate on destructive writes to its retained depth even if an
   already-consumed color still traces it. */
static inline fg_dep_result fg_dep_select(const fg_dep_frame *frame, fg_dep_value value,
                                         unsigned *candidate_index) {
    if (frame->failed || !frame->epoch) return FG_DEP_FAILED;
    if (!value.known) return FG_DEP_NO_ORIGIN;
    if (value.epoch != frame->epoch || !value.version) return FG_DEP_STALE;
    if (value.tainted) return FG_DEP_TAINTED;
    if (!value.origins) return FG_DEP_NO_ORIGIN;
    fg_dep_mask eligible = value.eligible & frame->valid_candidates & ~frame->revoked_candidates;
    if (value.unresolved & (value.unresolved - 1)) return FG_DEP_AMBIGUOUS;
    if (!eligible || eligible != value.unresolved) return FG_DEP_NO_ELIGIBLE;
    if (candidate_index) {
        unsigned index = 0;
        while (!(eligible & (UINT64_C(1) << index))) index++;
        *candidate_index = index;
    }
    return FG_DEP_SELECTED;
}

#endif
