#version 450
// Positive-height host viewport; NVN camera clip conversion is in the fragment shader.
layout(location=0) out vec2 uv;
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
    uv=p;
}
