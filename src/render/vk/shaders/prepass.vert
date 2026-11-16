#version 460
// Depth pre-pass (RT shadows / AO, screen-space AO): same position math as scene.vert (invariant), so the colour pass can
// depth-test LEQUAL against it and shade once per visible opaque pixel; the world normal for the AO's normal target.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; int lighting; float alphaRef; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out float vAlpha;
layout(location = 2) out vec3 vNormal;
layout(location = 3) out vec3 vWorld;
invariant gl_Position;
void main() {
    vec4 world = pc.model * vec4(aPos, 1.0);
    vUv = aUv; vAlpha = aColor.a;
    vNormal = mat3(pc.model) * aNormal;
    vWorld = world.xyz;
    gl_Position = pc.viewProj * world;
}
