#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"

AF4 FsrRcasLoadF(ASU2 p) {
    ivec2 lo = ivec2(pc.src_rect.xy);
    ivec2 hi = lo + ivec2(pc.src_rect.zw) - 1;
    return texelFetch(post_source, clamp(lo + p, lo, hi), 0);
}
// Input is already display-referred RGB UNORM, as required by upstream RCAS.
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
#include "fsr/ffx_fsr1.h"

void main() {
    AU4 con;
    // User 0 is weakest (two stops / quarter strength), 1 is strongest.
    FsrRcasCon(con, 2.0 * (1.0 - clamp(pc.params.x, 0.0, 1.0)));
    vec3 color;
    FsrRcasF(color.r, color.g, color.b, uvec2(gl_FragCoord.xy), con);
    post_color = vec4(color, 1.0);
}
