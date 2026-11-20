#version 460
// Original particle effects (FrameOptions::fxVertices, src/fx): colour bytes 0x80 = 1.0; vRaw = the bytes / 255 and the
// sprite frame (FxVertex::normal / tangent) for lit material sprites.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec3 aTangent;
layout(push_constant) uniform Push { mat4 viewProj; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vRaw;
layout(location = 3) out vec3 vNormal;
layout(location = 4) out vec3 vTangent;
void main() {
    vUv = aUv;
    vColor = min(aColor * 2.0, vec4(2.0, 2.0, 2.0, 1.0));
    vRaw = aColor;
    vNormal = aNormal;
    vTangent = aTangent;
    gl_Position = pc.viewProj * vec4(aPos, 1.0);
}
