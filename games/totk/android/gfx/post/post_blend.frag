#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#include "post_smaa.glsl"

void main() {
    vec2 coord = gl_FragCoord.xy / pc.sizes.xy;
    vec4 offset;
    SMAANeighborhoodBlendingVS(coord, offset);
    // Color maps into src_rect; weights remain in the crop-local UV domain.
    post_color = SMAANeighborhoodBlendingPS(coord, offset, 0, 1);
}
