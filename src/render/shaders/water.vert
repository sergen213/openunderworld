#version 460
// Enhanced water surface vertex shader (GL 4.5 / Vulkan 1.3, same mesh layout as the scene pass). water.frag has the
// technique; the waves are shaders/waves.glsl, shared with water.frag and the caustics.
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; vec4 waterPass[3]; uvec4 rays[157]; mat4 seaWaves; // as water.frag (GL links blocks across stages)
} w;
#ifdef VULKAN
layout(set = 0, binding = 3) uniform sampler2D uRipples;
#else
layout(binding = 7) uniform sampler2D uRipples;
#endif
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
layout(location = 4) flat out float vSea;

#include "shaders/waves.glsl"

void main() {
#ifdef VULKAN
    vec4 world = pc.model * vec4(aPos, 1.0);
    vNormal = mat3(pc.model) * aNormal;
#else
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
#endif
    // A sea's waves, as its original vertex shader (tru.exe 0x69DFB0): lowered by sum c63 (1 - cos(x c60 + y c61 + c62)).
    // The gameplay surface follows the same waves (game::waveRegisters).
    float rest = world.z;
    world.z -= dot(uWaves[3], 1.0 - cos(world.x * uWaves[0] + world.y * uWaves[1] + uWaves[2]));
    float t = w.eye.w;
    vSea = any(notEqual(uWaves[3], vec4(0.0))) ? 1.0 : 0.0;
    // Godot's height layers (amplitude detail.z) and on open seas afl_ext's waves (12 iterations, its geometry count),
    // centred on that surface (afl's height averages ~0.47) but never above the rest plane, the original's own crest
    // bound, so the yacht deck and every shore stay above the drawn sea. Visible where the surface mesh has interior
    // vertices (TRU's are mostly coarse).
    float h = 0.5 * (ouFbm(world.xy / kLambdaA + t * vec2(0.10, 0.02), 0.0).x + ouFbm(kWaterRot * world.xy / kLambdaB + t * vec2(-0.04, 0.12), 0.0).x);
    world.z += w.detail.z * (h - 0.5);
    if (vSea > 0.5) world.z += kSeaAmp * (ouSea(world.xy, t, 12) - 0.47);
    world.z = min(world.z, rest);
    // The interactive wave field's height (game::WaveField), faded over its outer 4 % as in water.frag.
    if (w.rippleInfo.y > 0.5) {
        vec2 uv = (world.xy - w.ripple.xy) / (w.ripple.z * w.rippleInfo.x);
        vec2 in2 = min(uv, 1.0 - uv);
        world.z += smoothstep(0.0, 0.04, min(in2.x, in2.y)) * textureLod(uRipples, uv, 0.0).r;
    }
    vWorld = world.xyz;
    vUv = aUv; vColor = aColor;
    gl_Position = w.viewProj * world;
}
