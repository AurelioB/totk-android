/* Independent scalar SGSR1 OperationMode=1 (green proxy) image oracle.
 * Formula provenance: SnapdragonGameStudios/snapdragon-gsr
 * d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e, sgsr/v1/include/glsl/
 * sgsr1_shader_mobile.frag and sgsr1_shader_mobile_edge_direction.frag
 * (UseEdgeDirection enabled for the latter). See post/sgsr/LICENSE.
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This runs from the input bytes, never from a GPU image/hash. Double precision,
 * scalar tap coordinates and explicit crop-local bilinear/gather addressing are
 * intentionally independent of the GLSL implementation and its float lowering.
 * RGB remains display encoded; writing an sRGB-view result must not encode again.
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static double sgsr_ref_clamp(double x, double lo, double hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

typedef struct {
    const uint8_t *pixels;
    uint32_t stride, cx, cy, w, h;
} sgsr_ref_source;

static double sgsr_ref_texel(const sgsr_ref_source *s, int x, int y, unsigned channel) {
    x = x < 0 ? 0 : x >= (int)s->w ? (int)s->w - 1 : x;
    y = y < 0 ? 0 : y >= (int)s->h ? (int)s->h - 1 : y;
    return s->pixels[((size_t)(s->cy + y) * s->stride + s->cx + x) * 4 + channel] / 255.0;
}

/* Pixel centers are n+0.5, with each contributing texel clamped to the crop. */
static double sgsr_ref_linear(const sgsr_ref_source *s, double x, double y, unsigned channel) {
    int ix = (int)floor(x - 0.5), iy = (int)floor(y - 0.5);
    double fx = x - 0.5 - ix, fy = y - 0.5 - iy;
    double top = sgsr_ref_texel(s, ix, iy, channel) * (1 - fx) + sgsr_ref_texel(s, ix + 1, iy, channel) * fx;
    double bottom = sgsr_ref_texel(s, ix, iy + 1, channel) * (1 - fx) + sgsr_ref_texel(s, ix + 1, iy + 1, channel) * fx;
    return top * (1 - fy) + bottom * fy;
}

/* Independent fixed-point sampler model: Q8 coordinate weights, and round-half-
 * even of the delta after EACH separable byte lerp. Mesa's lp_build_lerp_simple
 * and lp_build_sample_image_linear implement this RGBA8 fast path. Rounding
 * only the final 2-D result is not equivalent, especially at edge thresholds. */
static double sgsr_ref_linear_unorm8(const sgsr_ref_source *s, double x, double y, unsigned channel) {
    int fixed_x = (int)nearbyint(x * 256) - 128, fixed_y = (int)nearbyint(y * 256) - 128;
    int ix = fixed_x / 256, iy = fixed_y / 256;
    double fx = (fixed_x % 256) / 256.0, fy = (fixed_y % 256) / 256.0;
    double a = nearbyint(sgsr_ref_texel(s, ix, iy, channel) * 255);
    double b = nearbyint(sgsr_ref_texel(s, ix + 1, iy, channel) * 255);
    double c = nearbyint(sgsr_ref_texel(s, ix, iy + 1, channel) * 255);
    double d = nearbyint(sgsr_ref_texel(s, ix + 1, iy + 1, channel) * 255);
    double top = a + nearbyint((b - a) * fx), bottom = c + nearbyint((d - c) * fx);
    /* Mesa lp_build_unsigned_norm_to_float multiplies by a rounded FP32
     * reciprocal; it is not an exactly rounded integer/255 division. */
    return (float)(top + nearbyint((bottom - top) * fy)) * (1.0f / 255.0f);
}

/* GLSL textureGather component order: lower-left, lower-right, upper-right,
 * upper-left. Integer gather coordinates lie between four pixel centers. */
static void sgsr_ref_gather(const sgsr_ref_source *s, int x, int y, double values[4]) {
    values[0] = sgsr_ref_texel(s, x - 1, y, 1);
    values[1] = sgsr_ref_texel(s, x, y, 1);
    values[2] = sgsr_ref_texel(s, x, y - 1, 1);
    values[3] = sgsr_ref_texel(s, x - 1, y - 1, 1);
}

