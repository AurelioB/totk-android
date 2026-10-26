#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform sampler2D canvas;
void main() {
    vec4 c=texture(canvas,uv);
    vec3 rgb=clamp(c.rgb,vec3(0),vec3(1));
    color=vec4(mix(1.055*pow(rgb,vec3(1.0/2.4))-0.055,12.92*rgb,lessThanEqual(rgb,vec3(0.0031308))),c.a);
}
