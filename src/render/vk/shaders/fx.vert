#version 460
// Original particle effects (FrameOptions::fxVertices, src/fx): colour bytes 0x80 = 1.0.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(push_constant) uniform Push { mat4 viewProj; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
void main() {
    vUv = aUv;
    vColor = min(aColor * 2.0, vec4(2.0, 2.0, 2.0, 1.0));
    gl_Position = pc.viewProj * vec4(aPos, 1.0);
}
