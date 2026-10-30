#version 460
// Enhanced water surface vertex shader (GL 4.5 / Vulkan 1.3, same mesh layout as the scene pass).
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
} w;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
#ifdef VULKAN
layout(push_constant) uniform Push { mat4 model; mat4 waves; } pc;
#define uWaves pc.waves
#else
layout(location = 0) uniform mat4 uModel;
layout(location = 1) uniform mat4 uWaves; // render::waterWaves: columns c60-c63 (zero = flat)
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
    // A sea's waves, as its original vertex shader (tru.exe 0x69DFB0): lowered by sum c63 (1 - cos(x c60 + y c61 + c62)).
    world.z -= dot(uWaves[3], 1.0 - cos(world.x * uWaves[0] + world.y * uWaves[1] + uWaves[2]));
    // Water detail: geometry swell, the long waves of water.frag's swell layer as a height (amplitude detail.z);
    // only visible where the surface mesh has interior vertices (TRU's are mostly coarse).
    if (w.detail.z > 0.0) {
        float t = w.eye.w;
        world.z += w.detail.z * (sin(dot(world.xy, vec2(0.0021, 0.0013)) + t * 0.7) +
                                 0.6 * sin(dot(world.xy, vec2(-0.0017, 0.0026)) - t * 0.55));
    }
    vWorld = world.xyz;
    vUv = aUv; vColor = aColor;
    gl_Position = w.viewProj * world;
}
