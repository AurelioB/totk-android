/* Worker-owned, allocation-free pipeline aggregation. No Vulkan dependencies:
 * the host fixture feeds the same fenced-lease consumer with synthetic queries.
 * IDs are launch-local and never reused. Inactive dictionary entries can be
 * evicted between windows; current-window observations are never evicted. */
#ifndef VK_GPUPROF_COMPACT_H
#define VK_GPUPROF_COMPACT_H
#include <stdint.h>
#include <string.h>
#define GP_COMPACT_KEYS 8192u
#define GP_COMPACT_EXPOSURES 64u
#define GP_COMPACT_COLUMNS 20u
enum {
    GC_OBSERVED, GC_SELECTED, GC_VALID, GC_MISSING, GC_DISCARDED,
    GC_DRAWS, GC_DISPATCHES, GC_PRIMITIVES, GC_UNKNOWN_DRAWS,
    GC_MS, GC_SQUARE, GC_ELIGIBLE, GC_ELIGIBLE_MS, GC_ELIGIBLE_WEIGHTED,
    GC_ELIGIBLE_SQUARE, GC_ELIGIBLE_MISSING, GC_OUT_OF_RANGE,
    GC_SAMPLED_DRAWS, GC_SAMPLED_DISPATCHES, GC_WEIGHTED
};
static const char *const gp_compact_columns[GP_COMPACT_COLUMNS] = {
    "observed_groups", "selected_groups", "valid_samples", "missing_selected", "discarded_groups",
    "draws", "dispatches", "known_primitives", "unknown_primitive_draws", "gpu_ms_sum",
    "gpu_ms_square_sum", "eligible_samples", "eligible_gpu_ms_sum", "eligible_weighted_gpu_ms_sum",
    "eligible_weighted_gpu_ms_square_sum", "eligible_missing_selected", "out_of_range_groups",
    "sampled_draws", "sampled_dispatches", "weighted_gpu_ms_sum"
};
typedef struct { uint64_t pipeline; uint32_t compute; char shader[3][80]; } gp_compact_key;
typedef struct {
    gp_compact_key key;
    uint64_t id, hash, frame, window, announced_rotation;
    uint32_t map_slot, selected_discarded;
    double v[GP_COMPACT_COLUMNS];
    double pending[5]; /* valid, ms, weighted ms, weighted square, missing */
} gp_compact_entry;
typedef struct {
    uint32_t population, numerator, denominator;
    uint64_t frames, eligible_frames, observed, selected;
} gp_compact_exposure;
typedef struct {
    gp_compact_entry key[GP_COMPACT_KEYS];
    uint32_t map[GP_COMPACT_KEYS * 2], nkeys, evict;
    uint32_t touched[GP_COMPACT_KEYS], ntouched;
    gp_compact_exposure exposure[GP_COMPACT_EXPOSURES];
    uint32_t nexposure;
    uint64_t next_id, start, end, frames, eligible_frames, domain_excluded_frames;
    uint64_t observed, selected, valid, missing, timestamps, dropped, unretained, out_of_range, discarded;
    uint64_t unavailable, invalid, query_failures, key_overflow, aggregate_overflow, frame_overflow;
    uint64_t frame, frame_observed, frame_selected, frame_out_of_range, frame_dropped;
    uint32_t frame_population, frame_numerator;
    int frame_bad, counts_complete;
    double cpu_ms;
} gp_compact;
static uint64_t gp_compact_hash(const gp_compact_key *key) {
    /* The handle distributes buckets; full shader strings below still decide
       identity (including a reused handle). Do not rehash 140+ shader bytes for
       every optional observation. Mix aligned Vulkan handles before masking. */
    uint64_t h = key->pipeline ^ ((uint64_t)key->compute << 63);
    h ^= h >> 30; h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27; h *= UINT64_C(0x94d049bb133111eb);
    return h ^ (h >> 31);
}
static gp_compact_entry *gp_compact_get(gp_compact *c, const gp_compact_key *key) {
    uint64_t hash = gp_compact_hash(key);
    unsigned at = hash & (GP_COMPACT_KEYS * 2 - 1), insert = UINT32_MAX;
    for (unsigned n = 0; n < GP_COMPACT_KEYS * 2; n++, at = (at + 1) & (GP_COMPACT_KEYS * 2 - 1)) {
        uint32_t index = c->map[at];
        if (!index) { if (insert == UINT32_MAX) insert = at; break; }
        if (index == UINT32_MAX) { if (insert == UINT32_MAX) insert = at; continue; }
        gp_compact_entry *e = &c->key[index - 1];
        if (e->hash == hash && e->key.pipeline == key->pipeline && e->key.compute == key->compute &&
            !strcmp(e->key.shader[0], key->shader[0]) && !strcmp(e->key.shader[1], key->shader[1]) &&
            !strcmp(e->key.shader[2], key->shader[2])) { e->window = c->start; return e; }
    }
    if (insert == UINT32_MAX || c->next_id == UINT64_MAX) { c->aggregate_overflow++; return NULL; }
    unsigned index = c->nkeys;
    if (index == GP_COMPACT_KEYS) {
        unsigned n;
        for (n = 0; n < GP_COMPACT_KEYS; n++) {
            index = c->evict++ & (GP_COMPACT_KEYS - 1);
            if (c->key[index].window != c->start) break;
        }
        if (n == GP_COMPACT_KEYS) { c->key_overflow++; c->counts_complete = 0; return NULL; }
        c->map[c->key[index].map_slot] = UINT32_MAX;
    } else c->nkeys++;
    gp_compact_entry *e = &c->key[index];
    memset(e, 0, sizeof *e);
    e->key = *key; e->hash = hash; e->id = ++c->next_id; e->window = c->start; e->map_slot = insert;
    c->map[insert] = index + 1;
    return e;
}
static void gp_compact_begin(gp_compact *c, uint64_t frame, uint32_t population, uint32_t numerator) {
    if (!c->start) { c->start = (frame - 1) / 300 * 300 + 1; c->counts_complete = 1; }
    if (!c->frame) { c->frame = frame; c->frame_population = population; c->frame_numerator = numerator; }
    if (c->frame != frame || c->frame_population != population || c->frame_numerator != numerator) {
        c->frame_bad = 1; c->frame_overflow++;
    }
}
static void gp_compact_observe(gp_compact *c, const gp_compact_key *key, int selected,
        int valid, int discarded, int out_of_range, double ms, uint32_t population, uint32_t numerator,
        uint64_t draws, uint64_t dispatches, uint64_t primitives, uint64_t unknown_draws) {
    gp_compact_entry *e = gp_compact_get(c, key);
    if (!e) return;
    double *v = e->v;
    if (discarded) {
        v[GC_DISCARDED]++; e->selected_discarded += selected; c->frame_bad = 1; return;
    }
    /* Counter arrays use exact binary64 integers. Refuse estimator authority
       rather than silently rounding an exceptionally large per-key subtotal. */
    if (v[GC_DRAWS] + (double)draws > 9007199254740991.0 ||
        v[GC_DISPATCHES] + (double)dispatches > 9007199254740991.0 ||
        v[GC_PRIMITIVES] + (double)primitives > 9007199254740991.0 ||
        v[GC_UNKNOWN_DRAWS] + (double)unknown_draws > 9007199254740991.0) {
        c->aggregate_overflow++; c->counts_complete = 0;
    }
    v[GC_OBSERVED]++; v[GC_SELECTED] += selected; v[GC_OUT_OF_RANGE] += out_of_range;
    v[GC_DRAWS] += draws; v[GC_DISPATCHES] += dispatches;
    v[GC_PRIMITIVES] += primitives; v[GC_UNKNOWN_DRAWS] += unknown_draws;
    if (!selected) return;
    if (e->frame != c->frame) {
        e->frame = c->frame; memset(e->pending, 0, sizeof e->pending);
        c->touched[c->ntouched++] = (uint32_t)(e - c->key);
    }
    if (!valid || !population || !numerator || out_of_range) {
        v[GC_MISSING]++; e->pending[4]++; return;
    }
    double weighted = ms * population / numerator;
    v[GC_VALID]++; v[GC_MS] += ms; v[GC_SQUARE] += ms * ms; v[GC_WEIGHTED] += weighted;
    v[GC_SAMPLED_DRAWS] += draws; v[GC_SAMPLED_DISPATCHES] += dispatches;
    e->pending[0]++; e->pending[1] += ms; e->pending[2] += weighted; e->pending[3] += weighted * weighted;
}
static void gp_compact_finish(gp_compact *c, int complete) {
    if (!c->frame) return;
    int eligible = complete && !c->frame_bad && !c->frame_out_of_range && !c->frame_dropped;
    c->frames++; c->eligible_frames += eligible; c->domain_excluded_frames += !eligible; c->end = c->frame;
    for (unsigned i = 0; i < c->ntouched; i++) {
        gp_compact_entry *e = &c->key[c->touched[i]];
        if (eligible) {
            e->v[GC_ELIGIBLE] += e->pending[0]; e->v[GC_ELIGIBLE_MS] += e->pending[1];
            e->v[GC_ELIGIBLE_WEIGHTED] += e->pending[2]; e->v[GC_ELIGIBLE_SQUARE] += e->pending[3];
            e->v[GC_ELIGIBLE_MISSING] += e->pending[4];
        }
    }
    unsigned at;
    for (at = 0; at < c->nexposure; at++) if (c->exposure[at].population == c->frame_population &&
        c->exposure[at].numerator == c->frame_numerator) break;
    if (at == GP_COMPACT_EXPOSURES) c->aggregate_overflow++;
    else {
        if (at == c->nexposure) c->exposure[c->nexposure++] = (gp_compact_exposure){
            .population = c->frame_population, .numerator = c->frame_numerator, .denominator = c->frame_population};
        gp_compact_exposure *e = &c->exposure[at];
        e->frames++; e->eligible_frames += eligible; e->observed += c->frame_observed; e->selected += c->frame_selected;
    }
    c->frame = c->frame_observed = c->frame_selected = c->frame_out_of_range = c->frame_dropped = 0;
    c->frame_population = c->frame_numerator = c->ntouched = 0; c->frame_bad = 0;
}
static void gp_compact_reset(gp_compact *c) {
    /* Retain bounded dictionary identities, clear window/frame accumulators and
       rebuild the half-full index to remove eviction tombstones. */
    memset(c->map, 0, sizeof c->map);
    for (unsigned i = 0; i < c->nkeys; i++) {
        gp_compact_entry *e = &c->key[i];
        memset(e->v, 0, sizeof e->v); memset(e->pending, 0, sizeof e->pending); e->frame = 0;
        e->selected_discarded = 0;
        unsigned at = e->hash & (GP_COMPACT_KEYS * 2 - 1);
        while (c->map[at]) at = (at + 1) & (GP_COMPACT_KEYS * 2 - 1);
        c->map[at] = i + 1; e->map_slot = at;
    }
    uint64_t next = c->next_id;
    memset(c->touched, 0, sizeof *c - (size_t)((char *)c->touched - (char *)c));
    c->next_id = next;
}
#endif
