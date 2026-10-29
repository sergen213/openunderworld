#version 460
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(set = 0, binding = 0) uniform sampler2D uFont;
layout(set = 1, binding = 0) uniform sampler2D uImage0; // FrameOptions::uiImages (Renderer.hpp TextVertex)
layout(set = 2, binding = 0) uniform sampler2D uImage1;
layout(set = 3, binding = 0) uniform sampler2D uImage2;
layout(location = 0) out vec4 oColor;
void main() {
    if (vUv.x >= 6.0) { oColor = texture(uImage2, vUv - vec2(6.0, 0.0)) * vColor; return; }
    if (vUv.x >= 4.0) { oColor = texture(uImage1, vUv - vec2(4.0, 0.0)) * vColor; return; }
    if (vUv.x >= 2.0) { oColor = texture(uImage0, vUv - vec2(2.0, 0.0)) * vColor; return; }
    float a = vUv.x < 0.0 ? 1.0 : texture(uFont, vUv).r; // negative uv = solid background quad
    oColor = vec4(vColor.rgb, vColor.a * a);
}
