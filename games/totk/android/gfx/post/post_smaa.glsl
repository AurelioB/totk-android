// The algorithm is the vendored SMAA.hlsl, not a rewritten approximation.
// Integer texture handles let its generic sampler macros distinguish the
// cropped color source from crop-local edges/weights and the unmodified LUTs.
// glslc -O specializes these constant handles and removes unused samplers.
vec4 postSmaaSample(int image, vec2 coord) {
    if (image == 0) return textureLod(post_source, postSourceUV(coord), 0.0);
    if (image == 1) return textureLod(post_aux, coord, 0.0);
    if (image == 2) return textureLod(post_area, coord, 0.0);
    return textureLod(post_search, coord, 0.0);
}

#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS vec4(1.0 / pc.sizes.xy, pc.sizes.xy)
#define SMAATexture2D(tex) int tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) postSmaaSample(tex, coord)
#define SMAASampleLevelZeroPoint(tex, coord) postSmaaSample(tex, coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) postSmaaSample(tex, (coord) + vec2(offset) * SMAA_RT_METRICS.xy)
#define SMAASample(tex, coord) postSmaaSample(tex, coord)
#define SMAASamplePoint(tex, coord) postSmaaSample(tex, coord)
#define SMAASampleOffset(tex, coord, offset) postSmaaSample(tex, (coord) + vec2(offset) * SMAA_RT_METRICS.xy)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) fma(a, b, c)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#include "smaa/SMAA.hlsl"
