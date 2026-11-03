#version 460
// Splash droplets, pass 1 of 3 (screen-space fluid rendering, docs/RENDERING.md "Water"): every droplet quad splats a
// sphere into an RG32F target cleared to (-1e20, 0) and blended with MAX: r = -(distance from the eye to the sphere's
// front), g = thickness through it (x alpha, so fading droplets thin out). Occluded fragments are rejected against the
// opaque depth copy, so the target needs no depth attachment. Adapted from talvinckb/OpenGL-Water-Simulation
// fluid_depth.frag + fluid_thickness.frag (MIT, Copyright (c) 2026 Talvin Ackbaraly).
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
} w;
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D uDepth; // opaque scene depth copy
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#else
layout(binding = 6) uniform sampler2D uDepth;
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_NDC_Z(d) (2.0 * (d) - 1.0)
#endif
layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec3 vWorld;
layout(location = 0) out vec2 oSplat;
void main() {
    // The quad spans uv -1..1 over 2 r along orthonormal right / up, so |dWorld| / |dUv| is r (before any discard).
    float r = length(dFdx(vWorld)) / max(length(dFdx(vUv)), 1e-6);
    float q = dot(vUv, vUv);
    if (q >= 1.0 || vAlpha <= 0.004) discard;
    float z = sqrt(1.0 - q);
    float dist = length(vWorld - w.eye.xyz) - r * z;
    float sd = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd < 0.999999) {
        vec2 uv = gl_FragCoord.xy * w.screen.zw;
        vec4 p = w.invViewProj * vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(sd), 1.0);
        if (length(p.xyz / p.w - w.eye.xyz) < dist) discard;
    }
    oSplat = vec2(-dist, 2.0 * r * z * vAlpha);
}
