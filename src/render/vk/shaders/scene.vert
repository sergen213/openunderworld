#version 460
// Mirrors the GL scene shader (src/render/gl/GlRenderer.cpp kSceneVs).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; int lighting; } pc;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec4 vColor;
void main() {
    vNormal = mat3(pc.model) * aNormal; // ponytail: no inverse-transpose; fine without non-uniform scale
    vUv = aUv; vColor = aColor;
    gl_Position = pc.viewProj * pc.model * vec4(aPos, 1.0);
}
