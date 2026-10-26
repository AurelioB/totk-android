/* NVN's GLSLC control section stores early_fragment_tests at byte 0x718 (stage at 0x714).
   It is not SPH's DoesGlobalStore bit. The original control bytes are already part of the shader key.
   Public layout: nvnprogram/uam-nvn f4470ae, source/nvn_control.h. */
#ifndef TOTK_VK_EARLYZ_H
#define TOTK_VK_EARLYZ_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Guest-requested early tests are correctness state, not a performance opt-in. An explicit zero remains a diagnostic
   override for comparison with the old late-test path; it must never be the default. Read once by the runtime. */
static int vk_earlyz_enabled(const char *override) { return !override || atoi(override) != 0; }

/* Only the full TOTK compiler control layout is established here; shorter BOTW sections use different metadata. */
static int vk_nvn_early_tests(const uint8_t *control, size_t control_len, int stage) {
    return stage == 1 && control_len == 0x880 && control[0x718] != 0;
}

/* Normalize a single-entry fragment module to its requested execution mode. No IDs, instructions or other modes
   change. Existing seeds need insertion; newly translated seeds already carry the original mode. Returns -1 on an
   invalid module/allocation failure, 0 unchanged, 1 changed. The caller owns *words before and after this call. */
static int vk_spirv_early_tests(uint32_t **words, size_t *nw, int enable) {
    uint32_t *w = *words, entry = 0; size_t insert = 5, mode = 0; unsigned entries = 0;
    if (*nw < 5 || w[0] != 0x07230203) return -1;
    for (size_t i = 5; i < *nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || n > *nw - i) return -1;
        if (op == 15) { /* OpEntryPoint Fragment */
            if (n < 4 || w[i + 1] != 4 || ++entries != 1) return -1;
            entry = w[i + 2];
        }
        if (op == 15 || op == 16 || op == 331) insert = i + n;
        i += n;
    }
    if (!entry || entries != 1) return -1;
    for (size_t i = 5; i < *nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == 16 && n >= 3 && w[i + 1] == entry && w[i + 2] == 9) {
            if (n != 3 || mode) return -1;
            mode = i;
        }
        i += n;
    }
    if (enable) {
        if (mode) return 0;
        if (*nw > SIZE_MAX / sizeof *w - 3) return -1;
        uint32_t *p = realloc(w, (*nw + 3) * sizeof *w);
        if (!p) return -1;
        memmove(p + insert + 3, p + insert, (*nw - insert) * sizeof *p);
        p[insert] = (3u << 16) | 16; p[insert + 1] = entry; p[insert + 2] = 9;
        *words = p; *nw += 3;
    } else {
        if (!mode) return 0;
        memmove(w + mode, w + mode + 3, (*nw - mode - 3) * sizeof *w);
        *nw -= 3;
    }
    return 1;
}
#endif
