#version 460
// Splash droplet: camera-facing quad expanded on the CPU (render::SplashVertex).
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
} w;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in float aAlpha;
layout(location = 0) out vec2 vUv;
layout(location = 1) out float vAlpha;
void main() {
    vUv = aUv; vAlpha = aAlpha;
    gl_Position = w.viewProj * vec4(aPos, 1.0);
}
