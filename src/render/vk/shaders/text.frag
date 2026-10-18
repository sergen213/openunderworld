#version 460
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(set = 0, binding = 0) uniform sampler2D uFont;
layout(location = 0) out vec4 oColor;
void main() {
    float a = vUv.x < 0.0 ? 1.0 : texture(uFont, vUv).r; // negative uv = solid background quad
    oColor = vec4(vColor.rgb, vColor.a * a);
}
