#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#include "post_smaa.glsl"

void main() {
    // Derive all taps from the actual pixel center, not interpolator precision.
    vec2 coord = gl_FragCoord.xy / pc.sizes.xy;
    vec4 offset[3];
    SMAAEdgeDetectionVS(coord, offset);
    // The upstream function discards non-edges: attachment must clear to zero.
    post_color = vec4(SMAALumaEdgeDetectionPS(coord, offset, 0), 0.0, 0.0);
}
