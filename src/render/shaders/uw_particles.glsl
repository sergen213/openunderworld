// Enhanced under-water particles (docs/RENDERING.md "Under water: marine snow and bubbles"): the parameter block shared
// by uw_snow.* and uw_bubble.*, a literal port of Noniv/abyssal-garden (MIT, "Copyright (c) 2026 Maksymilian Dendura";
// shaders.js vsSnow / fsSnow, vsBub / fsBub; third_party/abyssal-garden/LICENSE, docs/RESEARCH.md 8.11). The vertex
// shaders work in the demo's "ag space" (metres, y up, the surface of the eye's water volume at y = 0; the fragment
// shaders get it from shaders/uw_medium.glsl agPos) and draw their quads back in TRU's world units.
// Vulkan: push constants (vertex + fragment). GL: UBO binding 7.
#ifdef VULKAN
layout(push_constant) uniform UwParticlePush {
#define OU_VERTEX_ID gl_VertexIndex
#define OU_INSTANCE_ID gl_InstanceIndex
#else
layout(std140, binding = 7) uniform UwParticlePush {
#define OU_VERTEX_ID gl_VertexID
#define OU_INSTANCE_ID gl_InstanceID
#endif
    mat4 viewProj; // world -> clip of the backend (Vulkan: with the z remap)
    vec4 eye;      // xyz camera position, w time (s): the demo's g.cam
    vec4 right;    // xyz camera right
    vec4 up;       // xyz camera up
    vec4 params;   // x snow instance count, y surface z of the eye's water volume (ouUwSurfaceZ), z units per metre
} uwp;
// Corner of the camera-facing quad drawn as 6 vertices (two triangles), -1..1 (the demo's quad(), a 4-vertex strip).
vec2 uwCorner(int i) {
    const int k[6] = int[6](0, 1, 2, 2, 1, 3);
    int c = k[i % 6];
    return vec2(float(c & 1), float(c >> 1)) * 2.0 - 1.0;
}
float uwHash(uint x) { // integer hash -> [0, 1) (lowbias32): the demo's per-speck rnd()
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return float(x >> 8) * (1.0 / 16777216.0);
}
float uwSs(float a, float b, float x) { float t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
// World (TRU units, z up) <-> ag space (= uw_medium.glsl agPos, for the vertex stage without the Water UBO).
vec3 uwAg(vec3 p) { return vec3(p.x, p.z - uwp.params.y, p.y) / uwp.params.z; }
vec3 uwWorld(vec3 a) { return vec3(a.x, a.z, a.y) * uwp.params.z + vec3(0.0, 0.0, uwp.params.y); }
