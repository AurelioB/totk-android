#version 450
layout(location=0) in vec2 position;
layout(set=0,binding=0,std140) uniform UI { vec4 offset; vec4 color; } ui;
void main() { gl_Position=vec4(position+ui.offset.xy,0.0,1.0); }
