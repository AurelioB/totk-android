/* Link-time default for fragment inputs the paired vertex shader never declares (issue #4).

   On Maxwell a fragment shader reads (0, 0, 0, 1) from every attribute the vertex stage did not write. Translator
   patch 0005 restores that for outputs a vertex shader declares, but a location the vertex shader has no output for
   at all is left to Vulkan, which makes the input undefined. Adreno returns garbage there: the strong-wind ribbons
   (updrafts, Tulin's gust, fans) multiply their alpha by such an input and draw as bright white boxes.

   Shaders are translated one at a time, so only the pipeline knows both stages. vk_varyings_missing() compares a
   vertex shader's Output locations with a fragment shader's Input locations; vk_spirv_add_outputs() returns a copy
   of the vertex module with one vec4 Output per missing location (the fragment input's component type), stored as
   (0, 0, 0, 1.0f bits) at the start of the entry point, exactly as the translator initializes declared outputs.
   Header-only, no Vulkan calls: host tests validate the words without a device. Nothing here mutates its input. */
#ifndef TOTK_VK_VARYINGS_H
#define TOTK_VK_VARYINGS_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { VK_VARY_FLOAT = 0, VK_VARY_SINT = 1, VK_VARY_UINT = 2, VK_VARY_MIXED = 0xfe, VK_VARY_NONE = 0xff };

typedef struct {
    uint32_t mask;     /* user locations covered by a Location-decorated Input / Output variable */
    uint32_t array;    /* locations covered by an array variable (indexed attributes) */
    uint8_t kind[32];  /* VK_VARY_*: 32-bit component type per location; MIXED for other or conflicting types */
} vk_varying_io;

/* Interface of one storage class (1 Input, 3 Output) of a single-entry-point module. 0 ok, -1 malformed or
   unsupported (unknown sizes, more than one entry point, a Location outside 0..31): callers then leave it alone. */
static inline int vk_spirv_io(const uint32_t *w, size_t nw, uint32_t storage, vk_varying_io *io) {
    memset(io, 0, sizeof *io); memset(io->kind, VK_VARY_NONE, sizeof io->kind);
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return -1;
    uint32_t bound = w[3]; unsigned entries = 0;
    /* per id: offset of its defining instruction; Location+1; BuiltIn/Component/Block/Patch flags */
    uint32_t *def = calloc(bound, sizeof *def), *loc = calloc(bound, sizeof *loc); uint8_t *flag = calloc(bound, 1);
    int result = -1;
    if (!def || !loc || !flag) goto done;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) goto done;
        if (op == 15 && ++entries > 1) goto done;
        if (op == 71 && n >= 3 && w[i + 1] < bound) {   /* OpDecorate */
            uint32_t id = w[i + 1], d = w[i + 2];
            if (d == 30 && n == 4) loc[id] = w[i + 3] + 1;      /* Location */
            else if (d == 11) flag[id] |= 1;                    /* BuiltIn */
            else if (d == 2 || d == 15) flag[id] |= 2;          /* Block / Patch */
        }
        uint32_t id = 0;
        if (((op >= 19 && op <= 39) && n >= 2)) id = w[i + 1];              /* types */
        else if ((op == 43 || op == 59) && n >= 3) id = w[i + 2];           /* OpConstant, OpVariable */
        if (id) { if (id >= bound) goto done; def[id] = (uint32_t)i; }
    }
    if (entries != 1) goto done;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op != 59 || n < 4 || w[i + 3] != storage) continue;
        uint32_t var = w[i + 2];
        if (flag[var] & 1) continue;                            /* built-ins are not user locations */
        if (!loc[var]) continue;                                /* only Location-decorated user variables */
        if (flag[var] & 2) goto done;                           /* blocks / per-patch: not produced for VS/FS here */
        uint32_t first = loc[var] - 1, count = 1, is_array = 0;
        uint32_t ptr = w[i + 1], at = ptr < bound ? def[ptr] : 0;
        if (!at || (w[at] & 0xffff) != 32 || (w[at] >> 16) != 4) goto done;
        uint32_t t = w[at + 3], tt = t < bound ? def[t] : 0;
        if (!tt) goto done;
        if ((w[tt] & 0xffff) == 28) {                           /* OpTypeArray: element, length constant */
            uint32_t len = w[tt + 3] < bound ? def[w[tt + 3]] : 0;
            if (!len || (w[len] & 0xffff) != 43 || (w[len] >> 16) != 4 || !w[len + 3] || w[len + 3] > 32) goto done;
            count = w[len + 3]; is_array = 1;
            t = w[tt + 2]; tt = t < bound ? def[t] : 0;
            if (!tt) goto done;
        }
        if ((w[tt] & 0xffff) == 23) { t = w[tt + 2]; tt = t < bound ? def[t] : 0; if (!tt) goto done; }   /* vector -> scalar */
        uint32_t top = w[tt] & 0xffff, kind = VK_VARY_MIXED;
        if (top == 22 && (w[tt] >> 16) == 3 && w[tt + 2] == 32) kind = VK_VARY_FLOAT;
        else if (top == 21 && (w[tt] >> 16) == 4 && w[tt + 2] == 32) kind = w[tt + 3] ? VK_VARY_SINT : VK_VARY_UINT;
        if (first >= 32 || count > 32 - first) goto done;
        for (uint32_t l = first; l < first + count; l++) {
            io->mask |= 1u << l;
            if (is_array) io->array |= 1u << l;
            io->kind[l] = io->kind[l] == VK_VARY_NONE || io->kind[l] == kind ? (uint8_t)kind : VK_VARY_MIXED;
        }
    }
    result = 0;
