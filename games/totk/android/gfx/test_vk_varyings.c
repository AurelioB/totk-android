/* Host probe for vk_varyings.h (no Vulkan device, no game files); driven by tests/test_vk_varyings.py.
   vkvaryingstest <vertex.spv> <fragment.spv> <out.spv>
     prints "vs <mask> fs <mask> array <mask> missing <mask>" and writes the patched vertex module (or nothing when
     no location is missing). Exit 0 ok, 2 when vk_spirv_add_outputs refuses, 1 on any other error. */
#include <stdio.h>
#include <stdlib.h>
#include "vk_varyings.h"

static uint32_t *load(const char *path, size_t *nw) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n % 4) { fclose(f); return NULL; }
    uint32_t *w = malloc((size_t)n);
    if (!w || fread(w, 1, (size_t)n, f) != (size_t)n) { free(w); fclose(f); return NULL; }
    fclose(f); *nw = (size_t)n / 4; return w;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s vertex.spv fragment.spv out.spv\n", argv[0]); return 1; }
    size_t nv = 0, nf = 0; uint32_t *v = load(argv[1], &nv), *f = load(argv[2], &nf);
    if (!v || !f) { fprintf(stderr, "cannot read modules\n"); return 1; }
    vk_varying_io vo, fi, again;
    if (vk_spirv_io(v, nv, 3, &vo) || vk_spirv_io(f, nf, 1, &fi)) { fprintf(stderr, "interface scan refused\n"); return 1; }
    uint32_t missing = vk_varyings_missing(&vo, &fi);
    printf("vs %08x fs %08x array %08x missing %08x\n", vo.mask, fi.mask, fi.array, missing);
    if (!missing) return 0;
    uint32_t *copy = malloc(nv * 4); memcpy(copy, v, nv * 4);
    size_t no = 0; uint32_t *o = vk_spirv_add_outputs(v, nv, missing, fi.kind, &no);
    if (memcmp(copy, v, nv * 4)) { fprintf(stderr, "input module mutated\n"); return 1; }
    if (!o) { fprintf(stderr, "add_outputs refused\n"); return 2; }
    /* The patched module must now cover every fragment input with the fragment's component types. */
    if (vk_spirv_io(o, no, 3, &again) || (again.mask & fi.mask & ~fi.array) != (fi.mask & ~fi.array) ||
        again.mask != (vo.mask | missing)) { fprintf(stderr, "patched interface wrong: %08x\n", again.mask); return 1; }
    for (unsigned l = 0; l < 32; l++)
        if ((missing >> l & 1) && again.kind[l] != fi.kind[l]) { fprintf(stderr, "location %u kind %u != %u\n", l, again.kind[l], fi.kind[l]); return 1; }
    /* Zero missing locations and a fragment module must both be refused without output. */
    size_t dummy; if (vk_spirv_add_outputs(v, nv, 0, fi.kind, &dummy) || vk_spirv_add_outputs(f, nf, missing, fi.kind, &dummy)) {
        fprintf(stderr, "expected refusal\n"); return 1;
    }
    FILE *out = fopen(argv[3], "wb");
    if (!out || fwrite(o, 4, no, out) != no) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
    fclose(out); printf("patched %zu -> %zu words\n", nv, no);
    free(o); free(copy); free(v); free(f);
    return 0;
}
