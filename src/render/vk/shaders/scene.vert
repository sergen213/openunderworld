#version 460
// Mirrors the GL scene shader (src/render/gl/GlRenderer.cpp kSceneVs).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; mat4 lightViewProj; int lighting; int shadows; float alphaRef; } pc;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec4 vColor;
layout(location = 3) out vec4 vLight;
layout(location = 4) out vec3 vWorld; // ray-traced variant only (scene.frag OU_RT)
#ifdef OU_RT
invariant gl_Position; // = prepass.vert: the colour pass depth-tests against the RT depth pre-pass
#endif
void main() {
    vec4 world = pc.model * vec4(aPos, 1.0);
    vNormal = mat3(pc.model) * aNormal; // ponytail: no inverse-transpose; fine without non-uniform scale
    vUv = aUv; vColor = aColor;
    vLight = pc.lightViewProj * world;
    vWorld = world.xyz;
    gl_Position = pc.viewProj * world;
}
