#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"

// A hardware gather clamps to the whole image, not src_rect. Use it for the
// interior; at the crop boundary reproduce its four taps with clamped loads.
// Integer-aligned crops preserve the upstream gather footprint and ordering.
bool postEasuInterior(vec2 p) {
    ivec2 base = ivec2(floor(p * pc.sizes.zw - 0.5));
    ivec2 lo = ivec2(pc.src_rect.xy);
    ivec2 hi = lo + ivec2(pc.src_rect.zw) - 1;
    return all(greaterThanEqual(base, lo)) && all(lessThan(base, hi));
}
mat4 postEasuBorder(vec2 p) {
    ivec2 base = ivec2(floor(p * pc.sizes.zw - 0.5));
    ivec2 lo = ivec2(pc.src_rect.xy);
    ivec2 hi = lo + ivec2(pc.src_rect.zw) - 1;
    return transpose(mat4(
        texelFetch(post_source, clamp(base + ivec2(0, 1), lo, hi), 0),
        texelFetch(post_source, clamp(base + ivec2(1, 1), lo, hi), 0),
        texelFetch(post_source, clamp(base + ivec2(1, 0), lo, hi), 0),
        texelFetch(post_source, clamp(base, lo, hi), 0)));
}
AF4 FsrEasuRF(AF2 p) {
    if (postEasuInterior(p)) return textureGather(post_source, p, 0);
    return postEasuBorder(p)[0];
}
AF4 FsrEasuGF(AF2 p) {
    if (postEasuInterior(p)) return textureGather(post_source, p, 1);
    return postEasuBorder(p)[1];
}
AF4 FsrEasuBF(AF2 p) {
    if (postEasuInterior(p)) return textureGather(post_source, p, 2);
    return postEasuBorder(p)[2];
}
#define FSR_EASU_F 1
#include "fsr/ffx_fsr1.h"

void main() {
    AU4 con0, con1, con2, con3;
    FsrEasuConOffset(con0, con1, con2, con3,
                     pc.src_rect.z, pc.src_rect.w, pc.sizes.z, pc.sizes.w,
                     pc.sizes.x, pc.sizes.y, pc.src_rect.x, pc.src_rect.y);
    vec3 color;
    FsrEasuF(color, uvec2(gl_FragCoord.xy), con0, con1, con2, con3);
    post_color = vec4(color, 1.0);
}
