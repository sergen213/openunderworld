#version 460
// Same mapping as GL: the Vulkan backend uses a negative-height viewport, so NDC +y is up.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(push_constant) uniform Push { vec2 screen; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
void main() {
    vUv = aUv; vColor = aColor;
    gl_Position = vec4(aPos.x / pc.screen.x * 2.0 - 1.0, 1.0 - aPos.y / pc.screen.y * 2.0, 0.0, 1.0);
}