static void sgsr_ref_pixel(const sgsr_ref_source *s, double x, double y,
                           double sharpness, double threshold, int directional, int sampler_unorm8, uint8_t rgba[4]) {
    double rgb[3];
    int bx = (int)floor(x - 0.5), by = (int)floor(y - 0.5);
    int hardware_linear = bx >= 0 && by >= 0 && bx + 1 < (int)s->w && by + 1 < (int)s->h;
    for (unsigned c = 0; c < 3; c++) {
        rgb[c] = sgsr_ref_linear(s, x, y, c);
        /* Border interpolation is explicit shader float math, not a sampler. */
        if (sampler_unorm8 && hardware_linear) rgb[c] = sgsr_ref_linear_unorm8(s, x, y, c);
    }
    double px = x - 0.5, py = y + 0.5;
    int ix = (int)floor(px), iy = (int)floor(py);
    double fx = px - ix, fy = py - iy;
    double left[4]; sgsr_ref_gather(s, ix, iy, left);
    /* The branch is shader FP32 even though the independent filter oracle is
     * FP64: rounding a sample can legitimately cross EdgeThreshold. */
    float ly = (float)left[1], lz = (float)left[2], green = (float)rgb[1];
    if (sampler_unorm8) {
        ly = (float)nearbyint(left[1] * 255) * (1.0f / 255.0f);
        lz = (float)nearbyint(left[2] * 255) * (1.0f / 255.0f);
    }
    float vote = fabsf(lz - ly) + fabsf(green - ly) + fabsf(green - lz);
    if (vote > threshold) {
        double right[4], up[4], down[4];
        sgsr_ref_gather(s, ix + 2, iy, right);
        sgsr_ref_gather(s, ix + 1, iy - 1, up);
        sgsr_ref_gather(s, ix + 1, iy + 1, down);
        double mean = (left[1] + left[2] + right[0] + right[3]) * 0.25;
        /* The upstream twelve-tap ordering, expressed as (offset, sample), not
         * copied shader expressions. The four center taps also bound ringing. */
        static const int offsets[12][2] = {
            {0,1},{-1,1},{-1,-2},{0,-2}, {1,-1},{0,-1},{0,0},{1,0},
            {-1,-1},{-2,-1},{-2,0},{-1,0}
        };
        double tap[12] = {up[3],up[2],down[1],down[0],
                          left[0],left[1],left[2],left[3],right[0],right[1],right[2],right[3]};
        double sum = 0;
        for (unsigned k = 0; k < 12; k++) { tap[k] -= mean; sum += fabs(tap[k]); }
        double std = directional ? (10.14185 / sum) * (10.14185 / sum) : 2.181818 / sum;
        double dir_x = 0, dir_y = 0;
        if (directional) {
            double a = right[0] - left[2], b = right[3] - left[1];
            double dx = a + b, dy = a - b;
            double inv_length = 1 / sqrt(dx * dx + 3.075740e-05 + dy * dy);
            dir_x = dx * inv_length; dir_y = dy * inv_length;
        }
        double weights = 0, weighted = 0;
        for (unsigned k = 0; k < 12; k++) {
            double dx = fx + offsets[k][0], dy = fy + offsets[k][1];
            double radius = dx * dx + dy * dy;
            double r;
            if (directional) {
                double across = dx * dir_y + dy * dir_x;
                r = radius + across * across * (sgsr_ref_clamp(tap[k] * tap[k] * std, 0, 1) * 0.7 - 1);
            } else r = radius * 0.55 + sgsr_ref_clamp(fabs(tap[k]) * std, 0, 1);
            double a = r - 4, b = r * a - a;
            double weight = b * a * a;
            weights += weight; weighted += weight * tap[k];
        }
        double lo = fmin(fmin(left[1], left[2]), fmin(right[0], right[3])) - mean;
        double hi = fmax(fmax(left[1], left[2]), fmax(right[0], right[3])) - mean;
        double final_y = sgsr_ref_clamp(sharpness * weighted / weights, lo, hi);
        double delta = sgsr_ref_clamp(final_y - (rgb[1] - mean), -23.0 / 255, 23.0 / 255);
        for (unsigned c = 0; c < 3; c++) rgb[c] = sgsr_ref_clamp(rgb[c] + delta, 0, 1);
    }
    for (unsigned c = 0; c < 3; c++) rgba[c] = (uint8_t)floor(sgsr_ref_clamp(rgb[c], 0, 1) * 255 + 0.5);
    rgba[3] = 255;
}