done:
    free(def); free(loc); free(flag);
    return result;
}

/* Fragment input locations the vertex shader does not cover and that can be given a plain vec4 default.
   Array (indexed) fragment inputs and mixed component types are left alone: they would need a matching array or
   per-component outputs, which the translator does not emit for these games. */
static inline uint32_t vk_varyings_missing(const vk_varying_io *vs_out, const vk_varying_io *fs_in) {
    uint32_t missing = fs_in->mask & ~vs_out->mask & ~fs_in->array;
    for (unsigned l = 0; l < 32; l++)
        if ((missing >> l & 1) && fs_in->kind[l] > VK_VARY_UINT) missing &= ~(1u << l);
    return missing;
}

/* A new vertex module with a vec4 Output (component type kind[l]) at each location of `missing`, initialized to
   (0, 0, 0, 0x3f800000) at the start of the entry function. NULL (input untouched) if missing is zero, the module
   is not a single-entry vertex module, or its layout is unexpected. *out_n receives the word count. */
static inline uint32_t *vk_spirv_add_outputs(const uint32_t *w, size_t nw, uint32_t missing, const uint8_t kind[32], size_t *out_n) {
    if (!missing || nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22) - 256) return NULL;
    for (unsigned l = 0; l < 32; l++) if ((missing >> l & 1) && kind[l] > VK_VARY_UINT) return NULL;
    size_t entry_at = 0, deco_at = 0, func_at = 0, store_at = 0; uint32_t entry = 0;
    /* existing scalar / vec4 / Output pointer types per kind, reused because duplicate scalar/vector types are invalid */
    uint32_t scalar[3] = {0}, vec4[3] = {0}, outptr[3] = {0};
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) return NULL;
        if (op == 15) {
            if (entry_at || n < 4 || w[i + 1] != 0) return NULL;   /* exactly one entry point, ExecutionModel Vertex */
            entry_at = i; entry = w[i + 2];
        }
        int preamble = op == 17 || op == 10 || op == 11 || op == 14 || op == 15 || op == 16 || op == 331 ||
                       (op >= 2 && op <= 7) || op == 330 || (op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633;
        if (!deco_at && !preamble) deco_at = i;
        if (!func_at && op == 54) func_at = i;
        if (func_at) continue;
        if (op == 22 && n == 3 && w[i + 2] == 32) scalar[VK_VARY_FLOAT] = w[i + 1];
        if (op == 21 && n == 4 && w[i + 2] == 32) scalar[w[i + 3] ? VK_VARY_SINT : VK_VARY_UINT] = w[i + 1];
    }
    if (!entry_at || !deco_at || !func_at || deco_at > func_at) return NULL;
    for (size_t i = 5; i < func_at; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        for (int k = 0; k < 3; k++) {
            if (op == 23 && n == 4 && scalar[k] && w[i + 2] == scalar[k] && w[i + 3] == 4) vec4[k] = w[i + 1];
            if (op == 32 && n == 4 && vec4[k] && w[i + 2] == 3 && w[i + 3] == vec4[k]) outptr[k] = w[i + 1];
        }
    }
    /* The entry function's first block: after OpFunction, its parameters, OpLabel, then OpVariable / OpLine / OpNoLine. */
    for (size_t i = func_at, in_entry = 0, labelled = 0; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) { if (in_entry) return NULL; in_entry = n == 5 && w[i + 2] == entry; continue; }
        if (!in_entry) continue;
        if (op == 55) continue;
        if (op == 248) { if (labelled) { store_at = i; break; } labelled = 1; continue; }
        if (!labelled) return NULL;
        if (op == 59 || op == 8 || op == 317) continue;
        store_at = i; break;
    }
    if (!store_at) return NULL;
    unsigned locations = (unsigned)__builtin_popcount(missing);
    size_t cap = nw + (size_t)locations * 12 + 3 * 27 + 8;
    uint32_t *out = malloc(cap * sizeof *out);
    if (!out) return NULL;
    uint32_t next = w[3], var[32] = {0}, value[3] = {0};
    for (unsigned l = 0; l < 32; l++) if (missing >> l & 1) var[l] = next++;
    size_t k = 0;
    for (size_t i = 0; i < 5; i++) out[k++] = w[i];
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16;
        if (i == deco_at) for (unsigned l = 0; l < 32; l++) if (var[l]) {
            out[k++] = 4u << 16 | 71; out[k++] = var[l]; out[k++] = 30; out[k++] = l;   /* OpDecorate Location */
        }
        if (i == func_at) for (int t = 0; t < 3; t++) {
            int used = 0;
            for (unsigned l = 0; l < 32; l++) if (var[l] && kind[l] == t) used = 1;
            if (!used) continue;
            if (!scalar[t]) {
                scalar[t] = next++;
                if (t == VK_VARY_FLOAT) { out[k++] = 3u << 16 | 22; out[k++] = scalar[t]; out[k++] = 32; }
                else { out[k++] = 4u << 16 | 21; out[k++] = scalar[t]; out[k++] = 32; out[k++] = t == VK_VARY_SINT; }
            }
            if (!vec4[t]) { vec4[t] = next++; out[k++] = 4u << 16 | 23; out[k++] = vec4[t]; out[k++] = scalar[t]; out[k++] = 4; }
            if (!outptr[t]) { outptr[t] = next++; out[k++] = 4u << 16 | 32; out[k++] = outptr[t]; out[k++] = 3; out[k++] = vec4[t]; }
            uint32_t zero = next++, one = next++; value[t] = next++;
            out[k++] = 4u << 16 | 43; out[k++] = scalar[t]; out[k++] = zero; out[k++] = 0;
            out[k++] = 4u << 16 | 43; out[k++] = scalar[t]; out[k++] = one; out[k++] = 0x3f800000u;   /* Maxwell: 1.0f bits */
            out[k++] = 7u << 16 | 44; out[k++] = vec4[t]; out[k++] = value[t];
            out[k++] = zero; out[k++] = zero; out[k++] = zero; out[k++] = one;
            for (unsigned l = 0; l < 32; l++) if (var[l] && kind[l] == t) {
                out[k++] = 4u << 16 | 59; out[k++] = outptr[t]; out[k++] = var[l]; out[k++] = 3;   /* OpVariable Output */
            }
        }
        if (i == store_at) for (unsigned l = 0; l < 32; l++) if (var[l]) {
            out[k++] = 3u << 16 | 62; out[k++] = var[l]; out[k++] = value[kind[l]];   /* OpStore */
        }
        size_t start = k;
        memcpy(out + k, w + i, n * sizeof *out); k += n;
        if (i == entry_at) {                                   /* OpEntryPoint interface += new outputs */
            for (unsigned l = 0; l < 32; l++) if (var[l]) out[k++] = var[l];
            out[start] = (uint32_t)(k - start) << 16 | 15;
        }
    }
    if (k > cap) abort();   /* the bound above is exact-or-larger by construction */
    out[3] = next; *out_n = k;
    return out;
}
#endif
