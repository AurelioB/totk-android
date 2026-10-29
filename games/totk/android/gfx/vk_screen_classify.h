/* Screen/capture policy only: no Vulkan state or environment access. */
#ifndef TOTK_VK_SCREEN_CLASSIFY_H
#define TOTK_VK_SCREEN_CLASSIFY_H

#include <stdint.h>

typedef enum {
    TOTK_SCREEN_CAPTURE = 0,
    TOTK_SCREEN_EXACT_16_9 = 1,
    TOTK_SCREEN_ALIGNED_8 = 8,
    TOTK_SCREEN_ALIGNED_16 = 16,
    TOTK_SCREEN_ALIGNED_32 = 32
} totk_screen_class;

static inline int totk_screen_classify(uint32_t w, uint32_t h, uint32_t configured_w, uint32_t configured_h) {
    /* Preserve the old strict tolerance first, including its small exact-aspect targets. */
    int64_t delta = (int64_t)w * 9 - (int64_t)h * 16;
    if (delta > -32 && delta < 32) return TOTK_SCREEN_EXACT_16_9;
    if (w < 640 || h < 360 || w <= h) return TOTK_SCREEN_CAPTURE;

    /* Match the runtime's render-resolution bounds; zero/partial/portrait settings add no aspect. */
    int configured = configured_w >= 64 && configured_w <= 4096 &&
                     configured_h >= 64 && configured_h <= 4096 && configured_w > configured_h;
    int64_t configured_delta = configured ? (int64_t)w * configured_h - (int64_t)h * configured_w : 0;
    for (int alignment = 8; alignment <= 32; alignment *= 2) {
        if ((w | h) & (uint32_t)(alignment - 1)) continue;
        /* [w,w+a-1] x [h,h+a-1] must intersect the exact-aspect ray. This admits
           floor alignment of either dimension, not an arbitrary aspect-error percentage. */
        int64_t slack = alignment - 1;
        if (delta >= -slack * 9 && delta <= slack * 16) return alignment;
        if (configured) {
            if (configured_delta >= -slack * configured_h && configured_delta <= slack * configured_w)
                return alignment;
        }
    }
    return TOTK_SCREEN_CAPTURE;
}

#endif
