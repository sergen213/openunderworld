#version 460
// Enhanced water surface vertex shader (GL 4.5 / Vulkan 1.3, same mesh layout as the scene pass). water.frag has the
// technique; the surface is shaders/water_surface.glsl, shared with water.frag and the other water effects.
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
layout(set = 0, binding = 4) uniform sampler2DArray uOcean; // the FFT ocean (shaders/ocean.comp)
#else
layout(binding = 7) uniform sampler2D uRipples;
layout(binding = 12) uniform sampler2DArray uOcean;
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

#include "shaders/water_surface.glsl"

void main() {
#ifdef VULKAN
    vec4 world = pc.model * vec4(aPos, 1.0);
    vNormal = mat3(pc.model) * aNormal;
#else
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
#endif
    // The surface (shaders/water_surface.glsl): a sea's waves as its original vertex shader (tru.exe 0x69DFB0: lowered by
    // sum c63 (1 - sin(x c60 + y c61 + c62)); the gameplay surface follows the same waves, game::waveRegisters), Godot's
    // height layers or on open seas the FFT ocean / afl_ext's waves, capped at the rest plane, plus the interactive wave
    // field on top. TRU's surface meshes are coarse (~3.5 m quads): with OU_TESS (tessellation shaders available) this
    // stage passes the undisplaced surface on and water.tesc / water.tese subdivide it inside the field's window and
    // displace every generated vertex; without, the mesh's own vertices are displaced here.
    vSea = any(notEqual(uWaves[3], vec4(0.0))) ? 1.0 : 0.0;
#ifndef OU_TESS
    world.z += ouBaseHeight(world.xy, uWaves, vSea > 0.5, w.eye.w) + ouFieldHeight(world.xy);
#endif
    vWorld = world.xyz;
    vUv = aUv; vColor = aColor;
    gl_Position = w.viewProj * world;
}
