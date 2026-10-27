#version 460
// Enhanced water surface vertex shader (GL 4.5 / Vulkan 1.3, same mesh layout as the scene pass).
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 absorb; vec4 shallow;
} w;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
#ifdef VULKAN
layout(push_constant) uniform Push { mat4 model; } pc;
#else
layout(location = 0) uniform mat4 uModel;
#endif
layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;
void main() {
#ifdef VULKAN
    vec4 world = pc.model * vec4(aPos, 1.0);
    vNormal = mat3(pc.model) * aNormal;
#else
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
#endif
    vWorld = world.xyz;
    vUv = aUv; vColor = aColor;
    gl_Position = w.viewProj * world;
}
