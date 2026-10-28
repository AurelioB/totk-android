#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
// Only the pre-UI scene was extrapolated. Current real UI stays in canvas
// coordinates and uses the same exact affine black/white blend as interpolation.
layout(set=0,binding=2) uniform sampler2D extrapolated_scene;
layout(set=0,binding=5) uniform sampler2D ui_black;
layout(set=0,binding=6) uniform sampler2D ui_white;
void main() {
    vec3 scene=texture(extrapolated_scene,uv).rgb;
    vec3 black=texture(ui_black,uv).rgb,white=texture(ui_white,uv).rgb;
    color=vec4(scene*(white-black)+black,1.0);
}