static void sgsr_reference(const sgsr_ref_source *s, uint32_t w, uint32_t h,
                           float sharpness, float threshold, int directional, int sampler_unorm8, uint8_t *pixels) {
    /* Match the public FP32 crop/output coordinate transform, including its
     * exactly-one ratio at 1x; filtering and the SGSR oracle remain FP64. */
    float scale_x = (float)s->w / (float)w, scale_y = (float)s->h / (float)h;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float sx = ((float)x + 0.5f) * scale_x, sy = ((float)y + 0.5f) * scale_y;
        sgsr_ref_pixel(s, sx, sy, sharpness, threshold, directional, sampler_unorm8, pixels + ((size_t)y * w + x) * 4);
    }
}

enum { SGSR_COLOR, SGSR_DIAGONAL, SGSR_GRADIENT, SGSR_FLAT, SGSR_CROP_BORDER, SGSR_PATTERNS };
static const char *const sgsr_pattern_names[] = {"color", "aliased-diagonal", "gradient", "flat", "crop-border"};

static void sgsr_fixture(uint8_t *pixels, uint32_t w, uint32_t h, uint32_t cx, uint32_t cy,
                         uint32_t cw, uint32_t ch, unsigned pattern, int alternate_margin) {
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint8_t *p = pixels + ((size_t)y * w + x) * 4;
        p[3] = (uint8_t)(x * 13 + y * 7); /* Source alpha must not survive SGSR. */
        if (x < cx || y < cy || x >= cx + cw || y >= cy + ch) {
            p[0] = alternate_margin ? 0 : 255; p[1] = alternate_margin ? 255 : 0; p[2] = 255;
            continue;
        }
        uint32_t u = x - cx, v = y - cy;
        switch (pattern) {
        case SGSR_COLOR:
            /* Independent RGB, asymmetric tiles, gradients and oblique edges:
             * using red/blue or luminance instead of the green proxy must fail. */
            p[0] = (uint8_t)((u * 17 + v * 29) & 255);
            p[1] = (uint8_t)(((u / 7 + v / 5) & 1) ? 205 - (u * 3 + v) % 53 : 25 + (u + v * 3) % 61);
            p[2] = (uint8_t)((u * 5 + v * 11 + 73) & 255);
            break;
        case SGSR_DIAGONAL: {
            int diagonal = (int)(u * 2) >= (int)(v * 3) + 7 && (int)(u * 2) < (int)(v * 3) + 34;
            p[0] = p[1] = p[2] = diagonal ? 255 : 0; break;
        }
        case SGSR_GRADIENT:
            p[0] = (uint8_t)(19 + u * 203 / (cw > 1 ? cw - 1 : 1));
            p[1] = (uint8_t)(31 + v * 167 / (ch > 1 ? ch - 1 : 1));
            p[2] = (uint8_t)(43 + (u + v) * 149 / (cw + ch > 2 ? cw + ch - 2 : 1));
            break;
        case SGSR_FLAT: p[0] = 37; p[1] = 113; p[2] = 201; break;
        default: {
            int border = u < 2 || v < 2 || u + 2 >= cw || v + 2 >= ch;
            p[0] = border ? 211 : 19; p[1] = border ? ((u + v) & 1 ? 29 : 231) : 97; p[2] = border ? 47 : 183;
            break;
        }
        }
    }
}
