#version 450
// Positive-height viewport: row zero remains row zero, as in the copy presenter.
layout(push_constant) uniform Crop { vec4 uv; } crop;
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = crop.uv.xy + p * crop.uv.zw;
}
