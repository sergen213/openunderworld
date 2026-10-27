#version 460
// Splash droplet: soft alpha disc, simply lit.
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 absorb; vec4 shallow;
} w;
layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(location = 0) out vec4 oColor;
void main() {
    float d = length(vUv);
    float a = (1.0 - smoothstep(0.35, 1.0, d)) * vAlpha;
    if (a < 0.004) discard;
    vec3 c = vec3(0.82, 0.92, 0.97) * (0.55 + 0.45 * max(w.sunDir.z, 0.0));
    oColor = vec4(c, a * 0.85);
}
