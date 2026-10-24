// Backend ABI: four combined linear-clamp samplers and 64 push-constant bytes.
layout(set = 0, binding = 0) uniform sampler2D post_source;
layout(set = 0, binding = 1) uniform sampler2D post_aux;
layout(set = 0, binding = 2) uniform sampler2D post_area;
layout(set = 0, binding = 3) uniform sampler2D post_search;
layout(push_constant) uniform PostConstants {
    vec4 src_rect; // Crop origin and extent, in binding-0 texels.
    vec4 sizes;    // Output extent, then binding-0 texture extent.
    vec4 params;   // RCAS: x=user sharpness[0,1]. SGSR: xy=EdgeSharpness,EdgeThreshold.
    vec4 spare;    // SGSR only: xy=crop/output ratio, zw=1/source texture extent (CPU-computed).
} pc;
layout(location = 0) out vec4 post_color;

vec2 postSourceUV(vec2 local_uv) {
    vec2 pixel = pc.src_rect.xy + local_uv * pc.src_rect.zw;
    return clamp(pixel, pc.src_rect.xy + 0.5,
                 pc.src_rect.xy + pc.src_rect.zw - 0.5) / pc.sizes.zw;
}
