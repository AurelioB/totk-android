#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#include "post_smaa.glsl"

void main() {
    vec2 coord = gl_FragCoord.xy / pc.sizes.xy;
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(coord, pixcoord, offset);
    // Binding 1 is the crop-sized edge map. SMAA 1x uses no subsample offsets.
    post_color = SMAABlendingWeightCalculationPS(coord, pixcoord, offset,
                                                1, 2, 3, vec4(0.0));
}
