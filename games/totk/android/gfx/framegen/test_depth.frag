#version 450
// Deliberately unproven numeric depth export; same UBO/VS interface as test_ui.
layout(set=0,binding=0,std140) uniform UI { vec4 offset; vec4 color; } ui;
layout(location=0) out vec4 color;
void main() {
    color=ui.color;
    gl_FragDepth=ui.offset.z;
}
