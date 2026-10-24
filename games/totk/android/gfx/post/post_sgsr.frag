#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"

// SGSR1 OperationMode 1 (RGBA, using green as the luma proxy), ported from
// SnapdragonGameStudios/snapdragon-gsr d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e:
// sgsr/v1/include/glsl/sgsr1_shader_mobile.frag and
// sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag (UseEdgeDirection).
// Exact upstream sources and BSD-3-Clause LICENSE are retained in sgsr/.
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// The basic and directional variants intentionally use different normalization
// constants, as in their respective upstream files. All arithmetic is float32;
// the original ES mediump qualifiers are not retained. No separate RCAS pass.
// params.x = EdgeSharpness (default 2), params.y = EdgeThreshold (default 8/255).
// spare.xy = crop extent / output extent, spare.zw = 1 / source texture extent;
// both are precomputed once per frame for the current binding-0 image.
layout(constant_id = 0) const bool post_sgsr_edge_direction = false;
// Scene canvases can retain HDR highlights. Never clip them to UNORM merely
// because the presentation-only variant used a display-encoded target.
layout(constant_id = 1) const bool post_scene_hdr = false;

// Work in crop-local source pixels, not normalized whole-texture coordinates.
// Integer crop origins/extents preserve the upstream gather footprint. A gather
// at integer lattice point q has base q-1 and order (0,1),(1,1),(1,0),(0,0).
// Only use hardware filtering when its entire footprint lies inside the crop;
// otherwise clamp each load before adding the crop origin. This also isolates
// nonzero crops from neighboring texels in their larger source texture.
vec3 postSgsrColor(vec2 sourcePixel) {
    vec2 centered = sourcePixel - 0.5;
    ivec2 base = ivec2(floor(centered));
    ivec2 hi = ivec2(pc.src_rect.zw) - 1;
    if (all(greaterThanEqual(base, ivec2(0))) && all(lessThan(base, hi))) {
        return textureLod(post_source, (pc.src_rect.xy + sourcePixel) * pc.spare.zw, 0.0).rgb;
    }
    ivec2 origin = ivec2(pc.src_rect.xy);
    vec2 f = fract(centered);
    vec3 c00 = texelFetch(post_source, origin + clamp(base, ivec2(0), hi), 0).rgb;
    vec3 c10 = texelFetch(post_source, origin + clamp(base + ivec2(1, 0), ivec2(0), hi), 0).rgb;
    vec3 c01 = texelFetch(post_source, origin + clamp(base + ivec2(0, 1), ivec2(0), hi), 0).rgb;
    vec3 c11 = texelFetch(post_source, origin + clamp(base + ivec2(1, 1), ivec2(0), hi), 0).rgb;
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

vec4 postSgsrGather(ivec2 q) {
    ivec2 base = q - 1;
    ivec2 hi = ivec2(pc.src_rect.zw) - 1;
    if (all(greaterThanEqual(base, ivec2(0))) && all(lessThan(base, hi))) {
        return textureGather(post_source, (pc.src_rect.xy + vec2(q)) * pc.spare.zw, 1);
    }
    ivec2 origin = ivec2(pc.src_rect.xy);
    return vec4(
        texelFetch(post_source, origin + clamp(base + ivec2(0, 1), ivec2(0), hi), 0).g,
        texelFetch(post_source, origin + clamp(base + ivec2(1, 1), ivec2(0), hi), 0).g,
        texelFetch(post_source, origin + clamp(base + ivec2(1, 0), ivec2(0), hi), 0).g,
        texelFetch(post_source, origin + clamp(base, ivec2(0), hi), 0).g);
}

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

vec2 weightY(float dx, float dy, float c, vec3 data) {
    float std = data.x;
    float x;
    if (post_sgsr_edge_direction) {
        vec2 dir = data.yz;
        float edgeDis = ((dx * dir.y) + (dy * dir.x));
        x = (((dx * dx) + (dy * dy)) +
             ((edgeDis * edgeDis) * ((clamp(((c * c) * std), 0.0, 1.0) * 0.7) + -1.0)));
    } else {
        x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    }
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}

vec2 edgeDirection(vec4 left, vec4 right) {
    float RxLz = (right.x + (-left.z));
    float RwLy = (right.w + (-left.y));
    vec2 delta;
    delta.x = (RxLz + RwLy);
    delta.y = (RxLz + (-RwLy));
    float lengthInv = inversesqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    return delta * lengthInv;
}

void main() {
    // A precomputed scale avoids per-fragment division and keeps 1x exact.
    vec2 sourcePixel = gl_FragCoord.xy * pc.spare.xy;
    vec3 color = postSgsrColor(sourcePixel);
    vec2 imgCoord = sourcePixel + vec2(-0.5, 0.5);
    vec2 imgCoordPixel = floor(imgCoord);
    ivec2 coord = ivec2(imgCoordPixel);
    vec2 pl = imgCoord + (-imgCoordPixel);
    vec4 left = postSgsrGather(coord);

    float edgeVote = abs(left.z - left.y) + abs(color.g - left.y) + abs(color.g - left.z);
    if (edgeVote > pc.params.y) {
        vec4 right = postSgsrGather(coord + ivec2(2, 0));
        vec4 upDown;
        upDown.xy = postSgsrGather(coord + ivec2(1, -1)).wz;
        upDown.zw = postSgsrGather(coord + ivec2(1, 1)).yx;

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        float colorY = color.g - mean;

        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                      (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +
                     (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        vec3 data;
        if (post_sgsr_edge_direction) {
            float sumMean = 1.014185e+01 / sum;
            float std = (sumMean * sumMean);
            data = vec3(std, edgeDirection(left, right));
        } else {
            data = vec3(2.181818 / sum, 0.0, 0.0);
        }

        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, data);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, data);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, data);
        aWY += weightY(pl.x, pl.y, left.z, data);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, data);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, data);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, data);

        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(pc.params.x * finalY, minY, maxY);
        float deltaY = finalY - colorY;
        // Upstream high-contrast smoothing, applied equally to encoded RGB.
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);
        color = max(color + vec3(deltaY), vec3(0.0));
        if (!post_scene_hdr) color = min(color, vec3(1.0));
    }
    post_color = vec4(color, 1.0);
}
