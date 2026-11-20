#version 460
// Enhanced water surface, tessellation evaluation (water.tesc has the subdivision): each generated vertex interpolated
// over the mesh triangle and displaced as water.vert displaces the mesh's own vertices without tessellation. Position
// math is `precise`: the vertices on an edge two triangles share come out bit-identical in both (the generator emits
// swapped barycentrics there, and a fused multiply-add would round the two sums differently), so no cracks open.
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; vec4 waterPass[3]; uvec4 rays[157]; mat4 seaWaves; vec4 optics[3]; vec4 ocean[2]; vec4 medium; // as water.frag (GL links blocks across stages)
} w;
#ifdef VULKAN
layout(set = 0, binding = 3) uniform sampler2D uRipples;
layout(set = 0, binding = 4) uniform sampler2DArray uOcean;
layout(push_constant) uniform Push { mat4 model; mat4 waves; } pc;
#define uWaves pc.waves
#else
layout(binding = 7) uniform sampler2D uRipples;
layout(binding = 12) uniform sampler2DArray uOcean;
layout(location = 1) uniform mat4 uWaves; // as water.vert
#endif
layout(triangles, fractional_odd_spacing, ccw) in;
layout(location = 0) in vec3 tWorld[];
layout(location = 1) in vec3 tNormal[];
layout(location = 2) in vec2 tUv[];
layout(location = 3) in vec4 tColor[];
layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;
layout(location = 4) flat out float vSea;

#include "shaders/water_surface.glsl"

void main() {
    vec3 b = gl_TessCoord;
    precise vec3 world = b.x * tWorld[0] + b.y * tWorld[1] + b.z * tWorld[2];
    vSea = any(notEqual(uWaves[3], vec4(0.0))) ? 1.0 : 0.0;
    world.z += ouBaseHeight(world.xy, uWaves, vSea > 0.5, w.eye.w) + ouFieldHeight(world.xy);
    vWorld = world;
    vNormal = b.x * tNormal[0] + b.y * tNormal[1] + b.z * tNormal[2];
    vUv = b.x * tUv[0] + b.y * tUv[1] + b.z * tUv[2];
    vColor = b.x * tColor[0] + b.y * tColor[1] + b.z * tColor[2];
    precise vec4 clip = w.viewProj * vec4(world, 1.0);
    gl_Position = clip;
}
