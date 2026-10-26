#version 450
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
void main() {
    // Sampling decodes sRGB, and the matching sRGB attachment view re-encodes it, just as vkCmdBlitImage does.
    // The old window is RGBX, so alpha must never make the game translucent.
    colour = vec4(textureLod(picture, uv, 0.0).rgb, 1.0);
}
