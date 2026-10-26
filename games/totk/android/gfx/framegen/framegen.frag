#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform sampler2D scene_image;
layout(set=0,binding=1) uniform sampler2D depth_image;
// Raw UNORM final real image: never automatically decode display RGB.
layout(set=0,binding=2) uniform sampler2D final_image;
layout(set=0,binding=9) uniform sampler2D fallback_image;
// Raw encoded real N; never an earlier synthetic terminal or an HDR scene.
layout(set=0,binding=10) uniform sampler2D earlier_real;
layout(push_constant) uniform Push {
    vec4 row[4]; // Row-major raw NVN current clip -> predicted clip.
    vec4 options; // scene encoding (-1: linear preUI warp; 2: replayed final copy), near, inverse extent.
} pc;

bool finite_value(float v) { return !isnan(v) && !isinf(v); }
bool inside(vec2 p) {
    vec2 edge=0.5/vec2(textureSize(depth_image,0));
    return all(greaterThanEqual(p,edge)) && all(lessThanEqual(p,vec2(1.0)-edge));
}
bool valid_depth(float d) { return finite_value(d) && d>=0.0 && d<1.0-1e-6; }
float distance_at(float d) { return pc.options.y/max(1.0-d,1e-6); }
vec3 scene_rgb(vec2 p) { return texture(scene_image,p).rgb; }
bool project_source(vec2 p,out vec2 projected,out float depth) {
    if (!inside(p)) return false;
    depth=texture(depth_image,p).r;
    if (!valid_depth(depth)) return false;
    // Source game draws flip the viewport and convert OpenGL/NVN clip depth.
    // Undo both before using its raw camera matrices.
    vec4 clip=vec4(2.0*p.x-1.0,1.0-2.0*p.y,2.0*depth-1.0,1.0);
    vec4 predicted=vec4(dot(pc.row[0],clip),dot(pc.row[1],clip),dot(pc.row[2],clip),dot(pc.row[3],clip));
    if (any(isnan(predicted)) || any(isinf(predicted)) || predicted.w<=1e-5 || abs(predicted.z)>predicted.w) return false;
    projected=vec2(predicted.x,-predicted.y)/predicted.w*0.5+0.5;
    return all(lessThanEqual(abs(projected-p),vec2(0.04)));
}
bool continuous_depth(vec2 p,float center) {
    vec2 texel=1.0/vec2(textureSize(depth_image,0));
    float lo=center, hi=center;
    const vec2 offsets[4]=vec2[4](vec2(-1,0),vec2(1,0),vec2(0,-1),vec2(0,1));
    for (int i=0;i<4;i++) {
        vec2 q=p+offsets[i]*texel;
        if (!inside(q)) return false;
        float d=texture(depth_image,q).r;
        if (!valid_depth(d)) return false;
        lo=min(lo,d); hi=max(hi,d);
    }
    // Hard normalized depth edge threshold is shared with the host golden tests.
    // Relative view-distance catches distant thin edges that raw depth compresses.
    return hi-lo<=0.02 && distance_at(hi)<=distance_at(lo)*1.08;
}
bool fallback_at(vec2 p) {
    ivec2 extent=textureSize(fallback_image,0);
    ivec2 base=ivec2(floor(p*vec2(extent)-0.5));
    // The spatial kernel is baked into retained real scenes. Its authority
    // halo is included once in pair dilation; output needs only bilinear taps.
    for (int y=0;y<2;y++) for (int x=0;x<2;x++)
        if (texelFetch(fallback_image,clamp(base+ivec2(x,y),ivec2(0),extent-1),0).r>0.0) return true;
    return false;
}
bool fallback_footprint(vec2 p) {
    // The final output may itself be scaled with bilinear filtering. Include
    // each final-image tap's canvas footprint, not just its nearest mask texel.
    ivec2 extent=textureSize(final_image,0);
    ivec2 base=ivec2(floor(p*vec2(extent)-0.5));
    for (int y=0;y<2;y++) for (int x=0;x<2;x++) {
        ivec2 tap=clamp(base+ivec2(x,y),ivec2(0),extent-1);
        if (fallback_at((vec2(tap)+0.5)/vec2(extent))) return true;
    }
    return false;
}
void main() {
    vec4 real=texture(final_image,uv);
    color=real;
    if (pc.options.x>3.5) {
        // A midpoint is equally far from both real ticks: ties choose earlier.
        // Substitute AFTER the guest terminal encoding, preserving actual RGB
        // bits rather than applying UI or display transfer twice. Alpha is the
        // existing opaque interpolation output contract, not scene alpha.
        if (fallback_footprint(uv)) color=vec4(texture(earlier_real,uv).rgb,1.0);
        return;
    }
    if (pc.options.x > 1.5) return; // exact replayed scene+UI, already final-transfer encoded
    if (!inside(uv)) return;
    vec2 source=uv;
    vec2 projected;
    float depth;
    // Fixed-point inverse warp. Never clamp an invalid iterate onto an edge:
    // newly revealed content and non-convergent mappings keep the real pixel.
    for (int i=0;i<3;i++) {
        if (!project_source(source,projected,depth)) return;
        source+=uv-projected;
    }
    if (!project_source(source,projected,depth) ||
        any(greaterThan(abs(projected-uv),pc.options.zw*1.5)) || !continuous_depth(source,depth)) return;
    float target_depth=texture(depth_image,uv).r;
    if (!valid_depth(target_depth)) return;
    float a=distance_at(depth),b=distance_at(target_depth);
    if (max(a,b)>min(a,b)*1.12) return;
    vec3 warped=scene_rgb(source);
    if (any(isnan(warped)) || any(isinf(warped))) return;
    color=vec4(warped,real.a);
}
