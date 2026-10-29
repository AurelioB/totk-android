#ifndef TOTK_PSO_TRACE_KEY_H
#define TOTK_PSO_TRACE_KEY_H

#include <stddef.h>
#include <stdint.h>

/* Stable, provider-independent projection of every field in vk_draw.c's pipe_key. */
typedef struct {
    const char *vs_id, *fs_id;
    uint32_t fs_early_tests;
    uint32_t color[8], depth, stencil, color_count, samples;
    uint32_t blend[8][7]; /* target, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a */
    uint32_t blend_enabled, channel_mask, a2c, polygon_mode, topology_class, depth_clamp, logic_op;
    uint32_t attributes[16][3]; /* fmt, offset, stream */
    uint32_t streams[16][2]; /* instanced, divisor */
} TotkPsoTraceKey;

static inline uint64_t totk_pso_trace_key_hash(const TotkPsoTraceKey *key) {
    uint64_t hash = UINT64_C(14695981039346656037);
#define TOTK_PSO_BYTE(b) do { hash ^= (uint8_t)(b); hash *= UINT64_C(1099511628211); } while (0)
#define TOTK_PSO_U32(v) do { uint32_t _v = (uint32_t)(v); TOTK_PSO_BYTE(_v); TOTK_PSO_BYTE(_v >> 8); TOTK_PSO_BYTE(_v >> 16); TOTK_PSO_BYTE(_v >> 24); } while (0)
    static const uint8_t domain[] = {'T','O','T','K','-','P','S','O','-','T','R','A','C','E','-','K','E','Y',0};
    for (size_t i = 0; i < sizeof domain; i++) TOTK_PSO_BYTE(domain[i]);
    TOTK_PSO_U32(1); /* schema version */
    const char *ids[2] = {key->vs_id, key->fs_id};
    for (unsigned s = 0; s < 2; s++) {
        if (!ids[s]) { TOTK_PSO_U32(UINT32_MAX); continue; }
        size_t n = 0; while (ids[s][n]) n++;
        TOTK_PSO_U32(n);
        for (size_t i = 0; i < n; i++) TOTK_PSO_BYTE(ids[s][i]);
    }
    TOTK_PSO_U32(key->fs_early_tests);
    for (unsigned i = 0; i < 8; i++) TOTK_PSO_U32(key->color[i]);
    TOTK_PSO_U32(key->depth); TOTK_PSO_U32(key->stencil); TOTK_PSO_U32(key->color_count); TOTK_PSO_U32(key->samples);
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 7; j++) TOTK_PSO_U32(key->blend[i][j]);
    TOTK_PSO_U32(key->blend_enabled); TOTK_PSO_U32(key->channel_mask); TOTK_PSO_U32(key->a2c);
    TOTK_PSO_U32(key->polygon_mode); TOTK_PSO_U32(key->topology_class); TOTK_PSO_U32(key->depth_clamp); TOTK_PSO_U32(key->logic_op);
    for (unsigned i = 0; i < 16; i++) for (unsigned j = 0; j < 3; j++) TOTK_PSO_U32(key->attributes[i][j]);
    for (unsigned i = 0; i < 16; i++) for (unsigned j = 0; j < 2; j++) TOTK_PSO_U32(key->streams[i][j]);
#undef TOTK_PSO_U32
#undef TOTK_PSO_BYTE
    return hash;
}

#endif
